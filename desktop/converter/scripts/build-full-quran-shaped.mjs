// Assembles the complete Quran as a SHAPED package: the same 114 surahs, but
// with a real HarfBuzz-shaped glyph atlas instead of a plain text WORD_INDEX,
// so the device can blit it without a shaping engine.
//
// A SEPARATE package from quran-full.qpk, and it has to be. FLAG_SHAPED_TEXT_DATA
// redefines what WORD_INDEX means for the *whole* package -- its spans point
// into ASSETS (glyph-id runs) rather than TEXT_DATA -- so one package cannot
// carry both a text WORD_INDEX and a shaped one. Same reason
// al-fatihah-glyph-atlas.qpk stands apart from examples/surahs/al-fatihah.qpk.
// This follows that script's structure deliberately: copy every non-word
// section verbatim, drop the plain WORD_INDEX, add the shaped one.
//
// The alignment check below is the important part. The text package and this
// one index the same words positionally, and a one-word drift would silently
// misalign highlighting across the whole Quran while still validating -- which
// is exactly what the leading-space phantom-word bug did before it was fixed
// (see docs/quran-content.md section 10). So the counts must agree exactly,
// and a sample of word TEXT is compared, not just the totals: two different
// splits can produce the same count and different words.
import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  FLAG_SHAPED_TEXT_DATA,
  MetadataKey,
  PackageType,
  SectionId,
  readPackage,
  writePackage,
} from '@quran-device/qpk-format';

import { buildGlyphAtlasSections } from '../dist/src/pipeline/glyph-atlas.js';
import { validatePackage } from '../dist/src/pipeline/validate.js';
import { buildQuranPackage } from '../dist/src/pipeline/quran-package.js';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(here, '..', '..', '..');
const basePath = join(here, '..', 'examples', 'quran-full.qpk');
const atlasPath = join(repoRoot, 'tools', 'arabic-pager', 'data', 'all', 'glyph-atlas.json');
const sourcePath = join(repoRoot, 'tools', 'arabic-pager', 'data', 'all', 'quran-source.json');
const wordLinesPath = join(repoRoot, 'tools', 'arabic-pager', 'data', 'all', 'word-lines.json');
const outPath = join(here, '..', 'examples', 'quran-full-shaped.qpk');
const stageDir = join(repoRoot, 'sdcard-staging', 'LIBRARY', 'QURAN');

const mb = (n) => `${(n / (1024 * 1024)).toFixed(2)} MB`;

console.log('reading base package and atlas...');
const atlas = JSON.parse(readFileSync(atlasPath, 'utf-8'));
const source = JSON.parse(readFileSync(sourcePath, 'utf-8'));
const wordLines = JSON.parse(readFileSync(wordLinesPath, 'utf-8'));

// Rebuild the base rather than reading quran-full.qpk, purely to give this one
// a DIFFERENT TITLE. Both packages are type QURAN and both list in the device's
// library browser; identical titles made the two rows indistinguishable, and
// only this one can actually be drawn (the other has no glyph atlas, so the
// reader shows "no shaped glyph atlas" and nothing else). A person choosing
// between two rows needs to be able to tell them apart.
const shapedSource = {
  ...source,
  metadata: { ...source.metadata, title: 'The Holy Quran (readable)' },
};

// --- end-of-ayah markers ----------------------------------------------------
// The shaper mints one atlas glyph per ayah NUMBER (1..286), contiguously and
// last, so the device finds a marker by arithmetic instead of a per-ayah
// lookup: glyph id firstGlyphId + (n - 1). Checked here rather than trusted,
// because the whole scheme rests on that contiguity and this is the last
// place that can see both the atlas and the package.
const markers = atlas.ayahMarkers ?? null;
if (markers) {
  const last = markers.firstGlyphId + markers.count - 1;
  const ids = new Set(atlas.glyphs.map((g) => g.id));
  for (let id = markers.firstGlyphId; id <= last; id++) {
    if (!ids.has(id)) {
      throw new Error(
        `ayah-marker glyph id ${id} is missing from the atlas. The device ` +
          'resolves a marker by arithmetic, so the range must be dense.',
      );
    }
  }
  const maxAyah = Math.max(...source.ayahs.map((a) => a.ayah));
  if (markers.count < maxAyah) {
    throw new Error(
      `atlas covers markers 1..${markers.count} but the text goes to ayah ` +
        `${maxAyah}; the uncovered ayahs would fall back to the Latin "(n)".`,
    );
  }
  console.log(`ayah markers ok: ${markers.count}, glyph ids ${markers.firstGlyphId}..${last}`);
} else {
  console.log('ayah markers: none in the atlas -- the device draws the Latin "(n)" fallback');
}

