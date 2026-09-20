// Host unit tests for the parts of Milestone 1 that can be tested without
// hardware.
//
// Scope, per the development rules in the spec:
//   * item 12, rotary encoder bounce/debouncing   -- covered here
//   * item 11, deep sleep / wake                  -- the DECISION logic
//     (idle policy) and the EXT1 wake-mask constraints are covered here; the
//     sleep transition itself is a hardware test, see docs/development.md
//   * the canvas pixel mapping, because the dual-controller seam offset is
//     the single most error-prone piece of pure code in the milestone
//
// Deliberately NOT covered: corrupted packages, interrupted transfers, power
// loss during install. There is no package format or transport yet; those
// tests arrive with Milestone 2 and Milestone 3.

#include <unity.h>

#include <string.h>

#include <string>

#include "board/board_crowpanel_579.h"
#include "gfx/canvas.h"
#include "util/screen_setup.h"
#include "util/battery.h"
#include "util/debouncer.h"
#include "util/idle_policy.h"
#include "util/rotary.h"
#include "util/text_pager.h"
#include "util/touch.h"

// ---------------------------------------------------------------------------
// Debouncer
// ---------------------------------------------------------------------------

void test_debouncer_ignores_short_bounce() {
  util::Debouncer d;
  d.begin(true, 25);

  TEST_ASSERT_FALSE(d.update(false, 1));   // first dip
  TEST_ASSERT_FALSE(d.update(true, 5));    // bounced back
  TEST_ASSERT_FALSE(d.update(false, 6));   // dips again, timer restarts
  TEST_ASSERT_FALSE(d.update(false, 20));  // only 14 ms stable
  TEST_ASSERT_TRUE(d.level());             // still idle-high
}

void test_debouncer_accepts_stable_change() {
  util::Debouncer d;
  d.begin(true, 25);
  TEST_ASSERT_FALSE(d.update(false, 6));
  TEST_ASSERT_TRUE(d.update(false, 31));   // 25 ms stable -> accepted
  TEST_ASSERT_FALSE(d.level());
  TEST_ASSERT_FALSE(d.update(false, 60));  // no repeat event
}

void test_debouncer_survives_millis_wraparound() {
  util::Debouncer d;
  d.begin(true, 25);
  const uint32_t near_wrap = 0xFFFFFFF0u;
  TEST_ASSERT_FALSE(d.update(false, near_wrap));
  // near_wrap + 30 wraps past zero.
  TEST_ASSERT_TRUE(d.update(false, static_cast<uint32_t>(near_wrap + 30)));
  TEST_ASSERT_FALSE(d.level());
}

// ---------------------------------------------------------------------------
// Button: click vs long press
// ---------------------------------------------------------------------------

namespace {
int kEv(util::ButtonEvent e) { return static_cast<int>(e); }
}  // namespace

void test_button_click_ignores_contact_bounce() {
  util::Button b;
  b.begin(/*active_low=*/true, /*initial_raw_level=*/true, 25, 700);

  // Bouncy press.
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 0)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(true, 3)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 5)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kDown), kEv(b.update(false, 30)));
  TEST_ASSERT_TRUE(b.pressed());

  // Bouncy release.
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(true, 100)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 104)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(true, 108)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kClick), kEv(b.update(true, 140)));
  TEST_ASSERT_FALSE(b.pressed());
}

void test_button_long_press_reported_once_then_up() {
  util::Button b;
  b.begin(true, true, 25, 700);
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 0)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kDown), kEv(b.update(false, 30)));

  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 700)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kLongPress), kEv(b.update(false, 730)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 900)));

  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(true, 1000)));
  // Released after a long press -> kUp, not a second kClick.
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kUp), kEv(b.update(true, 1040)));
}

void test_button_held_at_begin_is_not_a_new_long_press() {
  // This is the wake case: the button that woke the device is still held when
  // begin() runs, and the first poll() can be seconds later because the panel
  // init sequence sits in between. That must not be reported as a long press
  // (Phase 2 binds long press to a context action / bookmark).
  util::Button b;
  b.begin(/*active_low=*/true, /*initial_raw_level=*/false, 25, 700);
  TEST_ASSERT_TRUE(b.pressed());

  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 6000)));

  // Releasing it is a kUp, not a phantom kClick.
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(true, 6010)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kUp), kEv(b.update(true, 6040)));

  // A genuine long press after that still works.
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kNone), kEv(b.update(false, 7000)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kDown), kEv(b.update(false, 7030)));
  TEST_ASSERT_EQUAL_INT(kEv(util::ButtonEvent::kLongPress),
                        kEv(b.update(false, 7740)));
}

// ---------------------------------------------------------------------------
// Rotary pulse decoding
// ---------------------------------------------------------------------------

void test_pulse_decoder_counts_falling_edges() {
  util::PulseDecoder d;
  d.begin(true, true);
  TEST_ASSERT_EQUAL_INT8(1, d.update(true, false));   // B fell -> +1
  TEST_ASSERT_EQUAL_INT8(0, d.update(true, true));
  TEST_ASSERT_EQUAL_INT8(-1, d.update(false, true));  // A fell -> -1
  TEST_ASSERT_EQUAL_INT32(0, d.position());
}

