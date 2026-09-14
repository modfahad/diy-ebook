// debouncer.h -- pure, host-testable switch debouncing and click/long-press
// detection. No Arduino, no ESP-IDF, no globals.

#pragma once

#include <stdint.h>

namespace util {

// Time-based debouncer. A candidate level must hold steady for `debounce_ms`
// before it is accepted. All time arithmetic is unsigned so a millis()
// wraparound behaves correctly.
class Debouncer {
 public:
  void begin(bool initial_level, uint16_t debounce_ms) {
    stable_ = initial_level;
    candidate_ = initial_level;
    candidate_since_ = 0;
    debounce_ms_ = debounce_ms;
    changed_ = false;
  }

  // Returns true if the stable level changed on this call.
  bool update(bool raw_level, uint32_t now_ms) {
    changed_ = false;
    if (raw_level != candidate_) {
      candidate_ = raw_level;
      candidate_since_ = now_ms;
      return false;
    }
    if (candidate_ != stable_ &&
        static_cast<uint32_t>(now_ms - candidate_since_) >= debounce_ms_) {
      stable_ = candidate_;
      changed_ = true;
    }
    return changed_;
  }

  bool level() const { return stable_; }
  bool changed() const { return changed_; }

 private:
  bool stable_ = true;
  bool candidate_ = true;
  uint32_t candidate_since_ = 0;
  uint16_t debounce_ms_ = 0;
  bool changed_ = false;
};

enum class ButtonEvent : uint8_t {
  kNone = 0,
  kDown,
  kUp,         // released after a long press was already reported
  kClick,      // released before the long-press threshold
  kLongPress,  // reported once, while still held
};

// Debounced button with click / long-press classification.
// `active_low` describes the electrical sense of the raw pin level.
class Button {
 public:
  void begin(bool active_low, bool initial_raw_level, uint16_t debounce_ms,
             uint16_t long_press_ms) {
    active_low_ = active_low;
    long_press_ms_ = long_press_ms;
    debouncer_.begin(initial_raw_level, debounce_ms);
    pressed_ = isActive(initial_raw_level);
    pressed_since_ = 0;
    // A press that was ALREADY in progress when we started is not a new long
    // press. This matters on every wake: the button that woke the device is
    // often still held when begin() runs, and the first poll() can be seconds
    // later (the panel init sequence sits in between). Without this, that
    // press would be reported as a long press -- which in Phase 2 means a
    // context action / bookmark the user never asked for.
    long_reported_ = pressed_;
  }

  ButtonEvent update(bool raw_level, uint32_t now_ms) {
    if (debouncer_.update(raw_level, now_ms)) {
      const bool now_pressed = isActive(debouncer_.level());
      if (now_pressed && !pressed_) {
        pressed_ = true;
        pressed_since_ = now_ms;
        long_reported_ = false;
        return ButtonEvent::kDown;
      }
      if (!now_pressed && pressed_) {
        pressed_ = false;
        return long_reported_ ? ButtonEvent::kUp : ButtonEvent::kClick;
      }
    }
    if (pressed_ && !long_reported_ && long_press_ms_ != 0 &&
        static_cast<uint32_t>(now_ms - pressed_since_) >= long_press_ms_) {
      long_reported_ = true;
      return ButtonEvent::kLongPress;
    }
    return ButtonEvent::kNone;
  }

  bool pressed() const { return pressed_; }
  bool rawLevel() const { return debouncer_.level(); }

 private:
  bool isActive(bool raw_level) const {
    return active_low_ ? !raw_level : raw_level;
  }

  Debouncer debouncer_;
  bool active_low_ = true;
  bool pressed_ = false;
  bool long_reported_ = false;
  uint32_t pressed_since_ = 0;
  uint16_t long_press_ms_ = 0;
};

}  // namespace util
