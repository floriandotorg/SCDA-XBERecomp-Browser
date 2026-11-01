export const adaptArithmetic = source => {
  const lines = source.split('\n');
  return lines.map((line, n) => {
    const operation = /\/\/ [0-9A-F]{8} (rcl|rcr) /.exec(lines[n - 1] ?? '')?.[1];
    if (!operation || line.includes('uint32_t rotateCount =')) { return line; }
    const width = Number(/\buint(8|16|32)_t a =/.exec(line)?.[1]);
    const count = /uint32_t n = \(\(uint32_t\)\((.+?)\) & 31\) % (\d+);/.exec(line);
    const carry = operation === 'rcl' ? 'hi' : 'lo';
    if (!width || !count || Number(count[2]) !== width + 1 || !line.includes(`cf = ${carry}; }`)) {
      throw new Error('Unsupported generated carry rotation');
    }
    let adapted = line.replace(count[0], `uint32_t rotateCount = (uint32_t)(${count[1]}) & 31; uint32_t n = rotateCount % ${width + 1};`);
    if (operation === 'rcr') {
      adapted = adapted.replace('for (; n;', `if (rotateCount == 1) { of = ((uint32_t)(a >> ${width - 1}) & 1) ^ cf; } for (; n;`);
    } else {
      adapted = adapted.replace('cf = hi; }', `cf = hi; } if (rotateCount == 1) { of = ((uint32_t)(a >> ${width - 1}) & 1) ^ cf; }`);
    }
    return adapted;
  }).join('\n');
};