// ---------------------------------------------------------------------------
// Rotary coalescing (spec section 14)
// ---------------------------------------------------------------------------

void test_coalescer_waits_for_a_pause() {
  util::RotaryCoalescer c;
  c.begin(/*quiet_ms=*/140, /*max_pending=*/6);

  c.add(1, 0);
  TEST_ASSERT_FALSE(c.ready(0));
  c.add(1, 50);
  TEST_ASSERT_FALSE(c.ready(100));   // still spinning
  TEST_ASSERT_TRUE(c.ready(200));    // 150 ms since the last detent
  TEST_ASSERT_EQUAL_INT16(2, c.take());
  TEST_ASSERT_FALSE(c.pending());
}

void test_coalescer_releases_at_max_pending() {
  util::RotaryCoalescer c;
  c.begin(140, 6);
  for (int i = 0; i < 6; ++i) c.add(1, static_cast<uint32_t>(i));
  TEST_ASSERT_TRUE(c.ready(6));      // no pause needed once the cap is hit
  TEST_ASSERT_EQUAL_INT16(6, c.take());
}

void test_coalescer_drops_queue_on_direction_reversal() {
  util::RotaryCoalescer c;
  c.begin(140, 6);
  c.add(3, 0);
  c.add(-1, 10);   // user corrected course
  TEST_ASSERT_TRUE(c.ready(200));
  TEST_ASSERT_EQUAL_INT16(-1, c.take());
}

// ---------------------------------------------------------------------------
// Idle / sleep policy  (the decision half of "deep sleep + wake")
// ---------------------------------------------------------------------------

void test_idle_policy_sleeps_after_timeout() {
  util::IdlePolicy p;
  p.begin(1000, 0);
  TEST_ASSERT_FALSE(p.shouldSleep(999, false));
  TEST_ASSERT_TRUE(p.shouldSleep(1000, false));
}

void test_idle_policy_never_sleeps_while_a_pin_is_held() {
  util::IdlePolicy p;
  p.begin(1000, 0);
  // Entering deep sleep while a wake-source pin is still low would wake the
  // device again immediately.
  TEST_ASSERT_FALSE(p.shouldSleep(5000, true));
}

void test_idle_policy_never_sleeps_while_busy() {
  util::IdlePolicy p;
  p.begin(1000, 0);
  p.setBusy(true);
  TEST_ASSERT_FALSE(p.shouldSleep(5000, false));
  p.setBusy(false);
  TEST_ASSERT_TRUE(p.shouldSleep(5000, false));
}

void test_idle_policy_activity_defers_sleep() {
  util::IdlePolicy p;
  p.begin(1000, 0);
  p.noteActivity(900);
  TEST_ASSERT_FALSE(p.shouldSleep(1500, false));
  TEST_ASSERT_EQUAL_UINT32(400, p.remainingMs(1500));
  TEST_ASSERT_TRUE(p.shouldSleep(1900, false));
}

void test_idle_policy_survives_millis_wraparound() {
  util::IdlePolicy p;
  const uint32_t near_wrap = 0xFFFFFF00u;
  p.begin(1000, near_wrap);
  TEST_ASSERT_FALSE(p.shouldSleep(static_cast<uint32_t>(near_wrap + 999),
                                  false));
  TEST_ASSERT_TRUE(p.shouldSleep(static_cast<uint32_t>(near_wrap + 1000),
                                 false));
}

// ---------------------------------------------------------------------------
// Wake-source constraints
// ---------------------------------------------------------------------------

void test_every_wake_pin_is_rtc_capable() {
  // ESP32-S3 EXT1 only works on GPIO0..21. If a pin ever moves outside that
  // range EXT1 wake silently stops working, so assert it here as well as at
  // compile time.
  const int pins[] = {board::kBtnMenu, board::kBtnExit, board::kEncoderSw,
                      board::kEncoderA, board::kEncoderB};
  for (int pin : pins) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pin);
    TEST_ASSERT_LESS_OR_EQUAL_INT(board::kMaxRtcGpio, pin);
  }
}

void test_wake_mask_covers_exactly_the_front_panel() {
  uint64_t expected = 0;
  expected |= 1ULL << board::kBtnMenu;
  expected |= 1ULL << board::kBtnExit;
  expected |= 1ULL << board::kEncoderSw;
  expected |= 1ULL << board::kEncoderA;
  expected |= 1ULL << board::kEncoderB;
  TEST_ASSERT_TRUE(expected == board::kWakeMask);

  // Neither power-enable rail may be a wake source.
  TEST_ASSERT_TRUE((board::kWakeMask & (1ULL << board::kPanelPowerEn)) == 0);
  TEST_ASSERT_TRUE((board::kWakeMask & (1ULL << board::kSdPowerEn)) == 0);
}

// ---------------------------------------------------------------------------
// Canvas: rotation, clipping, and the (generic) two-controller seam
// ---------------------------------------------------------------------------

