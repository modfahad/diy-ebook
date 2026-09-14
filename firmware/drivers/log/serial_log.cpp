#include "drivers/serial_log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace drivers {
namespace {

// Same "has NTP ever answered" test as SystemClock::synced(): an unsynced
// ESP32 clock counts up from 1970.
constexpr time_t kEarliestPlausibleUnixTime = 1600000000;

constexpr size_t kMaxLineBytes = 384;

void WritePrefix() {
  char prefix[24];
  struct timeval now;
  gettimeofday(&now, nullptr);
  if (now.tv_sec >= kEarliestPlausibleUnixTime) {
    struct tm local;
    localtime_r(&now.tv_sec, &local);
    snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03ld] ", local.tm_hour,
             local.tm_min, local.tm_sec, static_cast<long>(now.tv_usec / 1000));
  } else {
    const unsigned long ms = millis();
    snprintf(prefix, sizeof(prefix), "[+%lu.%03lu] ", ms / 1000, ms % 1000);
  }
  Serial.print(prefix);
}

void WriteLines(const char* text) {
  const char* line = text;
  do {
    const char* newline = strchr(line, '\n');
    const size_t length =
        newline != nullptr ? static_cast<size_t>(newline - line) : strlen(line);
    WritePrefix();
    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
    Serial.println();
    if (newline == nullptr) break;
    line = newline + 1;
  } while (*line != '\0');
}

}  // namespace

void Logf(const char* format, ...) {
  char buffer[kMaxLineBytes];
  va_list args;
  va_start(args, format);
  const int n = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (n < 0) return;
  if (static_cast<size_t>(n) >= sizeof(buffer)) {
    // Say it was cut rather than silently dropping the end of a line.
    memcpy(buffer + sizeof(buffer) - 4, "...", 4);
  }
  WriteLines(buffer);
}

void LogLine(const char* text) { WriteLines(text != nullptr ? text : ""); }

}  // namespace drivers
