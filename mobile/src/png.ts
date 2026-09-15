// Greyscale pictures for <Image>: React Native has no canvas, so previews of
// photos, covers and page pictures are encoded as small PNG data URLs here.

import { zlibSync } from 'fflate';

import { PANEL_GREYS } from '@quran-device/device-client';
import { crc32 } from '@quran-device/qpk-format';

import { encodeBase64 } from './base64';

function chunk(type: string, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(12 + data.length);
  const view = new DataView(out.buffer);
  view.setUint32(0, data.length);
  for (let i = 0; i < 4; i++) out[4 + i] = type.charCodeAt(i);
  out.set(data, 8);
  view.setUint32(8 + data.length, crc32(out.subarray(4, 8 + data.length)));
  return out;
}

/** An 8-bit greyscale PNG (one byte per pixel, row-major) as a data URL. */
export function greyPngDataUrl(width: number, height: number, grey: Uint8Array): string {
  if (grey.length !== width * height) {
    throw new Error(`expected ${width * height} pixels for ${width}x${height}, got ${grey.length}`);
  }
  // Every row gets filter byte 0 (none).
  const raw = new Uint8Array((width + 1) * height);
  for (let y = 0; y < height; y++) {
    raw.set(grey.subarray(y * width, (y + 1) * width), y * (width + 1) + 1);
  }
  const header = new Uint8Array(13);
  const view = new DataView(header.buffer);
  view.setUint32(0, width);
  view.setUint32(4, height);
  header[8] = 8; // bit depth
  header[9] = 0; // colour type: greyscale
  const parts = [
    Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', header),
    chunk('IDAT', zlibSync(raw, { level: 6 })),
    chunk('IEND', new Uint8Array(0)),
  ];
  const png = new Uint8Array(parts.reduce((sum, part) => sum + part.length, 0));
  let at = 0;
  for (const part of parts) {
    png.set(part, at);
    at += part.length;
  }
  return `data:image/png;base64,${encodeBase64(png)}`;
}

/** Panel levels 0..3 (photos, covers) as a PNG data URL. */
export function levelsPngDataUrl(width: number, height: number, levels: Uint8Array): string {
  const grey = new Uint8Array(levels.length);
  for (let i = 0; i < levels.length; i++) grey[i] = PANEL_GREYS[(levels[i]! & 3) as 0 | 1 | 2 | 3];
  return greyPngDataUrl(width, height, grey);
}
