#include "qpk/glyph_blitter.h"

namespace qpk {

BlitError MeasureGlyphRun(const Reader& reader, const uint16_t* glyph_ids,
                          uint32_t glyph_count, int* width_out) {
  int width = 0;
  for (uint32_t i = 0; i < glyph_count; ++i) {
    GlyphRecord glyph;
    if (reader.getGlyph(glyph_ids[i], &glyph) != Error::kOk) {
      return BlitError::kReaderError;
    }
    width += glyph.x_advance;
  }
  if (width_out != nullptr) *width_out = width;
  return BlitError::kOk;
}

BlitError BlitGlyphRun(const Reader& reader, gfx::Canvas* canvas,
                       const uint16_t* glyph_ids, uint32_t glyph_count,
                       int pen_x, int baseline_y, int* pen_x_out) {
  uint8_t bitmap[kGlyphBlitterMaxBitmapBytes];
  int x = pen_x;

  for (uint32_t i = 0; i < glyph_count; ++i) {
    GlyphRecord glyph;
    if (reader.getGlyph(glyph_ids[i], &glyph) != Error::kOk) {
      return BlitError::kReaderError;
    }

    // A glyph with no ink (a pure positioning mark, or one dropped at shape
    // time) still occupies a slot in the run and still advances the pen --
    // it just has nothing to blit.
    if (glyph.width > 0 && glyph.height > 0) {
      const uint32_t stride = (static_cast<uint32_t>(glyph.width) + 7) / 8;
      const uint32_t nbytes = stride * glyph.height;
      if (nbytes > kGlyphBlitterMaxBitmapBytes) return BlitError::kBitmapTooLarge;

      const int32_t n =
          reader.readAsset(glyph.bitmap_offset, nbytes, bitmap, sizeof(bitmap));
      if (n < 0 || static_cast<uint32_t>(n) != nbytes) {
        return BlitError::kReaderError;
      }

      const int origin_x = x + glyph.x_offset;
      const int origin_y = baseline_y + glyph.y_offset;
      for (uint8_t row = 0; row < glyph.height; ++row) {
        const uint8_t* row_bytes = bitmap + row * stride;
        for (uint8_t col = 0; col < glyph.width; ++col) {
          if (row_bytes[col >> 3] & (0x80u >> (col & 7))) {
            canvas->setPixel(origin_x + col, origin_y + row, gfx::kBlack);
          }
        }
      }
    }

    x += glyph.x_advance;
  }

  if (pen_x_out != nullptr) *pen_x_out = x;
  return BlitError::kOk;
}

}  // namespace qpk
