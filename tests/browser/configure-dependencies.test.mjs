import { test } from 'node:test';
import { strict as assert } from 'node:assert';
import { readFileSync } from 'node:fs';

const cmake = readFileSync(new URL('../../tools/browser/CMakeLists.txt', import.meta.url), 'utf8');

test('configure dependencies come from the generated fingerprint inputs', () => {
  const inventory = cmake.indexOf('execute_process(COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/inventory.mjs');
  const manifest = cmake.indexOf('file(READ ${CMAKE_CURRENT_BINARY_DIR}/input-manifest.json');
  assert.ok(inventory >= 0 && manifest > inventory);
  assert.match(cmake, /string\(JSON \w+ LENGTH "\$\{\w+\}" inputs\)/);
  assert.match(cmake, /string\(JSON \w+ MEMBER "\$\{\w+\}" inputs \$\{n\}\)/);
  assert.match(cmake, /IS_ABSOLUTE/);
  assert.match(cmake, /CMAKE_CONFIGURE_DEPENDS \$\{identity_inputs\}/);
});

test('manifest inputs trigger reconfiguration while inactive files do not', async () => {
  const { mkdtempSync, mkdirSync, writeFileSync } = await import('node:fs');
  const { execFileSync } = await import('node:child_process');
  const { resolve, join } = await import('node:path');
  const parent = resolve('out/m4-current');
  mkdirSync(parent, { recursive: true });
  const fixture = mkdtempSync(join(parent, 'configure-dependencies-'));
  const source = join(fixture, 'source');
  const build = join(fixture, 'build');
  mkdirSync(source);
  const relative = join(source, 'menu-backend.cpp');
  const absolute = join(source, 'webgpu-service.mjs');
  const inactive = join(source, 'inactive.html');
  for (const path of [relative, absolute, inactive]) { writeFileSync(path, 'initial'); }
  const start = cmake.indexOf('file(READ ${CMAKE_CURRENT_BINARY_DIR}/input-manifest.json');
  const boundary = 'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${identity_inputs})';
  const end = cmake.indexOf(boundary, start);
  assert.ok(start >= 0 && end > start);
  const block = cmake.slice(start, end + boundary.length);
  const project = `cmake_minimum_required(VERSION 3.25)
project(fixture NONE)
set(root "\${CMAKE_CURRENT_SOURCE_DIR}")
file(APPEND "\${CMAKE_CURRENT_BINARY_DIR}/configure-log" "configured\n")
file(SHA256 "\${root}/menu-backend.cpp" relative_hash)
file(SHA256 "\${root}/webgpu-service.mjs" absolute_hash)
file(WRITE "\${CMAKE_CURRENT_BINARY_DIR}/input-manifest.json" "{\\"inputs\\":{\\"menu-backend.cpp\\":\\"\${relative_hash}\\",\\"\${root}/webgpu-service.mjs\\":\\"\${absolute_hash}\\"}}")
${block}
file(WRITE "\${CMAKE_CURRENT_BINARY_DIR}/identity" "\${relative_hash}\${absolute_hash}")
`;
  writeFileSync(join(source, 'CMakeLists.txt'), project);
  execFileSync('cmake', ['-S', source, '-B', build]);
  const identity = () => readFileSync(join(build, 'identity'), 'utf8');
  const initial = identity();
  const configureLog = () => readFileSync(join(build, 'configure-log'), 'utf8');
  const initialLog = configureLog();
  writeFileSync(inactive, 'changed');
  execFileSync('cmake', ['--build', build]);
  assert.equal(identity(), initial);
  assert.equal(configureLog(), initialLog);
  writeFileSync(relative, 'changed');
  execFileSync('cmake', ['--build', build]);
  const afterRelative = identity();
  assert.notEqual(afterRelative, initial);
  writeFileSync(absolute, 'changed');
  execFileSync('cmake', ['--build', build]);
  assert.notEqual(identity(), afterRelative);
});
