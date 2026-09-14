// selftest_screen.h -- Milestone 1 hardware self-test screen.
//
// Pure rendering: it takes a state struct and a canvas and draws. No hardware
// access, so the layout can be exercised on the host if a canvas is handed to
// it. This is the pattern every later screen follows.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "hal/input.h"
#include "hal/power.h"

namespace ui {

enum class TestResult : uint8_t {
  kUntested = 0,
  kPass,
  kFail,
};

struct SelfTestState {
  TestResult rotary = TestResult::kUntested;
  TestResult menu = TestResult::kUntested;
  TestResult exit = TestResult::kUntested;
  TestResult encoder_switch = TestResult::kUntested;
  TestResult sd = TestResult::kUntested;
  TestResult eink = TestResult::kUntested;
  TestResult psram = TestResult::kUntested;

  uint64_t sd_capacity_mb = 0;
  const char* sd_detail = "";
  bool framebuffer_in_psram = false;

  hal::WakeInfo wake;
  // wake.boot_count is every reset, including background battery-sample
  // wakes; this counts only the wakes setup() treats as real, so
  // development.md's manual "Boots: increments" test stays meaningful once
  // sampling wakes exist (see main.cpp's g_real_wake_count).
  uint32_t real_wake_count = 0;
  hal::InputDiagnostics input;
  const char* wake_reason_name = "";
  const char* firmware_version = "";

  int32_t ui_counter = 0;   // driven by the rotary, proves coalescing works
  uint32_t refresh_count = 0;

  // What the library index holds, taken straight from library_index.bin at
  // boot. Until the reader exists (Milestone 5) this is the only way an
  // installed package is visible on the glass rather than only in the serial
  // log -- and "the device found your book" is the one thing worth showing
  // before it can open it.
  uint16_t library_count = 0;
  const char* library_title = "";

  // Milestone 4: transfer mode, toggled by a long-press on the encoder
  // switch (see main.cpp's ToggleTransferMode). Replaces the footer hint
  // line while relevant; see selftest_screen.cpp.
  bool transfer_connecting = false;
  bool transfer_mode = false;
  const char* transfer_address = "";  // "host:port", set once connected

  // True whenever BLE provisioning is advertising -- first boot, or after a
  // factory reset (main.cpp's FactoryReset, a long-press on EXIT). Also
  // replaces the footer hint line; checked before transfer_mode's since the
  // two states are mutually exclusive (architecture.md 4.3: BLE and Wi-Fi
  // are never up at once) but this makes the precedence explicit regardless.
  bool pairing_mode = false;

  // Battery (util/battery.h). False until board::kBatteryAdcPin is resolved
  // -- see its TODO(hw) -- at which point IPower::batteryAvailable() starts
  // reporting true and this row stops rendering "--". battery_remaining is
  // pre-formatted ("20d, 12h" / "14h" / "--") rather than a raw ms figure:
  // this screen renders, it does not compute (see the class comment).
  bool battery_available = false;
  uint8_t battery_percent = 0;
  char battery_remaining[24] = "--";
};

class SelfTestScreen {
 public:
  static void render(gfx::Canvas& canvas, const SelfTestState& state);

  static const char* resultText(TestResult result);
};

}  // namespace ui
