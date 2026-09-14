// reader_screen.h -- a minimal book reader.
//
// This is the first screen that shows a *text* rather than a diagnostic, and
// it is deliberately the smallest possible version of what Milestone 5's
// renderer becomes: a title, wrapped body lines, and a page indicator, with
// the wrapping and pagination done by util::TextPager against text that stays
// where it is (flash, or eventually a QPK page cache) rather than being copied
// into RAM.
//
// Pure render: state in, canvas out, no hardware. Same pattern as
// SelfTestScreen.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "util/text_pager.h"

namespace ui {

// The 412 px body of the 800x480 panel holds 15 lines at x3 (26 px pitch);
// 16 leaves one line of headroom. Smaller scales are capped here.
constexpr uint8_t kReaderMaxLines = 16;

struct ReaderState {
  const char* title = "";
  /** The text the slices index into. */
  const char* body = "";

  const util::LineSlice* lines = nullptr;
  uint8_t line_count = 0;

  uint16_t page = 1;
  uint16_t page_count = 1;

  uint8_t scale = 6;
  /** Shown in the footer so the capacity trade-off is visible, not guessed. */
  uint16_t chars_per_line = 0;
  uint8_t lines_per_page = 0;
  /** Set when there is no card and the text is built into the firmware. */
  bool from_flash = false;
};

/** Body area the pager should be sized against, in pixels. */
uint16_t ReaderBodyHeight();

/** Characters that fit on one line at `scale`, for a canvas `width` px wide. */
uint16_t ReaderCharsPerLine(uint16_t width, uint8_t scale);

/** Lines that fit in the body area at `scale`. */
uint8_t ReaderLinesPerPage(uint8_t scale);

class ReaderScreen {
 public:
  static void render(gfx::Canvas& canvas, const ReaderState& state);
};

}  // namespace ui
