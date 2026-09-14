// Builds the complete Quran as ONE QURAN package plus ONE companion
// TRANSLATION package, from the corpus assembled by
// tools/arabic-pager/build_full_quran_source.py.
//
// Why one package and not 114: the format addresses surahs as
// SURAH_INDEX[id - 1], so a package holds contiguous ids starting at 1 --
// which 1..114 is exactly -- and the device's library index has 96 slots
// against the 228 a per-surah split would need. See docs/quran-content.md.
//
// This supersedes build-test-surahs.mjs's six single-surah packages, which
// each declare surah id 1 internally because they could not do otherwise.
import { mkdirSync, readFileSync, writeFileSync, statSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  buildQuranPackage,
  buildTranslationPackage,
  contentIdToHex,
  validatePackage,
} from '../dist/src/index.js';
import { readPackage, SectionId } from '@quran-device/qpk-format';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, '..', '..', '..');
const dataDir = join(repoRoot, 'tools', 'arabic-pager', 'data', 'all');
const outDir = join(here, '..', 'examples');
const quranStageDir = join(repoRoot, 'sdcard-staging', 'LIBRARY', 'QURAN');
const translationStageDir = join(repoRoot, 'sdcard-staging', 'LIBRARY', 'TRANSLATIONS');

mkdirSync(outDir, { recursive: true });
mkdirSync(quranStageDir, { recursive: true });
mkdirSync(translationStageDir, { recursive: true });

const readJson = (p) => JSON.parse(readFileSync(p, 'utf8'));
const mb = (n) => `${(n / (1024 * 1024)).toFixed(2)} MB`;

console.log('reading the assembled corpus...');
const source = readJson(join(dataDir, 'quran-source.json'));
const translationVerses = readJson(join(dataDir, 'translation-verses.json'));

// Independent re-checks. The Python assembler already asserts these, but this
// is a different process reading the file it wrote, and 6236 is the one number
// in this whole pipeline that is externally verifiable.
if (source.surahs.length !== 114) {
  throw new Error(`expected 114 surahs, source has ${source.surahs.length}`);
}
if (source.ayahs.length !== 6236) {
  throw new Error(`expected 6236 ayahs, source has ${source.ayahs.length}`);
}
if (translationVerses.length !== source.ayahs.length) {
  throw new Error(
    `translation has ${translationVerses.length} verses, Arabic has ${source.ayahs.length}`,
  );
}
// Surah ids must be 1..114 in order: that is the whole premise of one package.
source.surahs.forEach((s, i) => {
  if (s.id !== i + 1) throw new Error(`surah at index ${i} declares id ${s.id}`);
});
// No empty words. The API puts a leading space on the first ayah of 110 of the
// 114 surahs, and a naive split mints a phantom word for each -- which would
// desynchronise this package's WORD_INDEX from the shaped atlas package's and
// silently misalign every word-level feature. Caught once; asserted from now on.
const emptyWords = source.ayahs.reduce(
  (n, a) => n + a.words.filter((w) => w.trim() === '').length,
  0,
);
if (emptyWords > 0) throw new Error(`${emptyWords} empty words in the source`);
const wordTotal = source.ayahs.reduce((n, a) => n + a.words.length, 0);
if (wordTotal !== 82011) {
  throw new Error(`expected 82011 words (the shaper's count), source has ${wordTotal}`);
}

console.log('building the QURAN package (114 surahs, 6236 ayahs)...');
const quran = buildQuranPackage(source);
const quranValidation = validatePackage(quran.bytes);
if (!quranValidation.ok) {
  throw new Error(`QURAN package does not validate:\n  ${quranValidation.errors.join('\n  ')}`);
}
const quranPkg = readPackage(quran.bytes);
const quranId = contentIdToHex(quranPkg.header.contentId);

console.log('building the TRANSLATION package...');
const translation = buildTranslationPackage(
  translationVerses,
  {
    title: 'The Holy Quran - Saheeh International',
    author: 'Saheeh International',
    language: 'en',
    source: 'quran.com API v4, translation 20',
  },
  { alignedContentId: quranPkg.header.contentId },
);
const translationValidation = validatePackage(translation.bytes);
if (!translationValidation.ok) {
  throw new Error(
    `TRANSLATION package does not validate:\n  ${translationValidation.errors.join('\n  ')}`,
  );
}
const translationId = contentIdToHex(readPackage(translation.bytes).header.contentId);

// Read the Quran package back and confirm the index really is addressable by
// real surah number -- the property the whole one-package decision rests on.
const readback = readPackage(quran.bytes);
for (const id of [1, 2, 18, 36, 55, 114]) {
  const surah = readback.getSurah(id);
  if (surah === undefined) throw new Error(`SURAH_INDEX has no surah ${id}`);
}
const surahCount = readback.recordCount(SectionId.SurahIndex);
const ayahCount = readback.recordCount(SectionId.AyahIndex);
if (surahCount !== 114 || ayahCount !== 6236) {
  throw new Error(`readback says ${surahCount} surahs / ${ayahCount} ayahs`);
}

for (const [bytes, id, dir, label] of [
  [quran.bytes, quranId, quranStageDir, 'QURAN'],
  [translation.bytes, translationId, translationStageDir, 'TRANSLATION'],
]) {
  const example = join(outDir, label === 'QURAN' ? 'quran-full.qpk' : 'quran-full-en.qpk');
  const staged = join(dir, `${id}.qpk`);
  writeFileSync(example, bytes);
  writeFileSync(staged, bytes);
  console.log(`  ${label.padEnd(12)} ${mb(bytes.length).padStart(9)}  ${id}`);
  console.log(`    -> ${example}`);
  console.log(`    -> ${staged}`);
}

console.log('');
console.log(`surahs ${surahCount}, ayahs ${ayahCount}`);
console.log(`aligned: translation -> ${quranId}`);
console.log(`total staged: ${mb(statSync(join(quranStageDir, `${quranId}.qpk`)).size +
  statSync(join(translationStageDir, `${translationId}.qpk`)).size)}`);
