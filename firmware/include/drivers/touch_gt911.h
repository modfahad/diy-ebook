// touch_gt911.h -- hal::ITouchPanel for the GDEY075T7-T01's GT911 layer.
//
// Polling, not the INT line. The panel is polled from the same loop that
// polls the buttons (app::kInputPollIntervalMs), which keeps touch on the
// same timing as every other input and needs no ISR. INT is still wired: it
// is what a later "wake on touch" would use (board::kTouchInt is
// RTC-capable), and this driver only drives it during the reset strap.

#pragma once

#include <stdint.h>

#include "hal/touch.h"

namespace drivers {

class TouchGt911 : public hal::ITouchPanel {
 public:
  bool begin() override;
  bool present() const override { return info_.present; }
  hal::TouchInfo info() const override { return info_; }
  bool poll(hal::TouchFrame* out) override;

  // Tells the chip to stop scanning (GT911 command 0x05). It answers nothing
  // afterwards, including a finger, until begin() resets it -- which every
  // wake does anyway. Returns false if the command could not be sent.
  bool sleep();

  // Frames read since begin(), and I2C failures. The self-test screen shows
  // these; they are also what tells a "no finger registered" apart from a
  // "bus fell over".
  uint32_t frameCount() const { return frame_count_; }
  uint32_t errorCount() const { return error_count_; }
  // I2C failures since the last good frame. A bus that stops answering
  // mid-session is the failure this driver has actually seen on the board,
  // and it used to be silent: poll() returned false and touch simply died.
  uint16_t consecutiveErrors() const { return consecutive_errors_; }

 private:
  void noteError();
  bool readRegister(uint16_t reg, uint8_t* out, uint8_t length);
  bool writeRegister(uint16_t reg, uint8_t value);
  bool ack(uint8_t address);
  void resetChip(bool int_high);
  bool identify(uint8_t address);

  hal::TouchInfo info_;
  uint32_t frame_count_ = 0;
  uint32_t error_count_ = 0;
  uint16_t consecutive_errors_ = 0;
  uint32_t last_recovery_ms_ = 0;
  bool logged_first_frame_ = false;
  bool bus_started_ = false;
};

}  // namespace drivers
