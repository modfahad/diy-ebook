#include "ui/page_image_screen.h"

#include <stdio.h>
#include <string.h>

#include "gfx/font5x7.h"

namespace ui {

namespace {

// Drawing in page coordinates (x across the upright page, y down it), so
// overlays turn with the page instead of lying sideways on it.

void PagePixel(gfx::Canvas& canvas, const PageImageState& state, int x, int y,
               gfx::Color color) {
  if (x < 0 || y < 0 || x >= state.width || y >= state.height) return;
  int cx = 0;
  int cy = 0;
  PageImageScreen::mapPoint(canvas, state, x, y, &cx, &cy);
  canvas.setPixel(cx, cy, color);
}

void PageFill(gfx::Canvas& canvas, const PageImageState& state, int x, int y, int w,
              int h, gfx::Color color) {
  for (int row = 0; row < h; ++row) {
    for (int col = 0; col < w; ++col) PagePixel(canvas, state, x + col, y + row, color);
  }
}

void PageFrame(gfx::Canvas& canvas, const PageImageState& state, int x, int y, int w,
               int h, int thickness) {
  PageFill(canvas, state, x, y, w, thickness, gfx::kBlack);
  PageFill(canvas, state, x, y + h - thickness, w, thickness, gfx::kBlack);
  PageFill(canvas, state, x, y, thickness, h, gfx::kBlack);
  PageFill(canvas, state, x + w - thickness, y, thickness, h, gfx::kBlack);
}

void PageText(gfx::Canvas& canvas, const PageImageState& state, int x, int y,
              const char* text, int scale) {
  for (const char* p = text; *p != '\0'; ++p) {
    const uint8_t* glyph = gfx::Glyph(*p);
    for (int row = 0; row < gfx::kGlyphHeight; ++row) {
      for (int col = 0; col < gfx::kGlyphWidth; ++col) {
        if ((glyph[row] & (1u << (gfx::kGlyphWidth - 1 - col))) == 0) continue;
        PageFill(canvas, state, x + col * scale, y + row * scale, scale, scale, gfx::kBlack);
      }
    }
    x += (gfx::kGlyphWidth + 1) * scale;
  }
}

// Centres `text` across a box starting at `box_x`, `box_w` wide.
void PageTextCentred(gfx::Canvas& canvas, const PageImageState& state, int box_x,
                     int box_w, int y, const char* text, int scale) {
  const int width = gfx::Canvas::textWidth(text, static_cast<uint8_t>(scale));
  PageText(canvas, state, box_x + (box_w - width) / 2, y, text, scale);
}

void DrawPageNumber(gfx::Canvas& canvas, const PageImageState& state) {
  char text[24];
  snprintf(text, sizeof(text), "%u / %u", static_cast<unsigned>(state.page),
           static_cast<unsigned>(state.page_count));
  const int scale = 2;
  const int w = gfx::Canvas::textWidth(text, scale) + 8;
  const int h = gfx::kGlyphHeight * scale + 8;
  const int x = state.width - w - 6;
  const int y = state.height - h - 6;
  PageFill(canvas, state, x, y, w, h, gfx::kWhite);  // readable over the page
  PageFrame(canvas, state, x, y, w, h, 1);
  PageText(canvas, state, x + 4, y + 4, text, scale);
}

// A solid ribbon hanging from the page's top edge near its right corner, with
// a V cut from its foot -- the shape e-readers use for a bookmarked page.
void DrawRibbon(gfx::Canvas& canvas, const PageImageState& state) {
  constexpr int kWidth = 24;
  constexpr int kBody = 34;
  constexpr int kNotch = 12;
  const int x = state.width - kWidth - 20;
  PageFill(canvas, state, x - 2, 0, kWidth + 4, kBody + kNotch + 2, gfx::kWhite);  // a clear edge
  PageFill(canvas, state, x, 0, kWidth, kBody, gfx::kBlack);
  for (int i = 0; i < kNotch; ++i) {
    const int side = kWidth / 2 - i;
    PageFill(canvas, state, x, kBody + i, side, 1, gfx::kBlack);
    PageFill(canvas, state, x + kWidth - side, kBody + i, side, 1, gfx::kBlack);
  }
}

void DrawJumpBox(gfx::Canvas& canvas, const PageImageState& state) {
  const int w = 400;
  const int h = 200;
  const int x = (state.width - w) / 2;
  const int y = (state.height - h) / 2;
  PageFill(canvas, state, x, y, w, h, gfx::kWhite);
  PageFrame(canvas, state, x, y, w, h, 3);

  char target[24];
  snprintf(target, sizeof(target), "%u / %u", static_cast<unsigned>(state.jump_target),
           static_cast<unsigned>(state.page_count));
  PageTextCentred(canvas, state, x, w, y + 16, "Go to page", 3);
  PageTextCentred(canvas, state, x, w, y + 50, target, 4);

  // 32 characters at this size fill the box; a longer title is cut with "..".
  if (state.jump_chapter != nullptr && state.jump_chapter[0] != 0) {
    char chapter[33];
    if (strlen(state.jump_chapter) > 32) {
      snprintf(chapter, sizeof(chapter), "%.30s..", state.jump_chapter);
    } else {
      snprintf(chapter, sizeof(chapter), "%s", state.jump_chapter);
    }
    PageTextCentred(canvas, state, x, w, y + 96, chapter, 2);
  }
  PageTextCentred(canvas, state, x, w, y + 138, "WHEEL +-10  MENU next chapter", 2);
  PageTextCentred(canvas, state, x, w, y + 162, "OK go there  EXIT back", 2);
}

}  // namespace

void PageImageScreen::mapPoint(const gfx::Canvas& canvas, const PageImageState& state, int x,
                               int y, int* canvas_x, int* canvas_y) {
  // Turned a quarter, the page is `height` wide and `width` tall on the canvas.
  const int left = (static_cast<int>(canvas.width()) - state.height) / 2;
  const int top = (static_cast<int>(canvas.height()) - state.width) / 2;
  if (state.turn == PortraitTurn::kClockwise) {
    *canvas_x = left + (state.height - 1 - y);
    *canvas_y = top + x;
  } else {
    *canvas_x = left + y;
    *canvas_y = top + (state.width - 1 - x);
  }
}

bool PageImageScreen::render(gfx::Canvas& canvas, const PageImageState& state) {
  canvas.clear(gfx::kWhite);
  if (state.bits == nullptr || state.width == 0 || state.height == 0 ||
      state.height > canvas.width() || state.width > canvas.height()) {
    return false;
  }

  const int stride = (state.width + 7) / 8;
  for (int y = 0; y < state.height; ++y) {
    const uint8_t* row = state.bits + static_cast<uint32_t>(y) * stride;
    for (int byte = 0; byte < stride; ++byte) {
      const uint8_t value = row[byte];
      if (value == 0) continue;  // most of a page is white
      for (int bit = 0; bit < 8; ++bit) {
        if ((value & (0x80 >> bit)) == 0) continue;
        const int x = byte * 8 + bit;
        if (x >= state.width) break;
        int cx = 0;
        int cy = 0;
        mapPoint(canvas, state, x, y, &cx, &cy);
        canvas.setPixel(cx, cy, gfx::kBlack);
      }
    }
  }

  if (state.bookmarked) DrawRibbon(canvas, state);
  if (state.page_count > 0) DrawPageNumber(canvas, state);
  if (state.jump_mode && state.page_count > 0) DrawJumpBox(canvas, state);
  return true;
}

}  // namespace ui
