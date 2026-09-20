// touch.h -- pure, host-testable touch coordinate mapping and tap/long-press
// detection. No Arduino, no I2C, no globals: the GT911 itself lives in
// drivers/touch_gt911.cpp, and everything decided about what a finger MEANT
// is decided here, where the host tests can reach it.
//
// The shape deliberately mirrors util::Button (debouncer.h): one update()
// per poll, returning at most one event, unsigned time arithmetic throughout
// so a millis() wraparound behaves.

#pragma once

#include <stdint.h>

namespace util {

// How the touch layer's own coordinate system relates to the display's.
// A touch panel laminated onto a panel can report its axes swapped or
// mirrored, and which one it is cannot be known until a finger lands on real
// glass -- board::kTouchSwapXY and friends carry the answer, and
// src/touch_test.cpp is how they get filled in.
struct TouchMapping {
  uint16_t raw_width = 0;    // the chip's configured resolution
  uint16_t raw_height = 0;
  uint16_t out_width = 0;    // the display's, in the orientation the UI uses
  uint16_t out_height = 0;
  bool swap_xy = false;      // applied first
  bool invert_x = false;     // then mirrored
  bool invert_y = false;
};

struct TouchXY {
  int16_t x = 0;
  int16_t y = 0;
};

// Maps one raw touch point onto the display. Out-of-range input is clamped
// rather than rejected: a finger on the bezel edge reporting one pixel past
// the panel is a real thing that should still hit the row it is on, and a
// zero or absurd resolution (an unconfigured chip reports 0) must not divide
// by zero or wrap.
inline TouchXY MapTouchPoint(uint16_t raw_x, uint16_t raw_y,
                             const TouchMapping& m) {
  uint32_t a = raw_x, b = raw_y;
  uint32_t range_a = m.raw_width, range_b = m.raw_height;
  if (m.swap_xy) {
    a = raw_y;
    b = raw_x;
    range_a = m.raw_height;
    range_b = m.raw_width;
  }
  if (range_a == 0) range_a = m.out_width;
  if (range_b == 0) range_b = m.out_height;
  if (range_a == 0) range_a = 1;
  if (range_b == 0) range_b = 1;
  if (a >= range_a) a = range_a - 1;
  if (b >= range_b) b = range_b - 1;

  int32_t x = static_cast<int32_t>(a * m.out_width / range_a);
  int32_t y = static_cast<int32_t>(b * m.out_height / range_b);
  if (m.invert_x) x = static_cast<int32_t>(m.out_width) - 1 - x;
  if (m.invert_y) y = static_cast<int32_t>(m.out_height) - 1 - y;

  TouchXY out;
  out.x = static_cast<int16_t>(x < 0 ? 0 : x);
  out.y = static_cast<int16_t>(y < 0 ? 0 : y);
  if (m.out_width > 0 && out.x > static_cast<int16_t>(m.out_width - 1)) {
    out.x = static_cast<int16_t>(m.out_width - 1);
  }
  if (m.out_height > 0 && out.y > static_cast<int16_t>(m.out_height - 1)) {
    out.y = static_cast<int16_t>(m.out_height - 1);
  }
  return out;
}

// Which way a tap on a reading screen turns the page: -1 back, +1 forward,
// 0 for the band down the middle that does nothing.
//
// A middle band, rather than splitting the screen in half, is the whole
// point: a page turn on this panel costs half a second and the reader is
// holding the thing, so a thumb resting near the centre must not cost them
// their place. `edge_px` is how wide each acting edge is.
inline int8_t PageTapDelta(int16_t x, uint16_t width, uint16_t edge_px) {
  if (width == 0 || edge_px == 0) return 0;
  // Overlapping edges would make the middle band vanish; refuse rather than
  // guess which one wins.
  if (static_cast<uint32_t>(edge_px) * 2 >= width) return 0;
  if (x < 0) return 0;
  if (x < static_cast<int16_t>(edge_px)) return -1;
  if (x >= static_cast<int16_t>(width - edge_px) && x < static_cast<int16_t>(width)) {
    return 1;
  }
  return 0;
}

// True for a tap in the strip along the top of the screen, which opens the
// options menu on every screen that has one. It is the only way out of a book
// by finger: the menu is where "Close this book", "Library" and the rest are
// named, and a reader holding the device needs somewhere to press that is not
// a page turn.
//
// The strip is the header area every ui:: screen keeps above its first row
// (the rule at y = 72), so it never overlaps a list row or a shelf cover.
inline bool InMenuBand(int16_t y, uint16_t band_px) {
  if (band_px == 0) return false;
  return y >= 0 && y < static_cast<int16_t>(band_px);
}

enum class TouchEvent : uint8_t {
  kNone = 0,
  kDown,
  kUp,         // lifted after a long press, or after the finger slid
  kTap,        // lifted on the spot, before the long-press threshold
  kLongPress,  // reported once, while the finger is still down
};

// One finger, classified. Anything multi-touch is resolved before this: the
// panel reports up to five points and the UI only ever acts on the first, so
// a resting palm cannot fire a second action.
//
// A finger that slides more than `slop_px` stops being a tap and cannot
// become a long press either -- on a 7.5" panel a "tap" that drifts 40 px is
// someone starting a gesture, and acting on it would be acting on the wrong
// row. The slide itself is still activity, so the device stays awake.
class TapTracker {
 public:
  void begin(uint16_t long_press_ms, uint16_t slop_px) {
    long_press_ms_ = long_press_ms;
    slop_px_ = slop_px;
    down_ = false;
    moved_ = false;
    long_reported_ = false;
  }

  // `touching` false means no finger on the glass; x/y are then ignored.
  TouchEvent update(bool touching, int16_t x, int16_t y, uint32_t now_ms) {
    if (touching && !down_) {
      down_ = true;
      moved_ = false;
      long_reported_ = false;
      down_x_ = x;
      down_y_ = y;
      last_x_ = x;
      last_y_ = y;
      down_ms_ = now_ms;
      return TouchEvent::kDown;
    }
    if (touching) {
      last_x_ = x;
      last_y_ = y;
      if (!moved_ && (Abs(x - down_x_) > slop_px_ || Abs(y - down_y_) > slop_px_)) {
        moved_ = true;
      }
      if (!moved_ && !long_reported_ && long_press_ms_ != 0 &&
          static_cast<uint32_t>(now_ms - down_ms_) >= long_press_ms_) {
        long_reported_ = true;
        return TouchEvent::kLongPress;
      }
      return TouchEvent::kNone;
    }
    if (down_) {
      down_ = false;
      if (long_reported_ || moved_) return TouchEvent::kUp;
      return TouchEvent::kTap;
    }
    return TouchEvent::kNone;
  }

  // Where the gesture started. A tap acts on the position the finger landed
  // on, not where it left: the last frame before a lift can drift a few
  // pixels as the contact patch shrinks.
  int16_t x() const { return down_x_; }
  int16_t y() const { return down_y_; }
  int16_t lastX() const { return last_x_; }
  int16_t lastY() const { return last_y_; }
  bool down() const { return down_; }

 private:
  static int32_t Abs(int32_t v) { return v < 0 ? -v : v; }

  uint16_t long_press_ms_ = 0;
  uint16_t slop_px_ = 0;
  bool down_ = false;
  bool moved_ = false;
  bool long_reported_ = false;
  int16_t down_x_ = 0, down_y_ = 0;
  int16_t last_x_ = 0, last_y_ = 0;
  uint32_t down_ms_ = 0;
};

}  // namespace util
