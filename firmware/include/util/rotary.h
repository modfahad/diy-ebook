// rotary.h -- pure, host-testable rotary decoding plus the event coalescer
// that keeps E-Ink refreshes down (spec section 14).
//
// The CrowPanel wheel is NOT a quadrature encoder: Elecrow's "PRV"/"NEXT"
// naming was correct and literal. This was settled on real hardware by
// running a quadrature decoder and this pulse decoder side by side and
// logging both against the raw pin levels -- see bring-up-log.md section
// 2.1. Only the pulse interpretation is implemented now.

#pragma once

#include <stdint.h>

namespace util {

// A and B are two independent momentary lines, each falling edge being one
// step in its own direction.
class PulseDecoder {
 public:
  void begin(bool a, bool b) {
    last_a_ = a;
    last_b_ = b;
    position_ = 0;
  }

  int8_t update(bool a, bool b) {
    int8_t step = 0;
    if (last_a_ && !a) step -= 1;  // A fell -> counter-clockwise
    if (last_b_ && !b) step += 1;  // B fell -> clockwise
    last_a_ = a;
    last_b_ = b;
    position_ += step;
    return step;
  }

  int32_t position() const { return position_; }

 private:
  bool last_a_ = true;
  bool last_b_ = true;
  int32_t position_ = 0;
};

// Spec section 14: a fast spin must not produce one full E-Ink refresh per
// detent. Steps are accumulated and only released once the user pauses for
// `quiet_ms`, or once `max_pending` steps have piled up (so a long spin still
// gives feedback rather than nothing at all).
class RotaryCoalescer {
 public:
  void begin(uint16_t quiet_ms, int16_t max_pending) {
    quiet_ms_ = quiet_ms;
    max_pending_ = max_pending;
    pending_ = 0;
    last_step_ms_ = 0;
  }

  void add(int16_t steps, uint32_t now_ms) {
    if (steps == 0) return;
    // A direction reversal is a deliberate correction; drop what is queued so
    // the user does not overshoot twice.
    if ((pending_ > 0 && steps < 0) || (pending_ < 0 && steps > 0)) {
      pending_ = 0;
    }
    pending_ += steps;
    last_step_ms_ = now_ms;
  }

  bool pending() const { return pending_ != 0; }

  bool ready(uint32_t now_ms) const {
    if (pending_ == 0) return false;
    if (pending_ >= max_pending_ || pending_ <= -max_pending_) return true;
    return static_cast<uint32_t>(now_ms - last_step_ms_) >= quiet_ms_;
  }

  int16_t take() {
    const int16_t out = pending_;
    pending_ = 0;
    return out;
  }

 private:
  int16_t pending_ = 0;
  int16_t max_pending_ = 8;
  uint16_t quiet_ms_ = 0;
  uint32_t last_step_ms_ = 0;
};

}  // namespace util
