// serial_log.h -- every firmware log line, with the time in front of it.
//
// Before the wall clock has synced over NTP a line reads
//   [+123.456] [epd] ...          seconds.milliseconds since boot
// and afterwards
//   [14:32:05.123] [epd] ...      local time, in the configured time zone
//
// All firmware logging goes through here instead of Serial.printf/println,
// so no line reaches the serial monitor unstamped. GxEPD2's own timing prints
// are compiled out with DISABLE_DIAGNOSTIC_OUTPUT (platformio.ini); the one it
// cannot compile out, "Busy Timeout!", is followed by a stamped [epd] line.

#pragma once

namespace drivers {

// printf-style. A trailing '\n' is optional; embedded newlines start new,
// separately stamped lines. Lines longer than 383 bytes are truncated.
void Logf(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Same as Logf("%s", text).
void LogLine(const char* text);

}  // namespace drivers
