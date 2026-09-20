// input_manager.h -- hal::IInput for the CrowPanel front panel.
//
// Polling, not interrupts. At kInputPollIntervalMs the encoder is sampled far
// faster than a human can turn it, and polling keeps the whole decode path in
// pure, host-testable code (util/debouncer.h, util/rotary.h) instead of an
// ISR. Phase 2 can move to PCNT/interrupts behind this same interface if a
// fast spin ever proves to drop detents.

#pragma once

#include <stdint.h>

#include "hal/input.h"
#include "hal/touch.h"
#include "util/debouncer.h"
#include "util/rotary.h"
#include "util/touch.h"

namespace drivers {

class InputManager : public hal::IInput {
 public:
  // Optional, and must be called before begin(). Without it the front panel
  // works exactly as it did before touch existed -- which is also what
  // happens when the panel is fitted but its GT911 never answers.
  void attachTouch(hal::ITouchPanel* touch) { touch_ = touch; }

  // The three touch orientation flags, packed (bit 0 swap x/y, bit 1 mirror
  // x, bit 2 mirror y), as util::ScreenSetup stores them. Starts from the
  // board header's defaults; the setup screen changes it while running, which
  // is how the right one gets found without a reflash.
  void setTouchOrientation(uint8_t packed);
  uint8_t touchOrientation() const;

  // Probes the touch panel again, keeping the orientation already set. For
  // when it did not answer at boot: a bus that failed once is not a panel
  // that is missing, and a reboot is a poor way to find that out.
  bool retryTouch();

  bool begin() override;
  void poll(uint32_t now_ms) override;
  bool nextEvent(hal::InputEvent* out) override;
  uint32_t lastActivityMs() const override { return last_activity_ms_; }
  bool anyHeld() const override;
  hal::InputDiagnostics diagnostics() const override { return diag_; }

  // True while rotary steps are queued but not yet released to the UI.
  bool rotaryPending() const { return coalescer_.pending(); }

 private:
  void push(hal::InputSource source, hal::InputAction action, int16_t delta,
            uint32_t now_ms);
  void pumpButton(util::Button& button, hal::InputSource source, bool raw,
                  uint32_t now_ms, uint32_t* click_counter);
  void pumpTouch(uint32_t now_ms);
  void push(hal::InputSource source, hal::InputAction action, int16_t delta,
            int16_t x, int16_t y, uint32_t now_ms);

  static constexpr uint8_t kQueueSize = 16;
  hal::InputEvent queue_[kQueueSize];
  uint8_t head_ = 0;
  uint8_t count_ = 0;

  util::Button menu_;
  util::Button exit_;
  util::Button switch_;
  util::Debouncer encoder_a_;
  util::Debouncer encoder_b_;
  util::PulseDecoder pulse_;
  util::RotaryCoalescer coalescer_;

  hal::ITouchPanel* touch_ = nullptr;
  util::TapTracker tap_;
  util::TouchMapping mapping_;
  // The last frame's finger, kept because the GT911 only reports a frame
  // when something changed: between frames the finger is still down.
  bool touch_down_ = false;
  int16_t touch_x_ = 0;
  int16_t touch_y_ = 0;
  uint32_t touch_frame_ms_ = 0;   // when the last frame arrived

  hal::InputDiagnostics diag_;
  uint32_t last_activity_ms_ = 0;
  bool started_ = false;
};

}  // namespace drivers
