// page_image_screen.h -- a book page shown as the picture of the printed page.
//
// Books converted with "keep the PDF's page layout" carry one 480x800 1bpp
// picture per page (docs/qpk-format.md 9d), so columns, tables and figures
// survive exactly -- the way an e-reader shows a PDF. The reader is portrait:
// the device is held upright, and the 480-wide page is turned a quarter turn
// onto the 800x480 landscape canvas.
//
// Pure render, like every ui:: screen: a decoded page in, canvas out.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"

namespace ui {

enum class PortraitTurn : uint8_t {
  kClockwise,         // the page's top edge lands on the canvas's right edge
  kCounterClockwise,  // the page's top edge lands on the canvas's left edge
};

// "Go to page" moves this many pages per wheel detent.
constexpr uint32_t kPageJumpStep = 10;

struct PageImageState {
  // ceil(width / 8) * height bytes, row-major, MSB first, 1 = black.
  const uint8_t* bits = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  PortraitTurn turn = PortraitTurn::kClockwise;

  // "12 / 913" in a small box in the page's bottom-right corner, drawn
  // upright with the page. Nothing is drawn while page_count is 0.
  uint32_t page = 0;  // 1-based
  uint32_t page_count = 0;

  // "Go to page": a box in the middle of the page with the target page and
  // the controls, drawn over whatever page is on screen.
  bool jump_mode = false;
  uint32_t jump_target = 0;  // 1-based
  // The chapter holding jump_target, or "" when the book has none.
  const char* jump_chapter = "";

  // This page is bookmarked: a ribbon in the page's top-right corner.
  bool bookmarked = false;
};

class PageImageScreen {
 public:
  // Clears the canvas and draws the page turned and centred. Returns false,
  // leaving the canvas blank, when there is no page or it does not fit.
  static bool render(gfx::Canvas& canvas, const PageImageState& state);

  // Where page pixel (x, y) lands on the canvas.
  static void mapPoint(const gfx::Canvas& canvas, const PageImageState& state, int x, int y,
                       int* canvas_x, int* canvas_y);
};

}  // namespace ui
