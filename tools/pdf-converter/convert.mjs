#!/usr/bin/env node
// Headless document converter: PDF / EPUB / TXT / Quran JSON -> QPK1.
//
//   node tools/pdf-converter/convert.mjs <input> [options]
//
//   -o, --out <file>     where to write the package (default: <input>.qpk)
//       --title <text>
//       --author <text>
//       --language <tag>
//       --content-version <n>  revision number; bump it to update an installed
//                              copy in place rather than sitting beside it
//       --no-words       omit WORD_INDEX (smaller package, no highlighting)
//       --single-chapter skip structure detection
//       --preview <n>    print page n from the finished package
//       --json           machine-readable output
//       --dry-run        convert and validate but write nothing
//
// Nothing is written unless validation passes, which is the spec's
// "validate, preview, then install" order enforced at the CLI.
//
// Requires the converter to be built:
//   npm install --prefix desktop/converter
//   npm run build --prefix desktop/converter

import { readFileSync, writeFileSync } from 'node:fs';
import { basename, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const entry = join(here, '..', '..', 'desktop', 'converter', 'dist', 'src', 'index.js');

let converter;
try {
  converter = await import(`file://${entry.replace(/\\/g, '/')}`);
} catch (error) {
  console.error(
    'Could not load @quran-device/converter.\n' +
      'Build it first:\n' +
      '  npm install --prefix desktop/converter\n' +
      '  npm run build --prefix desktop/converter\n' +
      `\n(${error.message})`,
  );
  process.exit(2);
}

const argv = process.argv.slice(2);
const flag = (name) => argv.includes(name);
const value = (name, fallback) => {
  const at = argv.indexOf(name);
  return at >= 0 && at + 1 < argv.length ? argv[at + 1] : fallback;
};

// The first bare argument that is not the value of a preceding option.
const OPTIONS_TAKING_A_VALUE = new Set([
  '-o', '--out', '--title', '--author', '--language', '--preview',
  '--content-version',
]);
let input;
for (let i = 0; i < argv.length; i++) {
  const arg = argv[i];
  if (arg.startsWith('-')) {
    if (OPTIONS_TAKING_A_VALUE.has(arg)) i++;
    continue;
  }
  input = arg;
  break;
}
if (!input) {
  console.error('usage: convert.mjs <input> [-o out.qpk] [--title T] [--preview N] [--json]');
  process.exit(2);
}

const asJson = flag('--json');
const dryRun = flag('--dry-run');
const outPath = value('-o', value('--out', `${input.replace(/\.[^.]+$/u, '')}.qpk`));

const data = new Uint8Array(readFileSync(input));

let result;
try {
  result = await converter.convertAndValidate(data, {
    filename: basename(input),
    ...(value('--title') ? { title: value('--title') } : {}),
    ...(value('--author') ? { author: value('--author') } : {}),
    ...(value('--language') ? { language: value('--language') } : {}),
    ...(value('--content-version')
      ? { contentVersion: Number(value('--content-version')) }
      : {}),
    includeWordLayout: !flag('--no-words'),
    singleChapter: flag('--single-chapter'),
  });
} catch (error) {
  if (asJson) console.log(JSON.stringify({ ok: false, error: error.message }, null, 2));
  else console.error(`CONVERSION FAILED\n  ${error.message}`);
  process.exit(1);
}

const { report, validation, bytes } = result;

// The gate comes first: previewPage() parses the package, so previewing a
// package that fails validation would throw instead of reporting which rule
// broke.
if (!validation.ok) {
  if (asJson) {
    console.log(JSON.stringify({ ok: false, report, validation }, null, 2));
  } else {
    console.error('VALIDATION FAILED -- nothing was written');
    for (const error of validation.errors) console.error(`  ${error}`);
  }
  process.exit(1);
}

const previewPageNumber = value('--preview');
const preview = previewPageNumber
  ? converter.previewPage(bytes, Number(previewPageNumber))
  : undefined;

if (!dryRun) writeFileSync(outPath, bytes);

if (asJson) {
  console.log(
    JSON.stringify({ ok: true, out: dryRun ? null : outPath, report, validation, preview }, null, 2),
  );
  process.exit(0);
}

console.log(`INPUT     ${input}`);
console.log(`TYPE      ${report.contentType}`);
console.log(`OUTPUT    ${dryRun ? '(dry run, nothing written)' : outPath}`);
console.log(`SIZE      ${report.packageSize} bytes`);
console.log(`CONTENT   ${validation.summary.contentId}  v${validation.summary.contentVersion}`);
console.log(`PAGES     ${report.pageCount}`);
console.log(`CHAPTERS  ${report.chapterCount}${report.sectionCount ? ` (+${report.sectionCount} sections)` : ''}`);
console.log(`WORDS     ${report.wordCount}`);
if (report.layoutSpace.width) {
  console.log(`LAYOUT    ${report.layoutSpace.width} x ${report.layoutSpace.height}`);
}
if (report.wordCount > 0) {
  console.log(`ESTIMATED ${(report.estimatedWordFraction * 100).toFixed(1)}% of word boxes`);
}

const notes = [...report.warnings, ...validation.warnings];
if (notes.length > 0) {
  console.log('\nWARNINGS');
  for (const note of notes) console.log(`  - ${note}`);
}

if (preview) {
  console.log(`\nPREVIEW page ${preview.pageNumber} of ${preview.pageCount}`);
  if (preview.heading) console.log(`  [${preview.heading}]`);
  for (const line of preview.text.split('\n')) console.log(`  ${line}`);
}

console.log('\nVALID     ready to install');
