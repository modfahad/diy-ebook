#include "ui/selftest_screen.h"

#include <stdio.h>
#include <string.h>

#include "app/app_config.h"
#include "gfx/font5x7.h"

namespace ui {

namespace {

constexpr uint8_t kTitleScale = 3;
constexpr uint8_t kBodyScale = 2;

// Layout for the 800x480 panel. Text is the 5x7 face: (5 + 1) * scale px per
// character and 7 * scale px tall, with `y` at the top of the glyph cell.
constexpr int kBorderX = 4;
constexpr int kBorderY = 4;
constexpr int kBorderW = 792;
constexpr int kBorderH = 472;

constexpr int kRuleX = 24;
constexpr int kRuleW = 752;           // 24..775
constexpr int kHeaderRuleY = 72;
constexpr int kFooterRuleY = 446;
constexpr int kFooterTextY = 454;

constexpr int kLeftLabelX = 30;
constexpr int kLeftValueX = 170;
constexpr int kRightLabelX = 410;
// The "Card:" value is at most 19 characters (sd_storage.cpp's mount-failure
// strings): 19 * 12 - 2 = 226 px, so it ends at 765, inside the 775 rule.
constexpr int kRightValueX = 540;
constexpr int kRowTop = 84;
constexpr int kRowStep = 40;          // 7 rows: the last ends at y = 84 + 240 + 14
// Centred lines (subtitle, footer) are kept inside the rules.
constexpr int kMaxLineWidth = kRuleW;

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale,
                  gfx::kBlack);
}

// Shortens `text` in place, ending it in "..", until it is at most
// `max_width_px` wide at `scale`. The face is fixed-width, so this is exact.
void FitToWidth(char* text, int max_width_px, uint8_t scale) {
  if (gfx::Canvas::textWidth(text, scale) <= max_width_px) return;
  const int advance = (gfx::kGlyphWidth + 1) * scale;
  // n characters are n * advance - scale wide.
  int keep = (max_width_px + scale) / advance - 2;
  if (keep < 0) keep = 0;
  if (static_cast<size_t>(keep) > strlen(text)) return;
  text[keep] = '.';
  text[keep + 1] = '.';
  text[keep + 2] = '\0';
}

// Values are fitted to end at the rule (x = 775): real values are short, but
// a counter or detail string has no hard upper bound, and running into the
// border would be worse than a trailing "..".
void Row(gfx::Canvas& canvas, int row, int label_x, int value_x,
         const char* label, const char* value) {
  const int y = kRowTop + row * kRowStep;
  canvas.drawText(label_x, y, label, kBodyScale, gfx::kBlack);
  char fitted[64];
  snprintf(fitted, sizeof(fitted), "%s", value);
  FitToWidth(fitted, kRuleX + kRuleW - value_x, kBodyScale);
  canvas.drawText(value_x, y, fitted, kBodyScale, gfx::kBlack);
}

}  // namespace

const char* SelfTestScreen::resultText(TestResult result) {
  switch (result) {
    case TestResult::kPass: return "OK";
    case TestResult::kFail: return "FAIL";
    default:                return "--";
  }
}

