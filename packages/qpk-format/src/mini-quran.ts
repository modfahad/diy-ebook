// The shared synthetic fixture.
//
// This MUST stay byte-identical to BuildMiniQuran() in
// firmware/test/test_qpk/qpk_test_package.h. The C++ test suite asserts that
// the two writers agree byte for byte, which is the only real guarantee that
// the desktop and the device read the same format.
//
// IMPORTANT: every string here is obviously-synthetic placeholder content.
// No Quranic text is fabricated anywhere in this repository; real mushaf text
// needs a verified source and belongs to the converter milestone.

import {
  FLAG_HAS_WORD_LAYOUT,
  FLAG_RIGHT_TO_LEFT,
  MetadataKey,
  PackageType,
  SectionId,
  encodeAyah,
  encodeLayoutData,
  encodePage,
  encodeRange,
  encodeSurah,
  encodeWord,
} from './format.js';
import { TextPool, writePackage } from './writer.js';

export const MINI_SURAH_COUNT = 2;
export const MINI_AYAH_COUNT = 5;
export const MINI_WORD_COUNT = 12;
export const MINI_PAGE_COUNT = 2;

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

export function buildMiniQuran(): Uint8Array {
  const text = new TextPool();

  const surahName = [
    text.add('PLACEHOLDER-SURAH-ONE'),
    text.add('PLACEHOLDER-SURAH-TWO'),
  ];

  const wordsPerAyah = [2, 3, 2, 3, 2];
  const ayahSurah = [1, 1, 1, 2, 2];
  const ayahNumber = [1, 2, 3, 1, 2];
  const ayahPage = [1, 1, 1, 2, 2];

  const ayahText = ayahNumber.map((n, i) =>
    text.add(`PLACEHOLDER-AYAH-${ayahSurah[i]}-${n}`),
  );
  const wordText = Array.from({ length: MINI_WORD_COUNT }, (_, w) =>
    text.add(`WORD${w}`),
  );

  const surahs = concat([
    encodeSurah({
      surahId: 1,
      ayahCount: 3,
      firstAyahIndex: 0,
      firstPage: 1,
      lastPage: 1,
      firstJuz: 1,
      revelationPlace: 0,
      hasBismillah: 1,
      nameOffset: surahName[0]!.offset,
      nameLength: surahName[0]!.length,
    }),
    encodeSurah({
      surahId: 2,
      ayahCount: 2,
      firstAyahIndex: 3,
      firstPage: 2,
      lastPage: 2,
      firstJuz: 1,
      revelationPlace: 1,
      hasBismillah: 1,
      nameOffset: surahName[1]!.offset,
      nameLength: surahName[1]!.length,
    }),
  ]);

  let firstWord = 0;
  const ayahs = concat(
    ayahNumber.map((n, i) => {
      const record = encodeAyah({
        surahId: ayahSurah[i]!,
        ayahNumber: n,
        page: ayahPage[i]!,
        line: 1,
        wordCount: wordsPerAyah[i]!,
        // Bit 0 = starts a juz.
        flags: i === 0 ? 1 : 0,
        textOffset: ayahText[i]!.offset,
        textLength: ayahText[i]!.length,
        firstWordIndex: firstWord,
      });
      firstWord += wordsPerAyah[i]!;
      return record;
    }),
  );

  const words = concat(
    wordText.map((ref, w) =>
      encodeWord({
        textOffset: ref.offset,
        textLength: ref.length,
        x: 40 + w * 60,
        y: 100,
        width: 400, // deliberately > 255: exercises the u16 promotion
        height: 48,
        lineId: 1,
      }),
    ),
  );

  const pages = concat([
    encodePage({
      pageNumber: 1,
      ayahCount: 3,
      firstAyahIndex: 0,
      firstWordIndex: 0,
      lineCount: 15,
      juz: 1,
      flags: 0,
    }),
    encodePage({
      pageNumber: 2,
      ayahCount: 2,
      firstAyahIndex: 3,
      firstWordIndex: 7,
      lineCount: 15,
      juz: 1,
      flags: 0,
    }),
  ]);

  const juz = encodeRange({ id: 1, firstPage: 1, firstAyahIndex: 0 });

  // LAYOUT_DATA: every word in this fixture sits on line 1 (see the words
  // loop above), one line per page, so there is exactly one line record per
  // page, y/height matching the words' own fixed y=100/height=48.
  const layout = encodeLayoutData(
    [
      { page: 1, lineId: 1, y: 100, height: 48, firstWordIndex: 0 },
      { page: 2, lineId: 1, y: 100, height: 48, firstWordIndex: 7 },
    ],
    1000,
    1400,
  );

  const contentId = new Uint8Array(16);
  for (let i = 0; i < contentId.length; i++) contentId[i] = 0x10 + i;

  return writePackage({
    type: PackageType.Quran,
    flags: FLAG_HAS_WORD_LAYOUT | FLAG_RIGHT_TO_LEFT,
    contentVersion: 3,
    contentId,
    metadata: [
      [MetadataKey.Title, 'Mini Test Package'],
      [MetadataKey.Language, 'ar'],
      [MetadataKey.Script, 'placeholder'],
    ],
    sections: [
      { id: SectionId.SurahIndex, payload: surahs, count: MINI_SURAH_COUNT },
      { id: SectionId.AyahIndex, payload: ayahs, count: MINI_AYAH_COUNT },
      { id: SectionId.WordIndex, payload: words, count: MINI_WORD_COUNT },
      { id: SectionId.PageIndex, payload: pages, count: MINI_PAGE_COUNT },
      { id: SectionId.JuzIndex, payload: juz, count: 1 },
      { id: SectionId.TextData, payload: text.bytes(), count: 0 },
      { id: SectionId.LayoutData, payload: layout, count: 0 },
    ],
  });
}