namespace {
uint8_t g_buffer[board::kFramebufferBytes];

// Same shape as the product build's canvas (drivers::Epd750Display):
// logical 800x480, no seam. GxEPD2 applies the panel rotation itself, so the
// product passes rotation 0; 180 is still exercised because Canvas supports it.
gfx::Canvas makeCanvas(uint16_t rotation) {
  return gfx::Canvas(g_buffer, board::kWidth, board::kHeight,
                     board::kWidth, board::kHeight,
                     board::kStrideBytes, rotation, /*seam_x=*/0);
}

bool pixelIsBlack(uint32_t byte_index, uint8_t bit_mask) {
  return (g_buffer[byte_index] & bit_mask) == 0;
}
}  // namespace

void test_canvas_clear_fills_the_whole_buffer() {
  gfx::Canvas c = makeCanvas(0);
  c.clear(gfx::kWhite);
  TEST_ASSERT_EQUAL_UINT32(board::kFramebufferBytes, c.bufferSize());
  TEST_ASSERT_EQUAL_UINT8(0xFF, g_buffer[0]);
  TEST_ASSERT_EQUAL_UINT8(0xFF, g_buffer[board::kFramebufferBytes - 1]);
}

void test_canvas_rotation_zero_maps_origin_to_first_bit() {
  gfx::Canvas c = makeCanvas(0);
  c.clear(gfx::kWhite);
  c.setPixel(0, 0, gfx::kBlack);
  TEST_ASSERT_TRUE(pixelIsBlack(0, 0x80));
}

void test_canvas_rotation_180_maps_origin_to_last_bit() {
  gfx::Canvas c = makeCanvas(180);
  c.clear(gfx::kWhite);
  c.setPixel(0, 0, gfx::kBlack);
  // x=0 -> memory column 799 -> byte 99 of the last row (479).
  const uint32_t index = 99 + 479u * board::kStrideBytes;
  TEST_ASSERT_TRUE(pixelIsBlack(index, 0x01));
}

void test_canvas_seam_skips_eight_columns() {
  // The seam is a generic Canvas option for two-controller panels. The
  // GDEY075T7 does not use it, so this pins the mapping with the geometry of
  // the panel that did -- the CrowPanel's original 5.79": 792x272 logical,
  // 800x272 controller RAM, seam at x = 396.
  gfx::Canvas c(g_buffer, 792, 272, 800, 272, 100, 180, 396);
  c.clear(gfx::kWhite);

  // The last column of the master controller and the first column of the
  // slave must land in adjacent source bytes 50 and 49 (rotation 180 mirrors
  // the order), never in the 8-column dead gap between them.
  c.setPixel(395, 0, gfx::kBlack);
  c.setPixel(396, 0, gfx::kBlack);   // +8 skip

  const uint32_t row = 271u * 100u;
  TEST_ASSERT_TRUE(pixelIsBlack(row + 50, 0x08));  // x=395 -> memory col 404
  TEST_ASSERT_TRUE(pixelIsBlack(row + 49, 0x10));  // x=396 -> memory col 395
}

void test_canvas_rejects_out_of_bounds_pixels() {
  gfx::Canvas c = makeCanvas(180);
  c.clear(gfx::kWhite);
  c.setPixel(-1, 0, gfx::kBlack);
  c.setPixel(board::kWidth, 0, gfx::kBlack);
  c.setPixel(0, board::kHeight, gfx::kBlack);
  for (uint32_t i = 0; i < board::kFramebufferBytes; ++i) {
    if (g_buffer[i] != 0xFF) {
      TEST_FAIL_MESSAGE("out-of-bounds pixel wrote into the framebuffer");
    }
  }
  TEST_ASSERT_EQUAL_UINT32(3, c.clippedPixels());
}

void test_canvas_clear_resets_the_clip_count() {
  gfx::Canvas c = makeCanvas(0);
  c.clear(gfx::kWhite);
  c.drawText(static_cast<int>(board::kWidth) - 5, 0, "AB", 2, gfx::kBlack);
  TEST_ASSERT_TRUE(c.clippedPixels() > 0);  // ran off the right edge

  c.clear(gfx::kWhite);
  TEST_ASSERT_EQUAL_UINT32(0, c.clippedPixels());
  c.drawText(0, 0, "AB", 2, gfx::kBlack);
  TEST_ASSERT_EQUAL_UINT32(0, c.clippedPixels());
}

void test_canvas_text_metrics() {
  // Advance is (5 + 1) * scale per glyph, minus the trailing gap.
  TEST_ASSERT_EQUAL_INT(0, gfx::Canvas::textWidth("", 2));
  TEST_ASSERT_EQUAL_INT(10, gfx::Canvas::textWidth("A", 2));
  TEST_ASSERT_EQUAL_INT(22, gfx::Canvas::textWidth("AB", 2));
  TEST_ASSERT_EQUAL_INT(14, gfx::Canvas::textHeight(2));
}

