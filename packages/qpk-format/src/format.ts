// QPK1 constants and record codecs, mirroring firmware/include/qpk/qpk_format.h.
//
// If you change anything here, change it there too and re-run both test
// suites -- the golden fixture check will catch a divergence, but only if it
// is regenerated.

export const MAGIC = Uint8Array.from([0x51, 0x50, 0x4b, 0x31]); // "QPK1"
export const FORMAT_VERSION = 1;
export const HEADER_SIZE = 64;
export const SECTION_ENTRY_SIZE = 32;
export const MAX_SECTIONS = 32;
export const CONTENT_ID_BYTES = 16;

export enum PackageType {
  Unknown = 0,
  Quran = 1,
  Book = 2,
  Translation = 3,
  Tafsir = 4,
}

export enum SectionId {
  Metadata = 1,
  SurahIndex = 2,
  JuzIndex = 3,
  HizbIndex = 4,
  RubIndex = 5,
  PageIndex = 6,
  AyahIndex = 7,
  WordIndex = 8,
  SajdahIndex = 9,
  TextData = 10,
  LayoutData = 11,
  TranslationIndex = 12,
  TranslationData = 13,
  FontMetadata = 14,
  Assets = 15,
  ChapterIndex = 16,
  SectionIndex = 17,
  TextIndex = 18,
  /** Optional cover picture, any package type -- COVER_* below, qpk-format.md 9c. */
  Cover = 19,
  /** Pre-rendered page pictures, Book -- PAGE_IMAGE_* below, qpk-format.md 9d. */
  PageImageIndex = 20,
  PageImageData = 21,
}

export enum MetadataKey {
  Title = 1,
  Author = 2,
  Language = 3,
  Publisher = 4,
  Source = 5,
  BuildTimestamp = 6,
  ConverterVersion = 7,
  Script = 8,
  AlignedContentId = 9,
  /**
   * `"<firstGlyphId>:<count>"` -- the atlas glyph ids that draw end-of-ayah
   * markers, in a FLAG_SHAPED_TEXT_DATA package. The marker for ayah number
   * n is glyph id `firstGlyphId + (n - 1)`, for n in 1..count. See
   * docs/qpk-format.md 9b.
   */
  AyahMarkerGlyphs = 10,
}

export const FLAG_HAS_WORD_LAYOUT = 1 << 0;
export const FLAG_RIGHT_TO_LEFT = 1 << 1;
export const FLAG_IS_DELTA = 1 << 2;
// WORD_INDEX records' (textOffset, textLength) point into ASSETS instead of
// TEXT_DATA, and span textLength/2 pre-shaped glyph ids (u16 LE, FONT_METADATA
// space) in left-to-right visual order, instead of raw UTF-8 bytes. Requires
// FontMetadata and Assets (rule 15) -- see docs/qpk-format.md 9a. AYAH_INDEX
// and every other section's text spans are unaffected: still raw UTF-8 in
// TEXT_DATA, always, so search/translation-matching/TTS keep working
// regardless of this flag.
export const FLAG_SHAPED_TEXT_DATA = 1 << 3;

export const RECORD_SIZE: Readonly<Record<number, number>> = {
  [SectionId.SurahIndex]: 24,
  [SectionId.JuzIndex]: 8,
  [SectionId.HizbIndex]: 8,
  [SectionId.RubIndex]: 8,
  [SectionId.PageIndex]: 16,
  [SectionId.AyahIndex]: 24,
  [SectionId.WordIndex]: 16,
  [SectionId.SajdahIndex]: 8,
  [SectionId.TranslationIndex]: 8,
  [SectionId.ChapterIndex]: 24,
  [SectionId.SectionIndex]: 16,
  [SectionId.TextIndex]: 12,
  [SectionId.FontMetadata]: 12,
  [SectionId.PageImageIndex]: 16,
};

/** Fixed record size for a section id, or 0 for blob / variable sections. */
export function recordSizeFor(id: number): number {
  return RECORD_SIZE[id] ?? 0;
}

