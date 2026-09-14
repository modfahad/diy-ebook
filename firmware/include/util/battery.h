// battery.h -- battery percentage and a self-calibrating remaining-runtime
// estimate.
//
// WHY NOT AN ASSUMED AVERAGE CURRENT: this board has no current sensor, only
// battery voltage (once its ADC pin is resolved -- see board_crowpanel_579.h
// kBatteryAdcPin, currently -1/unknown). A hardcoded "assume N mA" constant
// would just be a guess that is wrong until someone measures it. Instead the
// remaining-time estimate comes from the device's own observed history: how
// fast the percentage has actually been falling, in (percent, real-elapsed-
// ms) samples. Battery capacity in mAh never enters this formula -- percent
// already normalises for it, the same way a phone estimates "6h left"
// without knowing its own cell's exact mAh.
//
// Pure and host-testable. The RTC-persisted BatteryEstimator instance and the
// periodic sampling wake that feeds it live in main.cpp.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace util {

// Single-cell LiPo, resting millivolts -> percent, piecewise-linear between
// commonly published discharge-curve breakpoints. Approximate and NOT
// calibrated against this board's actual pack -- retune once real
// measurements exist (docs/pending.md). Clamps outside the curve's range.
inline uint8_t BatteryPercentFromMillivolts(uint32_t mv) {
  static const struct {
    uint32_t mv;
    uint8_t percent;
  } kCurve[] = {
      {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80},
      {3980, 75},  {3950, 70}, {3910, 65}, {3870, 60}, {3850, 55},
      {3840, 50},  {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30},
      {3750, 25},  {3730, 20}, {3690, 15}, {3610, 10}, {3500, 5},
      {3000, 0},
  };
  constexpr int kCount = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv >= kCurve[0].mv) return 100;
  if (mv <= kCurve[kCount - 1].mv) return 0;
  for (int i = 1; i < kCount; ++i) {
    if (mv >= kCurve[i].mv) {
      const uint32_t hi_mv = kCurve[i - 1].mv;
      const uint32_t lo_mv = kCurve[i].mv;
      const uint8_t hi_pct = kCurve[i - 1].percent;
      const uint8_t lo_pct = kCurve[i].percent;
      const float t = static_cast<float>(mv - lo_mv) /
                      static_cast<float>(hi_mv - lo_mv);
      return static_cast<uint8_t>(lo_pct + t * (hi_pct - lo_pct));
    }
  }
  return 0;
}

// Self-calibrating remaining-runtime estimate from a short ring of
// (percent, elapsed_ms) samples. `elapsed_ms` is the caller's own cumulative
// real-time clock (see main.cpp), never millis() directly -- millis() resets
// on every deep-sleep wake, so on its own it cannot span a multi-day
// estimate.
//
// RTC-memory-safe, deliberately more strictly than util::RotaryCoalescer:
// no member has an in-class initializer (not even `= 0`), because ONE would
// make the implicitly-defined default constructor non-trivial -- and a
// non-trivial constructor runs at every boot, including a deep-sleep wake
// (crt0 reinitialises normally, and only *skips* that for memory it knows is
// preserved -- which it does at the section level, so a class instance is
// no exception). A trivial default constructor runs no code at all: a
// global `RTC_DATA_ATTR util::BatteryEstimator` (no initializer at the
// declaration either) is zero-initialised once, at cold boot, and every
// later wake leaves it exactly as this session last wrote it. begin() below
// is what a caller uses to get a known-zero starting point instead.
class BatteryEstimator {
 public:
  static constexpr uint8_t kCapacity = 8;
  // Fewer samples, or a shorter span between the oldest and newest, is more
  // ADC noise and battery-curve settling than signal -- wait rather than
  // report a wild first guess.
  static constexpr uint8_t kMinSamples = 3;

  // Not called by main.cpp's RTC-persisted global -- that one relies on the
  // static zero-init a trivial type gets once, at cold boot, and must NOT be
  // reset on every wake or the whole point of persisting it is lost. This is
  // for anyone using the type as an ordinary local/member instead, where a
  // bare `BatteryEstimator e;` default-initializes to indeterminate values,
  // same reason every util:: type here has an explicit begin().
  void begin() {
    count_ = 0;
    next_ = 0;
  }

