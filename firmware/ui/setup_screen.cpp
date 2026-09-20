#include "ui/setup_screen.h"

#include <stdio.h>

#include "util/screen_setup.h"

namespace ui {

namespace {

constexpr uint8_t kTitleScale = 3;
constexpr uint8_t kBodyScale = 2;
constexpr uint8_t kSmallScale = 1;

constexpr int kBorderX = 4;
constexpr int kBorderY = 4;
constexpr int kBorderW = 792;
constexpr int kBorderH = 472;

constexpr int kRuleX = 24;
constexpr int kRuleW = 752;
constexpr int kHeaderRuleY = 72;
constexpr int kFooterRuleY = 446;
constexpr int kFooterTextY = 454;

constexpr int kStatusY = 260;
constexpr int kStatusStep = 26;

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale,
                  gfx::kBlack);
}

void DrawTarget(gfx::Canvas& canvas, int x, int y, const char* label, bool hit) {
  canvas.drawRect(x, y, kSetupTargetSize, kSetupTargetSize, gfx::kBlack);
  canvas.drawRect(x + 1, y + 1, kSetupTargetSize - 2, kSetupTargetSize - 2,
                  gfx::kBlack);
  if (hit) {
    // Filled: this target has been hit under the orientation now showing.
    canvas.fillRect(x + 8, y + 8, kSetupTargetSize - 16, kSetupTargetSize - 16,
                    gfx::kBlack);
  } else {
    canvas.drawHLine(x + 16, y + kSetupTargetSize / 2, kSetupTargetSize - 32,
                     gfx::kBlack);
    canvas.drawText(x + kSetupTargetSize / 2 - 6, y + kSetupTargetSize / 2 - 20,
                    label, kBodyScale, gfx::kBlack);
  }
}

// A small cross where the device thinks the finger landed.
void DrawTapMark(gfx::Canvas& canvas, int16_t x, int16_t y) {
  const int arm = 14;
  int left = x - arm;
  int right = arm * 2;
  if (left < 0) {
    right += left;
    left = 0;
  }
  if (left + right > 800) right = 800 - left;
  if (right > 0) canvas.drawHLine(left, y, right, gfx::kBlack);
  for (int dy = -arm; dy <= arm; ++dy) {
    const int py = y + dy;
    if (py >= 0 && py < 480) canvas.drawHLine(x, py, 1, gfx::kBlack);
  }
}

}  // namespace

uint8_t SetupScreen::targetAt(int16_t x, int16_t y) {
  if (x >= kSetupTarget1X && x < kSetupTarget1X + kSetupTargetSize &&
      y >= kSetupTarget1Y && y < kSetupTarget1Y + kSetupTargetSize) {
    return 1;
  }
  if (x >= kSetupTarget2X && x < kSetupTarget2X + kSetupTargetSize &&
      y >= kSetupTarget2Y && y < kSetupTarget2Y + kSetupTargetSize) {
    return 2;
  }
  return 0;
}

void SetupScreen::noteTap(SetupState* state, int16_t x, int16_t y) {
  if (state == nullptr) return;
  state->has_tap = true;
  state->tap_x = x;
  state->tap_y = y;
  ++state->tap_count;
  const uint8_t target = targetAt(x, y);
  if (target == 1) state->target1_hit = true;
  if (target == 2) state->target2_hit = true;
}

void SetupScreen::nextTouchOrientation(SetupState* state) {
  if (state == nullptr) return;
  state->touch_orientation = static_cast<uint8_t>(
      (state->touch_orientation + 1) % util::kTouchOrientationCount);
  // The hits belonged to the old orientation; under the new one they mean
  // nothing, and leaving them ticked would say the job is done when it is not.
  state->target1_hit = false;
  state->target2_hit = false;
  state->has_tap = false;
  state->saved = false;
}

void SetupScreen::flipPicture(SetupState* state) {
  if (state == nullptr) return;
  state->rotation = state->rotation == 0 ? 2 : 0;
  // Turning the picture moves every canvas position to the physical spot
  // opposite it, while the touch layer keeps reporting exactly as before --
  // so a mapping that was right before the flip is 180 degrees wrong after
  // it. The hits have to go with it, or the screen would say "this is the
  // one" about an orientation that is now upside down.
  state->target1_hit = false;
  state->target2_hit = false;
  state->has_tap = false;
  state->saved = false;
}

bool SetupScreen::bothTargetsHit(const SetupState& state) {
  return state.target1_hit && state.target2_hit;
}

void SetupScreen::render(gfx::Canvas& canvas, const SetupState& state) {
  char buf[96];

  canvas.clear(gfx::kWhite);
  canvas.drawRect(kBorderX, kBorderY, kBorderW, kBorderH, gfx::kBlack);

  CenterText(canvas, 14, "Screen and touch setup", kTitleScale);
  CenterText(canvas, 48,
             state.touch_present
                 ? "Tap box 1, then box 2. Both fill in = touch is the right way round."
                 : "No touch panel found -- the picture can still be turned.",
             kSmallScale);
  canvas.drawHLine(kRuleX, kHeaderRuleY, kRuleW, gfx::kBlack);

  DrawTarget(canvas, kSetupTarget1X, kSetupTarget1Y, "1", state.target1_hit);
  DrawTarget(canvas, kSetupTarget2X, kSetupTarget2Y, "2", state.target2_hit);
  if (state.has_tap) DrawTapMark(canvas, state.tap_x, state.tap_y);

  snprintf(buf, sizeof(buf), "Touch %u of 8:  swap %s   flip left-right %s   flip up-down %s",
           static_cast<unsigned>(state.touch_orientation + 1),
           (state.touch_orientation & 1) ? "yes" : "no ",
           (state.touch_orientation & 2) ? "yes" : "no ",
           (state.touch_orientation & 4) ? "yes" : "no ");
  CenterText(canvas, kStatusY, buf, kBodyScale);

  snprintf(buf, sizeof(buf), "Picture: %s",
           state.rotation == 0 ? "normal" : "upside down");
  CenterText(canvas, kStatusY + kStatusStep, buf, kBodyScale);

  if (state.has_tap) {
    snprintf(buf, sizeof(buf), "Last tap read as %d,%d   (%lu so far)",
             static_cast<int>(state.tap_x), static_cast<int>(state.tap_y),
             static_cast<unsigned long>(state.tap_count));
  } else if (state.touch_present) {
    snprintf(buf, sizeof(buf), "No tap seen yet");
  } else {
    snprintf(buf, sizeof(buf), "Touch is off");
  }
  CenterText(canvas, kStatusY + 2 * kStatusStep, buf, kBodyScale);

  if (bothTargetsHit(state)) {
    CenterText(canvas, kStatusY + 3 * kStatusStep,
               "Both boxes hit -- this is the one. EXIT saves it.", kBodyScale);
  } else if (state.touch_present) {
    CenterText(canvas, kStatusY + 3 * kStatusStep,
               "Wrong box filling in? OK tries the next way round.", kBodyScale);
  }

  canvas.drawHLine(kRuleX, kFooterRuleY, kRuleW, gfx::kBlack);
  CenterText(canvas, kFooterTextY,
             "OK/WHEEL=next touch way   MENU=turn picture   EXIT=save and close",
             kBodyScale);
}

}  // namespace ui
