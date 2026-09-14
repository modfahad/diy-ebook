#include "net/time_zone.h"

namespace net {

bool ValidTimeZone(const char* tz) {
  if (tz == nullptr) return false;
  // A POSIX TZ starts with a zone name: 3+ letters, or a <quoted> name.
  if (!((tz[0] >= 'A' && tz[0] <= 'Z') || (tz[0] >= 'a' && tz[0] <= 'z') ||
        tz[0] == '<')) {
    return false;
  }
  uint8_t length = 0;
  for (const char* p = tz; *p != '\0'; ++p) {
    const char c = *p;
    const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '+' || c == '-' ||
                         c == ':' || c == ',' || c == '.' || c == '/' ||
                         c == '<' || c == '>';
    if (!allowed || length >= kTimeZoneMaxChars) return false;
    ++length;
  }
  return length >= 3;
}

}  // namespace net
