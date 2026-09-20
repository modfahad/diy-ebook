// setup_screen.h -- which way up the picture is, and which way round touch is.
//
// The touch orientation cannot be known until a finger lands on real glass:
// a laminated touch layer can report its axes swapped, mirrored, or both, and
// the datasheet does not say which. src/touch_test.cpp answers it with a
// separate build; this screen answers it on the device, so the answer costs
// eight presses instead of eight reflashes -- and it is where the picture
// gets turned round when the board sits upside down in its case.
//
// Two targets, not one. A single corner cannot tell a swapped pair of axes
// from a straight one: (0,0) maps to (0,0) either way. Target 2 sits in the
// opposite top corner, which swapping sends to the bottom of the screen, so
// hitting both means the orientation is genuinely right.
//
// Pure render, like every other ui:: screen: state in, canvas out.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"

namespace ui {

// Target boxes, in display pixels. Generous: this screen is used precisely
// when taps are landing in the wrong place.
constexpr int kSetupTargetSize = 96;
constexpr int kSetupTarget1X = 40;
constexpr int kSetupTarget1Y = 120;
constexpr int kSetupTarget2X = 800 - 40 - kSetupTargetSize;
constexpr int kSetupTarget2Y = 120;

struct SetupState {
  uint8_t rotation = 0;           // 0 or 2, quarter turns
  uint8_t touch_orientation = 0;  // bit 0 swap, bit 1 mirror x, bit 2 mirror y
  bool touch_present = false;     // the GT911 answered at boot

  // Where the last tap was understood to be, and whether each target has been
  // hit since the orientation last changed.
  bool has_tap = false;
  int16_t tap_x = 0;
  int16_t tap_y = 0;
  bool target1_hit = false;
  bool target2_hit = false;
  uint32_t tap_count = 0;

  bool saved = false;             // the last EXIT wrote the card
};

class SetupScreen {
 public:
  static void render(gfx::Canvas& canvas, const SetupState& state);

  // 1 or 2 for a tap inside that target, 0 for anywhere else.
  static uint8_t targetAt(int16_t x, int16_t y);

  // Records a tap: marks the target it hit, if any.
  static void noteTap(SetupState* state, int16_t x, int16_t y);

  // Steps to the next of the eight touch orientations, forgetting which
  // targets were hit -- they were hit under the old one.
  static void nextTouchOrientation(SetupState* state);

  static void flipPicture(SetupState* state);

  // True when both targets have been hit under the current orientation, which
  // is the whole point of the screen.
  static bool bothTargetsHit(const SetupState& state);
};

}  // namespace ui
