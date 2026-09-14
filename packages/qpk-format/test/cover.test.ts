import assert from 'node:assert/strict';
import test from 'node:test';

import {
  COVER_BYTES,
  COVER_HEIGHT,
  COVER_WIDTH,
  MetadataKey,
  PackageType,
  SectionId,
  decodeCover,
  encodeCover,
} from '../src/format.js';
import { readPackage } from '../src/reader.js';
import { writePackage } from '../src/writer.js';

test('a cover round-trips through encodeCover and decodeCover', () => {
  const levels = new Uint8Array(COVER_WIDTH * COVER_HEIGHT).map((_, i) => (i * 7) % 4);
  const payload = encodeCover(levels);
  assert.equal(payload.length, COVER_BYTES);
  assert.deepEqual(decodeCover(payload), levels);
});

test('decodeCover rejects anything that is not a QCV1 cover', () => {
  const good = encodeCover(new Uint8Array(COVER_WIDTH * COVER_HEIGHT));
  const badMagic = good.slice();
  badMagic[0] = 0;
  assert.throws(() => decodeCover(badMagic));
  const badDepth = good.slice();
  badDepth[8] = 1;
  assert.throws(() => decodeCover(badDepth));
  assert.throws(() => decodeCover(good.subarray(0, 100)));
});

test('a book carrying a COVER section reads, and the cover comes back intact', () => {
  const levels = new Uint8Array(COVER_WIDTH * COVER_HEIGHT).fill(2);
  const cover = encodeCover(levels);
  const bytes = writePackage({
    type: PackageType.Unknown,
    metadata: [[MetadataKey.Title, 'Covered']],
    sections: [{ id: SectionId.Cover, payload: cover, count: 0 }],
  });
  const pkg = readPackage(bytes);
  const entry = pkg.section(SectionId.Cover);
  assert.ok(entry);
  assert.equal(entry.length, COVER_BYTES);
  assert.deepEqual(decodeCover(bytes.subarray(entry.offset, entry.offset + entry.length)), levels);
});