/** True for sections whose CRC the device verifies eagerly at open. */
export function isFixedIndexSection(id: number): boolean {
  return recordSizeFor(id) !== 0;
}

export const REQUIRED_SECTIONS: Readonly<Record<number, SectionId[]>> = {
  [PackageType.Quran]: [
    SectionId.Metadata,
    SectionId.SurahIndex,
    SectionId.PageIndex,
    SectionId.AyahIndex,
    SectionId.TextData,
  ],
  [PackageType.Book]: [
    SectionId.Metadata,
    SectionId.ChapterIndex,
    SectionId.PageIndex,
    SectionId.TextIndex,
    SectionId.TextData,
  ],
  [PackageType.Translation]: [
    SectionId.Metadata,
    SectionId.TranslationIndex,
    SectionId.TranslationData,
  ],
  [PackageType.Tafsir]: [
    SectionId.Metadata,
    SectionId.TranslationIndex,
    SectionId.TranslationData,
  ],
};

// --- decoded record shapes ---------------------------------------------------

export interface Header {
  formatVersion: number;
  headerSize: number;
  packageType: PackageType;
  sectionCount: number;
  flags: number;
  packageSize: number;
  contentId: Uint8Array;
  contentVersion: number;
  sectionTableOffset: number;
  sectionTableLength: number;
  payloadCrc32: number;
  headerCrc32: number;
}

export interface SectionEntry {
  id: number;
  version: number;
  offset: number;
  length: number;
  count: number;
  crc32: number;
  recordSize: number;
  flags: number;
}

export interface SurahRecord {
  surahId: number;
  ayahCount: number;
  firstAyahIndex: number;
  firstPage: number;
  lastPage: number;
  firstJuz: number;
  revelationPlace: number;
  hasBismillah: number;
  nameOffset: number;
  nameLength: number;
}

export interface AyahRecord {
  surahId: number;
  ayahNumber: number;
  page: number;
  line: number;
  wordCount: number;
  flags: number;
  textOffset: number;
  textLength: number;
  firstWordIndex: number;
}

export interface WordRecord {
  textOffset: number;
  textLength: number;
  x: number;
  y: number;
  width: number;
  height: number;
  lineId: number;
}

export interface PageRecord {
  pageNumber: number;
  ayahCount: number;
  firstAyahIndex: number;
  firstWordIndex: number;
  lineCount: number;
  juz: number;
  flags: number;
}

export interface RangeRecord {
  id: number;
  firstPage: number;
  firstAyahIndex: number;
}

// --- record codecs -----------------------------------------------------------

export function encodeSurah(r: SurahRecord): Uint8Array {
  const b = new Uint8Array(24);
  const v = new DataView(b.buffer);
  v.setUint16(0, r.surahId, true);
  v.setUint16(2, r.ayahCount, true);
  v.setUint32(4, r.firstAyahIndex, true);
  v.setUint16(8, r.firstPage, true);
  v.setUint16(10, r.lastPage, true);
  v.setUint16(12, r.firstJuz, true);
  v.setUint8(14, r.revelationPlace);
  v.setUint8(15, r.hasBismillah);
  v.setUint32(16, r.nameOffset, true);
  v.setUint16(20, r.nameLength, true);
  v.setUint16(22, 0, true);
  return b;
}

export function decodeSurah(v: DataView, at: number): SurahRecord {
  return {
    surahId: v.getUint16(at + 0, true),
    ayahCount: v.getUint16(at + 2, true),
    firstAyahIndex: v.getUint32(at + 4, true),
    firstPage: v.getUint16(at + 8, true),
    lastPage: v.getUint16(at + 10, true),
    firstJuz: v.getUint16(at + 12, true),
    revelationPlace: v.getUint8(at + 14),
    hasBismillah: v.getUint8(at + 15),
    nameOffset: v.getUint32(at + 16, true),
    nameLength: v.getUint16(at + 20, true),
  };
}

