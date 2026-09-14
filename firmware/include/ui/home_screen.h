// home_screen.h -- the table clock: clock, date and month calendar on the
// left, a photo on the right.
//
// The canvas is 1bpp, so this screen draws everything except the photo and
// only reserves the photo's rectangle (kHomePhoto*). The display driver draws
// the photo's four greys into that rectangle when it pushes a grey frame
// (drivers::Epd750Display::flushGrey).
//
// Refresh plan (main.cpp): a full 4-grey frame when the screen is entered,
// when the photo changes and when the date changes; in between, each minute
// only the clock rectangle is pushed, as a fast black/white window update --
// which is why nothing but the clock digits is drawn inside it.
//
// Pure render, host-testable, like the other ui:: screens.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"

namespace ui {

constexpr int kHomePhotoX = 400;
constexpr int kHomePhotoY = 0;
constexpr int kHomePhotoW = 400;
constexpr int kHomePhotoH = 480;

// x and w are multiples of 8: the panel controller addresses RAM in whole
// bytes, so a window update of this rectangle covers exactly it.
constexpr int kHomeClockX = 16;
constexpr int kHomeClockY = 72;
constexpr int kHomeClockW = 368;
constexpr int kHomeClockH = 88;

struct HomeState {
  // False until the clock has synced; the screen then says so instead of
  // showing a wrong time.
  bool time_valid = false;
  uint16_t year = 2026;
  uint8_t month = 1;   // 1..12
  uint8_t day = 1;     // 1..31
  uint8_t hour = 0;    // 0..23
  uint8_t minute = 0;  // 0..59

  bool week_starts_monday = true;

  // Whether the driver will draw a photo into the photo rectangle. Without one
  // the rectangle says how to add photos.
  bool has_photo = false;

  // One line under the calendar: Wi-Fi, address, sync state.
  const char* status = "";
};

// month 1..12. Gregorian.
uint8_t DaysInMonth(uint16_t year, uint8_t month);
// 0 = Sunday .. 6 = Saturday.
uint8_t DayOfWeek(uint16_t year, uint8_t month, uint8_t day);

class HomeScreen {
 public:
  // The whole screen (clears the canvas).
  static void render(gfx::Canvas& canvas, const HomeState& state);

  // Only the clock rectangle: clears it and draws the time. Leaves the rest of
  // the canvas as it was.
  static void renderClock(gfx::Canvas& canvas, const HomeState& state);
};

}  // namespace ui
