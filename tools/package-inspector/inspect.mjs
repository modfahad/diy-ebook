#!/usr/bin/env node
// QPK package inspector.
//
//   node tools/package-inspector/inspect.mjs <file.qpk> [--quiet-index]
//
// Dumps the header, the section table and a sample of index records, and
// reports whether the package validates. This is the tool that makes a
// writer/parser disagreement legible instead of just "the device says no".
//
// Requires packages/qpk-format to be built:
//     npm install --prefix packages/qpk-format
//     npm run build --prefix packages/qpk-format

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const packageEntry = join(here, '..', '..', 'packages', 'qpk-format', 'dist', 'src', 'index.js');

let qpk;
try {
  qpk = await import(`file://${packageEntry.replace(/\\/g, '/')}`);
} catch {
  console.error(
    'Could not load @quran-device/qpk-format.\n' +
      'Build it first:\n' +
      '  npm install --prefix packages/qpk-format\n' +
      '  npm run build --prefix packages/qpk-format',
  );
  process.exit(2);
}

const args = process.argv.slice(2);
const path = args.find((a) => !a.startsWith('--'));
const quietIndex = args.includes('--quiet-index');

if (!path) {
  console.error('usage: inspect.mjs <file.qpk> [--quiet-index]');
  process.exit(2);
}

const bytes = new Uint8Array(readFileSync(path));

let pkg;
try {
  pkg = qpk.readPackage(bytes);
} catch (error) {
  console.log(`FILE      ${path}`);
  console.log(`SIZE      ${bytes.length} bytes`);
  console.log(`\nINVALID   ${error.message}`);
  process.exit(1);
}

const hex = (n, width = 8) => '0x' + (n >>> 0).toString(16).padStart(width, '0');
const contentId = [...pkg.header.contentId].map((b) => b.toString(16).padStart(2, '0')).join('');

console.log(`FILE      ${path}`);
console.log(`TYPE      ${qpk.packageTypeName(pkg.header.packageType)}`);
console.log(`FORMAT    QPK${pkg.header.formatVersion}`);
console.log(`SIZE      ${pkg.header.packageSize} bytes`);
console.log(`CONTENT   ${contentId}  v${pkg.header.contentVersion}`);
console.log(`FLAGS     ${hex(pkg.header.flags, 4)}${flagNames(pkg.header.flags)}`);
console.log(`PAYLOAD   crc ${hex(pkg.header.payloadCrc32)}`);

console.log('\nMETADATA');
const metadata = pkg.allMetadata();
if (metadata.size === 0) {
  console.log('  (none)');
}
for (const [key, value] of metadata) {
  console.log(`  ${(qpk.MetadataKey[key] ?? key).toString().padEnd(18)} ${value}`);
}

console.log('\nSECTIONS');
console.log('  id  name                 offset      length   records  rec  crc');
for (const section of pkg.sections) {
  console.log(
    '  ' +
      String(section.id).padStart(2) +
      '  ' +
      qpk.sectionName(section.id).padEnd(19) +
      String(section.offset).padStart(9) +
      String(section.length).padStart(11) +
      String(section.count).padStart(9) +
      String(section.recordSize).padStart(5) +
      '  ' +
      hex(section.crc32),
  );
}

if (!quietIndex && pkg.header.packageType === qpk.PackageType.Quran) {
  console.log('\nQURAN INDEX (sample)');
  const surahCount = pkg.recordCount(qpk.SectionId.SurahIndex);
  for (let id = 1; id <= Math.min(surahCount, 5); id++) {
    const s = pkg.getSurah(id);
    const name = s.nameLength ? pkg.text(s.nameOffset, s.nameLength) : '';
    console.log(
      `  surah ${String(id).padStart(3)}  ayahs ${String(s.ayahCount).padStart(4)}` +
        `  pages ${s.firstPage}-${s.lastPage}  ${name}`,
    );
  }
  if (surahCount > 5) console.log(`  ... ${surahCount - 5} more`);

  const first = pkg.getAyahByIndex(0);
  console.log(
    `\n  first ayah  ${first.surahId}:${first.ayahNumber}  page ${first.page}` +
      `  words ${first.wordCount}  "${pkg.text(first.textOffset, first.textLength)}"`,
  );
  if (pkg.section(qpk.SectionId.WordIndex)) {
    const w = pkg.getWord(first.surahId, first.ayahNumber, 0);
    console.log(
      `  first word  x=${w.x} y=${w.y} ${w.width}x${w.height} line ${w.lineId}` +
        `  "${pkg.text(w.textOffset, w.textLength)}"`,
    );
  }
  if (pkg.section(qpk.SectionId.LayoutData)) {
    const layout = pkg.getLayoutHeader();
    console.log(
      `  layout      ${layout.pageWidth}x${layout.pageHeight}` +
        `  ${layout.lineCountTotal} line(s)`,
    );
  }
}

console.log('\nVALID     all structural rules, checksums and index bounds pass');

function flagNames(flags) {
  const names = [];
  if (flags & qpk.FLAG_HAS_WORD_LAYOUT) names.push('word-layout');
  if (flags & qpk.FLAG_RIGHT_TO_LEFT) names.push('rtl');
  if (flags & qpk.FLAG_IS_DELTA) names.push('delta');
  return names.length ? `  (${names.join(', ')})` : '';
}
