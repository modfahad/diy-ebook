#include "drivers/touch_gt911.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>

#include "board/board_crowpanel_579.h"
#include "drivers/serial_log.h"

namespace drivers {

namespace {

// GT911 registers. 16-bit addresses, big-endian on the wire; the values
// inside them are little-endian.
constexpr uint16_t kRegProductId = 0x8140;  // 4 ASCII bytes, then fw, then res
constexpr uint16_t kRegStatus    = 0x814E;  // bit 7 = a frame is ready
constexpr uint16_t kRegCommand   = 0x8040;  // 0x05 = sleep
constexpr uint8_t kCommandSleep  = 0x05;
// Point data. 8 bytes per point: track id, x, y, size (all u16 LE), reserved.
// Published GT911 register maps disagree about whether point 1 starts here or
// at 0x8150; this chip settled it on the board on 2026-09-19 (0x814F -- see
// bring-up in pending.md). The first frame is still logged raw in poll(),
// which is what proved it and what would show a different chip disagreeing.
constexpr uint16_t kRegPoints    = 0x814F;

constexpr uint8_t kAddrPrimary = 0x5D;  // INT held LOW while RST rises
constexpr uint8_t kAddrAlt     = 0x14;  // INT held HIGH

// Polls come every app::kInputPollIntervalMs (2 ms), and each failed one
// costs an I2C timeout on top, so these are a fraction of a second of silence
// rather than a long wait.
constexpr uint16_t kErrorsBeforeComplaining = 20;
constexpr uint16_t kErrorsBeforeRecovering = 60;
constexpr uint32_t kRecoveryIntervalMs = 5000;

}  // namespace

bool TouchGt911::ack(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool TouchGt911::readRegister(uint16_t reg, uint8_t* out, uint8_t length) {
  Wire.beginTransmission(info_.address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  // Both arguments are cast explicitly: TwoWire has several requestFrom
  // overloads and two uint8_t arguments promote to int, which ESP32's
  // Arduino core has been ambiguous about.
  if (Wire.requestFrom(static_cast<uint8_t>(info_.address),
                       static_cast<uint8_t>(length)) != length) {
    return false;
  }
  for (uint8_t i = 0; i < length; ++i) out[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool TouchGt911::writeRegister(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(info_.address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// The datasheet's power-on strap: INT's level while RST rises picks the I2C
// address, then INT is held low for 50 ms and released so the chip can drive
// it as an output.
void TouchGt911::resetChip(bool int_high) {
  pinMode(board::kTouchRst, OUTPUT);
  pinMode(board::kTouchInt, OUTPUT);
  digitalWrite(board::kTouchRst, LOW);
  digitalWrite(board::kTouchInt, int_high ? HIGH : LOW);
  delay(10);
  digitalWrite(board::kTouchRst, HIGH);
  delay(10);
  digitalWrite(board::kTouchInt, LOW);
  delay(50);
  pinMode(board::kTouchInt, INPUT);
  delay(50);
}

bool TouchGt911::identify(uint8_t address) {
  info_.address = address;
  uint8_t id[10];
  if (!readRegister(kRegProductId, id, static_cast<uint8_t>(sizeof(id)))) {
    return false;
  }
  memcpy(info_.product, id, 4);
  info_.product[4] = 0;
  info_.firmware = static_cast<uint16_t>(id[4] | (id[5] << 8));
  info_.raw_width = static_cast<uint16_t>(id[6] | (id[7] << 8));
  info_.raw_height = static_cast<uint16_t>(id[8] | (id[9] << 8));
  // An unconfigured chip reports a zero resolution; the panel's own size is
  // the only sane assumption then, and util::MapTouchPoint falls back to it.
  if (info_.raw_width == 0) info_.raw_width = board::kWidth;
  if (info_.raw_height == 0) info_.raw_height = board::kHeight;
  return true;
}

bool TouchGt911::begin() {
  // begin() runs on every wake, so re-strapping and re-identifying is the
  // point, not wasted work: after a deep sleep the chip has forgotten
  // everything, including its address.
  info_ = hal::TouchInfo();
  frame_count_ = 0;
  error_count_ = 0;
  logged_first_frame_ = false;

  if (!bus_started_) {
    Wire.begin(board::kTouchSda, board::kTouchScl, board::kTouchI2cHz);
    bus_started_ = true;
  }

  // Three goes, not one. Observed on the board 2026-09-21: the same wiring
  // that works after a warm reset can fail to answer on a cold one, which is
  // what an I2C bus running on the ESP32's internal pull-ups alone looks like
  // (see the 4.7k note in board_crowpanel_579.h). A dead first probe used to
  // mean touch was off until the next reboot.
  bool ok = false;
  for (uint8_t attempt = 1; attempt <= 3 && !ok; ++attempt) {
    resetChip(false);
    ok = ack(kAddrPrimary) && identify(kAddrPrimary);
    if (!ok) {
      resetChip(true);
      ok = ack(kAddrAlt) && identify(kAddrAlt);
    }
    if (!ok) {
      Logf("[touch] attempt %u: no answer at 0x%02X or 0x%02X\n",
           static_cast<unsigned>(attempt), kAddrPrimary, kAddrAlt);
      delay(50);
    }
  }
  if (!ok) {
    info_ = hal::TouchInfo();
    // What else is on the bus narrows it down at a glance: nothing at all
    // means wiring or power, something at another address means the wrong
    // chip or a strapping problem.
    uint8_t seen = 0;
    char found[64];
    int written = 0;
    for (uint8_t a = 0x08; a < 0x78 && written < 50; ++a) {
      if (!ack(a)) continue;
      ++seen;
      written += snprintf(found + written, sizeof(found) - written, "0x%02X ", a);
    }
    if (seen == 0) snprintf(found, sizeof(found), "nothing");
    Logf("[touch] no GT911 on SDA%d/SCL%d (I2C scan: %s) -- buttons only\n",
         board::kTouchSda, board::kTouchScl, found);
    return false;
  }

  info_.present = true;
  Logf("[touch] GT911 at 0x%02X id=\"%s\" fw=0x%04X res=%ux%u\n", info_.address,
       info_.product, static_cast<unsigned>(info_.firmware),
       static_cast<unsigned>(info_.raw_width),
       static_cast<unsigned>(info_.raw_height));
  return true;
}

// One I2C failure is noise on a bus with no pull-up resistors of its own.
// A run of them means the chip has stopped answering, and the only way back
// is the reset line. Both are worth saying out loud: touch dying quietly is
// how it looked from the outside -- taps stopped working and nothing in the
// log mentioned touch at all.
void TouchGt911::noteError() {
  ++error_count_;
  if (consecutive_errors_ < 0xFFFF) ++consecutive_errors_;

  if (consecutive_errors_ == kErrorsBeforeComplaining) {
    Logf("[touch] I2C going quiet (%lu errors in all) -- weak pull-ups?\n",
         static_cast<unsigned long>(error_count_));
  }
  if (consecutive_errors_ < kErrorsBeforeRecovering) return;

  // Rate-limited: a chip that is unplugged must not turn into a reset loop.
  const uint32_t now = millis();
  if (last_recovery_ms_ != 0 &&
      static_cast<uint32_t>(now - last_recovery_ms_) < kRecoveryIntervalMs) {
    return;
  }
  last_recovery_ms_ = now;
  consecutive_errors_ = 0;
  Logf("[touch] no answer -- resetting the chip\n");
  const bool was_present = info_.present;
  if (begin()) {
    Logf("[touch] recovered\n");
  } else if (was_present) {
    Logf("[touch] still not answering; will try again in %lus\n",
         static_cast<unsigned long>(kRecoveryIntervalMs / 1000));
  }
}

bool TouchGt911::sleep() {
  if (!info_.present) return false;
  // There is no wake command over I2C -- the datasheet's way back is INT or
  // RST, and begin() already toggles RST on every wake, so nothing here has
  // to remember that the chip was asleep.
  const bool ok = writeRegister(kRegCommand, kCommandSleep);
  Logf("[touch] %s\n", ok ? "asleep" : "sleep command failed");
  return ok;
}

bool TouchGt911::poll(hal::TouchFrame* out) {
  if (!info_.present || out == nullptr) return false;

  uint8_t status = 0;
  if (!readRegister(kRegStatus, &status, 1)) {
    noteError();
    return false;
  }
  consecutive_errors_ = 0;
  if ((status & 0x80) == 0) return false;  // nothing new since the last read

  uint8_t count = status & 0x0F;
  if (count > hal::kMaxTouchPoints) count = hal::kMaxTouchPoints;

  uint8_t raw[8 * hal::kMaxTouchPoints];
  bool ok = true;
  if (count > 0) {
    ok = readRegister(kRegPoints, raw, static_cast<uint8_t>(8 * count));
  }
  // The status register MUST be cleared after every ready frame, failed read
  // or not: leave it set and the chip never reports another one.
  if (!writeRegister(kRegStatus, 0)) ok = false;
  if (!ok) {
    noteError();
    return false;
  }
  consecutive_errors_ = 0;

  out->count = count;
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t* p = raw + 8 * i;
    out->points[i].id = p[0];
    out->points[i].x = static_cast<uint16_t>(p[1] | (p[2] << 8));
    out->points[i].y = static_cast<uint16_t>(p[3] | (p[4] << 8));
    out->points[i].size = static_cast<uint16_t>(p[5] | (p[6] << 8));
  }
  ++frame_count_;

  // The first frame with a finger on it, raw. This is what says whether
  // kRegPoints is the right base register: byte 0 should be a small track id
  // (0-4) and bytes 1-2 an x inside the panel.
  if (count > 0 && !logged_first_frame_) {
    logged_first_frame_ = true;
    const uint8_t* p = raw;
    Logf("[touch] first frame: n=%u bytes=%02X %02X %02X %02X %02X %02X %02X %02X"
         " -> id=%u raw=(%u,%u) size=%u\n",
         static_cast<unsigned>(count), static_cast<unsigned>(p[0]),
         static_cast<unsigned>(p[1]), static_cast<unsigned>(p[2]),
         static_cast<unsigned>(p[3]), static_cast<unsigned>(p[4]),
         static_cast<unsigned>(p[5]), static_cast<unsigned>(p[6]),
         static_cast<unsigned>(p[7]), static_cast<unsigned>(out->points[0].id),
         static_cast<unsigned>(out->points[0].x),
         static_cast<unsigned>(out->points[0].y),
         static_cast<unsigned>(out->points[0].size));
  }
  return true;
}

}  // namespace drivers