void test_canvas_text_stays_inside_the_panel() {
  gfx::Canvas c = makeCanvas(180);
  c.clear(gfx::kWhite);
  // A long footer at body scale must fit the 800 px width.
  const char* footer = "MENU=refresh EXIT=sleep turn=count fw 0.1.0-m1";
  TEST_ASSERT_LESS_OR_EQUAL_INT(static_cast<int>(board::kWidth),
                                gfx::Canvas::textWidth(footer, 2));
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------------

void test_battery_percent_from_millivolts_matches_curve_endpoints() {
  TEST_ASSERT_EQUAL_UINT8(100, util::BatteryPercentFromMillivolts(4200));
  TEST_ASSERT_EQUAL_UINT8(100, util::BatteryPercentFromMillivolts(5000));  // clamps high
  TEST_ASSERT_EQUAL_UINT8(0, util::BatteryPercentFromMillivolts(3000));
  TEST_ASSERT_EQUAL_UINT8(0, util::BatteryPercentFromMillivolts(0));      // clamps low
  TEST_ASSERT_EQUAL_UINT8(50, util::BatteryPercentFromMillivolts(3840));
}

void test_battery_percent_interpolates_between_breakpoints() {
  // Halfway between the 3840mV/50% and 3820mV/45% breakpoints.
  const uint8_t p = util::BatteryPercentFromMillivolts(3830);
  TEST_ASSERT_TRUE(p >= 46 && p <= 49);
}

void test_battery_estimator_has_no_estimate_before_min_samples() {
  util::BatteryEstimator e;
  e.begin();
  TEST_ASSERT_FALSE(e.hasSamples());
  e.addSample(80, 0);
  TEST_ASSERT_TRUE(e.hasSamples());
  TEST_ASSERT_FALSE(e.hasEstimate());
  e.addSample(78, 3600000);
  TEST_ASSERT_FALSE(e.hasEstimate());  // still below kMinSamples
  e.addSample(76, 7200000);
  TEST_ASSERT_TRUE(e.hasEstimate());
}

void test_battery_estimator_rate_and_remaining_from_steady_drain() {
  // 2% per hour, three samples an hour apart -- textbook steady drain.
  util::BatteryEstimator e;
  e.begin();
  e.addSample(80, 0);
  e.addSample(78, 3600000);
  e.addSample(76, 7200000);
  TEST_ASSERT_TRUE(e.hasEstimate());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, e.percentPerHour());
  TEST_ASSERT_EQUAL_UINT8(76, e.latestPercent());
  // 76% / 2%/h = 38h = 1d, 14h.
  const uint32_t remaining = e.remainingMs();
  TEST_ASSERT_UINT32_WITHIN(60000, 38ul * 3600000ul, remaining);
}

void test_battery_estimator_ring_forgets_the_oldest_sample() {
  // Feed one more than kCapacity: the very first sample (100%, t=0) must age
  // out, or a long-uptime device would never adapt to a changed usage
  // pattern.
  util::BatteryEstimator e;
  e.begin();
  for (uint8_t i = 0; i < util::BatteryEstimator::kCapacity + 1; ++i) {
    e.addSample(static_cast<uint8_t>(100 - i), i * 3600000ul);
  }
  // Oldest surviving sample is now index 1 (99%, t=1h), not index 0.
  TEST_ASSERT_TRUE(e.hasEstimate());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, e.percentPerHour());
}

void test_battery_estimator_dedupes_flat_samples_and_scales_to_real_drain() {
  // Simulates main.cpp's real sampling cadence (30min) on a slow-draining
  // 20-day pack: ~5%/day means most consecutive 30-minute reads report the
  // *same* percent. A ring that stored every call would span only
  // kCapacity*30min (~3.5h) -- under 1% of drain, noise-dominated. Deduping
  // flat reads should instead let the ring span however long real drain
  // takes, and still land on the simulated rate.
  util::BatteryEstimator e;
  e.begin();
  const uint32_t kIntervalMs = 30ul * 60ul * 1000ul;
  uint64_t elapsed = 0;
  // 10 samples per percentage point * 30min = 5h/point = 0.2%/h, ~4.8d/pack.
  for (uint8_t percent = 100; percent > 90; --percent) {
    for (int i = 0; i < 10; ++i) {
      e.addSample(percent, elapsed);
      elapsed += kIntervalMs;
    }
  }
  TEST_ASSERT_TRUE(e.hasEstimate());
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.2f, e.percentPerHour());
}

void test_battery_estimator_flat_or_charging_has_no_estimate() {
  util::BatteryEstimator e;
  e.begin();
  e.addSample(80, 0);
  e.addSample(80, 3600000);
  e.addSample(85, 7200000);  // charging
  TEST_ASSERT_FALSE(e.hasEstimate());
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, e.remainingMs());
}

