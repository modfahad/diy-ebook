// Builds a QURAN + TRANSLATION package pair for each of six short surahs,
// fetched by tools/arabic-pager/fetch_verses.py, fetch_translation.py and
// fetch_chapter_meta.py -- the first real (non-synthetic, non-An-Naba) Quran
// content to go through this pipeline.
//
// Each surah is its own single-surah QURAN package (surah id 1 internally --
// see quran-package.ts's header comment on why SURAH_INDEX can't hold a
// sparse, non-contiguous set of real chapter numbers yet). The real chapter
// number and Arabic name live in metadata.title and the surah's name field,
// which is all that exists to identify it anyway since the library browser
// (Milestone 5) isn't built yet.
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  buildQuranPackage,
  buildTranslationPackage,
  contentIdToHex,
  validatePackage,
} from '../dist/src/index.js';
import { readPackage } from '@quran-device/qpk-format';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, '..', '..', '..');
const dataDir = join(repoRoot, 'tools', 'arabic-pager', 'data');
const outDir = join(here, '..', 'examples', 'surahs');
const quranStageDir = join(repoRoot, 'sdcard-staging', 'LIBRARY', 'QURAN');
const translationStageDir = join(repoRoot, 'sdcard-staging', 'LIBRARY', 'TRANSLATIONS');

mkdirSync(outDir, { recursive: true });
mkdirSync(quranStageDir, { recursive: true });
mkdirSync(translationStageDir, { recursive: true });

const CHAPTERS = [1, 103, 108, 112, 113, 114];

function readJson(path) {
  return JSON.parse(readFileSync(path, 'utf8'));
}

const rows = [];

for (const ch of CHAPTERS) {
  const verses = readJson(join(dataDir, `ch${ch}-imlaei.json`));
  const translation = readJson(join(dataDir, `ch${ch}-en.json`));
  const meta = readJson(join(dataDir, `ch${ch}-meta.json`));

  if (verses.verses.length !== meta.verses_count) {
    throw new Error(
      `${meta.name_simple}: fetched ${verses.verses.length} verses, chapter metadata says ${meta.verses_count}`,
    );
  }
  if (translation.verses.length !== meta.verses_count) {
    throw new Error(
      `${meta.name_simple}: fetched ${translation.verses.length} translation verses, expected ${meta.verses_count}`,
    );
  }

  // Placeholder pagination: page 1, one ayah per line. There is no canonical
  // Madinah-mushaf page/line-break data in this repo yet (docs/pending.md
  // section 4, "page-boundary fidelity") -- these are short single-page
  // surahs, so this is an honest simplification, not a claim of real mushaf
  // pagination.
  const quranSource = {
    schemaVersion: 1,
    metadata: {
      title: `${meta.name_simple} (Surah ${meta.id})`,
      script: 'imlaei',
      language: 'ar',
      source: verses.source,
    },
    surahs: [
      {
        id: 1,
        name: meta.name_arabic,
        revelationPlace: meta.revelation_place === 'makkah' ? 'meccan' : 'medinan',
        hasBismillah: meta.bismillah_pre,
      },
    ],
    ayahs: verses.verses.map((v, i) => ({
      surah: 1,
      ayah: i + 1,
      page: 1,
      line: i + 1,
      words: v.text_uthmani.split(' '),
      text: v.text_uthmani,
    })),
  };

  const quranResult = buildQuranPackage(quranSource);
  const quranValidation = validatePackage(quranResult.bytes);
  if (!quranValidation.ok) {
    throw new Error(`${meta.name_simple}: QURAN package does not validate: ${quranValidation.errors.join('; ')}`);
  }
  const quranPkg = readPackage(quranResult.bytes);
  const quranId = contentIdToHex(quranPkg.header.contentId);

  const translationResult = buildTranslationPackage(
    translation.verses.map((v, i) => ({ surah: 1, ayah: i + 1, text: v.text })),
    {
      title: `${meta.name_simple} - ${translation.translation_name}`,
      author: translation.author ?? undefined,
      language: 'en',
      source: translation.source,
    },
    { alignedContentId: quranPkg.header.contentId },
  );
  const translationValidation = validatePackage(translationResult.bytes);
  if (!translationValidation.ok) {
    throw new Error(
      `${meta.name_simple}: TRANSLATION package does not validate: ${translationValidation.errors.join('; ')}`,
    );
  }
  const translationId = contentIdToHex(readPackage(translationResult.bytes).header.contentId);

  const slug = meta.name_simple.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
  const quranPath = join(outDir, `${slug}.qpk`);
  const translationPath = join(outDir, `${slug}-en.qpk`);
  writeFileSync(quranPath, quranResult.bytes);
  writeFileSync(translationPath, translationResult.bytes);
  writeFileSync(join(quranStageDir, `${quranId}.qpk`), quranResult.bytes);
  writeFileSync(join(translationStageDir, `${translationId}.qpk`), translationResult.bytes);

  rows.push({
    surah: `${meta.name_simple} (${meta.id})`,
    ayat: meta.verses_count,
    quranBytes: quranResult.bytes.length,
    translationBytes: translationResult.bytes.length,
    quranId: quranId.slice(0, 12),
    warnings: [...quranResult.report.warnings, ...translationResult.warnings],
  });
}

console.log(`built ${rows.length} surah pairs -> ${outDir}`);
console.log(`staged -> ${quranStageDir}`);
console.log(`staged -> ${translationStageDir}`);
console.table(
  rows.map((r) => ({
    surah: r.surah,
    ayat: r.ayat,
    'quran.qpk': r.quranBytes,
    'translation.qpk': r.translationBytes,
    contentId: r.quranId,
  })),
);
for (const r of rows) {
  for (const w of r.warnings) console.log(`  ${r.surah}: ${w}`);
}