  // Only stores a sample when percent has actually moved since the last one
  // kept. Sampling on a fixed 30-minute clock (see main.cpp), a kCapacity=8
  // ring of *every* call would span ~3.5h -- for a multi-day pack that is
  // under a percentage point of real drain, dominated by ADC noise and the
  // flat part of the discharge curve rather than signal. Storing only on
  // change instead makes the ring span however long it takes to see ~8
  // points of real drain -- self-scaling to the pack's actual rate, and
  // never wasting a ring slot on "nothing happened."
  void addSample(uint8_t percent, uint64_t elapsed_ms) {
    if (count_ > 0 && percent == latestPercent()) return;
    ring_[next_] = Sample{percent, elapsed_ms};
    next_ = static_cast<uint8_t>((next_ + 1) % kCapacity);
    if (count_ < kCapacity) ++count_;
  }

  bool hasSamples() const { return count_ > 0; }

  // False while there are too few samples, or the visible window shows a
  // flat/charging battery (rate <= 0) -- a falling percentage is what makes
  // "time remaining" a meaningful question.
  bool hasEstimate() const { return count_ >= kMinSamples && percentPerHour() > 0.0f; }

  uint8_t latestPercent() const {
    return count_ == 0 ? 0 : ring_[index(/*newest=*/true)].percent;
  }

  // Percent lost per hour, averaged over the whole visible ring (oldest vs
  // newest sample). 0 if there are fewer than 2 samples, or they land on the
  // same instant.
  float percentPerHour() const {
    if (count_ < 2) return 0.0f;
    const Sample& oldest = ring_[index(false)];
    const Sample& newest = ring_[index(true)];
    if (newest.elapsed_ms <= oldest.elapsed_ms) return 0.0f;
    const float hours =
        static_cast<float>(newest.elapsed_ms - oldest.elapsed_ms) / 3600000.0f;
    return (static_cast<float>(oldest.percent) - static_cast<float>(newest.percent)) /
           hours;
  }

  // Estimated milliseconds until 0%, or UINT32_MAX ("unknown") if
  // hasEstimate() is false -- never a bogus multi-year runtime from a
  // near-zero rate.
  uint32_t remainingMs() const {
    if (!hasEstimate()) return UINT32_MAX;
    const float hours = static_cast<float>(latestPercent()) / percentPerHour();
    const float ms = hours * 3600000.0f;
    if (ms >= static_cast<float>(UINT32_MAX)) return UINT32_MAX - 1;
    return static_cast<uint32_t>(ms);
  }

 private:
  // No in-class initializers here either, for the same reason as the class
  // comment: this struct is what ring_ is made of, so it has to stay trivial
  // too, or ring_'s own array member (still inside BatteryEstimator) would
  // make BatteryEstimator's constructor non-trivial regardless of anything
  // done at the outer level.
  struct Sample {
    uint8_t percent;
    uint64_t elapsed_ms;
  };

  uint8_t index(bool newest) const {
    if (newest) return static_cast<uint8_t>((next_ + kCapacity - 1) % kCapacity);
    return static_cast<uint8_t>(count_ < kCapacity ? 0 : next_);
  }

  Sample ring_[kCapacity];
  uint8_t count_;  // samples ever added, saturates at kCapacity
  uint8_t next_;   // ring write cursor
};

// Formats `remaining_ms` as "<days>d, <hours>h" once at least a day remains,
// or "<hours>h" alone under a day. BatteryEstimator's UINT32_MAX ("unknown")
// sentinel formats as "--". NUL-terminated; returns the snprintf-style
// would-be length.
inline int FormatRemaining(uint32_t remaining_ms, char* out, size_t capacity) {
  if (remaining_ms == UINT32_MAX) {
    return snprintf(out, capacity, "--");
  }
  const uint32_t total_hours = remaining_ms / 3600000u;
  const uint32_t days = total_hours / 24u;
  const uint32_t hours = total_hours % 24u;
  if (days > 0) {
    return snprintf(out, capacity, "%lud, %luh", static_cast<unsigned long>(days),
                    static_cast<unsigned long>(hours));
  }
  return snprintf(out, capacity, "%luh", static_cast<unsigned long>(hours));
}

}  // namespace util