void test_format_remaining_switches_from_days_to_hours_under_a_day() {
  char buf[32];
  util::FormatRemaining(20ul * 24 * 3600000ul + 12ul * 3600000ul, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("20d, 12h", buf);

  util::FormatRemaining(14ul * 3600000ul, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("14h", buf);

  util::FormatRemaining(23ul * 3600000ul + 59ul * 60000ul, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("23h", buf);  // rounds down, does not roll to 1d

  util::FormatRemaining(UINT32_MAX, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("--", buf);
}

// ---------------------------------------------------------------------------
// Text pagination
// ---------------------------------------------------------------------------

namespace {

std::string LineText(const char* body, const util::LineSlice& slice) {
  return std::string(body + slice.offset, slice.length);
}

}  // namespace

void test_pager_wraps_at_the_last_space_that_fits() {
  const char* body = "the quick brown fox jumps";
  util::TextPager pager;
  pager.reset(body, strlen(body), 10, 4);

  util::LineSlice lines[4];
  const uint8_t n = pager.linesForPage(1, lines, 4);
  TEST_ASSERT_EQUAL_UINT8(3, n);
  // Never mid-word, and never past the width.
  TEST_ASSERT_EQUAL_STRING("the quick", LineText(body, lines[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("brown fox", LineText(body, lines[1]).c_str());
  TEST_ASSERT_EQUAL_STRING("jumps", LineText(body, lines[2]).c_str());
  for (uint8_t i = 0; i < n; ++i) {
    TEST_ASSERT_TRUE(lines[i].length <= 10);
  }
}

void test_pager_hard_splits_a_word_longer_than_the_line() {
  // A word that cannot fit must be broken rather than allowed to run off the
  // glass, where it would simply be invisible.
  const char* body = "supercalifragilistic";
  util::TextPager pager;
  pager.reset(body, strlen(body), 8, 4);

  util::LineSlice lines[4];
  const uint8_t n = pager.linesForPage(1, lines, 4);
  TEST_ASSERT_EQUAL_UINT8(3, n);
  TEST_ASSERT_EQUAL_STRING("supercal", LineText(body, lines[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("ifragili", LineText(body, lines[1]).c_str());
  TEST_ASSERT_EQUAL_STRING("stic", LineText(body, lines[2]).c_str());
}

void test_pager_keeps_paragraph_breaks() {
  // A blank line survives as a zero-length slice; collapsing it would run
  // paragraphs together.
  const char* body = "one\n\ntwo";
  util::TextPager pager;
  pager.reset(body, strlen(body), 20, 4);

  util::LineSlice lines[4];
  const uint8_t n = pager.linesForPage(1, lines, 4);
  TEST_ASSERT_EQUAL_UINT8(3, n);
  TEST_ASSERT_EQUAL_STRING("one", LineText(body, lines[0]).c_str());
  TEST_ASSERT_EQUAL_UINT16(0, lines[1].length);
  TEST_ASSERT_EQUAL_STRING("two", LineText(body, lines[2]).c_str());
}

void test_pager_page_count_and_last_page() {
  const char* body = "a b c d e f g";   // seven one-character words
  util::TextPager pager;
  pager.reset(body, strlen(body), 3, 2);   // "a b" / "c d" / "e f" / "g"

  TEST_ASSERT_EQUAL_UINT32(4, pager.lineCount());
  TEST_ASSERT_EQUAL_UINT16(2, pager.pageCount());

  util::LineSlice lines[2];
  TEST_ASSERT_EQUAL_UINT8(2, pager.linesForPage(1, lines, 2));
  TEST_ASSERT_EQUAL_STRING("a b", LineText(body, lines[0]).c_str());

  // The last page is short, and must report how many lines it really has.
  TEST_ASSERT_EQUAL_UINT8(2, pager.linesForPage(2, lines, 2));
  TEST_ASSERT_EQUAL_STRING("g", LineText(body, lines[1]).c_str());

  // Past the end yields nothing rather than repeating the last page.
  TEST_ASSERT_EQUAL_UINT8(0, pager.linesForPage(3, lines, 2));
}

void test_pager_handles_degenerate_input() {
  util::TextPager pager;

  pager.reset(nullptr, 0, 10, 3);
  TEST_ASSERT_EQUAL_UINT16(1, pager.pageCount());   // never zero pages
  util::LineSlice lines[3];
  TEST_ASSERT_EQUAL_UINT8(0, pager.linesForPage(1, lines, 3));

  const char* body = "text";
  pager.reset(body, strlen(body), 0, 0);   // both clamped to 1
  TEST_ASSERT_TRUE(pager.pageCount() >= 1);
  TEST_ASSERT_EQUAL_UINT8(0, pager.linesForPage(0, lines, 3));
}

void test_pager_repagination_is_stable_across_widths() {
  // Changing the glyph size re-wraps everything; a narrower line can only
  // ever need at least as many lines as a wider one.
  const char* body =
      "the measured line of a longer passage here and a shorter one";
  util::TextPager wide;
  util::TextPager narrow;
  wide.reset(body, strlen(body), 40, 3);
  narrow.reset(body, strlen(body), 20, 3);
  TEST_ASSERT_TRUE(narrow.lineCount() >= wide.lineCount());
  TEST_ASSERT_TRUE(narrow.pageCount() >= wide.pageCount());
}

void setUp() {}
void tearDown() {}


// ---------------------------------------------------------------------------
// Touch: coordinate mapping and tap classification (util/touch.h)
//
// Every one of these is a question the panel itself will answer once
// src/touch_test.cpp has been run (the orientation flags), but the mapping
// arithmetic and the tap rules have to be right whatever the answer is.
// ---------------------------------------------------------------------------

namespace {

util::TouchMapping PanelMapping(bool swap, bool inv_x, bool inv_y) {
  util::TouchMapping m;
  m.raw_width = 800;
  m.raw_height = 480;
  m.out_width = 800;
  m.out_height = 480;
  m.swap_xy = swap;
  m.invert_x = inv_x;
  m.invert_y = inv_y;
  return m;
}

}  // namespace

void test_touch_mapping_is_identity_when_nothing_is_flipped() {
  const util::TouchMapping m = PanelMapping(false, false, false);
  util::TouchXY p = util::MapTouchPoint(0, 0, m);
  TEST_ASSERT_EQUAL_INT16(0, p.x);
  TEST_ASSERT_EQUAL_INT16(0, p.y);
  p = util::MapTouchPoint(799, 479, m);
  TEST_ASSERT_EQUAL_INT16(799, p.x);
  TEST_ASSERT_EQUAL_INT16(479, p.y);
  p = util::MapTouchPoint(400, 240, m);
  TEST_ASSERT_EQUAL_INT16(400, p.x);
  TEST_ASSERT_EQUAL_INT16(240, p.y);
}

void test_touch_mapping_mirrors_each_axis() {
  util::TouchXY p = util::MapTouchPoint(0, 0, PanelMapping(false, true, false));
  TEST_ASSERT_EQUAL_INT16(799, p.x);
  TEST_ASSERT_EQUAL_INT16(0, p.y);

  p = util::MapTouchPoint(0, 0, PanelMapping(false, false, true));
  TEST_ASSERT_EQUAL_INT16(0, p.x);
  TEST_ASSERT_EQUAL_INT16(479, p.y);

  // Upside down: the top-left corner reports as the bottom-right one.
  p = util::MapTouchPoint(0, 0, PanelMapping(false, true, true));
  TEST_ASSERT_EQUAL_INT16(799, p.x);
  TEST_ASSERT_EQUAL_INT16(479, p.y);
}

void test_touch_mapping_swaps_axes_with_their_own_ranges() {
  // A portrait-wired touch layer: the chip counts 0..479 across what the
  // display calls x. Scaling must use each axis's OWN range, or the corners
  // land off-panel.
  util::TouchMapping m = PanelMapping(true, false, false);
  m.raw_width = 480;
  m.raw_height = 800;

  util::TouchXY p = util::MapTouchPoint(0, 0, m);
  TEST_ASSERT_EQUAL_INT16(0, p.x);
  TEST_ASSERT_EQUAL_INT16(0, p.y);

  p = util::MapTouchPoint(479, 799, m);
  TEST_ASSERT_EQUAL_INT16(799, p.x);
  TEST_ASSERT_EQUAL_INT16(479, p.y);
}

void test_touch_mapping_clamps_and_survives_a_zero_resolution() {
  const util::TouchMapping m = PanelMapping(false, false, false);
  util::TouchXY p = util::MapTouchPoint(5000, 5000, m);
  TEST_ASSERT_EQUAL_INT16(799, p.x);
  TEST_ASSERT_EQUAL_INT16(479, p.y);

  // An unconfigured GT911 reports 0x0. The panel's own size stands in, and
  // nothing divides by zero.
  util::TouchMapping unconfigured = PanelMapping(false, false, false);
  unconfigured.raw_width = 0;
  unconfigured.raw_height = 0;
  p = util::MapTouchPoint(400, 240, unconfigured);
  TEST_ASSERT_EQUAL_INT16(400, p.x);
  TEST_ASSERT_EQUAL_INT16(240, p.y);
}

void test_touch_tap_reports_down_then_tap_at_the_landing_spot() {
  util::TapTracker t;
  t.begin(700, 24);

  TEST_ASSERT_EQUAL(util::TouchEvent::kDown, t.update(true, 100, 200, 1000));
  // A couple of pixels of wobble is still the same tap.
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 103, 198, 1050));
  TEST_ASSERT_EQUAL(util::TouchEvent::kTap, t.update(false, 0, 0, 1100));
  // The tap belongs where the finger landed, not where it drifted to.
  TEST_ASSERT_EQUAL_INT16(100, t.x());
  TEST_ASSERT_EQUAL_INT16(200, t.y());
  TEST_ASSERT_FALSE(t.down());
}

void test_touch_long_press_fires_once_while_the_finger_is_still_down() {
  util::TapTracker t;
  t.begin(700, 24);

  t.update(true, 400, 240, 0);
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 400, 240, 699));
  TEST_ASSERT_EQUAL(util::TouchEvent::kLongPress, t.update(true, 400, 240, 700));
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 400, 240, 1500));
  // Releasing after a long press is not also a tap -- that would run two
  // actions off one gesture.
  TEST_ASSERT_EQUAL(util::TouchEvent::kUp, t.update(false, 0, 0, 1600));
}

void test_touch_a_slide_is_neither_a_tap_nor_a_long_press() {
  util::TapTracker t;
  t.begin(700, 24);

  t.update(true, 100, 100, 0);
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 100, 140, 100));
  // Held well past the threshold, but it has moved: no long press.
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 100, 140, 900));
  TEST_ASSERT_EQUAL(util::TouchEvent::kUp, t.update(false, 0, 0, 1000));
  TEST_ASSERT_EQUAL_INT16(140, t.lastY());
}

