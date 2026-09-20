// touch.h -- touch panel hardware abstraction.
//
// Only drivers::InputManager talks to this; the UI sees touch the way it sees
// every other input, as hal::InputEvent off the queue (hal/input.h). The
// split is the same one hal::IInput exists for: no screen reads a GPIO, and
// no screen reads an I2C register either.
//
// Lifecycle: begin() is idempotent and is called on every wake, like
// hal::IDisplay::begin(). The GT911 loses its state when its rail drops.

#pragma once

#include <stdint.h>

namespace hal {

// The panel reports up to five fingers; kMaxTouchPoints is that hardware
// limit, not a UI one. The UI acts on one finger (util::TapTracker).
constexpr uint8_t kMaxTouchPoints = 5;

struct TouchPoint {
  uint8_t id = 0;        // the chip's track id: stable while a finger stays down
  uint16_t x = 0;        // RAW chip coordinates, not display pixels
  uint16_t y = 0;
  uint16_t size = 0;     // contact area, as the chip reports it
};

struct TouchFrame {
  uint8_t count = 0;
  TouchPoint points[kMaxTouchPoints];
};

struct TouchInfo {
  bool present = false;      // the chip answered and identified itself
  uint8_t address = 0;       // 0x5D or 0x14
  uint16_t firmware = 0;
  uint16_t raw_width = 0;    // the chip's configured resolution
  uint16_t raw_height = 0;
  char product[5] = {0};     // "911"
};

class ITouchPanel {
 public:
  virtual ~ITouchPanel() = default;

  // Powers up, straps the I2C address and reads the chip's identity.
  // False means no usable touch panel -- every caller must keep working,
  // because this device's buttons are not a fallback for touch, they are the
  // primary input and touch is the addition.
  virtual bool begin() = 0;

  virtual bool present() const = 0;
  virtual TouchInfo info() const = 0;

  // Reads whatever the chip has. True when `out` was filled from a fresh
  // frame (including an empty one: that is "all fingers lifted"); false when
  // there is nothing new, or the read failed. Never blocks.
  virtual bool poll(TouchFrame* out) = 0;
};

}  // namespace hal
