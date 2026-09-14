import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { crc32, crc32Finish, crc32Update, CRC32_INIT } from '../src/crc32.js';
import {
  encodeAyah,
  encodeGlyph,
  encodeLayoutData,
  encodePage,
  encodeSurah,
  encodeWord,
  FLAG_HAS_WORD_LAYOUT,
  FLAG_RIGHT_TO_LEFT,
  FLAG_SHAPED_TEXT_DATA,
  HEADER_SIZE,
  MetadataKey,
  PackageType,
  SectionId,
} from '../src/format.js';
import { buildMiniQuran } from '../src/mini-quran.js';
import { QpkError, readPackage } from '../src/reader.js';
import { writePackage } from '../src/writer.js';

function fixHeaderCrc(bytes: Uint8Array): void {
  new DataView(bytes.buffer, bytes.byteOffset).setUint32(
    60,
    crc32(bytes.subarray(0, 60)),
    true,
  );
}

function expectCode(fn: () => unknown, code: string): void {
  try {
    fn();
  } catch (error) {
    if (!(error instanceof QpkError)) {
      assert.fail(`expected QpkError, got ${String(error)}`);
    }
    assert.equal(error.code, code);
    return;
  }
  assert.fail(`expected ${code}, but nothing was thrown`);
}

/** Flips bits in one byte. noUncheckedIndexedAccess makes `b[i] ^= m` a type error. */
function flip(bytes: Uint8Array, index: number, mask: number): void {
  bytes.set([(bytes.at(index) ?? 0) ^ mask], index);
}

test('crc32 matches the canonical check value', () => {
  // Same constant the C++ suite asserts. If these ever disagree, no package
  // written on one side will validate on the other.
  assert.equal(crc32(new TextEncoder().encode('123456789')), 0xcbf43926);
});

test('crc32 is streamable', () => {
  const data = new TextEncoder().encode('123456789');
  let crc = CRC32_INIT;
  crc = crc32Update(crc, data.subarray(0, 4));
  crc = crc32Update(crc, data.subarray(4));
  assert.equal(crc32Finish(crc), 0xcbf43926);
});

test('the mini package round-trips through the writer and reader', () => {
  const pkg = readPackage(buildMiniQuran());

  assert.equal(pkg.header.packageType, PackageType.Quran);
  assert.equal(pkg.header.contentVersion, 3);
  assert.equal(pkg.header.flags, FLAG_HAS_WORD_LAYOUT | FLAG_RIGHT_TO_LEFT);
  assert.equal(pkg.header.contentId[0], 0x10);
  assert.equal(pkg.header.contentId[15], 0x1f);

  const surah = pkg.getSurah(2);
  assert.equal(surah.ayahCount, 2);
  assert.equal(surah.firstAyahIndex, 3);
  assert.equal(pkg.text(surah.nameOffset, surah.nameLength), 'PLACEHOLDER-SURAH-TWO');

  const ayah = pkg.getAyah(2, 2);
  assert.equal(ayah.wordCount, 2);
  assert.equal(ayah.firstWordIndex, 10);
  assert.equal(pkg.text(ayah.textOffset, ayah.textLength), 'PLACEHOLDER-AYAH-2-2');

  const word = pkg.getWord(2, 2, 1);
  assert.equal(word.width, 400);
  assert.equal(pkg.text(word.textOffset, word.textLength), 'WORD11');

  assert.equal(pkg.getPage(2).firstWordIndex, 7);
  assert.equal(pkg.getJuz(1).firstPage, 1);
  assert.equal(pkg.metadata(MetadataKey.Title), 'Mini Test Package');
  assert.equal(pkg.metadata(MetadataKey.Script), 'placeholder');
  assert.equal(pkg.metadata(MetadataKey.Author), undefined);
});

test('the writer produces a deterministic byte stream', () => {
  // Byte-for-byte determinism is what lets the golden fixture be a contract
  // rather than a snapshot that drifts.
  assert.deepEqual(buildMiniQuran(), buildMiniQuran());
});

test('sections are emitted in ascending id order regardless of input order', () => {
  const bytes = writePackage({
    type: PackageType.Translation,
    metadata: [[MetadataKey.Title, 'Reordered']],
    sections: [
      { id: SectionId.TranslationData, payload: new TextEncoder().encode('abcd'), count: 0 },
      {
        id: SectionId.TranslationIndex,
        payload: new Uint8Array(8), // one record: offset 0, length 0
        count: 1,
      },
    ],
  });
  const pkg = readPackage(bytes);
  assert.deepEqual(
    pkg.sections.map((s) => s.id),
    [SectionId.Metadata, SectionId.TranslationIndex, SectionId.TranslationData],
  );
});