void test_touch_a_finger_returning_to_the_start_is_still_a_slide() {
  // Slop is measured against the landing point at every step, so a swipe out
  // and back cannot come home and count as a tap.
  util::TapTracker t;
  t.begin(700, 24);

  t.update(true, 300, 300, 0);
  t.update(true, 300, 400, 50);
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 300, 300, 100));
  TEST_ASSERT_EQUAL(util::TouchEvent::kUp, t.update(false, 0, 0, 150));
}

void test_touch_second_tap_starts_clean_after_the_first() {
  util::TapTracker t;
  t.begin(700, 24);

  t.update(true, 10, 10, 0);
  t.update(true, 10, 200, 100);            // slides: not a tap
  TEST_ASSERT_EQUAL(util::TouchEvent::kUp, t.update(false, 0, 0, 200));

  TEST_ASSERT_EQUAL(util::TouchEvent::kDown, t.update(true, 500, 100, 300));
  TEST_ASSERT_EQUAL(util::TouchEvent::kTap, t.update(false, 0, 0, 350));
  TEST_ASSERT_EQUAL_INT16(500, t.x());
}

void test_touch_survives_millis_wraparound() {
  util::TapTracker t;
  t.begin(700, 24);

  const uint32_t near_wrap = 0xFFFFFF00u;
  t.update(true, 200, 200, near_wrap);
  // 0x100 ms later, the counter has wrapped through zero.
  TEST_ASSERT_EQUAL(util::TouchEvent::kNone, t.update(true, 200, 200, 0x50u));
  TEST_ASSERT_EQUAL(util::TouchEvent::kLongPress, t.update(true, 200, 200, 0x200u));
}


