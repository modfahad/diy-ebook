// power_manager.h -- hal::IPower for the ESP32-S3 / CrowPanel.

#pragma once

#include <stdint.h>

#include "hal/power.h"

namespace drivers {

class PowerManager : public hal::IPower {
 public:
  bool begin() override;
  hal::WakeInfo wakeInfo() const override { return wake_; }

  bool batteryAvailable() const override;
  uint32_t batteryMillivolts() const override;

  void setStatusLed(bool on) override;

  void deepSleep() override;
  void deepSleepFor(uint64_t ms) override;

  static const char* wakeReasonName(hal::WakeReason reason);

 private:
  void prepareRailsForSleep();

  hal::WakeInfo wake_;
};

}  // namespace drivers
