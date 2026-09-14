#include "gfx/canvas.h"

#include <string.h>

#include "gfx/font5x7.h"

namespace gfx {

namespace {
constexpr int kSeamColumns = 8;  // dead columns between the two controllers
}

Canvas::Canvas(uint8_t* buffer, uint16_t width, uint16_t height,
               uint16_t width_memory, uint16_t height_memory,
               uint16_t stride_bytes, uint16_t rotation, uint16_t seam_x)
    : buffer_(buffer),
      width_(width),
      height_(height),
      width_memory_(width_memory),
      height_memory_(height_memory),
      stride_bytes_(stride_bytes),
      rotation_(rotation == 180 ? 180 : 0),
      seam_x_(seam_x) {}

bool Canvas::mapPixel(int x, int y, uint32_t* byte_index,
                      uint8_t* bit_mask) const {
  if (buffer_ == nullptr) return false;
  if (x < 0 || y < 0 || x >= static_cast<int>(width_) ||
      y >= static_cast<int>(height_)) {
    return false;
  }

  int px = x;
  if (seam_x_ != 0 && px >= static_cast<int>(seam_x_)) px += kSeamColumns;

  int mx;
  int my;
  if (rotation_ == 180) {
    mx = static_cast<int>(width_memory_) - px - 1;
    my = static_cast<int>(height_memory_) - y - 1;
  } else {
    mx = px;
    my = y;
  }
  if (mx < 0 || my < 0 || mx >= static_cast<int>(width_memory_) ||
      my >= static_cast<int>(height_memory_)) {
    return false;
  }

  *byte_index = static_cast<uint32_t>(mx / 8) +
                static_cast<uint32_t>(my) * stride_bytes_;
  *bit_mask = static_cast<uint8_t>(0x80u >> (mx & 7));
  return true;
}

void Canvas::clear(Color color) {
  if (buffer_ == nullptr) return;
  memset(buffer_, color == kWhite ? 0xFF : 0x00, bufferSize());
  clipped_pixels_ = 0;
}

void Canvas::setPixel(int x, int y, Color color) {
  uint32_t index;
  uint8_t mask;
  if (!mapPixel(x, y, &index, &mask)) {
    if (buffer_ != nullptr) ++clipped_pixels_;
    return;
  }
  if (color == kWhite) {
    buffer_[index] |= mask;   // 1 == white
  } else {
    buffer_[index] &= static_cast<uint8_t>(~mask);
  }
}

void Canvas::fillRect(int x, int y, int w, int h, Color color) {
  if (w <= 0 || h <= 0) return;
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) {
      setPixel(x + col, y + row, color);
    }
  }
}

void Canvas::drawHLine(int x, int y, int w, Color color) {
  if (w < 0) {
    x += w;
    w = -w;
  }
  for (int i = 0; i < w; ++i) setPixel(x + i, y, color);
}

void Canvas::drawVLine(int x, int y, int h, Color color) {
  if (h < 0) {
    y += h;
    h = -h;
  }
  for (int i = 0; i < h; ++i) setPixel(x, y + i, color);
}

void Canvas::drawRect(int x, int y, int w, int h, Color color) {
  if (w <= 0 || h <= 0) return;
  drawHLine(x, y, w, color);
  drawHLine(x, y + h - 1, w, color);
  drawVLine(x, y, h, color);
  drawVLine(x + w - 1, y, h, color);
}

int Canvas::drawChar(int x, int y, char c, uint8_t scale, Color color) {
  if (scale == 0) scale = 1;
  const uint8_t* glyph = Glyph(c);
  for (uint8_t row = 0; row < kGlyphHeight; ++row) {
    const uint8_t bits = glyph[row];
    for (uint8_t col = 0; col < kGlyphWidth; ++col) {
      if ((bits & (1u << (kGlyphWidth - 1 - col))) == 0) continue;
      if (scale == 1) {
        setPixel(x + col, y + row, color);
      } else {
        fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
  return (kGlyphWidth + 1) * scale;
}

int Canvas::drawText(int x, int y, const char* text, uint8_t scale,
                     Color color) {
  if (text == nullptr) return 0;
  if (scale == 0) scale = 1;
  int cursor = x;
  for (const char* p = text; *p != '\0'; ++p) {
    cursor += drawChar(cursor, y, *p, scale, color);
  }
  return cursor - x;
}

int Canvas::textWidth(const char* text, uint8_t scale) {
  if (text == nullptr) return 0;
  if (scale == 0) scale = 1;
  int n = 0;
  for (const char* p = text; *p != '\0'; ++p) ++n;
  if (n == 0) return 0;
  return n * (kGlyphWidth + 1) * scale - scale;
}

int Canvas::textHeight(uint8_t scale) {
  if (scale == 0) scale = 1;
  return kGlyphHeight * scale;
}

}  // namespace gfx
