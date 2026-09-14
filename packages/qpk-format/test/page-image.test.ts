import assert from 'node:assert/strict';
import test from 'node:test';

import {
  MetadataKey,
  PAGE_IMAGE_HEIGHT,
  PAGE_IMAGE_WIDTH,
  PackageType,
  SectionId,
  decodePageImageRecord,
  encodePageImageRecord,
  packBits,
  pageImageBytes,
  unpackBits,
} from '../src/format.js';
import { readPackage } from '../src/reader.js';
import { writePackage } from '../src/writer.js';

function roundTrip(data: Uint8Array): Uint8Array {
  const packed = packBits(data);
  assert.deepEqual(unpackBits(packed, data.length), data);
  return packed;
}

test('PackBits round-trips runs, literals and everything in between', () => {
  roundTrip(new Uint8Array(0));
  roundTrip(Uint8Array.from([7]));
  roundTrip(Uint8Array.from([1, 1]));
  roundTrip(Uint8Array.from([1, 1, 1]));
  roundTrip(new Uint8Array(1000).fill(0));
  roundTrip(Uint8Array.from({ length: 1000 }, (_, i) => (i * 37) % 251));
  let seed = 12345;
  const noisy = Uint8Array.from({ length: 5000 }, () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed % 4 === 0 ? 0 : (seed >> 8) & 0xff;
  });
  roundTrip(noisy);
});

test('a blank page compresses to a few hundred bytes, and random data barely grows', () => {
  const blank = new Uint8Array(pageImageBytes(PAGE_IMAGE_WIDTH, PAGE_IMAGE_HEIGHT));
  assert.ok(roundTrip(blank).length < 1000);
  const random = Uint8Array.from({ length: 48000 }, (_, i) => (i * 7919 + 13) % 256);
  assert.ok(roundTrip(random).length <= 48000 + Math.ceil(48000 / 128));
});

test('unpackBits rejects data that is short, long or truncated', () => {
  const packed = packBits(new Uint8Array(300).fill(9));
  assert.throws(() => unpackBits(packed, 299));
  assert.throws(() => unpackBits(packed, 301));
  assert.throws(() => unpackBits(Uint8Array.from([5, 1, 2]), 6));
});

test('page image records round-trip, and a package carrying them reads', () => {
  const record = { offset: 1234, length: 5678, width: PAGE_IMAGE_WIDTH, height: PAGE_IMAGE_HEIGHT };
  const bytes = encodePageImageRecord(record);
  assert.equal(bytes.length, 16);
  assert.deepEqual(decodePageImageRecord(new DataView(bytes.buffer), 0), record);

  const page = packBits(new Uint8Array(pageImageBytes(PAGE_IMAGE_WIDTH, PAGE_IMAGE_HEIGHT)));
  const pkg = readPackage(
    writePackage({
      type: PackageType.Unknown,
      metadata: [[MetadataKey.Title, 'Pictures']],
      sections: [
        {
          id: SectionId.PageImageIndex,
          payload: encodePageImageRecord({ ...record, offset: 0, length: page.length }),
          count: 1,
        },
        { id: SectionId.PageImageData, payload: page, count: 0 },
      ],
    }),
  );
  assert.equal(pkg.recordCount(SectionId.PageImageIndex), 1);
});
