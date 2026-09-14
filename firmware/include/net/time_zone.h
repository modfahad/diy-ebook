// time_zone.h -- checking a POSIX TZ string before it is stored and applied.
//
// The device has no time-zone database, so the desktop app sends the rule
// itself, e.g. "IST-5:30" or "CET-1CEST,M3.5.0,M10.5.0/3". newlib parses it at
// tzset(). This only rejects what cannot be one: empty, too long, or
// characters no POSIX TZ uses. A string that passes but names a nonsensical
// rule makes newlib fall back to UTC, which is visible on the clock, not a
// crash.

#pragma once

#include <stdint.h>

namespace net {

constexpr uint8_t kTimeZoneMaxChars = 63;

bool ValidTimeZone(const char* tz);

}  // namespace net
