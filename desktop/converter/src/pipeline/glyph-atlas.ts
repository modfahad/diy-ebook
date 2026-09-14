// Shaped-glyph atlas -> FONT_METADATA + ASSETS + shaped WORD_INDEX
// (docs/qpk-format.md 9a, kFlagShapedTextData).
//
// Pure data transformation: takes what tools/arabic-pager/shape_glyph_atlas.py
// already shaped and rasterized (HarfBuzz + FreeType, on the desktop -- see
// that script's header for why this never runs on-device) and packs it into
// the three wire sections a shaped QURAN package needs. No I/O here; a
// script reads the Python tool's JSON and calls this.
//
// No page/line layout is produced HERE, and that is still right: this stage
// only knows glyphs. Real geometry now exists -- the Madinah mushaf's line
// breaks, measured against this very atlas -- but it is assembled in
// scripts/build-full-quran-shaped.mjs, which is the only place that sees both
// the atlas metrics and the mushaf line assignment. WordRecord's
// x/y/width/height/lineId are filled in there, via QuranSource.layout.

import { encodeGlyph, encodeWord, type GlyphRecord, type WordRecord } from '@quran-device/qpk-format';

export interface AtlasGlyphInput {
  /** Atlas-local id, referenced by AtlasWordInput.glyphIds. Not the font's own glyph id. */
  id: number;
  width: number;
  height: number;
  xAdvance: number;
  xOffset: number;
  yOffset: number;
  /** Already 1bpp, MSB-first, each row padded to a whole byte. Empty for a zero-size glyph. */
  bitmap: Uint8Array;
}

export interface AtlasWordInput {
  /** Left-to-right visual order -- see shape_glyph_atlas.py's header comment. */
  glyphIds: number[];
}

export interface GlyphAtlasSections {
  fontMetadata: Uint8Array;
  assets: Uint8Array;
  wordIndex: Uint8Array;
  glyphCount: number;
  wordCount: number;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

export function buildGlyphAtlasSections(
  glyphs: AtlasGlyphInput[],
  words: AtlasWordInput[],
): GlyphAtlasSections {
  // ASSETS holds two regions back to back: glyph bitmaps first
  // (GlyphRecord.bitmapOffset points into this region), then each word's
  // glyph-id run packed as little-endian u16s (WordRecord.textOffset points
  // here once FLAG_SHAPED_TEXT_DATA is set -- qpk_format.h's readAsset
  // comment: decode with Read16, not memcpy, so byte order is load-bearing).
  const glyphRecords: Uint8Array[] = [];
  const bitmapBlobs: Uint8Array[] = [];
  let cursor = 0;
  const offsetByGlyphId = new Map<number, number>();
  for (const g of glyphs) {
    offsetByGlyphId.set(g.id, cursor);
    const record: GlyphRecord = {
      glyphId: g.id,
      bitmapOffset: cursor,
      width: g.width,
      height: g.height,
      xAdvance: g.xAdvance,
      xOffset: g.xOffset,
      yOffset: g.yOffset,
    };
    glyphRecords.push(encodeGlyph(record));
    bitmapBlobs.push(g.bitmap);
    cursor += g.bitmap.length;
  }
  const bitmapRegionLength = cursor;

  const wordRunBlobs: Uint8Array[] = [];
  const wordRecords: Uint8Array[] = [];
  let runCursor = bitmapRegionLength;
  for (const w of words) {
    const runBytes = new Uint8Array(w.glyphIds.length * 2);
    const view = new DataView(runBytes.buffer);
    for (let i = 0; i < w.glyphIds.length; i++) {
      const id = w.glyphIds[i]!;
      if (!offsetByGlyphId.has(id)) {
        throw new Error(`word references glyph id ${id}, which is not in the atlas`);
      }
      view.setUint16(i * 2, id, true);
    }
    const record: WordRecord = {
      textOffset: runCursor,
      textLength: runBytes.length,
      x: 0,
      y: 0,
      width: 0,
      height: 0,
      lineId: 0,
    };
    wordRecords.push(encodeWord(record));
    wordRunBlobs.push(runBytes);
    runCursor += runBytes.length;
  }

  return {
    fontMetadata: concat(glyphRecords),
    assets: concat([...bitmapBlobs, ...wordRunBlobs]),
    wordIndex: concat(wordRecords),
    glyphCount: glyphs.length,
    wordCount: words.length,
  };
}