// ---------------------------------------------------------------------------
// The saved screen/touch orientation (util/screen_setup.h)
//
// Two numbers on the card that decide which way up the picture is and which
// way round touch is. They are written by a screen the user drives by hand,
// read at boot before the panel comes up, and may be edited by a human with a
// card reader -- so the parser has to survive whatever comes back.
// ---------------------------------------------------------------------------

void test_screen_setup_round_trips_through_text() {
  for (uint8_t touch = 0; touch < util::kTouchOrientationCount; ++touch) {
    for (uint8_t rotation = 0; rotation <= 2; rotation += 2) {
      util::ScreenSetup written;
      written.rotation = rotation;
      written.touch_orientation = touch;

      char text[util::kScreenSetupMaxChars + 1] = {0};
      const uint8_t n = util::FormatScreenSetup(written, text, sizeof(text) - 1);
      TEST_ASSERT_TRUE(n > 0);
      TEST_ASSERT_EQUAL_UINT8(n, static_cast<uint8_t>(strlen(text)));

      util::ScreenSetup read;
      TEST_ASSERT_TRUE(util::ParseScreenSetup(text, &read));
      TEST_ASSERT_EQUAL_UINT8(rotation, read.rotation);
      TEST_ASSERT_EQUAL_UINT8(touch, read.touch_orientation);
    }
  }
}

void test_screen_setup_flags_unpack_the_way_the_mapping_expects() {
  util::ScreenSetup setup;
  setup.touch_orientation = 0;
  TEST_ASSERT_FALSE(setup.swapXY());
  TEST_ASSERT_FALSE(setup.invertX());
  TEST_ASSERT_FALSE(setup.invertY());

  setup.touch_orientation = 7;
  TEST_ASSERT_TRUE(setup.swapXY());
  TEST_ASSERT_TRUE(setup.invertX());
  TEST_ASSERT_TRUE(setup.invertY());

  setup.touch_orientation = 4;
  TEST_ASSERT_FALSE(setup.swapXY());
  TEST_ASSERT_FALSE(setup.invertX());
  TEST_ASSERT_TRUE(setup.invertY());
}

void test_screen_setup_keeps_defaults_when_the_file_is_rubbish() {
  util::ScreenSetup setup;
  setup.rotation = 2;
  setup.touch_orientation = 5;

  // Nothing usable in any of these: the values already in `setup` stand,
  // which at boot means the board header's build-time defaults.
  TEST_ASSERT_FALSE(util::ParseScreenSetup("", &setup));
  TEST_ASSERT_FALSE(util::ParseScreenSetup("\n\n", &setup));
  TEST_ASSERT_FALSE(util::ParseScreenSetup("nonsense", &setup));
  TEST_ASSERT_FALSE(util::ParseScreenSetup("rotation=", &setup));
  TEST_ASSERT_FALSE(util::ParseScreenSetup("rotation=1 touch=9", &setup));
  TEST_ASSERT_FALSE(util::ParseScreenSetup("myrotation=0 mytouch=0", &setup));
  TEST_ASSERT_EQUAL_UINT8(2, setup.rotation);
  TEST_ASSERT_EQUAL_UINT8(5, setup.touch_orientation);

  // A half-valid line keeps the half that parsed.
  TEST_ASSERT_TRUE(util::ParseScreenSetup("rotation=0 touch=99", &setup));
  TEST_ASSERT_EQUAL_UINT8(0, setup.rotation);
  TEST_ASSERT_EQUAL_UINT8(5, setup.touch_orientation);
}

