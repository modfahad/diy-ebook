// QPK1 reader and validator.
//
// This is the desktop-side counterpart to firmware/qpk/qpk_reader.cpp. It
// enforces the same rules in the same order, and additionally does the checks
// the device deliberately skips for cost reasons: every section CRC, the
// payload CRC, and the full index-consistency sweep. docs/qpk-format.md
// section 11 calls this out as the validator's job.

import { crc32 } from './crc32.js';
import {
  AyahRecord,
  BookSectionRecord,
  ChapterRecord,
  GlyphRecord,
  LayoutHeader,
  LineRecord,
  SajdahRecord,
  TextRecord,
  TranslationRecord,
  CONTENT_ID_BYTES,
  FLAG_HAS_WORD_LAYOUT,
  FLAG_SHAPED_TEXT_DATA,
  FORMAT_VERSION,
  HEADER_SIZE,
  Header,
  LAYOUT_HEADER_SIZE,
  LINE_RECORD_SIZE,
  MAGIC,
  MAX_SECTIONS,
  MetadataKey,
  PackageType,
  PageRecord,
  RangeRecord,
  REQUIRED_SECTIONS,
  SECTION_ENTRY_SIZE,
  SectionEntry,
  SectionId,
  SurahRecord,
  WordRecord,
  decodeAyah,
  decodeBookSection,
  decodeChapter,
  decodeGlyph,
  decodeLayoutHeader,
  decodeLine,
  decodePage,
  decodeSajdah,
  decodeTextRecord,
  decodeTranslation,
  decodeRange,
  decodeSurah,
  decodeWord,
  isFixedIndexSection,
  recordSizeFor,
} from './format.js';

export type QpkErrorCode =
  | 'TOO_SMALL'
  | 'BAD_MAGIC'
  | 'UNSUPPORTED_VERSION'
  | 'BAD_HEADER_SIZE'
  | 'HEADER_CHECKSUM'
  | 'SIZE_MISMATCH'
  | 'BAD_SECTION_TABLE'
  | 'SECTION_MISALIGNED'
  | 'SECTION_OUT_OF_RANGE'
  | 'SECTION_OVERLAP'
  | 'SECTION_ORDER'
  | 'SECTION_SIZE_MISMATCH'
  | 'MISSING_SECTION'
  | 'SECTION_CHECKSUM'
  | 'INDEX_INCONSISTENT'
  | 'FONT_SECTIONS_REQUIRED'
  | 'LAYOUT_SECTION_REQUIRED'
  | 'NO_SUCH_SECTION'
  | 'OUT_OF_RANGE';

export class QpkError extends Error {
  constructor(
    readonly code: QpkErrorCode,
    message: string,
  ) {
    super(`${code}: ${message}`);
    this.name = 'QpkError';
  }
}

export interface ReadOptions {
  /**
   * Verify every section CRC and the payload CRC. Default true -- the desktop
   * can afford it and it is the check that catches a bad transfer. The device
   * verifies only the fixed-size index sections at open.
   */
  verifyChecksums?: boolean;
  /** Full index-consistency sweep rather than the device's sampled one. */
  deepIndexCheck?: boolean;
}

/** offset + length <= limit, written so it cannot overflow. */
function within(offset: number, length: number, limit: number): boolean {
  return offset >= 0 && length >= 0 && offset <= limit && length <= limit - offset;
}

function u64(view: DataView, at: number): number {
  const value = view.getBigUint64(at, true);
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) {
    // Not representable, and far past any plausible package size. Returning a
    // huge finite number keeps the bounds checks below meaningful.
    return Number.MAX_SAFE_INTEGER;
  }
  return Number(value);
}

export class QpkPackage {
  private constructor(
    readonly bytes: Uint8Array,
    readonly header: Header,
    readonly sections: SectionEntry[],
  ) {}

  private get view(): DataView {
    return new DataView(this.bytes.buffer, this.bytes.byteOffset, this.bytes.byteLength);
  }