void SelfTestScreen::render(gfx::Canvas& canvas, const SelfTestState& s) {
  char buf[64];

  canvas.clear(gfx::kWhite);
  canvas.drawRect(kBorderX, kBorderY, kBorderW, kBorderH, gfx::kBlack);

  CenterText(canvas, 14, app::kProductName, kTitleScale);
  // The subtitle gives way to the library when there is one: with no reader
  // yet, this is where an installed package shows itself. A title can be up
  // to net::kTitleMaxBytes long, far wider than the panel, so it is fitted.
  if (s.library_count > 0 && s.library_title[0] != '\0') {
    if (s.library_count > 1) {
      char more[24];
      snprintf(more, sizeof(more), "  (+%u more)",
               static_cast<unsigned>(s.library_count - 1));
      snprintf(buf, sizeof(buf), "%s", s.library_title);
      FitToWidth(buf, kMaxLineWidth - gfx::Canvas::textWidth(more, kBodyScale) -
                          kBodyScale,
                 kBodyScale);
      strncat(buf, more, sizeof(buf) - strlen(buf) - 1);
    } else {
      snprintf(buf, sizeof(buf), "%s", s.library_title);
      FitToWidth(buf, kMaxLineWidth, kBodyScale);
    }
    CenterText(canvas, 48, buf, kBodyScale);
  } else {
    CenterText(canvas, 48, "Hardware Test", kBodyScale);
  }
  canvas.drawHLine(kRuleX, kHeaderRuleY, kRuleW, gfx::kBlack);

  // --- left column: the pass/fail checklist ------------------------------
  Row(canvas, 0, kLeftLabelX, kLeftValueX, "Rotary:", resultText(s.rotary));
  Row(canvas, 1, kLeftLabelX, kLeftValueX, "MENU:", resultText(s.menu));
  Row(canvas, 2, kLeftLabelX, kLeftValueX, "EXIT:", resultText(s.exit));
  Row(canvas, 3, kLeftLabelX, kLeftValueX, "OK btn:",
      resultText(s.encoder_switch));
  Row(canvas, 4, kLeftLabelX, kLeftValueX, "SD:", resultText(s.sd));
  Row(canvas, 5, kLeftLabelX, kLeftValueX, "E-Ink:", resultText(s.eink));
  Row(canvas, 6, kLeftLabelX, kLeftValueX, "PSRAM:", resultText(s.psram));

  // --- right column: the diagnostics that resolve the open TODOs ---------
  Row(canvas, 0, kRightLabelX, kRightValueX, "Wake:", s.wake_reason_name);

  snprintf(buf, sizeof(buf), "%lu",
           static_cast<unsigned long>(s.real_wake_count));
  Row(canvas, 1, kRightLabelX, kRightValueX, "Boots:", buf);

  if (s.sd == TestResult::kPass) {
    snprintf(buf, sizeof(buf), "%lu MB",
             static_cast<unsigned long>(s.sd_capacity_mb));
  } else {
    snprintf(buf, sizeof(buf), "%s", s.sd_detail);
  }
  Row(canvas, 2, kRightLabelX, kRightValueX, "Card:", buf);

  snprintf(buf, sizeof(buf), "A=%d B=%d", s.input.encoder_a_level ? 1 : 0,
           s.input.encoder_b_level ? 1 : 0);
  Row(canvas, 3, kRightLabelX, kRightValueX, "Enc pins:", buf);

  snprintf(buf, sizeof(buf), "%ld", static_cast<long>(s.input.pulse_position));
  Row(canvas, 4, kRightLabelX, kRightValueX, "Pulse:", buf);

  snprintf(buf, sizeof(buf), "%ld  (r%lu)", static_cast<long>(s.ui_counter),
           static_cast<unsigned long>(s.refresh_count));
  Row(canvas, 5, kRightLabelX, kRightValueX, "Counter:", buf);

  if (s.battery_available) {
    snprintf(buf, sizeof(buf), "%u%%  %s", s.battery_percent, s.battery_remaining);
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  Row(canvas, 6, kRightLabelX, kRightValueX, "Batt:", buf);

  // --- footer -------------------------------------------------------------
  // Transfer mode (Milestone 4, toggled by a long-press on the encoder
  // switch) and BLE pairing borrow the footer line rather than adding rows,
  // so the checklist keeps the same shape in every state.
  canvas.drawHLine(kRuleX, kFooterRuleY, kRuleW, gfx::kBlack);
  if (s.pairing_mode) {
    CenterText(canvas, kFooterTextY, "Pairing mode: connect via Bluetooth",
              kBodyScale);
  } else if (s.transfer_connecting) {
    CenterText(canvas, kFooterTextY, "Connecting to Wi-Fi...  hold OK to cancel",
              kBodyScale);
  } else if (s.transfer_mode) {
    snprintf(buf, sizeof(buf), "Transfer mode: %s  hold OK to stop",
             s.transfer_address);
    CenterText(canvas, kFooterTextY, buf, kBodyScale);
  } else {
    snprintf(buf, sizeof(buf), "MENU=refresh EXIT=sleep hold OK=transfer fw %s",
             s.firmware_version);
    CenterText(canvas, kFooterTextY, buf, kBodyScale);
  }
}

}  // namespace ui
