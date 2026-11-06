import { test } from 'node:test';
import assert from 'node:assert/strict';
import { canvasSize, frameShape } from '../../src/browser/frame-shape.mjs';

const near = (actual, expected) => assert.ok(Math.abs(actual - expected) < 1e-6, `${actual} != ${expected}`);

test('auto follows the window, at least 4:3', () => {
  const four = frameShape({ width: 1280, height: 960 });
  near(four.aspect, 4 / 3);
  assert.deepEqual([four.width, four.height], [1280, 960]);
  const wide = frameShape({ width: 1920, height: 1080 });
  near(wide.aspect, 16 / 9);
  assert.deepEqual([wide.width, wide.height], [1920, 1080]);
  const ultra = frameShape({ width: 2520, height: 1080 });
  near(ultra.aspect, 21 / 9);
  const portrait = frameShape({ width: 800, height: 1200 });
  near(portrait.aspect, 4 / 3);
  near(portrait.width, 800);
  near(portrait.height, 600);
});

test('a fixed shape letterboxes or pillarboxes, stretch distorts', () => {
  const fixed = frameShape({ aspect: '16:9', width: 2520, height: 1080 });
  near(fixed.aspect, 16 / 9);
  near(fixed.width, 1920);
  assert.equal(fixed.height, 1080);
  const tall = frameShape({ aspect: '21:9', width: 1280, height: 960 });
  near(tall.height, 1280 / (21 / 9));
  const stretch = frameShape({ aspect: '4:3', scaling: 'stretch', width: 1920, height: 1080 });
  near(stretch.aspect, 4 / 3);
  assert.deepEqual([stretch.width, stretch.height], [1920, 1080]);
});

test('pixel perfect steps whole multiples of 480 device rows and derives the width', () => {
  const box = frameShape({ scaling: 'integer', width: 1920, height: 1080, ratio: 2 });
  near(box.aspect, 16 / 9);
  assert.equal(box.height, 960);
  near(box.width, 960 * 16 / 9);
  const small = frameShape({ scaling: 'integer', width: 600, height: 400 });
  near(small.height, 400 * 1);
  near(small.width, 600);
});

test('the canvas is as wide as the shape at 480 rows', () => {
  assert.deepEqual(canvasSize(4 / 3), { width: 640, height: 480 });
  assert.deepEqual(canvasSize(16 / 9), { width: 853, height: 480 });
  assert.deepEqual(canvasSize(21 / 9), { width: 1120, height: 480 });
});
