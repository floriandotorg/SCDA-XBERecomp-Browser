import { spawnSync } from 'node:child_process';
import { setTimeout } from 'node:timers/promises';

const root = Number(process.argv[2]);
const duration = Number(process.argv[3] ?? 10000);
if (!Number.isInteger(root) || root < 2 || !Number.isInteger(duration) || duration < 100 || duration > 60000) {
  throw new Error('Expected session process ID and duration between 100 and 60000 milliseconds');
}
const started = performance.now();
const samples = [];
while (performance.now() - started < duration) {
  const result = spawnSync('ps', ['-axo', 'pid=,ppid=,rss='], { encoding: 'utf8', timeout: 1000 });
  if (result.status !== 0) { throw new Error(result.stderr); }
  const processes = result.stdout.trim().split('\n').map(line => {
    const [pid, parent, residentKiB] = line.trim().split(/\s+/).map(Number);
    return { pid, parent, residentKiB };
  });
  if (!processes.some(process => process.pid === root)) { throw new Error('Session process exited during measurement'); }
  const descendants = new Set([root]);
  let previous = 0;
  while (descendants.size !== previous) {
    previous = descendants.size;
    for (const process of processes) {
      if (descendants.has(process.parent)) { descendants.add(process.pid); }
    }
  }
  const residentBytes = processes.filter(process => descendants.has(process.pid)).reduce((sum, process) => sum + process.residentKiB * 1024, 0);
  samples.push({ elapsedMs: Math.round(performance.now() - started), processes: descendants.size, residentBytes });
  await setTimeout(50);
}
console.log(JSON.stringify({ metric: 'sampled aggregate process tree RSS, shared pages can be counted more than once', durationMs: Math.round(performance.now() - started), peakResidentBytes: Math.max(...samples.map(sample => sample.residentBytes)), samples }, null, 2));
