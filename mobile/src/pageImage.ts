// One page picture out of a package (docs/qpk-format.md 9d), as a PNG data
// URL -- the Converter's "exactly as the device shows it" preview.

import {
  SectionId,
  decodePageImageRecord,
  pageImageBytes,
  readPackage,
  unpackBits,
} from '@quran-device/qpk-format';

import { greyPngDataUrl } from './png';

/** The page picture for `pageNumber` (1-based), or null if the package has none. */
export function pageImagePng(bytes: Uint8Array, pageNumber: number): string | null {
  const pkg = readPackage(bytes, { verifyChecksums: false });
  const index = pkg.section(SectionId.PageImageIndex);
  const data = pkg.section(SectionId.PageImageData);
  if (!index || !data) return null;
  const count = Math.floor(index.length / 16);
  if (pageNumber < 1 || pageNumber > count) return null;

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const record = decodePageImageRecord(view, index.offset + (pageNumber - 1) * 16);
  const start = data.offset + record.offset;
  const bits = unpackBits(bytes.subarray(start, start + record.length), pageImageBytes(record.width, record.height));

  const stride = Math.ceil(record.width / 8);
  const grey = new Uint8Array(record.width * record.height);
  for (let y = 0; y < record.height; y++) {
    for (let x = 0; x < record.width; x++) {
      const ink = (bits[y * stride + (x >> 3)]! >> (7 - (x & 7))) & 1; // 1 = black
      grey[y * record.width + x] = ink ? 0 : 255;
    }
  }
  return greyPngDataUrl(record.width, record.height, grey);
}
