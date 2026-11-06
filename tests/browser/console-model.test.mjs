import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CONSOLE_LINES, createConsoleModel, routeHostMessage } from '../../src/browser/console-model.mjs';
import { HOST_MESSAGES } from '../../src/browser/host-messages.mjs';

test('a submitted line is echoed, sent and kept in history that Up and Down step through', () => {
  const sent = [];
  const model = createConsoleModel({ send: line => { sent.push(line); return true; } });
  assert.equal(model.submit('  '), false);
  model.submit('god');
  model.submit('ghost');
  model.submit('ghost');
  assert.deepEqual(sent, ['god', 'ghost', 'ghost']);
  assert.deepEqual(model.lines, ['> god', '> ghost', '> ghost']);
  assert.equal(model.recall(-1), 'ghost');
  assert.equal(model.recall(-1), 'god');
  assert.equal(model.recall(-1), 'god');
  assert.equal(model.recall(1), 'ghost');
  assert.equal(model.recall(1), '');
  model.print('unrecognized command: nonsense');
  assert.equal(model.lines.at(-1), 'unrecognized command: nonsense');
});

test('the log is bounded and a refused line is reported', () => {
  const model = createConsoleModel({ send: () => false });
  model.submit('open x');
  assert.equal(model.lines.at(-1), 'the game did not take the line');
  for (let n = 0; n < CONSOLE_LINES + 10; ++n) { model.print(`line ${n}`); }
  assert.equal(model.lines.length, CONSOLE_LINES);
  assert.equal(model.lines.at(-1), `line ${CONSOLE_LINES + 9}`);
});

test('a guest quit returns to the main menu, output goes to the console, the rest are settings', () => {
  const seen = [];
  const handlers = { quit: () => seen.push('quit'), output: text => seen.push(`out ${text}`), setting: message => seen.push(`set ${message.text}`) };
  routeHostMessage({ kind: HOST_MESSAGES.quit, text: '' }, handlers, HOST_MESSAGES);
  routeHostMessage({ kind: HOST_MESSAGES.output, text: 'hello' }, handlers, HOST_MESSAGES);
  routeHostMessage({ kind: HOST_MESSAGES.set, text: 'volume=5' }, handlers, HOST_MESSAGES);
  assert.deepEqual(seen, ['quit', 'out hello', 'set volume=5']);
});
