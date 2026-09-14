import assert from 'node:assert/strict';
import test from 'node:test';

import {
  FLAG_SHAPED_TEXT_DATA,
  MetadataKey,
  PackageType,
  SectionId,
  encodeAyah,
  encodePage,
  encodeSurah,
  readPackage,
  writePackage,
} from '@quran-device/qpk-format';

import { buildGlyphAtlasSections, type AtlasGlyphInput, type AtlasWordInput } from '../src/pipeline/glyph-atlas.js';

// Two glyphs: one with a real 1x1 bitmap, one a zero-size positioning mark
// (no ink of its own -- a real case, see shape_glyph_atlas.py's handling of
// zero-width/height FreeType bitmaps).
const glyphs: AtlasGlyphInput[] = [
  { id: 1, width: 1, height: 1, xAdvance: 10, xOffset: 0, yOffset: -5, bitmap: new Uint8Array([0x80]) },
  { id: 2, width: 0, height: 0, xAdvance: 0, xOffset: 2, yOffset: 3, bitmap: new Uint8Array(0) },
];
const words: AtlasWordInput[] = [
  { glyphIds: [1, 2] },
  { glyphIds: [2, 1, 1] },
];

test('buildGlyphAtlasSections sizes sections to record count x record size', () => {
  const sections = buildGlyphAtlasSections(glyphs, words);
  assert.equal(sections.fontMetadata.length, glyphs.length * 12); // GlyphRecord is 12 bytes
  assert.equal(sections.wordIndex.length, words.length * 16); // WordRecord is 16 bytes
  assert.equal(sections.glyphCount, 2);
  assert.equal(sections.wordCount, 2);
});

test('a shaped QURAN package round-trips glyph metrics, bitmaps and word runs', () => {
  const sections = buildGlyphAtlasSections(glyphs, words);

  const bytes = writePackage({
    type: PackageType.Quran,
    flags: FLAG_SHAPED_TEXT_DATA,
    metadata: [[MetadataKey.Title, 'Glyph atlas smoke test']],
    sections: [
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
      { id: SectionId.FontMetadata, payload: sections.fontMetadata, count: sections.glyphCount },
      { id: SectionId.Assets, payload: sections.assets, count: 0 },
      { id: SectionId.WordIndex, payload: sections.wordIndex, count: sections.wordCount },
    ],
  });

  const pkg = readPackage(bytes);

  const g1 = pkg.getGlyph(1);
  assert.equal(g1.width, 1);
  assert.equal(g1.xAdvance, 10);
  assert.equal(g1.yOffset, -5);
  const g2 = pkg.getGlyph(2);
  assert.equal(g2.width, 0);
  assert.equal(g2.xOffset, 2);

  // g1's bitmap: one byte, 0x80, at offset 0.
  const g1Bitmap = pkg.assetBytes(g1.bitmapOffset, 1);
  assert.deepEqual(Array.from(g1Bitmap), [0x80]);

  // Decode each word's glyph-id run back out of ASSETS and confirm it
  // matches what was handed in -- this is the exact byte layout
  // firmware/qpk/qpk_reader.cpp's readAsset()/Read16 contract depends on.
  function decodeWord(index: number): number[] {
    const word = pkg.getWordByIndex(index);
    const raw = pkg.assetBytes(word.textOffset, word.textLength);
    const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
    const ids: number[] = [];
    for (let i = 0; i < word.textLength; i += 2) ids.push(view.getUint16(i, true));
    return ids;
  }
  assert.deepEqual(decodeWord(0), [1, 2]);
  assert.deepEqual(decodeWord(1), [2, 1, 1]);
});

test('a word referencing an unknown glyph id is rejected, not silently written', () => {
  assert.throws(
    () => buildGlyphAtlasSections(glyphs, [{ glyphIds: [1, 999] }]),
    /glyph id 999/,
  );
});
