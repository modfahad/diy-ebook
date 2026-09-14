// glyph_blitter.h -- draws a shaped glyph-id run onto a canvas.
//
// The consumer half of docs/qpk-format.md 9a: tools/arabic-pager and
// desktop/converter's glyph-atlas.ts already shape Arabic and pack it into
// FONT_METADATA/ASSETS/a shaped WORD_INDEX (see docs/pending.md's glyph-atlas
// entry) -- this is what walks that data back into ink on the panel. No
// shaping happens here; a glyph id is just looked up and blitted.
//
// Ink-only: a glyph bitmap's 0 bits are transparent, never drawn (see
// GlyphRecord's own documented convention), which is what lets a harakah's
// bitmap overlap its base letter's without erasing it.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "qpk/qpk_reader.h"

namespace qpk {

enum class BlitError : uint8_t {
  kOk = 0,
  kReaderError,     // getGlyph()/readAsset() failed -- a corrupt or mismatched atlas
  kBitmapTooLarge,  // exceeds kGlyphBlitterMaxBitmapBytes; see its comment
};

// A generous cap for one glyph's packed bitmap, sized well above anything
// tools/arabic-pager has produced (Al-Fatihah's atlas tops out under 200
// bytes/glyph at --px 36) so real content should never hit it -- this exists
// to fail loudly on a corrupt/mismatched package rather than overrun a
// stack buffer. 96x96px at 1bpp, row-padded: ceil(96/8)*96.
constexpr uint32_t kGlyphBlitterMaxBitmapBytes = 12 * 96;

// Sums xAdvance across `glyph_ids` without touching the canvas -- for laying
// a word out (in particular, right-aligning it for RTL) before drawing it.
// Does not validate bitmaps exist; only metrics are read.
BlitError MeasureGlyphRun(const Reader& reader, const uint16_t* glyph_ids,
                          uint32_t glyph_count, int* width_out);

// Draws `glyph_ids` (left-to-right visual order -- see
// tools/arabic-pager/shape_glyph_atlas.py's header) starting at
// (pen_x, baseline_y), advancing the pen by each glyph's xAdvance in turn.
// `*pen_x_out` receives the pen position after the last glyph, so callers
// can lay out consecutive words without re-measuring.
BlitError BlitGlyphRun(const Reader& reader, gfx::Canvas* canvas,
                       const uint16_t* glyph_ids, uint32_t glyph_count,
                       int pen_x, int baseline_y, int* pen_x_out);

}  // namespace qpk