// --- LAYOUT_DATA: the Madinah mushaf's own line breaks -----------------------
// The line ASSIGNMENT is the mushaf's, from quran.com's per-word line_number
// (tools/arabic-pager/build_mushaf_layout.py). The GEOMETRY is ours, measured
// from this very atlas -- the source gives no x/width, and inventing one from
// a different font's metrics would be worse than measuring the font we
// actually blit with. So: mushaf breaks, our advances.
//
// Right-to-left within each line, right-aligned to the declared page width.
// No kashida justification -- that remains open (docs/pending.md section 4)
// and is a typography problem, not a layout-data one.
const LINE_HEIGHT = 52;   // ui::kLineHeight
const SPACE = 10;         // ui::kSpaceWidth
const MUSHAF_LINES = 15;

const advanceOf = new Map(atlas.glyphs.map((g) => [g.id, g.xAdvance]));
const wordWidth = (glyphIds) =>
  glyphIds.reduce((sum, id) => sum + (advanceOf.get(id) ?? 0), 0);

// One flat pass in Quranic order -- the same order atlas.words is in, which
// the alignment check above has already proved.
const flatPositions = [];
for (const verse of wordLines.verses) {
  for (const word of verse.words) flatPositions.push(word);
}
if (flatPositions.length !== atlas.words.length) {
  throw new Error(
    `word-lines.json has ${flatPositions.length} words, the atlas has ` +
      `${atlas.words.length}. These index the same words positionally.`,
  );
}

// Two passes: measure every line's total width first, because a word's x
// depends on where its line starts, and a line is only right-aligned once its
// full width is known.
const lineWidth = new Map();
const lineItems = new Map();
flatPositions.forEach((position, index) => {
  const lineKey = `${position.page}:${position.line}`;
  const width = wordWidth(atlas.words[index].glyphIds);
  const items = lineItems.get(lineKey) ?? [];
  items.push({ index, width });
  lineItems.set(lineKey, items);
  lineWidth.set(lineKey, (lineWidth.get(lineKey) ?? -SPACE) + SPACE + width);
});

const pageWidth = Math.max(...lineWidth.values());
const pageHeight = MUSHAF_LINES * LINE_HEIGHT;

const boxOfIndex = new Array(flatPositions.length);
for (const [lineKey, items] of lineItems) {
  let pen = pageWidth;  // right edge; RTL runs leftward from here
  for (const item of items) {
    boxOfIndex[item.index] = { x: pen - item.width, width: item.width };
    pen -= item.width + SPACE;
  }
}

// Back from flat order to (surah, ayah, word), which is how the converter
// keys boxes.
const layoutWords = [];
let flat = 0;
for (const ayah of source.ayahs) {
  for (let w = 0; w < ayah.words.length; w++) {
    const position = flatPositions[flat];
    const box = boxOfIndex[flat];
    layoutWords.push({
      surah: ayah.surah,
      ayah: ayah.ayah,
      word: w,
      x: box.x,
      y: (position.line - 1) * LINE_HEIGHT,
      width: box.width,
      height: LINE_HEIGHT,
      line: position.line,
      page: position.page,
    });
    flat++;
  }
}
if (flat !== flatPositions.length) {
  throw new Error(`consumed ${flat} of ${flatPositions.length} word positions`);
}