export function encodeAyah(r: AyahRecord): Uint8Array {
  const b = new Uint8Array(24);
  const v = new DataView(b.buffer);
  v.setUint16(0, r.surahId, true);
  v.setUint16(2, r.ayahNumber, true);
  v.setUint16(4, r.page, true);
  v.setUint16(6, r.line, true);
  v.setUint16(8, r.wordCount, true);
  v.setUint16(10, r.flags, true);
  v.setUint32(12, r.textOffset, true);
  v.setUint32(16, r.textLength, true);
  v.setUint32(20, r.firstWordIndex, true);
  return b;
}

export function decodeAyah(v: DataView, at: number): AyahRecord {
  return {
    surahId: v.getUint16(at + 0, true),
    ayahNumber: v.getUint16(at + 2, true),
    page: v.getUint16(at + 4, true),
    line: v.getUint16(at + 6, true),
    wordCount: v.getUint16(at + 8, true),
    flags: v.getUint16(at + 10, true),
    textOffset: v.getUint32(at + 12, true),
    textLength: v.getUint32(at + 16, true),
    firstWordIndex: v.getUint32(at + 20, true),
  };
}

export function encodeWord(r: WordRecord): Uint8Array {
  const b = new Uint8Array(16);
  const v = new DataView(b.buffer);
  v.setUint32(0, r.textOffset, true);
  v.setUint16(4, r.textLength, true);
  v.setUint16(6, r.x, true);
  v.setUint16(8, r.y, true);
  v.setUint16(10, r.width, true);
  v.setUint16(12, r.height, true);
  v.setUint16(14, r.lineId, true);
  return b;
}

export function decodeWord(v: DataView, at: number): WordRecord {
  return {
    textOffset: v.getUint32(at + 0, true),
    textLength: v.getUint16(at + 4, true),
    x: v.getUint16(at + 6, true),
    y: v.getUint16(at + 8, true),
    width: v.getUint16(at + 10, true),
    height: v.getUint16(at + 12, true),
    lineId: v.getUint16(at + 14, true),
  };
}

export function encodePage(r: PageRecord): Uint8Array {
  const b = new Uint8Array(16);
  const v = new DataView(b.buffer);
  v.setUint16(0, r.pageNumber, true);
  v.setUint16(2, r.ayahCount, true);
  v.setUint32(4, r.firstAyahIndex, true);
  v.setUint32(8, r.firstWordIndex, true);
  v.setUint8(12, r.lineCount);
  v.setUint8(13, r.juz);
  v.setUint16(14, r.flags, true);
  return b;
}

export function decodePage(v: DataView, at: number): PageRecord {
  return {
    pageNumber: v.getUint16(at + 0, true),
    ayahCount: v.getUint16(at + 2, true),
    firstAyahIndex: v.getUint32(at + 4, true),
    firstWordIndex: v.getUint32(at + 8, true),
    lineCount: v.getUint8(at + 12),
    juz: v.getUint8(at + 13),
    flags: v.getUint16(at + 14, true),
  };
}

export function encodeRange(r: RangeRecord): Uint8Array {
  const b = new Uint8Array(8);
  const v = new DataView(b.buffer);
  v.setUint16(0, r.id, true);
  v.setUint16(2, r.firstPage, true);
  v.setUint32(4, r.firstAyahIndex, true);
  return b;
}

export function decodeRange(v: DataView, at: number): RangeRecord {
  return {
    id: v.getUint16(at + 0, true),
    firstPage: v.getUint16(at + 2, true),
    firstAyahIndex: v.getUint32(at + 4, true),
  };
}

export interface SajdahRecord {
  ayahIndex: number;
  page: number;
  kind: number;
}

export interface TranslationRecord {
  textOffset: number;
  textLength: number;
}

export interface ChapterRecord {
  chapterNumber: number;
  firstPage: number;
  firstSectionIndex: number;
  titleOffset: number;
  titleLength: number;
  depth: number;
  parentIndex: number;
}

export const NO_PARENT = 0xffffffff;

