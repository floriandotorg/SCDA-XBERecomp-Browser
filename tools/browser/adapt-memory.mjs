export const adaptMemory = source => source.split('\n').map(line => {
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
