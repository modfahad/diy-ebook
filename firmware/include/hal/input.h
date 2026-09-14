// input.h -- user input abstraction (rotary encoder + MENU + EXIT).
//
// The UI never reads a GPIO. It pulls debounced, decoded events out of a
// queue, which is what lets Phase 2/3 screens be written and tested without
// hardware.

#pragma once

#include <stdint.h>

namespace hal {

enum class InputSource : uint8_t {
  kNone = 0,
  kMenu,
  kExit,
  kEncoderSwitch,
  kEncoder,
};

enum class InputAction : uint8_t {
  kNone = 0,
  kDown,
  kUp,
  kClick,
  kLongPress,
  kRotate,
};

struct InputEvent {
  InputSource source = InputSource::kNone;
  InputAction action = InputAction::kNone;
  int16_t delta = 0;          // signed detents, kRotate only (+ = clockwise)
  uint32_t timestamp_ms = 0;
};

// Raw view used by the hardware self-test screen. Not for application code.
struct InputDiagnostics {
  bool menu_level = true;      // raw pin level, true = idle/high
  bool exit_level = true;
  bool switch_level = true;
  bool encoder_a_level = true;
  bool encoder_b_level = true;

  uint32_t menu_count = 0;     // debounced click counts
  uint32_t exit_count = 0;
  uint32_t switch_count = 0;
  uint32_t long_press_count = 0;

  int32_t pulse_position = 0;       // decoded by the independent-pulse decoder
  uint32_t edge_count = 0;          // total A/B edges seen
};

class IInput {
 public:
  virtual ~IInput() = default;

  virtual bool begin() = 0;

  // Must be called frequently while awake. `now_ms` is a monotonic
  // millisecond clock.
  virtual void poll(uint32_t now_ms) = 0;

  // Pops one event. Returns false when the queue is empty.
  virtual bool nextEvent(InputEvent* out) = 0;

  // Timestamp of the last debounced activity, for the idle/sleep policy.
  virtual uint32_t lastActivityMs() const = 0;

  // True while any input is physically actuated. Deep sleep must not be
  // entered while a wake-source pin is still held low, or the device
  // immediately re-wakes.
  virtual bool anyHeld() const = 0;

  virtual InputDiagnostics diagnostics() const = 0;
};

}  // namespace hal