void test_screen_setup_tolerates_hand_editing() {
  util::ScreenSetup setup;
  TEST_ASSERT_TRUE(util::ParseScreenSetup("touch=3\nrotation=2\n", &setup));
  TEST_ASSERT_EQUAL_UINT8(2, setup.rotation);
  TEST_ASSERT_EQUAL_UINT8(3, setup.touch_orientation);

  util::ScreenSetup no_newline;
  TEST_ASSERT_TRUE(util::ParseScreenSetup("rotation=2 touch=1", &no_newline));
  TEST_ASSERT_EQUAL_UINT8(2, no_newline.rotation);
  TEST_ASSERT_EQUAL_UINT8(1, no_newline.touch_orientation);

  util::ScreenSetup with_extra;
  TEST_ASSERT_TRUE(
      util::ParseScreenSetup("# saved by the device\nrotation=0\ttouch=6\n",
                             &with_extra));
  TEST_ASSERT_EQUAL_UINT8(6, with_extra.touch_orientation);
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_debouncer_ignores_short_bounce);
  RUN_TEST(test_debouncer_accepts_stable_change);
  RUN_TEST(test_debouncer_survives_millis_wraparound);

  RUN_TEST(test_button_click_ignores_contact_bounce);
  RUN_TEST(test_button_long_press_reported_once_then_up);
  RUN_TEST(test_button_held_at_begin_is_not_a_new_long_press);

  RUN_TEST(test_pulse_decoder_counts_falling_edges);

  RUN_TEST(test_coalescer_waits_for_a_pause);
  RUN_TEST(test_coalescer_releases_at_max_pending);
  RUN_TEST(test_coalescer_drops_queue_on_direction_reversal);

  RUN_TEST(test_idle_policy_sleeps_after_timeout);
  RUN_TEST(test_idle_policy_never_sleeps_while_a_pin_is_held);
  RUN_TEST(test_idle_policy_never_sleeps_while_busy);
  RUN_TEST(test_idle_policy_activity_defers_sleep);
  RUN_TEST(test_idle_policy_survives_millis_wraparound);

  RUN_TEST(test_every_wake_pin_is_rtc_capable);
  RUN_TEST(test_wake_mask_covers_exactly_the_front_panel);

  RUN_TEST(test_canvas_clear_fills_the_whole_buffer);
  RUN_TEST(test_canvas_rotation_zero_maps_origin_to_first_bit);
  RUN_TEST(test_canvas_rotation_180_maps_origin_to_last_bit);
  RUN_TEST(test_canvas_seam_skips_eight_columns);
  RUN_TEST(test_canvas_rejects_out_of_bounds_pixels);
  RUN_TEST(test_canvas_clear_resets_the_clip_count);
  RUN_TEST(test_canvas_text_metrics);
  RUN_TEST(test_canvas_text_stays_inside_the_panel);

  RUN_TEST(test_battery_percent_from_millivolts_matches_curve_endpoints);
  RUN_TEST(test_battery_percent_interpolates_between_breakpoints);
  RUN_TEST(test_battery_estimator_has_no_estimate_before_min_samples);
  RUN_TEST(test_battery_estimator_rate_and_remaining_from_steady_drain);
  RUN_TEST(test_battery_estimator_ring_forgets_the_oldest_sample);
  RUN_TEST(test_battery_estimator_dedupes_flat_samples_and_scales_to_real_drain);
  RUN_TEST(test_battery_estimator_flat_or_charging_has_no_estimate);
  RUN_TEST(test_format_remaining_switches_from_days_to_hours_under_a_day);

  RUN_TEST(test_pager_wraps_at_the_last_space_that_fits);
  RUN_TEST(test_pager_hard_splits_a_word_longer_than_the_line);
  RUN_TEST(test_pager_keeps_paragraph_breaks);
  RUN_TEST(test_pager_page_count_and_last_page);
  RUN_TEST(test_pager_handles_degenerate_input);
  RUN_TEST(test_pager_repagination_is_stable_across_widths);

  RUN_TEST(test_touch_mapping_is_identity_when_nothing_is_flipped);
  RUN_TEST(test_touch_mapping_mirrors_each_axis);
  RUN_TEST(test_touch_mapping_swaps_axes_with_their_own_ranges);
  RUN_TEST(test_touch_mapping_clamps_and_survives_a_zero_resolution);
  RUN_TEST(test_touch_tap_reports_down_then_tap_at_the_landing_spot);
  RUN_TEST(test_touch_long_press_fires_once_while_the_finger_is_still_down);
  RUN_TEST(test_touch_a_slide_is_neither_a_tap_nor_a_long_press);
  RUN_TEST(test_touch_a_finger_returning_to_the_start_is_still_a_slide);
  RUN_TEST(test_touch_second_tap_starts_clean_after_the_first);
  RUN_TEST(test_touch_survives_millis_wraparound);

  RUN_TEST(test_screen_setup_round_trips_through_text);
  RUN_TEST(test_screen_setup_flags_unpack_the_way_the_mapping_expects);
  RUN_TEST(test_screen_setup_keeps_defaults_when_the_file_is_rubbish);
  RUN_TEST(test_screen_setup_tolerates_hand_editing);

  return UNITY_END();
}
