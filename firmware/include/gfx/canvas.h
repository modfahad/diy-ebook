// canvas.h -- 1bpp drawing surface.
//
// Deliberately free of Arduino/ESP-IDF dependencies so it can be unit-tested
// on the host and reused unchanged by the Milestone 5 QuranRenderer /
// BookRenderer.
//
// Memory format (the same convention GxEPD2 uses for its own buffer, which is
// why drivers::Epd750Display can hand it over with drawInvertedBitmap()):
//   * one bit per pixel, MSB = leftmost pixel of the byte
//   * bit value 1 = WHITE, 0 = BLACK
//   * rows are `stride_bytes` apart
//
// Two memory-layout options are handled here rather than at every call site:
//   * rotation (0 or 180)
//   * a seam: a panel built from two cascaded controllers (the CrowPanel's
//     original 5.79" panel) has dead columns in controller RAM. `seam_x` is
//     the logical x at which drawing skips 8 physical columns; pass 0 to
//     disable. The GDEY075T7 has one controller and passes 0.

#pragma once

#include <stdint.h>

namespace gfx {

enum Color : uint8_t {
  kWhite = 0,
  kBlack = 1,
};

class Canvas {
 public:
  Canvas() = default;

  // `buffer` must be at least stride_bytes * height_memory bytes.
  Canvas(uint8_t* buffer, uint16_t width, uint16_t height,
         uint16_t width_memory, uint16_t height_memory, uint16_t stride_bytes,
         uint16_t rotation, uint16_t seam_x);

  bool valid() const { return buffer_ != nullptr; }

  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }
  uint8_t* buffer() const { return buffer_; }
  uint32_t bufferSize() const {
    return static_cast<uint32_t>(stride_bytes_) * height_memory_;
  }

  // Also resets clippedPixels(): every screen starts its render with clear().
  void clear(Color color);
  void setPixel(int x, int y, Color color);

  // Pixels dropped since the last clear() because they fell outside
  // width() x height(). A screen whose layout fits the panel drops none, so
  // this is what the host tests assert on and what the repaint log reports --
  // text running off the edge is otherwise silent.
  uint32_t clippedPixels() const { return clipped_pixels_; }

  void fillRect(int x, int y, int w, int h, Color color);
  void drawRect(int x, int y, int w, int h, Color color);
  void drawHLine(int x, int y, int w, Color color);
  void drawVLine(int x, int y, int h, Color color);

  // Text. `scale` >= 1 replicates each font pixel scale x scale.
  // Advance per glyph is (kGlyphWidth + 1) * scale. `y` is the TOP of the
  // glyph cell, not a baseline.
  int drawChar(int x, int y, char c, uint8_t scale, Color color);
  int drawText(int x, int y, const char* text, uint8_t scale, Color color);

  static int textWidth(const char* text, uint8_t scale);
  static int textHeight(uint8_t scale);

 private:
  // Maps a logical pixel to a physical bit; returns false if off-panel.
  bool mapPixel(int x, int y, uint32_t* byte_index, uint8_t* bit_mask) const;

  uint8_t* buffer_ = nullptr;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  uint16_t width_memory_ = 0;
  uint16_t height_memory_ = 0;
  uint16_t stride_bytes_ = 0;
  uint16_t rotation_ = 0;
  uint16_t seam_x_ = 0;
  uint32_t clipped_pixels_ = 0;
};

}  // namespace gfx
