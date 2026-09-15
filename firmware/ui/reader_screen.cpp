#include "ui/reader_screen.h"

#include <stdio.h>
#include <string.h>

#include "gfx/font5x7.h"

namespace ui {
namespace {

// Layout for the 800x480 panel.
constexpr int kMarginX = 14;
constexpr int kHeaderY = 6;
constexpr int kHeaderRuleY = 26;
constexpr int kFooterRuleY = 454;
constexpr int kFooterY = 462;         // 7 px tall at kChromeScale -> 468
constexpr uint8_t kChromeScale = 1;

constexpr int kBodyTop = kHeaderRuleY + 10;
constexpr int kBodyBottom = kFooterRuleY - 6;

// Leading: a line box is the glyph plus a little air, or the text sits so
// tight that the descenders of one line touch the caps of the next.
constexpr uint8_t kLeadingNumerator = 5;
constexpr uint8_t kLeadingDenominator = 4;

}  // namespace

uint16_t ReaderBodyHeight() {
  return static_cast<uint16_t>(kBodyBottom - kBodyTop);
}

uint16_t ReaderCharsPerLine(uint16_t width, uint8_t scale) {
  if (scale == 0) scale = 1;
  // Canvas::drawText advances (kGlyphWidth + 1) * scale per glyph. Dividing by
  // the glyph width instead would over-count by one gap per character and
  // promise more text than actually fits.
  const int advance = (gfx::kGlyphWidth + 1) * scale;
  const int drawable = static_cast<int>(width) - 2 * kMarginX;
  if (drawable <= 0) return 0;
  return static_cast<uint16_t>(drawable / advance);
}

uint8_t ReaderLinesPerPage(uint8_t scale) {
  if (scale == 0) scale = 1;
  const int pitch =
      (gfx::kGlyphHeight * scale * kLeadingNumerator) / kLeadingDenominator;
  if (pitch <= 0) return 1;
  int lines = ReaderBodyHeight() / pitch;
  if (lines < 1) lines = 1;
  if (lines > kReaderMaxLines) lines = kReaderMaxLines;
  return static_cast<uint8_t>(lines);
}

void ReaderScreen::render(gfx::Canvas& canvas, const ReaderState& s) {
  char buf[96];

  canvas.clear(gfx::kWhite);

  // --- header ---------------------------------------------------------------
  canvas.drawText(kMarginX, kHeaderY, s.title, kChromeScale, gfx::kBlack);

  snprintf(buf, sizeof(buf), "page %u / %u", static_cast<unsigned>(s.page),
           static_cast<unsigned>(s.page_count));
  const int page_w = gfx::Canvas::textWidth(buf, kChromeScale);
  canvas.drawText(static_cast<int>(canvas.width()) - kMarginX - page_w,
                  kHeaderY, buf, kChromeScale, gfx::kBlack);
  canvas.drawHLine(kMarginX, kHeaderRuleY,
                   static_cast<int>(canvas.width()) - 2 * kMarginX,
                   gfx::kBlack);

  // --- body -----------------------------------------------------------------
  const int pitch =
      (gfx::kGlyphHeight * s.scale * kLeadingNumerator) / kLeadingDenominator;
  char line[128];
  for (uint8_t i = 0; i < s.line_count && i < kReaderMaxLines; ++i) {
    const util::LineSlice& slice = s.lines[i];
    if (slice.length == 0) continue;  // a paragraph break: leave the air

    uint16_t n = slice.length;
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, s.body + slice.offset, n);
    line[n] = '\0';
    canvas.drawText(kMarginX, kBodyTop + i * pitch, line, s.scale, gfx::kBlack);
  }

  // --- footer ---------------------------------------------------------------
  canvas.drawHLine(kMarginX, kFooterRuleY,
                   static_cast<int>(canvas.width()) - 2 * kMarginX,
                   gfx::kBlack);
  snprintf(buf, sizeof(buf), "%s  x%u  %u chars/line  %u lines/page  %s",
           s.from_flash ? "in firmware" : "from card",
           static_cast<unsigned>(s.scale),
           static_cast<unsigned>(s.chars_per_line),
           static_cast<unsigned>(s.lines_per_page),
           s.hint != nullptr ? s.hint : "turn=page  OK=size  EXIT=back");
  canvas.drawText(kMarginX, kFooterY, buf, kChromeScale, gfx::kBlack);
}

}  // namespace ui