export interface BookSectionRecord {
  firstPage: number;
  titleOffset: number;
  titleLength: number;
  depth: number;
  firstTextIndex: number;
}

export interface TextRecord {
  textOffset: number;
  textLength: number;
  pageNumber: number;
  flags: number;
}

export function encodeSajdah(r: SajdahRecord): Uint8Array {
  const b = new Uint8Array(8);
  const v = new DataView(b.buffer);
  v.setUint32(0, r.ayahIndex, true);
  v.setUint16(4, r.page, true);
  v.setUint8(6, r.kind);
  v.setUint8(7, 0);
  return b;
}

export function decodeSajdah(v: DataView, at: number): SajdahRecord {
  return {
    ayahIndex: v.getUint32(at + 0, true),
    page: v.getUint16(at + 4, true),
    kind: v.getUint8(at + 6),
  };
}

export function encodeTranslation(r: TranslationRecord): Uint8Array {
  const b = new Uint8Array(8);
  const v = new DataView(b.buffer);
  v.setUint32(0, r.textOffset, true);
  v.setUint32(4, r.textLength, true);
  return b;
}

export function decodeTranslation(v: DataView, at: number): TranslationRecord {
  return {
    textOffset: v.getUint32(at + 0, true),
    textLength: v.getUint32(at + 4, true),
  };
}

export function encodeChapter(r: ChapterRecord): Uint8Array {
  const b = new Uint8Array(24);
  const v = new DataView(b.buffer);
  v.setUint32(0, r.chapterNumber, true);
  v.setUint32(4, r.firstPage, true);
  v.setUint32(8, r.firstSectionIndex, true);
  v.setUint32(12, r.titleOffset, true);
  v.setUint16(16, r.titleLength, true);
  v.setUint16(18, r.depth, true);
  v.setUint32(20, r.parentIndex, true);
  return b;
}

export function decodeChapter(v: DataView, at: number): ChapterRecord {
  return {
    chapterNumber: v.getUint32(at + 0, true),
    firstPage: v.getUint32(at + 4, true),
    firstSectionIndex: v.getUint32(at + 8, true),
    titleOffset: v.getUint32(at + 12, true),
    titleLength: v.getUint16(at + 16, true),
    depth: v.getUint16(at + 18, true),
    parentIndex: v.getUint32(at + 20, true),
  };
}

export function encodeBookSection(r: BookSectionRecord): Uint8Array {
  const b = new Uint8Array(16);
  const v = new DataView(b.buffer);
  v.setUint32(0, r.firstPage, true);
  v.setUint32(4, r.titleOffset, true);
  v.setUint16(8, r.titleLength, true);
  v.setUint16(10, r.depth, true);
  v.setUint32(12, r.firstTextIndex, true);
  return b;
}

export function decodeBookSection(v: DataView, at: number): BookSectionRecord {
  return {
    firstPage: v.getUint32(at + 0, true),
    titleOffset: v.getUint32(at + 4, true),
    titleLength: v.getUint16(at + 8, true),
    depth: v.getUint16(at + 10, true),
    firstTextIndex: v.getUint32(at + 12, true),
  };
}

export function encodeTextRecord(r: TextRecord): Uint8Array {
  const b = new Uint8Array(12);
  const v = new DataView(b.buffer);
  v.setUint32(0, r.textOffset, true);
  v.setUint32(4, r.textLength, true);
  v.setUint16(8, r.pageNumber, true);
  v.setUint16(10, r.flags, true);
  return b;
}

export function decodeTextRecord(v: DataView, at: number): TextRecord {
  return {
    textOffset: v.getUint32(at + 0, true),
    textLength: v.getUint32(at + 4, true),
    pageNumber: v.getUint16(at + 8, true),
    flags: v.getUint16(at + 10, true),
  };
}

