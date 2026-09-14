// idle_policy.h -- pure, host-testable "may we go back to sleep yet?" rule.
//
// Kept separate from the power driver so the decision can be unit-tested on
// the host; the driver only executes it.

#pragma once

#include <stdint.h>

namespace util {

class IdlePolicy {
 public:
  void begin(uint32_t idle_timeout_ms, uint32_t now_ms) {
    idle_timeout_ms_ = idle_timeout_ms;
    last_activity_ms_ = now_ms;
    busy_ = false;
  }

  void noteActivity(uint32_t now_ms) { last_activity_ms_ = now_ms; }

  // Set while a refresh / mount / transfer is in flight. Sleep is forbidden
  // until it clears, whatever the timer says.
  void setBusy(bool busy) { busy_ = busy; }
  bool busy() const { return busy_; }

  uint32_t idleMs(uint32_t now_ms) const {
    return static_cast<uint32_t>(now_ms - last_activity_ms_);
  }

  uint32_t remainingMs(uint32_t now_ms) const {
    const uint32_t idle = idleMs(now_ms);
    return idle >= idle_timeout_ms_ ? 0u : idle_timeout_ms_ - idle;
  }

  // `input_held` guards the re-wake trap: entering deep sleep while a
  // wake-source pin is still pulled low wakes the device again immediately.
  bool shouldSleep(uint32_t now_ms, bool input_held) const {
    if (busy_ || input_held) return false;
    return idleMs(now_ms) >= idle_timeout_ms_;
  }

 private:
  uint32_t idle_timeout_ms_ = 0;
  uint32_t last_activity_ms_ = 0;
  bool busy_ = false;
};

}  // namespace util
