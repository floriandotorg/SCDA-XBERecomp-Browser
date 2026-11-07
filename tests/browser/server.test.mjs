import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync, symlinkSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { createDiagnosticServer } from '../../tools/browser/serve.mjs';

test('server isolates memory and confines files including symlinks and encoded traversal', async () => {
  const root = resolve('out/server-test');
  mkdirSync(root, { recursive: true });
  writeFileSync(`${root}/index.html`, 'diagnostic');
  writeFileSync(`${root}/module.wasm`, Buffer.from([0, 97, 115, 109]));
  if (!existsSync(`${root}/escape.html`)) {
    symlinkSync(resolve('README.md'), `${root}/escape.html`);
  }
  const server = await createDiagnosticServer(root, 0);
  try {
    const base = `http://127.0.0.1:${server.address().port}`;
    const page = await fetch(base);
    assert.equal(page.status, 200);
    assert.equal(page.headers.get('Cross-Origin-Opener-Policy'), 'same-origin');
    assert.equal(page.headers.get('Cross-Origin-Embedder-Policy'), 'require-corp');
    assert.equal(await page.text(), 'diagnostic');
    const wasm = await fetch(`${base}/module.wasm`);
    assert.equal(wasm.headers.get('Content-Type'), 'application/wasm');
    for (const path of ['/escape.html', '/%2e%2e%2fREADME.md', '/%ZZ', '/config.toml', '/.git/config']) {
      const response = await fetch(`${base}${path}`);
      assert.ok(response.status >= 400, path);
    }
    assert.equal((await fetch(base, { method: 'POST' })).status, 405);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});

test('server serves the install manifest and icons, and HTTPS with a generated certificate for phones', async () => {
  const { request } = await import('node:https');
  const { localCertificate } = await import('../../tools/browser/serve.mjs');
  const root = resolve('out/server-test');
  mkdirSync(root, { recursive: true });
  writeFileSync(`${root}/index.html`, 'diagnostic');
  writeFileSync(`${root}/manifest.webmanifest`, '{}');
  writeFileSync(`${root}/icon.svg`, '<svg/>');
  writeFileSync(`${root}/icon-180.png`, Buffer.from([137, 80, 78, 71]));
  const tls = await localCertificate(resolve('out/server-test-certificate'));
  const server = await createDiagnosticServer(root, 0, { tls });
  try {
    const get = path => new Promise((done, fail) => {
      request({ host: '127.0.0.1', port: server.address().port, path, rejectUnauthorized: false }, response => {
        response.resume();
        response.on('end', () => done(response));
      }).on('error', fail).end();
    });
    const page = await get('/');
    assert.equal(page.statusCode, 200);
    assert.equal(page.headers['cross-origin-embedder-policy'], 'require-corp');
    assert.equal((await get('/manifest.webmanifest')).headers['content-type'], 'application/manifest+json');
    assert.equal((await get('/icon.svg')).headers['content-type'], 'image/svg+xml');
    assert.equal((await get('/icon-180.png')).headers['content-type'], 'image/png');
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
});
