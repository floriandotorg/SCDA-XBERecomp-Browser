import test from 'node:test';
import assert from 'node:assert/strict';
import { createGPUService } from '../../src/browser/webgpu-service.mjs';
import { Opcode, Status } from '../../src/browser/webgpu-transport.mjs';

test('deferred validation retains its command identity, fails barriers and ignores retired completions', async () => {
  const moduleURL = `data:text/javascript,${encodeURIComponent(`export default async options => {
    globalThis.deferredGPUFixture = { options, calls: [] };
    return {
      HEAPU8: new Uint8Array(16), _malloc: () => 0, _free: () => {},
      _webgpu_resources: () => 0, _webgpu_shutdown: () => {},
      _webgpu_start: () => options.onGPUReady(0, ''),
      _webgpu_submit: (opcode, resource, pointer, length, command) => {
        deferredGPUFixture.calls.push({ opcode, resource, command });
        if (opcode === ${Opcode.DrawBatch}) { options.onGPUComplete(0, new Uint8Array(), ''); }
      }
    };
  }`)}`;
  const diagnostics = [], presented = [];
  const service = await createGPUService({ canvas: {}, moduleURL, diagnostic: value => diagnostics.push(value), onPresented: value => presented.push(value) });
  const draw = { opcode: Opcode.DrawBatch, resource: 7, command: 17, bytes: new Uint8Array() };
  assert.equal((await service.execute(draw)).status, Status.Ok);
  const barrier = service.execute({ opcode: Opcode.Present, resource: 8, command: 18, bytes: new Uint8Array() });
  const failure = { command: 17, resource: 7, status: Status.Error, message: 'invalid original draw' };
  deferredGPUFixture.options.onGPUDeferredError(failure);
  assert.equal((await barrier).status, Status.Error);
  assert.deepEqual(diagnostics, [failure]);
  assert.deepEqual(presented, []);
  assert.equal((await service.execute(draw)).status, Status.Error);
  assert.deepEqual(deferredGPUFixture.calls.map(value => value.command), [17, 18]);
  service.shutdown();
  deferredGPUFixture.options.onGPUDeferredError(failure);
  assert.deepEqual(diagnostics, [failure]);
  assert.equal((await service.execute(draw)).status, Status.Closed);
  delete globalThis.deferredGPUFixture;
});
