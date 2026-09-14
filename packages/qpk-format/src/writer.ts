// QPK1 writer.
//
// Byte-for-byte compatible with the C++ test builder in
// firmware/test/test_qpk/qpk_test_package.h, which is what the golden fixture
// check asserts. Two rules make that possible and must not drift:
//   * sections are emitted in ascending section_id order
//   * metadata records are emitted in ascending key order
// Everything else follows from the layout in docs/qpk-format.md.

import { crc32 } from './crc32.js';
import {
  CONTENT_ID_BYTES,
  FORMAT_VERSION,
  HEADER_SIZE,
  MAGIC,
  MAX_SECTIONS,
  PackageType,
  SECTION_ENTRY_SIZE,
  SectionId,
  MetadataKey,
  recordSizeFor,
} from './format.js';

export interface SectionInput {
  id: SectionId | number;
  /** Payload bytes, excluding alignment padding. */
  payload: Uint8Array;
  /** Record count for fixed-size sections; 0 for blobs. */
  count: number;
}

export interface PackageInput {
  type: PackageType;
  flags?: number;
  contentVersion?: number;
  /** Exactly 16 bytes. Defaults to all zeroes. */
  contentId?: Uint8Array;
  metadata?: Iterable<readonly [MetadataKey | number, string]>;
  sections: SectionInput[];
}

/** Text pool: accumulates strings into TEXT_DATA and hands back refs. */
export class TextPool {
  private readonly chunks: Uint8Array[] = [];
  private length = 0;

  add(text: string): { offset: number; length: number } {
    const bytes = new TextEncoder().encode(text);
    const ref = { offset: this.length, length: bytes.length };
    this.chunks.push(bytes);
    this.length += bytes.length;
    return ref;
  }

  bytes(): Uint8Array {
    const out = new Uint8Array(this.length);
    let at = 0;
    for (const chunk of this.chunks) {
      out.set(chunk, at);
      at += chunk.length;
    }
    return out;
  }
}

function pad4(length: number): number {
  return (length + 3) & ~3;
}

function encodeMetadata(
  entries: Iterable<readonly [MetadataKey | number, string]>,
): { payload: Uint8Array; count: number } {
  // Ascending key order, matching std::map on the C++ side.
  const sorted = [...entries].sort((a, b) => a[0] - b[0]);
  const parts: Uint8Array[] = [];
  let total = 0;
  for (const [key, value] of sorted) {
    const valueBytes = new TextEncoder().encode(value);
    if (valueBytes.length > 0xffff) {
      throw new Error(`metadata value for key ${key} exceeds 65535 bytes`);
    }
    const record = new Uint8Array(pad4(4 + valueBytes.length));
    const view = new DataView(record.buffer);
    view.setUint16(0, key, true);
    view.setUint16(2, valueBytes.length, true);
    record.set(valueBytes, 4);
    parts.push(record);
    total += record.length;
  }
  const payload = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    payload.set(part, at);
    at += part.length;
  }
  return { payload, count: sorted.length };
}

export function writePackage(input: PackageInput): Uint8Array {
  const sections: SectionInput[] = [...input.sections];

  const metadataEntries = [...(input.metadata ?? [])];
  if (metadataEntries.length > 0) {
    const { payload, count } = encodeMetadata(metadataEntries);
    sections.push({ id: SectionId.Metadata, payload, count });
  }

  // Ascending id, and no duplicates: both are validation rules on the reader
  // side, so the writer must not be able to produce a file that fails them.
  sections.sort((a, b) => a.id - b.id);
  for (let i = 1; i < sections.length; i++) {
    if (sections[i]!.id === sections[i - 1]!.id) {
      throw new Error(`duplicate section id ${sections[i]!.id}`);
    }
  }
  if (sections.length === 0 || sections.length > MAX_SECTIONS) {
    throw new Error(`section count ${sections.length} out of range`);
  }

  const tableOffset = HEADER_SIZE;
  const tableLength = sections.length * SECTION_ENTRY_SIZE;

  // Lay the payloads out, 4-byte aligned, in id order.
  interface Placed extends SectionInput {
    offset: number;
    recordSize: number;
    crc: number;
  }
  const placed: Placed[] = [];
  let cursor = pad4(tableOffset + tableLength);
  for (const section of sections) {
    const recordSize = recordSizeFor(section.id);
    if (recordSize !== 0 && section.payload.length !== section.count * recordSize) {
      throw new Error(
        `section ${section.id}: ${section.payload.length} bytes is not ` +
          `${section.count} x ${recordSize}`,
      );
    }
    placed.push({
      ...section,
      offset: cursor,
      recordSize,
      crc: crc32(section.payload),
    });
    cursor = pad4(cursor + section.payload.length);
  }

  const file = new Uint8Array(cursor);
  const view = new DataView(file.buffer);

  for (const section of placed) {
    file.set(section.payload, section.offset);
  }

  // Section table.
  placed.forEach((section, index) => {
    const at = tableOffset + index * SECTION_ENTRY_SIZE;
    view.setUint16(at + 0, section.id, true);
    view.setUint16(at + 2, 1, true);
    view.setBigUint64(at + 4, BigInt(section.offset), true);
    view.setBigUint64(at + 12, BigInt(section.payload.length), true);
    view.setUint32(at + 20, section.count, true);
    view.setUint32(at + 24, section.crc, true);
    view.setUint16(at + 28, section.recordSize, true);
    view.setUint16(at + 30, 0, true);
  });

  // Header.
  file.set(MAGIC, 0);
  view.setUint16(4, FORMAT_VERSION, true);
  view.setUint16(6, HEADER_SIZE, true);
  view.setUint16(8, input.type, true);
  view.setUint16(10, sections.length, true);
  view.setUint32(12, input.flags ?? 0, true);
  view.setBigUint64(16, BigInt(file.length), true);

  const contentId = input.contentId ?? new Uint8Array(CONTENT_ID_BYTES);
  if (contentId.length !== CONTENT_ID_BYTES) {
    throw new Error(`contentId must be ${CONTENT_ID_BYTES} bytes`);
  }
  file.set(contentId, 24);

  view.setUint32(40, input.contentVersion ?? 1, true);
  view.setUint32(44, tableOffset, true);
  view.setUint32(48, tableLength, true);
  view.setUint32(52, crc32(file.subarray(HEADER_SIZE)), true);
  view.setUint32(56, 0, true);
  view.setUint32(60, crc32(file.subarray(0, 60)), true);

  return file;
}
