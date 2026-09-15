// Base64 in Hermes, which has no Buffer and whose atob/btoa work on
// "binary strings" -- too slow and memory-hungry for multi-megabyte files.
// Encoding is device-client's (the same one its base64 chunk bodies use).

export { encodeBase64 } from '@quran-device/device-client';

const LOOKUP = (() => {
  const table = new Int16Array(256).fill(-1);
  const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  for (let i = 0; i < alphabet.length; i++) table[alphabet.charCodeAt(i)] = i;
  return table;
})();

/** Standard base64 (padding optional) to bytes. Throws on a bad character. */
export function decodeBase64(text: string): Uint8Array {
  let length = text.length;
  while (length > 0 && text.charCodeAt(length - 1) === 61 /* = */) length--;
  const out = new Uint8Array(Math.floor((length * 3) / 4));
  let written = 0;
  let accumulator = 0;
  let bits = 0;
  for (let i = 0; i < length; i++) {
    const value = LOOKUP[text.charCodeAt(i) & 0xff]!;
    if (value < 0 || text.charCodeAt(i) > 0xff) throw new Error(`bad base64 character at ${i}`);
    accumulator = ((accumulator << 6) | value) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[written++] = (accumulator >> bits) & 0xff;
    }
  }
  return written === out.length ? out : out.subarray(0, written);
}
