// Assembles a QURAN package carrying a real shaped glyph atlas -- the first
// end-to-end proof of docs/qpk-format.md 9a's FONT_METADATA/ASSETS/
// FLAG_SHAPED_TEXT_DATA path, built from real HarfBuzz+FreeType shaping
// (tools/arabic-pager/shape_glyph_atlas.py), not synthetic test data.
//
// A SEPARATE example file, not an update to examples/surahs/al-fatihah.qpk:
// that package already carries a real (unshaped) WORD_INDEX with word boxes
// pointing into TEXT_DATA, and FLAG_SHAPED_TEXT_DATA redefines what
// WORD_INDEX means for the *whole* package -- a package cannot have both an
// unshaped and a shaped WORD_INDEX at once. So this copies the base
// package's non-word sections verbatim, drops its plain WORD_INDEX, and adds
// the shaped one in its place. No real word-box geometry (x/y/width/height)
// exists for the glyph runs yet -- placeholder zeros, same discipline the
// base package's own Quran pipeline already uses elsewhere for pagination
// that has no real layout source (see docs/pending.md). This package is not
// meant to replace the one on the card; it exists to prove the mechanism.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  FLAG_SHAPED_TEXT_DATA,
  PackageType,
  SectionId,
  readPackage,
  writePackage,
} from '@quran-device/qpk-format';

import { buildGlyphAtlasSections } from '../dist/src/pipeline/glyph-atlas.js';
import { validatePackage } from '../dist/src/pipeline/validate.js';

const here = dirname(fileURLToPath(import.meta.url));
const basePath = join(here, '..', 'examples', 'surahs', 'al-fatihah.qpk');
const atlasPath = join(here, '..', '..', '..', 'tools', 'arabic-pager', 'data', 'ch1-glyph-atlas.json');
const outPath = join(here, '..', 'examples', 'al-fatihah-glyph-atlas.qpk');

const base = readPackage(new Uint8Array(readFileSync(basePath)));
const atlas = JSON.parse(readFileSync(atlasPath, 'utf-8'));

// --- glyph atlas: JSON -> the shapes glyph-atlas.ts expects -----------------
const glyphs = atlas.glyphs.map((g) => ({
  id: g.id,
  width: g.width,
  height: g.height,
  xAdvance: g.xAdvance,
  xOffset: g.xOffset,
  yOffset: g.yOffset,
  bitmap: Uint8Array.from(Buffer.from(g.bitmapHex, 'hex')),
}));
const words = atlas.words.map((w) => ({ glyphIds: w.glyphIds }));
const shaped = buildGlyphAtlasSections(glyphs, words);

if (words.length !== base.recordCount(SectionId.WordIndex)) {
  console.warn(
    `note: shaped word count (${words.length}) differs from the base ` +
      `package's plain WORD_INDEX count (${base.recordCount(SectionId.WordIndex)})`,
  );
}

// --- copy every non-word section verbatim -----------------------------------
const carriedSections = base.sections
  .filter((s) => s.id !== SectionId.WordIndex)
  .map((s) => ({
    id: s.id,
    payload: base.bytes.slice(s.offset, s.offset + s.length),
    count: s.count,
  }));

const bytes = writePackage({
  type: PackageType.Quran,
  flags: base.header.flags | FLAG_SHAPED_TEXT_DATA,
  contentId: base.header.contentId,
  contentVersion: base.header.contentVersion,
  sections: [
    ...carriedSections,
    { id: SectionId.FontMetadata, payload: shaped.fontMetadata, count: shaped.glyphCount },
    { id: SectionId.Assets, payload: shaped.assets, count: 0 },
    { id: SectionId.WordIndex, payload: shaped.wordIndex, count: shaped.wordCount },
  ],
});

const validation = validatePackage(bytes);
if (!validation.ok) {
  throw new Error(`assembled package does not validate:\n  ${validation.errors.join('\n  ')}`);
}

writeFileSync(outPath, bytes);
console.log(`glyphs        ${shaped.glyphCount}`);
console.log(`words         ${shaped.wordCount}`);
console.log(`package size  ${bytes.length} bytes`);
console.log(`validation    ok (desktop + device profiles)`);
console.log(`wrote         ${outPath}`);
