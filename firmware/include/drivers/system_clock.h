// system_clock.h -- hal::IClock over ESP32 Arduino's SNTP client.

#pragma once

#include "hal/clock.h"
#include "net/time_zone.h"

namespace drivers {

class SystemClock : public hal::IClock {
 public:
  void begin(const char* ntp_server) override;
  bool synced() const override;
  uint32_t nowUnix() const override;

  void setTimeZone(const char* posix_tz) override;
  const char* timeZone() const override { return tz_; }

 private:
  char tz_[net::kTimeZoneMaxChars + 1] = "UTC0";
};

}  // namespace drivers