// One entry in FONT_METADATA -- the glyph atlas index. `glyphId` is arbitrary
// (assigned by whatever shaping tool built the atlas) and is what a
// FLAG_SHAPED_TEXT_DATA package's WORD_INDEX-via-ASSETS glyph runs reference.
// Not necessarily sorted or dense.
//
// The bitmap at `bitmapOffset` in ASSETS is `width` x `height`, 1bpp,
// MSB-first, each row padded to a whole byte. Bit 1 is ink (foreground); bit 0
// is transparent, not "background white" -- this is what lets a diacritic's
// bitmap overlap a base letter's without clobbering it.
//
// xAdvance/xOffset/yOffset are per-glyph-id constants, not per-occurrence:
// this format does not support context-dependent mark repositioning (mint a
// distinct glyphId per visually distinct base+mark combination instead). See
// docs/qpk-format.md 9a.
export interface GlyphRecord {
  glyphId: number;
  bitmapOffset: number; // into ASSETS
  width: number;
  height: number;
  xAdvance: number; // signed: pen movement after drawing this glyph
  xOffset: number;  // signed: pen position -> bitmap left edge
  yOffset: number;  // signed: line baseline -> bitmap top edge
}

export function encodeGlyph(r: GlyphRecord): Uint8Array {
  const b = new Uint8Array(12);
  const v = new DataView(b.buffer);
  v.setUint16(0, r.glyphId, true);
  v.setUint32(2, r.bitmapOffset, true);
  v.setUint8(6, r.width);
  v.setUint8(7, r.height);
  v.setInt8(8, r.xAdvance);
  v.setInt8(9, r.xOffset);
  v.setInt8(10, r.yOffset);
  v.setUint8(11, 0);
  return b;
}

export function decodeGlyph(v: DataView, at: number): GlyphRecord {
  return {
    glyphId: v.getUint16(at + 0, true),
    bitmapOffset: v.getUint32(at + 2, true),
    width: v.getUint8(at + 6),
    height: v.getUint8(at + 7),
    xAdvance: v.getInt8(at + 8),
    xOffset: v.getInt8(at + 9),
    yOffset: v.getInt8(at + 10),
  };
}

// LAYOUT_DATA (id 11) -- per-line geometry for a Quran or Book package. Blob
// section: a fixed 12-byte header followed by `lineCountTotal` 12-byte line
// records. See docs/qpk-format.md section 9.
//
// Line records are written in ascending (page, lineId) order, but that is a
// writer discipline, not a wire-format guarantee the way SURAH_INDEX/
// PAGE_INDEX slot addressing is: there is no page -> first-line-index field
// yet, because nothing reads LAYOUT_DATA at runtime to demand one. Finding a
// page's lines today means walking the records, the same shape of tradeoff
// FONT_METADATA already makes for glyphIds (see getGlyph).
export interface LayoutHeader {
  layoutVersion: number;
  pageWidth: number;
  pageHeight: number;
  lineCountTotal: number;
  lineRecordsOffset: number; // relative to the section start
}

export const LAYOUT_HEADER_SIZE = 12;
export const LINE_RECORD_SIZE = 12;

export interface LineRecord {
  page: number;
  lineId: number;
  y: number;
  height: number;
  firstWordIndex: number;
}

export function encodeLayoutData(lines: LineRecord[], pageWidth: number, pageHeight: number): Uint8Array {
  const out = new Uint8Array(LAYOUT_HEADER_SIZE + lines.length * LINE_RECORD_SIZE);
  const v = new DataView(out.buffer);
  v.setUint16(0, 1, true); // layout_version
  v.setUint16(2, pageWidth, true);
  v.setUint16(4, pageHeight, true);
  v.setUint16(6, lines.length, true);
  v.setUint32(8, LAYOUT_HEADER_SIZE, true);
  lines.forEach((line, i) => {
    const at = LAYOUT_HEADER_SIZE + i * LINE_RECORD_SIZE;
    v.setUint16(at + 0, line.page, true);
    v.setUint16(at + 2, line.lineId, true);
    v.setUint16(at + 4, line.y, true);
    v.setUint16(at + 6, line.height, true);
    v.setUint32(at + 8, line.firstWordIndex, true);
  });
  return out;
}

