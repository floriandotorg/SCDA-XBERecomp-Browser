const instructionCalls = new Set(['if', 'X86_MEM8', 'X86_MEM16', 'X86_MEM32', 'X86_MEM64', 'X86_MEMF32', 'X86_MEMF64', 'X86_PUSH32', 'X86_PARITY', 'X87_ST', 'x87_push', 'x87_pop', 'x87_compare', 'x87_status', 'std::memcpy', 'std::sqrt', 'std::fabs', 'sse_min', 'sse_max', 'sse_cmp', 'sse_cvt', 'sse_cvtt', '__builtin_bswap32', '__builtin_ctz']);
const leafCalls = new Set([...instructionCalls, 'X86_LOAD_FLAGS', 'X86_SAVE_FLAGS', 'x87_round', 'x87_to_i16', 'x87_to_i32', 'x87_to_i64', 'x87_examine', 'sse_cvt_guest']);

const scopedAccess = (source, scope) => {
  const types = { 8: 'uint8_t', 16: 'uint16_t', 32: 'uint32_t', 64: 'uint64_t', F32: 'float', F64: 'double' };
  return source.replace(/\bX86_MEM(8|16|32|64|F32|F64)\(/g, (_, type) => `browser::BlockAccess<${types[type]}>(${scope}, ctx, `)
    .replace(/\bX86_PUSH32\(/g, `browser::BlockPush32(${scope}, ctx, `)
    .replace(/\bX86_PTR\(\s*X86Xmm\s*,/g, `browser::BlockPointer<X86Xmm>(${scope}, ctx, `);
};

const leaseLeaf = source => {
  if (source.includes('browser::Memory::BlockScope guestMemoryBlock')) { return source; }
  const body = source.slice(source.indexOf('{') + 1, source.lastIndexOf('}')).replace(/\/\/[^\n]*/g, '');
  if (body.split('\n').filter(line => line.trim()).length > 64 || /\b(?:try|catch|throw|for|while|do)\b/.test(body)) { return undefined; }
  const pointers = [...body.matchAll(/X86_PTR\(\s*([^,]+),/g)];
  if (pointers.some(([, type]) => type.trim() !== 'X86Xmm')) { return undefined; }
  const calls = [...body.matchAll(/\b([A-Za-z_][A-Za-z_0-9:]*)\s*\(/g)];
  if (calls.some(([, name]) => !leafCalls.has(name) && !(name === 'X86_PTR' && pointers.length))) { return undefined; }
  const labels = new Map([...body.matchAll(/^\s*(loc_[0-9A-F]+):/gm)].map(match => [match[1], match.index]));
  const jumps = [...body.matchAll(/\bgoto\s+(\w+)/g)];
  if (jumps.length !== [...body.matchAll(/\bgoto\b/g)].length || jumps.some(match => !labels.has(match[1]) || labels.get(match[1]) <= match.index)) { return undefined; }
  if ([...body.matchAll(/\bX86_(?:MEM\w+|PUSH32|PTR)\(/g)].length < 2) { return undefined; }
  return scopedAccess(source.replace('{\n', '{\n\tbrowser::Memory::BlockScope guestMemoryBlock{browser::Memory::From(base)};\n'), 'guestMemoryBlock');
};

const leasePointers = source => source.split('\n').map(line => {
    const pointers = [...line.matchAll(/X86_PTR\(\s*([^,]+),/g)];
    const raw = pointers.filter(([, type]) => type.trim() !== 'X86Xmm');
    if (!raw.length) { return line; }
    if (raw.some(([, type]) => !['uint8_t', 'uint32_t'].includes(type.trim()))) {
      throw new Error('Unsupported generated host pointer type');
    }
    if (!/^\s*\{ (?:auto guestMemoryLease = browser::Memory::From\(base\).Borrow\(\); )?uint32_t ea = .+ \}$/.test(line)) {
      throw new Error('Unscoped generated host pointer');
    }
    if (line.includes('auto guestMemoryLease =')) { return line; }
    return line.replace('{ uint32_t ea =', '{ auto guestMemoryLease = browser::Memory::From(base).Borrow(); uint32_t ea =');
  }).join('\n');

const leaseBlocks = source => {
  const lines = source.replace(/\bx86_rep_(movs|stos)\(ctx, base, ([124])\)/g, (_, operation, width) =>
    `browser::Memory::From(base).${operation === 'movs' ? 'MoveRepeated' : 'StoreRepeated'}(ctx, ${width})`)
    .replace(/\b(sub_[0-9A-F]{8})\(ctx, base\); return;/g, 'BROWSER_GUEST_TAIL $1(ctx, base);').split('\n');
  if (lines.some(line => line.includes('.BeginBlock();') || line.includes('BlockScope guestInstructionBlock'))) { return lines.join('\n'); }
  const output = [];
  let pending = [];
  let instructions = 0;
  let accesses = 0;
  const flush = () => {
    const following = [];
    while (pending.length && /^\s*\/\//.test(pending.at(-1))) { following.unshift(pending.pop()); }
    if (instructions >= 2 && accesses >= 2) {
      const indent = /^\s*/.exec(pending[0])[0];
      output.push(`${indent}{ browser::Memory::BlockScope guestInstructionBlock{browser::Memory::From(base)};`,
        scopedAccess(pending.join('\n'), 'guestInstructionBlock'), `${indent}}`);
    } else {
      output.push(...pending);
    }
    output.push(...following);
    pending = [];
    instructions = 0;
    accesses = 0;
  };
  for (const line of lines) {
    if (/^\s*\/\/ [0-9A-F]{8} /.test(line)) { pending.push(line); continue; }
    const counters = [];
    const bounded = line.replace(/for \(int ([A-Za-z_]\w*) = 0; \1 < 4; (?:\+\+\1|\1\+\+)\)/g, (_, name) => {
      counters.push(name);
      return '';
    });
    const boundedCounters = counters.every(name => !new RegExp(`\\b${name}\\s*(?:[+*/%&|^<>-]*=(?!=)|\\+\\+|--)|(?:\\+\\+|--|&)\\s*\\b${name}\\b`).test(bounded));
    const calls = [...bounded.matchAll(/\b([A-Za-z_][A-Za-z_0-9:]*)\s*\(/g)];
    const valuePointers = [...line.matchAll(/X86_PTR\(\s*([^,]+),/g)];
    const scoped = /^\s*\{ .+ \}$/.test(line);
    const assignment = /^\s*ctx\.\w+ = [^;]+;$/.test(line);
    if ((scoped || assignment) && boundedCounters && !/\b(?:goto|return|throw|break|continue)\b/.test(line) && calls.every(([, name]) =>
      instructionCalls.has(name) || (name === 'X86_PTR' && valuePointers.length && valuePointers.every(([, type]) => type.trim() === 'X86Xmm')))) {
      pending.push(line);
      ++instructions;
      accesses += [...line.matchAll(/\bX86_(?:MEM\w+|PUSH32|PTR)\(/g)].length;
      if (instructions === 64) { flush(); }
    } else {
      flush();
      output.push(line);
    }
  }
  flush();
  return output.join('\n');
};

export const adaptMemory = (source, { blocks = false } = {}) => {
  const adapted = leasePointers(source);
  if (!blocks) { return adapted; }
  const output = [];
  let cursor = 0;
  for (const match of adapted.matchAll(/^X86_FUNC\([^)]+\) \{\n[\s\S]*?^\}/gm)) {
    const body = /^\s*try\s*\{/m.test(match[0]) ? match[0] : leaseLeaf(match[0]) ?? leaseBlocks(match[0]);
    output.push(leaseBlocks(adapted.slice(cursor, match.index)), body);
    cursor = match.index + match[0].length;
  }
  output.push(leaseBlocks(adapted.slice(cursor)));
  return output.join('');
};
