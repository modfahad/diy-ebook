#!/usr/bin/env node
// Dumps a QURAN package's ayat to the terminal, Arabic text with the aligned
// TRANSLATION package's English beneath each one when one can be found.
//
// Not a reader -- there isn't one yet (Milestone 5). This is a quick way to
// see the actual text sitting in a package before that exists.
//
//   node tools/quran-reader/read.mjs <quran.qpk> [translation.qpk]
//
// If the translation isn't given, this looks for one in the same directory,
// and in a sibling TRANSLATIONS/ directory if the Quran package lives under
// a QURAN/ directory (the sdcard-staging/device layout) -- matched by
// MetadataKey.AlignedContentId, not by filename.
//
// Requires the qpk-format package to be built:
//   npm install --prefix packages/qpk-format
//   npm run build --prefix packages/qpk-format

import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const entry = join(here, '..', '..', 'packages', 'qpk-format', 'dist', 'src', 'index.js');

let qpkFormat;
try {
  qpkFormat = await import(`file://${entry.replace(/\\/g, '/')}`);
} catch (error) {
  console.error(
    'Could not load @quran-device/qpk-format.\n' +
      'Build it first:\n' +
      '  npm install --prefix packages/qpk-format\n' +
      '  npm run build --prefix packages/qpk-format\n' +
      `\n(${error.message})`,
  );
  process.exit(2);
}
const { readPackage, PackageType, MetadataKey } = qpkFormat;

const argv = process.argv.slice(2);
const quranPath = argv[0];
let translationPath = argv[1];
if (!quranPath) {
  console.error('usage: read.mjs <quran.qpk> [translation.qpk]');
  process.exit(2);
}

function loadPackage(path) {
  return readPackage(new Uint8Array(readFileSync(path)));
}

function contentIdHex(id) {
  return [...id].map((b) => b.toString(16).padStart(2, '0')).join('');
}

const quranPkg = loadPackage(quranPath);
if (quranPkg.header.packageType !== PackageType.Quran) {
  console.error(`${quranPath} is not a QURAN package`);
  process.exit(2);
}
const quranId = contentIdHex(quranPkg.header.contentId);

function findAlignedTranslation() {
  const candidateDirs = [dirname(quranPath)];
  const quranDir = dirname(quranPath);
  if (quranDir.toUpperCase().endsWith('QURAN')) {
    candidateDirs.push(join(dirname(quranDir), 'TRANSLATIONS'));
  }
  for (const dir of candidateDirs) {
    let files;
    try {
      files = readdirSync(dir).filter((f) => f.toLowerCase().endsWith('.qpk'));
    } catch {
      continue;
    }
    for (const file of files) {
      const path = join(dir, file);
      if (path === quranPath) continue;
      let pkg;
      try {
        pkg = loadPackage(path);
      } catch {
        continue;
      }
      if (pkg.header.packageType !== PackageType.Translation) continue;
      if (pkg.metadata(MetadataKey.AlignedContentId) === quranId) return { path, pkg };
    }
  }
  return null;
}

let translationPkg = null;
if (translationPath) {
  translationPkg = loadPackage(translationPath);
} else {
  const found = findAlignedTranslation();
  if (found) {
    translationPath = found.path;
    translationPkg = found.pkg;
  }
}

const title = quranPkg.metadata(MetadataKey.Title) ?? '(no title)';
console.log(`${quranId.slice(0, 12)}  ${title}`);
if (translationPkg) {
  console.log(`translation: ${translationPkg.metadata(MetadataKey.Title) ?? translationPath}`);
} else {
  console.log('(no aligned translation found)');
}
console.log();

const { SectionId } = qpkFormat;
const ayahCount = quranPkg.recordCount(SectionId.AyahIndex);

for (let i = 0; i < ayahCount; i++) {
  const ayah = quranPkg.getAyahByIndex(i);
  const arabic = quranPkg.text(ayah.textOffset, ayah.textLength);
  console.log(`${ayah.surahId}:${ayah.ayahNumber}  ${arabic}`);
  if (translationPkg) {
    const translation = translationPkg.getTranslation(i);
    const english = translationPkg.translationText(translation.textOffset, translation.textLength);
    console.log(`     ${english}`);
  }
  console.log();
}