test('the writer refuses a payload that does not match its record count', () => {
  assert.throws(() =>
    writePackage({
      type: PackageType.Quran,
      sections: [{ id: SectionId.SurahIndex, payload: new Uint8Array(23), count: 1 }],
    }),
  );
});

test('a truncated file is rejected', () => {
  expectCode(() => readPackage(buildMiniQuran().subarray(0, 32)), 'TOO_SMALL');
});

test('bad magic is rejected', () => {
  const bytes = buildMiniQuran();
  bytes[0] = 0x58;
  expectCode(() => readPackage(bytes), 'BAD_MAGIC');
});

test('an unsupported format version is rejected', () => {
  const bytes = buildMiniQuran();
  new DataView(bytes.buffer, bytes.byteOffset).setUint16(4, 2, true);
  fixHeaderCrc(bytes);
  expectCode(() => readPackage(bytes), 'UNSUPPORTED_VERSION');
});

test('a corrupt header is rejected', () => {
  const bytes = buildMiniQuran();
  flip(bytes, 30, 0xff);
  expectCode(() => readPackage(bytes), 'HEADER_CHECKSUM');
});

test('a size mismatch is rejected', () => {
  const bytes = buildMiniQuran();
  new DataView(bytes.buffer, bytes.byteOffset).setBigUint64(
    16,
    BigInt(bytes.length + 4),
    true,
  );
  fixHeaderCrc(bytes);
  expectCode(() => readPackage(bytes), 'SIZE_MISMATCH');
});

test('a damaged index section is rejected', () => {
  const bytes = buildMiniQuran();
  const pkg = readPackage(bytes);
  const section = pkg.requireSection(SectionId.WordIndex);
  const damaged = Uint8Array.from(bytes);
  flip(damaged, section.offset + 7, 0x01);
  expectCode(() => readPackage(damaged), 'SECTION_CHECKSUM');
});

test('a damaged blob is caught by the desktop but not by the device rules', () => {
  const bytes = buildMiniQuran();
  const section = readPackage(bytes).requireSection(SectionId.TextData);
  const damaged = Uint8Array.from(bytes);
  flip(damaged, section.offset + 2, 0x40);

  // Desktop default: full verification catches it.
  expectCode(() => readPackage(damaged), 'SECTION_CHECKSUM');
  // Device rules: index sections only, so the structure still parses. This
  // asymmetry is deliberate -- see docs/qpk-format.md rule 13.
  const lenient = readPackage(damaged, { verifyChecksums: false, deepIndexCheck: false });
  assert.equal(lenient.header.packageType, PackageType.Quran);
});

test('a required section must be present', () => {
  const bytes = writePackage({
    type: PackageType.Quran,
    metadata: [[MetadataKey.Title, 'No text data']],
    sections: [
      { id: SectionId.SurahIndex, payload: new Uint8Array(24), count: 1 },
      { id: SectionId.PageIndex, payload: new Uint8Array(16), count: 1 },
      { id: SectionId.AyahIndex, payload: new Uint8Array(24), count: 1 },
    ],
  });
  expectCode(() => readPackage(bytes), 'MISSING_SECTION');
});

test('FLAG_SHAPED_TEXT_DATA requires FONT_METADATA and ASSETS', () => {
  const shapedNoFont = writePackage({
    type: PackageType.Unknown,
    flags: FLAG_SHAPED_TEXT_DATA,
    metadata: [[MetadataKey.Title, 'Shaped, no font sections']],
    sections: [],
  });
  expectCode(() => readPackage(shapedNoFont), 'FONT_SECTIONS_REQUIRED');

  const shapedFontOnly = writePackage({
    type: PackageType.Unknown,
    flags: FLAG_SHAPED_TEXT_DATA,
    metadata: [[MetadataKey.Title, 'Font index, no bitmaps']],
    sections: [
      {
        id: SectionId.FontMetadata,
        payload: encodeGlyph({
          glyphId: 1,
          bitmapOffset: 0,
          width: 8,
          height: 8,
          xAdvance: 8,
          xOffset: 0,
          yOffset: 0,
        }),
        count: 1,
      },
    ],
  });
  expectCode(() => readPackage(shapedFontOnly), 'FONT_SECTIONS_REQUIRED');

  const shapedComplete = writePackage({
    type: PackageType.Unknown,
    flags: FLAG_SHAPED_TEXT_DATA,
    metadata: [[MetadataKey.Title, 'Font index and bitmaps']],
    sections: [
      {
        id: SectionId.FontMetadata,
        payload: encodeGlyph({
          glyphId: 1,
          bitmapOffset: 0,
          width: 8,
          height: 8,
          xAdvance: 8,
          xOffset: 0,
          yOffset: 0,
        }),
        count: 1,
      },
      { id: SectionId.Assets, payload: new Uint8Array(8), count: 0 },
    ],
  });
  const pkg = readPackage(shapedComplete);
  const glyph = pkg.getGlyph(1);
  assert.equal(glyph.width, 8);
  assert.equal(glyph.yOffset, 0);
  expectCode(() => pkg.getGlyph(999), 'OUT_OF_RANGE');
  assert.equal(pkg.assetBytes(0, 8).length, 8);
});

