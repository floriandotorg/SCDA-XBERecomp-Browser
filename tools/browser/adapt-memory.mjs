const instructionCalls = new Set(['if', 'X86_MEM8', 'X86_MEM16', 'X86_MEM32', 'X86_MEM64', 'X86_MEMF32', 'X86_MEMF64', 'X86_PUSH32', 'X86_PARITY', 'X87_ST', 'x87_push', 'x87_pop', 'x87_compare', 'x87_status', 'std::memcpy', 'std::sqrt', 'std::fabs', 'sse_min', 'sse_max', 'sse_cmp', 'sse_cvt', 'sse_cvtt', '__builtin_bswap32', '__builtin_ctz']);

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
  const lines = source.replace(/\b(sub_[0-9A-F]{8})\(ctx, base\); return;/g, 'BROWSER_GUEST_TAIL $1(ctx, base);').split('\n');
  if (lines.some(line => line.includes('.BeginBlock();'))) { return lines.join('\n'); }
  const output = [];
  let pending = [];
  let instructions = 0;
  let accesses = 0;
  const flush = () => {
    const following = [];
    while (pending.length && /^\s*\/\//.test(pending.at(-1))) { following.unshift(pending.pop()); }
    if (instructions >= 2 && accesses >= 2) {
      const indent = /^\s*/.exec(pending[0])[0];
      output.push(`${indent}{ browser::Memory::From(base).BeginBlock(); try {`, ...pending,
        `${indent}} catch (...) { browser::Memory::From(base).EndBlock(); throw; } browser::Memory::From(base).EndBlock(); }`);
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
    const calls = [...line.matchAll(/\b([A-Za-z_][A-Za-z_0-9:]*)\s*\(/g)];
    const scoped = /^\s*\{ .+ \}$/.test(line);
    const assignment = /^\s*ctx\.\w+ = [^;]+;$/.test(line);
    if ((scoped || assignment) && !/\b(?:goto|return|throw|break|continue)\b/.test(line) && calls.every(([, name]) => instructionCalls.has(name))) {
      pending.push(line);
      ++instructions;
      accesses += [...line.matchAll(/\bX86_(?:MEM\w+|PUSH32)\(/g)].length;
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
    if (!/^\s*try\s*\{/m.test(match[0])) { continue; }
    output.push(leaseBlocks(adapted.slice(cursor, match.index)), match[0]);
    cursor = match.index + match[0].length;
  }
  output.push(leaseBlocks(adapted.slice(cursor)));
  return output.join('');
};
