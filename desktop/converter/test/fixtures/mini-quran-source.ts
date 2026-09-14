// A synthetic Quran source in the import schema.
//
// IMPORTANT: every string here is obviously-synthetic placeholder content.
// No Quranic text is fabricated anywhere in this repository. This fixture
// exists to exercise the shape of the pipeline, not its content.

import type { QuranSource } from '../../src/quran/schema.js';

export function miniQuranSource(): QuranSource {
  const words = (surah: number, ayah: number, count: number): string[] =>
    Array.from({ length: count }, (_, i) => `W${surah}-${ayah}-${i}`);

  const ayahs = [
    { surah: 1, ayah: 1, page: 1, line: 1, words: words(1, 1, 3) },
    { surah: 1, ayah: 2, page: 1, line: 2, words: words(1, 2, 2) },
    { surah: 1, ayah: 3, page: 2, line: 1, words: words(1, 3, 4) },
    { surah: 2, ayah: 1, page: 2, line: 3, words: words(2, 1, 2) },
    { surah: 2, ayah: 2, page: 3, line: 1, words: words(2, 2, 3) },
    { surah: 2, ayah: 3, page: 3, line: 2, words: words(2, 3, 2) },
  ].map((entry) => ({ ...entry, text: entry.words.join(' ') }));

  return {
    schemaVersion: 1,
    metadata: {
      title: 'Synthetic Test Mushaf',
      script: 'placeholder',
      language: 'ar',
      source: 'synthetic fixture, not real text',
    },
    surahs: [
      { id: 1, name: 'PLACEHOLDER-SURAH-ONE', revelationPlace: 'meccan', hasBismillah: true },
      { id: 2, name: 'PLACEHOLDER-SURAH-TWO', revelationPlace: 'medinan', hasBismillah: true },
    ],
    ayahs,
    pages: [
      { page: 1, lineCount: 15 },
      { page: 2, lineCount: 15 },
      { page: 3, lineCount: 15 },
    ],
    juz: [
      { id: 1, surah: 1, ayah: 1 },
      { id: 2, surah: 2, ayah: 2 },
    ],
    sajdah: [{ surah: 2, ayah: 3, kind: 'recommended' }],
    layout: {
      pageWidth: 1000,
      pageHeight: 1400,
      words: ayahs.flatMap((ayah) =>
        ayah.words.map((_, index) => ({
          surah: ayah.surah,
          ayah: ayah.ayah,
          word: index,
          x: 100 + index * 120,
          y: 200 + (ayah.line ?? 1) * 60,
          width: 110,
          height: 50,
          line: ayah.line ?? 1,
        })),
      ),
    },
  };
}