// Regression: checkIndexConsistency's deep sweep validated every WORD_INDEX
// entry's textOffset/textLength against TEXT_DATA unconditionally, even
// under FLAG_SHAPED_TEXT_DATA -- where the format's own documented contract
// (see the comment on FLAG_SHAPED_TEXT_DATA above) is that those spans point
// into ASSETS instead. A real shaped WORD_INDEX (offsets past TEXT_DATA's
// length, valid within ASSETS) failed to open at all until this was fixed.
// The test above never caught it: it has FONT_METADATA and ASSETS but no
// WORD_INDEX section, so the buggy TEXT_DATA-only check never ran.
test('a real shaped WORD_INDEX validates against ASSETS, not TEXT_DATA', () => {
  const bitmap = new Uint8Array([0xff]);
  const glyphRun = new Uint8Array(2);
  new DataView(glyphRun.buffer).setUint16(0, 1, true); // one glyph id, referencing id 1
  const assets = new Uint8Array([...bitmap, ...glyphRun]);

  const bytes = writePackage({
    type: PackageType.Unknown,
    flags: FLAG_SHAPED_TEXT_DATA,
    metadata: [[MetadataKey.Title, 'Real shaped WORD_INDEX']],
    sections: [
      {
        id: SectionId.FontMetadata,
        payload: encodeGlyph({
          glyphId: 1,
          bitmapOffset: 0,
          width: 1,
          height: 1,
          xAdvance: 1,
          xOffset: 0,
          yOffset: 0,
        }),
        count: 1,
      },
      { id: SectionId.Assets, payload: assets, count: 0 },
      {
        id: SectionId.WordIndex,
        // textOffset 1 is past TEXT_DATA's length (there is no TEXT_DATA
        // section at all here) but well within ASSETS' 3 bytes -- exactly
        // the case the buggy check rejected.
        payload: encodeWord({ textOffset: 1, textLength: 2, x: 0, y: 0, width: 0, height: 0, lineId: 0 }),
        count: 1,
      },
    ],
  });

  const pkg = readPackage(bytes); // must not throw INDEX_INCONSISTENT
  const word = pkg.getWordByIndex(0);
  const raw = pkg.assetBytes(word.textOffset, word.textLength);
  assert.equal(new DataView(raw.buffer, raw.byteOffset, raw.byteLength).getUint16(0, true), 1);
});

test('LAYOUT_DATA round-trips header and line records', () => {
  const pkg = readPackage(buildMiniQuran());
  const header = pkg.getLayoutHeader();
  assert.equal(header.layoutVersion, 1);
  assert.equal(header.pageWidth, 1000);
  assert.equal(header.pageHeight, 1400);
  assert.equal(header.lineCountTotal, 2);

  const line0 = pkg.getLine(0);
  assert.deepEqual(line0, { page: 1, lineId: 1, y: 100, height: 48, firstWordIndex: 0 });
  const line1 = pkg.getLine(1);
  assert.deepEqual(line1, { page: 2, lineId: 1, y: 100, height: 48, firstWordIndex: 7 });

  expectCode(() => pkg.getLine(2), 'OUT_OF_RANGE');
});

test('a LAYOUT_DATA header claiming more lines than the section holds is rejected', () => {
  // lineCountTotal says 5000 lines; the payload only has room for 1. A
  // malformed header must fail at getLayoutHeader(), not surface as N
  // individual getLine() failures downstream.
  const payload = encodeLayoutData(
    [{ page: 1, lineId: 1, y: 0, height: 10, firstWordIndex: 0 }],
    800,
    1200,
  );
  new DataView(payload.buffer).setUint16(6, 5000, true); // lineCountTotal
  const bytes = writePackage({
    type: PackageType.Unknown,
    metadata: [[MetadataKey.Title, 'Bad layout header']],
    sections: [{ id: SectionId.LayoutData, payload, count: 0 }],
  });
  const pkg = readPackage(bytes);
  expectCode(() => pkg.getLayoutHeader(), 'OUT_OF_RANGE');
});

