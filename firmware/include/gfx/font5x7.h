// font5x7.h -- built-in 5x7 bitmap font covering ASCII 0x20..0x7E.
//
// Rows, not columns: each glyph is 7 bytes, one per row, top row first, and
// the low 5 bits of each byte are the pixels with bit4 = leftmost. This makes
// the table in font5x7.cpp readable as ASCII art in binary literals, which is
// the only practical way to keep a hand-authored font reviewable.
//
// Anything outside the range renders as a hollow box.

#pragma once

#include <stdint.h>

namespace gfx {

constexpr uint8_t kGlyphWidth = 5;
constexpr uint8_t kGlyphHeight = 7;
constexpr char kFirstGlyph = 0x20;
constexpr char kLastGlyph = 0x7E;

// Returns a pointer to kGlyphHeight row bytes. Never null.
const uint8_t* Glyph(char c);

}  // namespace gfx
