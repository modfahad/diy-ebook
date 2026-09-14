// clock.h -- wall-clock abstraction.
//
// The device has no RTC and, until Wi-Fi comes up in transfer mode, no way
// to know the real date at all. `nowUnix()` returning 0 is not an error --
// it is the documented "clock unset" value threaded through already (see
// LibraryEntry::installed_at). Callers that need a real timestamp check
// synced() first rather than trusting a 0 they can't tell apart from
// 1970-01-01.

#pragma once

#include <stdint.h>

namespace hal {

class IClock {
 public:
  virtual ~IClock() = default;

  // Starts an SNTP sync against the given server. Wi-Fi must already be
  // connected. Non-blocking: sync happens in the background, poll synced().
  virtual void begin(const char* ntp_server) = 0;

  // True once a real time has been obtained (year > 2020, in practice).
  virtual bool synced() const = 0;

  // Unix seconds, or 0 if never synced.
  virtual uint32_t nowUnix() const = 0;

  // The POSIX TZ rule local time is shown in (net::ValidTimeZone). Applies
  // immediately and to later syncs. A no-op for clocks that only deal in UTC.
  virtual void setTimeZone(const char* posix_tz) { (void)posix_tz; }
  virtual const char* timeZone() const { return "UTC0"; }
};

}  // namespace hal