test('FLAG_HAS_WORD_LAYOUT requires LAYOUT_DATA', () => {
  const minimalQuranSections = [
    { id: SectionId.SurahIndex, payload: encodeSurah({
        surahId: 1, ayahCount: 1, firstAyahIndex: 0, firstPage: 1, lastPage: 1,
        firstJuz: 0, revelationPlace: 0, hasBismillah: 0, nameOffset: 0, nameLength: 0,
      }), count: 1 },
    { id: SectionId.PageIndex, payload: encodePage({
        pageNumber: 1, ayahCount: 1, firstAyahIndex: 0, firstWordIndex: 0,
        lineCount: 1, juz: 0, flags: 0,
      }), count: 1 },
    { id: SectionId.AyahIndex, payload: encodeAyah({
        surahId: 1, ayahNumber: 1, page: 1, line: 1, wordCount: 0, flags: 0,
        textOffset: 0, textLength: 0, firstWordIndex: 0,
      }), count: 1 },
    { id: SectionId.TextData, payload: new Uint8Array(0), count: 0 },
  ];

  const bytes = writePackage({
    type: PackageType.Quran,
    flags: FLAG_HAS_WORD_LAYOUT,
    metadata: [[MetadataKey.Title, 'Word boxes, no layout section']],
    sections: minimalQuranSections,
  });
  expectCode(() => readPackage(bytes), 'LAYOUT_SECTION_REQUIRED');

  const withLayout = writePackage({
    type: PackageType.Quran,
    flags: FLAG_HAS_WORD_LAYOUT,
    metadata: [[MetadataKey.Title, 'Word boxes, with layout section']],
    sections: [
      ...minimalQuranSections,
      {
        id: SectionId.LayoutData,
        payload: encodeLayoutData(
          [{ page: 1, lineId: 1, y: 0, height: 10, firstWordIndex: 0 }],
          800,
          1200,
        ),
        count: 0,
      },
    ],
  });
  const pkg = readPackage(withLayout);
  assert.equal(pkg.getLayoutHeader().lineCountTotal, 1);
});

test('out-of-range lookups are reported, not clamped', () => {
  const pkg = readPackage(buildMiniQuran());
  expectCode(() => pkg.getSurah(3), 'OUT_OF_RANGE');
  expectCode(() => pkg.getAyah(1, 4), 'OUT_OF_RANGE');
  expectCode(() => pkg.getWord(1, 1, 2), 'OUT_OF_RANGE');
  expectCode(() => pkg.getPage(3), 'OUT_OF_RANGE');
  expectCode(() => pkg.getHizb(1), 'NO_SUCH_SECTION');
  expectCode(() => pkg.text(0, 0xffffffff), 'OUT_OF_RANGE');
});

test('the committed golden fixture matches the current writer', () => {
  // The other half of the cross-language contract. The C++ suite compares its
  // builder against the committed golden_fixture.h; without this check, a
  // change on the TypeScript side with a stale fixture would go unnoticed --
  // C++ would just compare two unchanged things and pass.
  // This file runs from dist/test/, so the package root is two levels up.
  const here = dirname(fileURLToPath(import.meta.url));
  const fixture = join(here, '..', '..', 'fixtures', 'mini-quran.qpk');
  const committed = new Uint8Array(readFileSync(fixture));
  assert.deepEqual(
    buildMiniQuran(),
    committed,
    'fixtures/mini-quran.qpk is stale -- run `npm run fixture` and commit both artifacts',
  );
});

test('a malformed metadata record is rejected, not read as an absent key', () => {
  const bytes = buildMiniQuran();
  const section = readPackage(bytes).requireSection(SectionId.Metadata);
  const damaged = Uint8Array.from(bytes);
  const view = new DataView(damaged.buffer, damaged.byteOffset);

  // First record: value_length at +2, made to overrun the section.
  view.setUint16(section.offset + 2, 0xffff, true);

  // Repair every checksum, so the structural rule is what fails and not a CRC.
  view.setUint32(
    findSectionCrcOffset(damaged, SectionId.Metadata),
    crc32(damaged.subarray(section.offset, section.offset + section.length)),
    true,
  );
  view.setUint32(52, crc32(damaged.subarray(64)), true);
  fixHeaderCrc(damaged);

  expectCode(() => readPackage(damaged), 'INDEX_INCONSISTENT');
});

/** Byte offset of a section entry's crc32 field within the section table. */
function findSectionCrcOffset(bytes: Uint8Array, id: SectionId): number {
  const view = new DataView(bytes.buffer, bytes.byteOffset);
  const tableOffset = view.getUint32(44, true);
  const count = view.getUint16(10, true);
  for (let i = 0; i < count; i++) {
    const at = tableOffset + i * 32;
    if (view.getUint16(at, true) === id) return at + 24;
  }
  throw new Error(`section ${id} not found`);
}

test('the header is exactly 64 bytes and the table follows it', () => {
  const pkg = readPackage(buildMiniQuran());
  assert.equal(pkg.header.headerSize, HEADER_SIZE);
  assert.equal(pkg.header.sectionTableOffset, HEADER_SIZE);
  assert.equal(pkg.header.sectionTableLength, pkg.sections.length * 32);
  for (const section of pkg.sections) {
    assert.equal(section.offset % 4, 0, `section ${section.id} is misaligned`);
  }
});
