const reject = () => { throw new Error('Invalid or duplicate export catalog declaration.'); };
const sizeOf = (expression) => {
  if (!/^\s*(?:0x[\da-f]+|\d+)(?:\s*[+*]\s*(?:0x[\da-f]+|\d+))*\s*$/i.test(expression)) { reject(); }
  const size = expression.split('+').reduce((sum, term) => sum + term.split('*').reduce((product, value) => product * Number(value.trim()), 1), 0);
  if (!Number.isSafeInteger(size) || size <= 0 || size > 4096) { reject(); }
  return size;
};

export const exportCatalog = (files) => {
  const entries = new Map();
  const insert = (entry) => {
    if (!Number.isInteger(entry.ordinal) || entry.ordinal <= 0 || entry.ordinal >= 400 || entries.has(entry.ordinal)) { reject(); }
    entries.set(entry.ordinal, entry);
  };
  for (const { path, text } of files) {
    for (const [, ordinal, name, entry] of text.matchAll(/\bRegisterExport\((\d+),\s*"(\w+)",\s*(\w+)\)/g)) {
      insert({ ordinal: Number(ordinal), name, kind: 'function', abi: 'custom', entry, source: path });
    }
    for (const match of text.matchAll(/^KERNEL_(EXPORT|FASTCALL|DATA)\(([^)\n]*)\)/gm)) {
      const [, type, declaration] = match;
      const [ordinalText, name, argument, ...extra] = declaration.split(',').map((value) => value.trim());
      const ordinal = Number(ordinalText);
      if (!Number.isInteger(ordinal) || ordinal <= 0 || ordinal >= 400 || entries.has(ordinal) ||
          !/^\w+$/.test(name) || extra.length || (type === 'FASTCALL' ? argument !== undefined : argument === undefined)) { reject(); }
      const entry = { ordinal, name, kind: type === 'DATA' ? 'data' : 'function' };
      if (type === 'DATA') {
        entry.size = sizeOf(argument);
      } else {
        entry.abi = type === 'FASTCALL' ? 'fastcall' : 'stdcall';
        if (type === 'EXPORT') {
          if (!/^\d+$/.test(argument) || Number(argument) > 16) { reject(); }
          entry.arguments = Number(argument);
        }
      }
      insert({ ...entry, source: path });
    }
  }
  return [...entries.values()].sort((left, right) => left.ordinal - right.ordinal);
};