export function decodeLayoutHeader(v: DataView, at: number): LayoutHeader {
  return {
    layoutVersion: v.getUint16(at + 0, true),
    pageWidth: v.getUint16(at + 2, true),
    pageHeight: v.getUint16(at + 4, true),
    lineCountTotal: v.getUint16(at + 6, true),
    lineRecordsOffset: v.getUint32(at + 8, true),
  };
}

export function decodeLine(v: DataView, at: number): LineRecord {
  return {
    page: v.getUint16(at + 0, true),
    lineId: v.getUint16(at + 2, true),
    y: v.getUint16(at + 4, true),
    height: v.getUint16(at + 6, true),
    firstWordIndex: v.getUint32(at + 8, true),
  };
}

// --- COVER (section 19) -------------------------------------------------------
//
// A 16-byte header -- "QCV1", width u16, height u16, bits per pixel u8 (2),
// seven zero bytes -- then COVER_WIDTH x COVER_HEIGHT pixels at 2 bpp, row
// major, four to a byte, most significant first, 0 = black .. 3 = white: the
// panel's four greys, packed exactly as the device's photos are.

export const COVER_MAGIC = Uint8Array.from([0x51, 0x43, 0x56, 0x31]); // "QCV1"
export const COVER_WIDTH = 108;
export const COVER_HEIGHT = 144;
export const COVER_HEADER_BYTES = 16;
export const COVER_BYTES = COVER_HEADER_BYTES + (COVER_WIDTH / 4) * COVER_HEIGHT;

/** A COVER payload from COVER_WIDTH x COVER_HEIGHT levels 0..3, row-major. */
export function encodeCover(levels: ArrayLike<number>): Uint8Array {
  const count = COVER_WIDTH * COVER_HEIGHT;
  if (levels.length !== count) throw new Error(`a cover is ${count} levels, got ${levels.length}`);
  const payload = new Uint8Array(COVER_BYTES);
  payload.set(COVER_MAGIC, 0);
  const view = new DataView(payload.buffer);
  view.setUint16(4, COVER_WIDTH, true);
  view.setUint16(6, COVER_HEIGHT, true);
  payload[8] = 2;
  const stride = COVER_WIDTH / 4;
  for (let y = 0; y < COVER_HEIGHT; y++) {
    for (let x = 0; x < COVER_WIDTH; x++) {
      const raw = Math.trunc(levels[y * COVER_WIDTH + x] ?? 3);
      const level = raw < 0 ? 0 : raw > 3 ? 3 : raw;
      const at = COVER_HEADER_BYTES + y * stride + (x >> 2);
      payload[at] = (payload[at] ?? 0) | (level << (6 - 2 * (x & 3)));
    }
  }
  return payload;
}

/** The levels back out of a COVER payload. Throws if it is not one. */
export function decodeCover(payload: Uint8Array): Uint8Array {
  if (payload.length !== COVER_BYTES) {
    throw new Error(`a cover is ${COVER_BYTES} bytes, this is ${payload.length}`);
  }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.length);
  const magicOk = COVER_MAGIC.every((byte, i) => payload[i] === byte);
  const reservedZero = payload.subarray(9, COVER_HEADER_BYTES).every((byte) => byte === 0);
  if (
    !magicOk ||
    view.getUint16(4, true) !== COVER_WIDTH ||
    view.getUint16(6, true) !== COVER_HEIGHT ||
    payload[8] !== 2 ||
    !reservedZero
  ) {
    throw new Error(`not a ${COVER_WIDTH}x${COVER_HEIGHT} 2 bpp QCV1 cover`);
  }
  const levels = new Uint8Array(COVER_WIDTH * COVER_HEIGHT);
  const stride = COVER_WIDTH / 4;
  for (let y = 0; y < COVER_HEIGHT; y++) {
    for (let x = 0; x < COVER_WIDTH; x++) {
      const byte = payload[COVER_HEADER_BYTES + y * stride + (x >> 2)] ?? 0;
      levels[y * COVER_WIDTH + x] = (byte >> (6 - 2 * (x & 3))) & 3;
    }
  }
  return levels;
}

