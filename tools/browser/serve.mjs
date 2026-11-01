import { createServer } from 'node:http';
import { readFile, realpath, stat } from 'node:fs/promises';
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
]);
const isInside = (root, path) => {
  const distance = relative(root, path);
  return distance !== '..' && !distance.startsWith('../') && !isAbsolute(distance);
};

export const createDiagnosticServer = async (directory, port = 8080) => {
  const root = await realpath(directory);
  const server = createServer(async (request, response) => {
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
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', resolve);
  });
  return server;
};

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const server = await createDiagnosticServer(process.argv[2] ?? 'out/browser/web', Number(process.argv[3] ?? 8080));
  console.log(`Diagnostic: http://127.0.0.1:${server.address().port}`);
}
