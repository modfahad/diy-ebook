import assert from 'node:assert/strict';
import test from 'node:test';

import { ditherToBits } from '../src/photo.js';

function solid(width: number, height: number, grey: number): Uint8Array {
  const rgba = new Uint8Array(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    rgba.set([grey, grey, grey, 255], i * 4);
  }
  return rgba;
}

test('black and white areas come out solid, with no dithering speckle', () => {
  assert.ok(ditherToBits(solid(20, 10, 0), 20, 10).every((byte, i) => byte === (i % 3 === 2 ? 0xf0 : 0xff)));
  assert.ok(ditherToBits(solid(20, 10, 255), 20, 10).every((byte) => byte === 0));
  assert.ok(ditherToBits(solid(16, 4, 60), 16, 4).every((byte) => byte === 0xff));
});

test('a mid grey dithers to roughly half ink', () => {
  const bits = ditherToBits(solid(64, 64, 128), 64, 64);
  let ink = 0;
  for (const byte of bits) for (let b = 0; b < 8; b++) ink += (byte >> b) & 1;
  const fraction = ink / (64 * 64);
  assert.ok(fraction > 0.4 && fraction < 0.6, `ink fraction ${fraction}`);
});

test('rows are ceil(width / 8) bytes and the size is checked', () => {
  assert.equal(ditherToBits(solid(10, 3, 255), 10, 3).length, 6);
  assert.throws(() => ditherToBits(new Uint8Array(12), 10, 3));
});
