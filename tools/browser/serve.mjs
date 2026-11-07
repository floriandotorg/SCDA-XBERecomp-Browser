import { createServer } from 'node:http';
import { createServer as createSecureServer } from 'node:https';
import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { readFile, realpath, stat, writeFile } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import { extname, isAbsolute, relative, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const mimeTypes = new Map([
  ['.html', 'text/html; charset=utf-8'],
  ['.mjs', 'text/javascript; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.wasm', 'application/wasm'],
  ['.wgsl', 'text/plain; charset=utf-8'],
  ['.bin', 'application/octet-stream'],
  ['.css', 'text/css; charset=utf-8'],
  ['.webmanifest', 'application/manifest+json'],
  ['.svg', 'image/svg+xml'],
  ['.png', 'image/png'],
]);
const isInside = (root, path) => {
  const distance = relative(root, path);
  return distance !== '..' && !distance.startsWith('../') && !isAbsolute(distance);
};

const lanAddresses = () => Object.values(networkInterfaces()).flat()
  .filter(entry => entry && entry.family === 'IPv4' && !entry.internal).map(entry => entry.address);

// a self-signed certificate for this machine's names and LAN addresses, kept in a directory and reused: a phone
// reaches the page over the network only as HTTPS, the secure context shared memory and WebGPU need
export const localCertificate = async directory => {
  mkdirSync(directory, { recursive: true });
  const key = resolve(directory, 'key.pem'), cert = resolve(directory, 'cert.pem');
  const names = ['DNS:localhost', 'IP:127.0.0.1', ...lanAddresses().map(address => `IP:${address}`)].join(',');
  const current = await readFile(resolve(directory, 'names.txt'), 'utf8').catch(() => '');
  if (current !== names) {
    execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-days', '825', '-subj', '/CN=Deep Cover local server',
      '-addext', `subjectAltName=${names}`, '-keyout', key, '-out', cert], { stdio: 'ignore' });
    await writeFile(resolve(directory, 'names.txt'), names);
  }
  return { key: await readFile(key), cert: await readFile(cert) };
};

export const createDiagnosticServer = async (directory, port = 8080, { host = '127.0.0.1', tls } = {}) => {
  const root = await realpath(directory);
  const handle = async (request, response) => {
    response.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
    response.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
    response.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('Cache-Control', 'no-store');
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      response.writeHead(405).end();
      return;
    }
    try {
      const pathname = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
      const path = resolve(root, `.${pathname === '/' ? '/index.html' : pathname}`);
      if (!isInside(root, path) || pathname.split('/').some((part) => part.startsWith('.')) || !mimeTypes.has(extname(path))) {
        response.writeHead(403).end();
        return;
      }
      const canonical = await realpath(path);
      if (!isInside(root, canonical) || !(await stat(canonical)).isFile()) {
        response.writeHead(403).end();
        return;
      }
      const content = await readFile(canonical);
      response.writeHead(200, { 'Content-Type': mimeTypes.get(extname(path)) });
      response.end(request.method === 'HEAD' ? undefined : content);
    } catch {
      response.writeHead(404).end();
    }
  };
  const server = tls ? createSecureServer(tls, handle) : createServer(handle);
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  return server;
};

// node serve.mjs <web-dir> <port> [--lan] [--https]: --lan listens on every interface for a phone on the same
// network, --https adds the certificate it needs (out/https-certificate)
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const [directory = 'out/browser/web', port = '8080'] = process.argv.slice(2).filter(value => !value.startsWith('--'));
  const lan = process.argv.includes('--lan'), secure = process.argv.includes('--https');
  const tls = secure ? await localCertificate(resolve(import.meta.dirname, '../../out/https-certificate')) : undefined;
  const server = await createDiagnosticServer(directory, Number(port), { host: lan ? '0.0.0.0' : '127.0.0.1', tls });
  const scheme = secure ? 'https' : 'http';
  console.log(`Diagnostic: ${scheme}://127.0.0.1:${server.address().port}`);
  if (lan) {
    for (const address of lanAddresses()) { console.log(`On this network: ${scheme}://${address}:${server.address().port}`); }
    if (!secure) { console.log('Phones need --https: browsers allow WebGPU and shared memory only in a secure context.'); }
  }
}