const widths = [...lineWidth.values()].sort((a, b) => a - b);
const usable = 792 - 2 * 18;   // panel width less ui::kMarginX either side
const over = widths.filter((w) => w > usable).length;
console.log(
  `layout: ${lineWidth.size} mushaf lines, page space ${pageWidth}x${pageHeight}, ` +
    `median line ${widths[widths.length >> 1]}px, ${over} lines (` +
    `${((100 * over) / widths.length).toFixed(2)}%) wider than the panel's ${usable}px ` +
    `-- those wrap on the device`,
);

console.log('rebuilding the base with a distinguishing title...');
const base = readPackage(
  buildQuranPackage(
    { ...shapedSource, layout: { pageWidth, pageHeight, words: layoutWords } },
    {
      extraMetadata: markers
        ? [[MetadataKey.AyahMarkerGlyphs, `${markers.firstGlyphId}:${markers.count}`]]
        : [],
    },
  ).bytes,
);

// --- alignment, checked before anything is built ----------------------------
const baseWordCount = base.recordCount(SectionId.WordIndex);
if (atlas.words.length !== baseWordCount) {
  throw new Error(
    `shaped word count ${atlas.words.length} != text package's ${baseWordCount}. ` +
      'These index the same words positionally; a mismatch misaligns every ' +
      'word-level feature. See docs/quran-content.md section 10.',
  );
}
if (atlas.warnings && atlas.warnings.length > 0) {
  throw new Error(`the shaper reported font coverage gaps: ${atlas.warnings.join('; ')}`);
}

// Same count is necessary but not sufficient: compare the actual word text at
// spread-out positions against the source the text package was built from.
const sourceWords = [];
for (const ayah of source.ayahs) {
  for (const word of ayah.words) sourceWords.push(word);
}
if (sourceWords.length !== atlas.words.length) {
  throw new Error(`source has ${sourceWords.length} words, atlas has ${atlas.words.length}`);
}
const probes = [0, 1, 6, 100, 5000, 40000, 82010];
for (const i of probes) {
  if (sourceWords[i] !== atlas.words[i].text) {
    throw new Error(
      `word ${i} disagrees: source "${sourceWords[i]}" vs atlas "${atlas.words[i].text}"`,
    );
  }
}
console.log(`alignment ok: ${atlas.words.length} words, ${probes.length} text probes matched`);

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

console.log('packing the atlas...');
const shaped = buildGlyphAtlasSections(glyphs, words);

// --- copy every non-word section verbatim -----------------------------------
const carriedSections = base.sections
  .filter((s) => s.id !== SectionId.WordIndex)
  .map((s) => ({
    id: s.id,
    payload: base.bytes.slice(s.offset, s.offset + s.length),
    count: s.count,
  }));

console.log('writing the shaped package...');
const bytes = writePackage({
  type: PackageType.Quran,
  flags: base.header.flags | FLAG_SHAPED_TEXT_DATA,
  // Naturally distinct from the text package's: the content id is derived
  // from title/author/language, and the title differs. No hand-forced
  // difference needed, and two distinct installable items get two ids.
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
  throw new Error(`shaped package does not validate:\n  ${validation.errors.join('\n  ')}`);
}

const hex = [...bytes.slice(24, 40)].map((b) => b.toString(16).padStart(2, '0')).join('');
writeFileSync(outPath, bytes);
writeFileSync(join(stageDir, `${hex}.qpk`), bytes);

console.log('');
console.log(`glyphs        ${shaped.glyphCount}`);
console.log(
  `ayah markers  ${markers ? `${markers.count} (ids ${markers.firstGlyphId}..${markers.firstGlyphId + markers.count - 1})` : 'none'}`,
);
console.log(`words         ${shaped.wordCount}`);
console.log(`package size  ${mb(bytes.length)}`);
console.log(`content id    ${hex}`);
console.log(`validation    ok (desktop + device profiles)`);
console.log(`wrote         ${outPath}`);
console.log(`staged        ${join(stageDir, `${hex}.qpk`)}`);
