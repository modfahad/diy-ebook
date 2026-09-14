#include "drivers/system_clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <Arduino.h>

namespace drivers {
namespace {

// Any real NTP response lands well past this; an unsynced ESP32 clock sits
// at a small number of seconds since boot. 2020-09-13T12:26:40Z -- there is
// nothing special about the date beyond "clearly not that".
constexpr time_t kEarliestPlausibleUnixTime = 1600000000;

}  // namespace

void SystemClock::begin(const char* ntp_server) {
  // configTzTime rather than configTime: configTime(0, 0, ...) resets TZ to
  // UTC, which would undo setTimeZone() on every sync. SNTP then re-syncs in
  // the background on its own interval (hourly by default).
  configTzTime(tz_, ntp_server);
}

void SystemClock::setTimeZone(const char* posix_tz) {
  if (!net::ValidTimeZone(posix_tz)) return;
  snprintf(tz_, sizeof(tz_), "%s", posix_tz);
  setenv("TZ", tz_, 1);
  tzset();
}

bool SystemClock::synced() const {
  return time(nullptr) >= kEarliestPlausibleUnixTime;
}

uint32_t SystemClock::nowUnix() const {
  const time_t now = time(nullptr);
  if (now < kEarliestPlausibleUnixTime) return 0;
  return static_cast<uint32_t>(now);
}

}  // namespace drivers
