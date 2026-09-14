// power.h -- power/sleep abstraction.
//
// The device spends nearly all of its life in deep sleep with the last page
// still on the glass. Everything about that transition lives behind this
// interface.

#pragma once

#include <stdint.h>

namespace hal {

enum class WakeReason : uint8_t {
  kColdBoot = 0,   // power-on / reset / flash
  kInput,          // EXT1: a front-panel pin went low
  kTimer,
  kOther,
};

struct WakeInfo {
  WakeReason reason = WakeReason::kColdBoot;
  int gpio = -1;           // -1 when unknown or not an EXT1 wake
  uint64_t pin_mask = 0;   // raw EXT1 status mask
  uint32_t boot_count = 0; // survives deep sleep in RTC memory
};

class IPower {
 public:
  virtual ~IPower() = default;

  // Reads the wake cause and increments the RTC boot counter. Call first,
  // before touching any peripheral.
  virtual bool begin() = 0;

  virtual WakeInfo wakeInfo() const = 0;

  // Battery telemetry. Not available on this board revision; see
  // board_crowpanel_579.h TODO(hw).
  virtual bool batteryAvailable() const = 0;
  virtual uint32_t batteryMillivolts() const = 0;

  virtual void setStatusLed(bool on) = 0;

  // Configures EXT1 wake and enters deep sleep. Does not return.
  // Callers must have already flushed the display, unmounted storage and
  // dropped the peripheral rails.
  virtual void deepSleep() = 0;

  // Same, but also arms a timer wake after `ms`. Does not return.
  virtual void deepSleepFor(uint64_t ms) = 0;
};

}  // namespace hal