// --- PAGE_IMAGE_INDEX (20) and PAGE_IMAGE_DATA (21) ---------------------------
//
// A book whose pages are pictures of the source's own pages (a PDF rendered
// the way an e-reader does), so the layout survives exactly. Each page is a
// width x height bitmap, 1 bit per pixel, row-major, ceil(width / 8) bytes a
// row, most significant bit first, 1 = black -- compressed with PackBits and
// stored end to end in PAGE_IMAGE_DATA. PAGE_IMAGE_INDEX has one 16-byte
// record per page: offset u32 and length u32 into PAGE_IMAGE_DATA, width u16,
// height u16, reserved u32 = 0. The device's reader is portrait, 480x800.

export const PAGE_IMAGE_WIDTH = 480;
export const PAGE_IMAGE_HEIGHT = 800;

export interface PageImageRecord {
  offset: number;
  length: number;
  width: number;
  height: number;
}

export function encodePageImageRecord(r: PageImageRecord): Uint8Array {
  const out = new Uint8Array(16);
  const v = new DataView(out.buffer);
  v.setUint32(0, r.offset, true);
  v.setUint32(4, r.length, true);
  v.setUint16(8, r.width, true);
  v.setUint16(10, r.height, true);
  return out;
}

export function decodePageImageRecord(v: DataView, at: number): PageImageRecord {
  return {
    offset: v.getUint32(at, true),
    length: v.getUint32(at + 4, true),
    width: v.getUint16(at + 8, true),
    height: v.getUint16(at + 10, true),
  };
}

/** Uncompressed size of a width x height 1bpp page bitmap. */
export function pageImageBytes(width: number, height: number): number {
  return Math.ceil(width / 8) * height;
}

/**
 * PackBits: a header byte n, then either n + 1 literal bytes (n 0..127) or one
 * byte repeated 257 - n times (n 129..255). 128 is never written. Mostly-white
 * text pages shrink several times over; the worst case grows by 1 byte in 128.
 */
export function packBits(data: Uint8Array): Uint8Array {
  const out: number[] = [];
  let i = 0;
  while (i < data.length) {
    let run = 1;
    while (i + run < data.length && run < 128 && data[i + run] === data[i]) run++;
    if (run >= 3) {
      out.push(257 - run, data[i]!);
      i += run;
      continue;
    }
    const start = i;
    let length = 0;
    while (i < data.length && length < 128) {
      if (i + 2 < data.length && data[i] === data[i + 1] && data[i] === data[i + 2]) break;
      i++;
      length++;
    }
    out.push(length - 1);
    for (let k = start; k < start + length; k++) out.push(data[k]!);
  }
  return Uint8Array.from(out);
}

/** Undoes packBits. Throws unless `src` decodes to exactly `expectedLength` bytes. */
export function unpackBits(src: Uint8Array, expectedLength: number): Uint8Array {
  const out = new Uint8Array(expectedLength);
  let i = 0;
  let o = 0;
  while (i < src.length) {
    const n = src[i++]!;
    if (n < 128) {
      const count = n + 1;
      if (i + count > src.length || o + count > expectedLength) {
        throw new Error('PackBits literal runs past the end');
      }
      out.set(src.subarray(i, i + count), o);
      i += count;
      o += count;
    } else if (n > 128) {
      const count = 257 - n;
      if (i >= src.length || o + count > expectedLength) {
        throw new Error('PackBits repeat runs past the end');
      }
      out.fill(src[i]!, o, o + count);
      i++;
      o += count;
    }
  }
  if (o !== expectedLength) {
    throw new Error(`PackBits data decodes to ${o} bytes, expected ${expectedLength}`);
  }
  return out;
}

export function sectionName(id: number): string {
  return SectionId[id] ?? `UNKNOWN(${id})`;
}

export function packageTypeName(type: number): string {
  return PackageType[type] ?? `UNKNOWN(${type})`;
}
