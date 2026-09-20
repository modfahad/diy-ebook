// screen_setup.h -- the screen and touch orientation, as saved on the card.
//
// Two numbers the device cannot know until someone looks at it: which way up
// the panel sits in its case, and how the touch layer's axes relate to the
// picture. board_crowpanel_579.h carries the build-time defaults; this is the
// runtime override the setup screen writes, so finding the right touch
// orientation is eight presses on the device instead of eight reflashes.
//
// Stored as one short line of text rather than a struct: it is two small
// numbers, it will be read by a human debugging a device far from a compiler,
// and "rotation=2 touch=5" says what it is on sight.
//
// Pure parsing and formatting, no storage: main.cpp reads and writes the file
// the same way it does the time zone.

#pragma once

#include <stdint.h>

namespace util {

struct ScreenSetup {
  // Quarter turns, as ui:: and GxEPD2 mean it. Only 0 and 2 keep the 800x480
  // landscape geometry every screen is laid out for.
  uint8_t rotation = 0;
  // Bit 0 swap x/y, bit 1 mirror x, bit 2 mirror y -- the same three flags as
  // board::kTouchSwapXY / kTouchInvertX / kTouchInvertY, packed so the setup
  // screen can step through all eight with one press.
  uint8_t touch_orientation = 0;

  bool swapXY() const { return (touch_orientation & 1) != 0; }
  bool invertX() const { return (touch_orientation & 2) != 0; }
  bool invertY() const { return (touch_orientation & 4) != 0; }
};

constexpr uint8_t kTouchOrientationCount = 8;
constexpr uint8_t kScreenSetupMaxChars = 40;  // "rotation=2 touch=7\n" and room

// Writes "rotation=R touch=T\n" into `out` (at least kScreenSetupMaxChars + 1
// bytes). Returns the number of characters written, excluding the terminator.
inline uint8_t FormatScreenSetup(const ScreenSetup& setup, char* out,
                                 uint8_t capacity) {
  if (out == nullptr || capacity < 21) return 0;
  const uint8_t rotation = setup.rotation == 2 ? 2 : 0;
  const uint8_t touch =
      static_cast<uint8_t>(setup.touch_orientation % kTouchOrientationCount);
  const char digits[] = {'0', '1', '2', '3', '4', '5', '6', '7'};
  uint8_t n = 0;
  const char* prefix = "rotation=";
  while (*prefix != '\0') out[n++] = *prefix++;
  out[n++] = rotation == 2 ? '2' : '0';
  const char* middle = " touch=";
  while (*middle != '\0') out[n++] = *middle++;
  out[n++] = digits[touch];
  out[n++] = '\n';
  out[n] = '\0';
  return n;
}

// Reads what FormatScreenSetup wrote. Unknown keys, extra whitespace and a
// missing trailing newline are all tolerated; anything out of range falls back
// to `out`'s existing value, so a truncated or hand-edited file degrades to
// the build-time defaults instead of an unusable screen.
inline bool ParseScreenSetup(const char* text, ScreenSetup* out) {
  if (text == nullptr || out == nullptr) return false;
  bool found = false;
  for (const char* p = text; *p != '\0'; ++p) {
    // Only look at the start of a key: "rotation=" must not match inside
    // another word.
    const bool at_start = (p == text) || p[-1] == '\n' || p[-1] == '\r' ||
                          p[-1] == ' ' || p[-1] == '\t';
    if (!at_start) continue;

    const char* key = nullptr;
    uint8_t* field = nullptr;
    uint8_t limit = 0;
    if (p[0] == 'r' && p[1] == 'o' && p[2] == 't' && p[3] == 'a' && p[4] == 't' &&
        p[5] == 'i' && p[6] == 'o' && p[7] == 'n' && p[8] == '=') {
      key = "rotation=";
      field = &out->rotation;
      limit = 3;  // 0..3; only 0 and 2 are kept below
    } else if (p[0] == 't' && p[1] == 'o' && p[2] == 'u' && p[3] == 'c' &&
               p[4] == 'h' && p[5] == '=') {
      key = "touch=";
      field = &out->touch_orientation;
      limit = kTouchOrientationCount - 1;
    }
    if (key == nullptr) continue;

    const char* value = p;
    while (*value != '=' ) ++value;
    ++value;
    if (*value < '0' || *value > '9') continue;
    uint16_t number = 0;
    while (*value >= '0' && *value <= '9') {
      number = static_cast<uint16_t>(number * 10 + (*value - '0'));
      if (number > 255) break;
      ++value;
    }
    if (number > limit) continue;
    if (field == &out->rotation && number != 0 && number != 2) continue;
    *field = static_cast<uint8_t>(number);
    found = true;
  }
  return found;
}

}  // namespace util
