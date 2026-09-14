#!/usr/bin/env node
// Validates either a Quran import source (JSON, the schema in
// desktop/converter/src/quran/schema.ts) or a finished QPK package.
//
//   node tools/quran-validator/validate.mjs <source.json | package.qpk> [--json]
//
// For a package it runs BOTH profiles: the strict desktop one (every checksum,
// full index sweep) and the device's, so a package that would install and then
// be rejected on the device is caught here instead of on the device.
//
// Requires the converter to be built:
//   npm install --prefix desktop/converter
//   npm run build --prefix desktop/converter

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
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
const asJson = argv.includes('--json');
const path = argv.find((a) => !a.startsWith('--'));
if (!path) {
  console.error('usage: validate.mjs <source.json | package.qpk> [--json]');
  process.exit(2);
}

const data = new Uint8Array(readFileSync(path));
const kind = converter.detectInputKind(data, path);

if (kind === 'quran-json') {
  const source = JSON.parse(new TextDecoder().decode(data));
  const result = converter.validateQuranSource(source);
  report('Quran source', path, result.errors, result.warnings, {
    surahs: source.surahs?.length ?? 0,
    ayahs: source.ayahs?.length ?? 0,
    pages: source.ayahs?.length ? Math.max(...source.ayahs.map((a) => a.page)) : 0,
    wordBoxes: source.layout?.words?.length ?? 0,
  });
} else {
  const result = converter.validatePackage(data);
  report('package', path, result.errors, result.warnings, result.summary);
}

function report(what, file, errors, warnings, details) {
  if (asJson) {
    console.log(JSON.stringify({ what, file, ok: errors.length === 0, errors, warnings, details }, null, 2));
    process.exit(errors.length === 0 ? 0 : 1);
  }

  console.log(`FILE      ${file}`);
  console.log(`KIND      ${what}`);
  if (details) {
    for (const [key, value] of Object.entries(details)) {
      if (value === undefined || value === null) continue;
      if (typeof value === 'object') continue;
      console.log(`  ${key.padEnd(16)} ${value}`);
    }
  }

  if (warnings.length > 0) {
    console.log('\nWARNINGS');
    for (const warning of warnings) console.log(`  - ${warning}`);
  }

  if (errors.length > 0) {
    console.log('\nERRORS');
    for (const error of errors) console.log(`  - ${error}`);
    console.log(`\nINVALID   ${errors.length} problem(s)`);
    process.exit(1);
  }

  console.log('\nVALID');
  process.exit(0);
}
