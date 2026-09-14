// One-off regeneration of examples/for-bushra.qpk (and the staged copy under
// sdcard-staging/), needed because rule 16 now rejects the old file: it set
// kFlagHasWordLayout without a LAYOUT_DATA section, which did not exist when
// the old file was built. Source text and metadata are unchanged.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { convertAndValidate } from '../dist/src/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, '..', '..', '..');
const srcPath = join(here, '..', 'examples', 'for-bushra.txt');
const outPath = join(here, '..', 'examples', 'for-bushra.qpk');
const stagedPath = join(
  repoRoot,
  'sdcard-staging',
  'LIBRARY',
  'BOOKS',
  '85c9fc7c213db48752d5f73e8d720380.qpk',
);

const bytes = readFileSync(srcPath);
const result = await convertAndValidate(bytes, {
  filename: 'for-bushra.txt',
  title: 'For Bushra',
  author: 'Fahad',
  language: 'en',
});

if (!result.validation.ok) {
  throw new Error(`regenerated package does not validate: ${result.validation.errors.join('; ')}`);
}

writeFileSync(outPath, result.bytes);
writeFileSync(stagedPath, result.bytes);
console.log(`wrote ${outPath} (${result.bytes.length} bytes)`);
console.log(`wrote ${stagedPath} (${result.bytes.length} bytes)`);