  static parse(bytes: Uint8Array, options: ReadOptions = {}): QpkPackage {
    const verifyChecksums = options.verifyChecksums ?? true;
    const deepIndexCheck = options.deepIndexCheck ?? true;

    // Rule 1.
    if (bytes.length < HEADER_SIZE) {
      throw new QpkError('TOO_SMALL', `${bytes.length} bytes is under a header`);
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

    // Rule 2.
    for (let i = 0; i < MAGIC.length; i++) {
      if (bytes[i] !== MAGIC[i]) {
        throw new QpkError('BAD_MAGIC', 'not a QPK1 file');
      }
    }

    // Rule 3.
    const formatVersion = view.getUint16(4, true);
    if (formatVersion !== FORMAT_VERSION) {
      throw new QpkError('UNSUPPORTED_VERSION', `format version ${formatVersion}`);
    }

    // Rule 4.
    const headerSize = view.getUint16(6, true);
    if (headerSize < HEADER_SIZE || headerSize > bytes.length) {
      throw new QpkError('BAD_HEADER_SIZE', `header_size ${headerSize}`);
    }

    // Rule 5.
    const headerCrc = view.getUint32(60, true);
    const actualHeaderCrc = crc32(bytes.subarray(0, 60));
    if (actualHeaderCrc !== headerCrc) {
      throw new QpkError(
        'HEADER_CHECKSUM',
        `expected ${headerCrc.toString(16)}, got ${actualHeaderCrc.toString(16)}`,
      );
    }

    const header: Header = {
      formatVersion,
      headerSize,
      packageType: view.getUint16(8, true) as PackageType,
      sectionCount: view.getUint16(10, true),
      flags: view.getUint32(12, true),
      packageSize: u64(view, 16),
      contentId: bytes.slice(24, 24 + CONTENT_ID_BYTES),
      contentVersion: view.getUint32(40, true),
      sectionTableOffset: view.getUint32(44, true),
      sectionTableLength: view.getUint32(48, true),
      payloadCrc32: view.getUint32(52, true),
      headerCrc32: headerCrc,
    };

    // Rule 6.
    if (header.packageSize !== bytes.length) {
      throw new QpkError(
        'SIZE_MISMATCH',
        `header says ${header.packageSize}, file is ${bytes.length}`,
      );
    }

    // Rule 7.
    if (header.sectionCount === 0 || header.sectionCount > MAX_SECTIONS) {
      throw new QpkError('BAD_SECTION_TABLE', `section_count ${header.sectionCount}`);
    }
    if (header.sectionCount * SECTION_ENTRY_SIZE !== header.sectionTableLength) {
      throw new QpkError('BAD_SECTION_TABLE', 'section_table_length inconsistent');
    }
    if (header.sectionTableOffset < header.headerSize) {
      throw new QpkError('BAD_SECTION_TABLE', 'section table overlaps the header');
    }
    if (!within(header.sectionTableOffset, header.sectionTableLength, header.packageSize)) {
      throw new QpkError('BAD_SECTION_TABLE', 'section table runs past end of file');
    }

    const sections: SectionEntry[] = [];
    for (let i = 0; i < header.sectionCount; i++) {
      const at = header.sectionTableOffset + i * SECTION_ENTRY_SIZE;
      sections.push({
        id: view.getUint16(at + 0, true),
        version: view.getUint16(at + 2, true),
        offset: u64(view, at + 4),
        length: u64(view, at + 12),
        count: view.getUint32(at + 20, true),
        crc32: view.getUint32(at + 24, true),
        recordSize: view.getUint16(at + 28, true),
        flags: view.getUint16(at + 30, true),
      });
    }

    const tableEnd = header.sectionTableOffset + header.sectionTableLength;
    sections.forEach((section, index) => {
      // Rule 10.
      if (index > 0 && section.id <= sections[index - 1]!.id) {
        throw new QpkError('SECTION_ORDER', `section id ${section.id} out of order`);
      }
      // Rule 8.
      if (section.offset % 4 !== 0) {
        throw new QpkError('SECTION_MISALIGNED', `section ${section.id}`);
      }
      if (section.offset < tableEnd) {
        throw new QpkError('SECTION_OUT_OF_RANGE', `section ${section.id} inside the table`);
      }
      if (!within(section.offset, section.length, header.packageSize)) {
        throw new QpkError('SECTION_OUT_OF_RANGE', `section ${section.id}`);
      }
      // Rule 11.
      const expected = recordSizeFor(section.id);
      if (expected !== 0 && section.recordSize !== expected) {
        throw new QpkError(
          'SECTION_SIZE_MISMATCH',
          `section ${section.id} record_size ${section.recordSize} != ${expected}`,
        );
      }
      if (section.recordSize !== 0 && section.count * section.recordSize !== section.length) {
        throw new QpkError('SECTION_SIZE_MISMATCH', `section ${section.id}`);
      }
    });

    // Rule 9. Entries are ordered by id, not offset, so this is pairwise.
    for (let i = 0; i < sections.length; i++) {
      const a = sections[i]!;
      if (a.length === 0) continue;
      for (let j = i + 1; j < sections.length; j++) {
        const b = sections[j]!;
        if (b.length === 0) continue;
        if (a.offset < b.offset + b.length && b.offset < a.offset + a.length) {
          throw new QpkError('SECTION_OVERLAP', `sections ${a.id} and ${b.id}`);
        }
      }
    }

    // Rule 12.
    const required = REQUIRED_SECTIONS[header.packageType] ?? [SectionId.Metadata];
    for (const id of required) {
      if (!sections.some((s) => s.id === id)) {
        throw new QpkError('MISSING_SECTION', `${SectionId[id] ?? id} is required`);
      }
    }

    // Rule 15. A package that claims its WORD_INDEX text spans are shaped
    // glyph runs has to actually carry the atlas and bitmaps those runs
    // reference.
    if (header.flags & FLAG_SHAPED_TEXT_DATA) {
      if (!sections.some((s) => s.id === SectionId.FontMetadata)) {
        throw new QpkError('FONT_SECTIONS_REQUIRED', 'FONT_METADATA is required');
      }
      if (!sections.some((s) => s.id === SectionId.Assets)) {
        throw new QpkError('FONT_SECTIONS_REQUIRED', 'ASSETS is required');
      }
    }

    // Rule 16. FLAG_HAS_WORD_LAYOUT's own documented meaning (section 4.2) is
    // "WORD_INDEX + LAYOUT_DATA present" -- a package claiming word-level
    // layout has to actually carry the line geometry, not just word boxes.
    if (header.flags & FLAG_HAS_WORD_LAYOUT) {
      if (!sections.some((s) => s.id === SectionId.LayoutData)) {
        throw new QpkError('LAYOUT_SECTION_REQUIRED', 'LAYOUT_DATA is required');
      }
    }

    // Rule 13. The device verifies only the fixed-size index sections at open;
    // the desktop verifies every section plus the payload, because it is the
    // side that can afford a full read and the one that catches a bad transfer.
    for (const section of sections) {
      if (!verifyChecksums && !isFixedIndexSection(section.id)) continue;
      {
        const actual = crc32(bytes.subarray(section.offset, section.offset + section.length));
        if (actual !== section.crc32) {
          throw new QpkError('SECTION_CHECKSUM', `section ${section.id}`);
        }
      }
    }
    if (verifyChecksums) {
      const payload = crc32(bytes.subarray(header.headerSize));
      if (payload !== header.payloadCrc32) {
        throw new QpkError('SECTION_CHECKSUM', 'payload checksum mismatch');
      }
    }

    const pkg = new QpkPackage(bytes, header, sections);
    pkg.checkIndexConsistency(deepIndexCheck);
    return pkg;
  }

  // --- section access -------------------------------------------------------

  section(id: SectionId | number): SectionEntry | undefined {
    return this.sections.find((s) => s.id === id);
  }

  requireSection(id: SectionId | number): SectionEntry {
    const section = this.section(id);
    if (!section) {
      throw new QpkError('NO_SUCH_SECTION', `${SectionId[id] ?? id}`);
    }
    return section;
  }

  recordCount(id: SectionId | number): number {
    return this.section(id)?.count ?? 0;
  }

  private recordAt(id: SectionId, index: number): number {
    const section = this.requireSection(id);
    if (index < 0 || index >= section.count) {
      throw new QpkError('OUT_OF_RANGE', `${SectionId[id]} index ${index}`);
    }
    return section.offset + index * section.recordSize;
  }

  // --- Quran access ---------------------------------------------------------

  getSurah(surahId: number): SurahRecord {
    const record = decodeSurah(this.view, this.recordAt(SectionId.SurahIndex, surahId - 1));
    if (record.surahId !== surahId) {
      throw new QpkError('INDEX_INCONSISTENT', `slot ${surahId - 1} holds surah ${record.surahId}`);
    }
    return record;
  }

  getAyahByIndex(index: number): AyahRecord {
    return decodeAyah(this.view, this.recordAt(SectionId.AyahIndex, index));
  }

  getAyah(surahId: number, ayahNumber: number): AyahRecord {
    const surah = this.getSurah(surahId);
    if (ayahNumber < 1 || ayahNumber > surah.ayahCount) {
      throw new QpkError('OUT_OF_RANGE', `surah ${surahId} has no ayah ${ayahNumber}`);
    }
    const record = this.getAyahByIndex(surah.firstAyahIndex + ayahNumber - 1);
    if (record.surahId !== surahId || record.ayahNumber !== ayahNumber) {
      throw new QpkError('INDEX_INCONSISTENT', `ayah ${surahId}:${ayahNumber} mislinked`);
    }
    return record;
  }

  getWordByIndex(index: number): WordRecord {
    return decodeWord(this.view, this.recordAt(SectionId.WordIndex, index));
  }

  getWord(surahId: number, ayahNumber: number, wordId: number): WordRecord {
    const ayah = this.getAyah(surahId, ayahNumber);
    if (wordId < 0 || wordId >= ayah.wordCount) {
      throw new QpkError('OUT_OF_RANGE', `ayah ${surahId}:${ayahNumber} has no word ${wordId}`);
    }
    return this.getWordByIndex(ayah.firstWordIndex + wordId);
  }

  getPage(pageNumber: number): PageRecord {
    const record = decodePage(this.view, this.recordAt(SectionId.PageIndex, pageNumber - 1));
    if (record.pageNumber !== pageNumber) {
      throw new QpkError('INDEX_INCONSISTENT', `page slot holds ${record.pageNumber}`);
    }
    return record;
  }

  private getRange(id: SectionId, rangeId: number): RangeRecord {
    const record = decodeRange(this.view, this.recordAt(id, rangeId - 1));
    if (record.id !== rangeId) {
      throw new QpkError('INDEX_INCONSISTENT', `${SectionId[id]} slot holds ${record.id}`);
    }
    return record;
  }

  getJuz(id: number): RangeRecord {
    return this.getRange(SectionId.JuzIndex, id);
  }

  getHizb(id: number): RangeRecord {
    return this.getRange(SectionId.HizbIndex, id);
  }

  getRub(id: number): RangeRecord {
    return this.getRange(SectionId.RubIndex, id);
  }

  // --- generic book / translation access ------------------------------------

  getChapter(index: number): ChapterRecord {
    return decodeChapter(this.view, this.recordAt(SectionId.ChapterIndex, index));
  }

  getBookSection(index: number): BookSectionRecord {
    return decodeBookSection(this.view, this.recordAt(SectionId.SectionIndex, index));
  }

  getTextRecord(index: number): TextRecord {
    return decodeTextRecord(this.view, this.recordAt(SectionId.TextIndex, index));
  }

  getTranslation(ayahIndex: number): TranslationRecord {
    return decodeTranslation(this.view, this.recordAt(SectionId.TranslationIndex, ayahIndex));
  }

  getSajdah(index: number): SajdahRecord {
    return decodeSajdah(this.view, this.recordAt(SectionId.SajdahIndex, index));
  }

  // --- font / glyph atlas (docs/qpk-format.md 9a) ----------------------------

  // Linear search over FONT_METADATA by glyphId: entries are not required to
  // be sorted or dense, unlike SurahIndex/PageIndex.
  getGlyph(glyphId: number): GlyphRecord {
    const section = this.requireSection(SectionId.FontMetadata);
    for (let i = 0; i < section.count; i++) {
      const record = decodeGlyph(this.view, section.offset + i * section.recordSize);
      if (record.glyphId === glyphId) return record;
    }
    throw new QpkError('OUT_OF_RANGE', `no glyph ${glyphId} in FONT_METADATA`);
  }

  // --- layout (docs/qpk-format.md 9) -----------------------------------------

  getLayoutHeader(): LayoutHeader {
    const section = this.requireSection(SectionId.LayoutData);
    if (section.length < LAYOUT_HEADER_SIZE) {
      throw new QpkError('OUT_OF_RANGE', 'LAYOUT_DATA shorter than its own header');
    }
    const header = decodeLayoutHeader(this.view, section.offset);
    // The line records the header claims must actually fit -- checked here,
    // once, rather than leaving every getLine() caller to discover a bad
    // lineCountTotal one failed read at a time (rule 11 applies the same
    // discipline to fixed-size sections' own count * recordSize).
    if (
      !within(
        header.lineRecordsOffset,
        header.lineCountTotal * LINE_RECORD_SIZE,
        section.length,
      )
    ) {
      throw new QpkError('OUT_OF_RANGE', 'LAYOUT_DATA line records run past the section');
    }
    return header;
  }

  // Direct index, like getWordByIndex -- NOT resolved from a page number.
  // There is no page -> first-line-index field in the wire format yet (see
  // the LayoutHeader comment in format.ts), so finding a page's lines means
  // walking records 0..lineCountTotal-1 and testing `.page`.
  getLine(index: number): LineRecord {
    const section = this.requireSection(SectionId.LayoutData);
    // getLayoutHeader() already proved lineRecordsOffset + lineCountTotal *
    // LINE_RECORD_SIZE <= section.length, so once index is in range the read
    // below cannot run past the section -- no separate bounds check needed.
    const layout = this.getLayoutHeader();
    if (index < 0 || index >= layout.lineCountTotal) {
      throw new QpkError('OUT_OF_RANGE', `LAYOUT_DATA line ${index}`);
    }
    const at = section.offset + layout.lineRecordsOffset + index * LINE_RECORD_SIZE;
    return decodeLine(this.view, at);
  }

  // --- payload --------------------------------------------------------------

  private blob(id: SectionId, offset: number, length: number): Uint8Array {
    const section = this.requireSection(id);
    if (!within(offset, length, section.length)) {
      throw new QpkError('OUT_OF_RANGE', `${SectionId[id]} ${offset}+${length}`);
    }
    return this.bytes.subarray(section.offset + offset, section.offset + offset + length);
  }

  text(offset: number, length: number): string {
    return new TextDecoder().decode(this.blob(SectionId.TextData, offset, length));
  }

  translationText(offset: number, length: number): string {
    return new TextDecoder().decode(this.blob(SectionId.TranslationData, offset, length));
  }

  // Raw bytes out of ASSETS -- a glyph's packed 1bpp bitmap
  // (GlyphRecord.bitmapOffset), or, for a FLAG_SHAPED_TEXT_DATA package, a
  // word's shaped glyph-id run (WordRecord's textOffset/textLength,
  // reinterpreted as textLength/2 little-endian u16 glyph ids). Not decoded
  // as text: ASSETS is binary, never UTF-8.
  assetBytes(offset: number, length: number): Uint8Array {
    return this.blob(SectionId.Assets, offset, length);
  }

  metadata(key: MetadataKey | number): string | undefined {
    const section = this.section(SectionId.Metadata);
    if (!section) return undefined;
    const view = this.view;
    let position = 0;
    while (position + 4 <= section.length) {
      const at = section.offset + position;
      const recordKey = view.getUint16(at, true);
      const valueLength = view.getUint16(at + 2, true);
      if (!within(position + 4, valueLength, section.length)) return undefined;
      if (recordKey === key) {
        return new TextDecoder().decode(
          this.bytes.subarray(at + 4, at + 4 + valueLength),
        );
      }
      const next = (position + 4 + valueLength + 3) & ~3;
      if (next <= position) return undefined;
      position = next;
    }
    return undefined;
  }

  allMetadata(): Map<number, string> {
    const out = new Map<number, string>();
    const section = this.section(SectionId.Metadata);
    if (!section) return out;
    const view = this.view;
    let position = 0;
    while (position + 4 <= section.length) {
      const at = section.offset + position;
      const key = view.getUint16(at, true);
      const valueLength = view.getUint16(at + 2, true);
      if (!within(position + 4, valueLength, section.length)) break;
      out.set(key, new TextDecoder().decode(this.bytes.subarray(at + 4, at + 4 + valueLength)));
      const next = (position + 4 + valueLength + 3) & ~3;
      if (next <= position) break;
      position = next;
    }
    return out;
  }

  // --- rule 14 --------------------------------------------------------------

  private checkIndexConsistency(deep: boolean): void {
    const surahs = this.section(SectionId.SurahIndex);
    const ayahs = this.section(SectionId.AyahIndex);
    const words = this.section(SectionId.WordIndex);
    const pages = this.section(SectionId.PageIndex);
    const text = this.section(SectionId.TextData);
    const view = this.view;

    if (surahs && ayahs) {
      for (let i = 0; i < surahs.count; i++) {
        const s = decodeSurah(view, surahs.offset + i * surahs.recordSize);
        if (!within(s.firstAyahIndex, s.ayahCount, ayahs.count)) {
          throw new QpkError('INDEX_INCONSISTENT', `surah ${s.surahId} ayah range`);
        }
        if (text && !within(s.nameOffset, s.nameLength, text.length)) {
          throw new QpkError('INDEX_INCONSISTENT', `surah ${s.surahId} name range`);
        }
      }
    }

    if (ayahs && ayahs.count > 0) {
      const probes = deep
        ? Array.from({ length: ayahs.count }, (_, i) => i)
        : [0, ayahs.count - 1];
      for (const i of probes) {
        const a = decodeAyah(view, ayahs.offset + i * ayahs.recordSize);
        if (text && !within(a.textOffset, a.textLength, text.length)) {
          throw new QpkError('INDEX_INCONSISTENT', `ayah index ${i} text range`);
        }
        if (words && !within(a.firstWordIndex, a.wordCount, words.count)) {
          throw new QpkError('INDEX_INCONSISTENT', `ayah index ${i} word range`);
        }
      }
    }

    // Rule 15's own comment (writer.ts / qpk_format.h) is explicit that a
    // FLAG_SHAPED_TEXT_DATA package's WORD_INDEX spans point into ASSETS,
    // not TEXT_DATA -- so that is what this range check has to validate
    // against once the flag is set, not TEXT_DATA unconditionally. Missed
    // until a real shaped package with a real WORD_INDEX was ever built
    // (the existing FLAG_SHAPED_TEXT_DATA test only exercises the
    // FONT_METADATA/ASSETS presence check above, with no WORD_INDEX
    // section at all).
    const shaped = (this.header.flags & FLAG_SHAPED_TEXT_DATA) !== 0;
    const wordTextBlob = shaped ? this.section(SectionId.Assets) : text;
    if (words && wordTextBlob && deep) {
      for (let i = 0; i < words.count; i++) {
        const w = decodeWord(view, words.offset + i * words.recordSize);
        if (!within(w.textOffset, w.textLength, wordTextBlob.length)) {
          throw new QpkError('INDEX_INCONSISTENT', `word index ${i} text range`);
        }
      }
    }

    // METADATA is walked in full so that `metadata()` returning undefined
    // means "absent", never "absent or malformed".
    const metadata = this.section(SectionId.Metadata);
    if (metadata) {
      let position = 0;
      while (position + 4 <= metadata.length) {
        const valueLength = view.getUint16(metadata.offset + position + 2, true);
        if (!within(position + 4, valueLength, metadata.length)) {
          throw new QpkError('INDEX_INCONSISTENT', 'metadata record overruns the section');
        }
        const next = (position + 4 + valueLength + 3) & ~3;
        if (next <= position) {
          throw new QpkError('INDEX_INCONSISTENT', 'metadata makes no forward progress');
        }
        position = next;
      }
    }

    if (pages && pages.count > 0) {
      const probes = deep
        ? Array.from({ length: pages.count }, (_, i) => i)
        : [0, pages.count - 1];
      for (const i of probes) {
        const p = decodePage(view, pages.offset + i * pages.recordSize);
        if (p.pageNumber !== i + 1) {
          throw new QpkError(
            'INDEX_INCONSISTENT',
            `page slot ${i} is numbered ${p.pageNumber}`,
          );
        }
      }
    }
  }
}

export function readPackage(bytes: Uint8Array, options?: ReadOptions): QpkPackage {
  return QpkPackage.parse(bytes, options);
}
