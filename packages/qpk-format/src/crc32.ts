// CRC-32/ISO-HDLC (poly 0xEDB88320, init/final 0xFFFFFFFF).
//
// Must agree byte for byte with firmware/qpk/crc32.cpp. The canonical check
// value CRC32("123456789") === 0xCBF43926 is asserted in both test suites,
// which is what makes the two implementations comparable.

const TABLE: Uint32Array = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[i] = c >>> 0;
  }
  return table;
})();

export const CRC32_INIT = 0xffffffff;

export function crc32Update(crc: number, data: Uint8Array): number {
  let c = crc >>> 0;
  for (let i = 0; i < data.length; i++) {
    c = (TABLE[(c ^ data[i]!) & 0xff]! ^ (c >>> 8)) >>> 0;
  }
  return c >>> 0;
}

export function crc32Finish(crc: number): number {
  return (crc ^ 0xffffffff) >>> 0;
}

export function crc32(data: Uint8Array): number {
  return crc32Finish(crc32Update(CRC32_INIT, data));
}
