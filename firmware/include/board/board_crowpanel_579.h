// board_crowpanel_579.h
//
// Single source of truth for every board-specific constant of the
// Elecrow CrowPanel ESP32-S3 E-Paper HMI carrier board. The board was sold
// with a 5.79" 792x272 panel; this device has a Good Display GDEY075T7-T01
// (7.5", 800x480, UC8179) plugged into the same 24-pin FPC socket instead.
// The file keeps the board's name because everything except the panel block
// is still the 5.79" board's own wiring.
//
// RULE: no raw GPIO number may appear anywhere else in the firmware.
//
// Provenance of each value is recorded so it can be re-verified:
//   [E:spi.h]   example/arduino/Examples/5.79_key/spi.h
//   [E:key]     example/arduino/Examples/5.79_key/5.79_key.ino
//   [E:TF]      example/arduino/Examples/5.79_TF/5.79_TF.ino
//   [E:PWR]     example/arduino/Examples/5.79_PWR/5.79_PWR.ino
//   [E:GPIO]    example/arduino/Examples/5.79_GPIO/5.79_GPIO.ino
// from github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792
//   [GxEPD2]    GxEPD2 src/gdey/GxEPD2_750_GDEY075T7.h
//   [GD:T01]    good-display.com/product/483.html (GDEY075T7-T01: GT911,
//               I2C, 6-pin touch FPC, 2.8-3.6 V)
//   [GD:T01p5]  the same part's spec sheet, page 5 mechanical drawing, read
//               off the drawing's label positions (it has no pin table):
//               v4.cecdn.yun300.cn/100001_1909185148/GDEY075T7-T01.pdf
//
// Anything still unknown is marked TODO(hw) and MUST NOT be guessed at
// the call site.

#pragma once

#include <stdint.h>

namespace board {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------
constexpr const char* kBoardName =
    "CrowPanel ESP32-S3 E-Paper HMI + GDEY075T7 7.5in";
constexpr const char* kMcu       = "ESP32-S3-WROOM-1 (8MB flash / 8MB PSRAM)";

// ---------------------------------------------------------------------------
// E-Paper panel: Good Display GDEY075T7-T01  [E:spi.h] [GxEPD2]
//
// 800 x 480 black/white, a single UC8179 controller, driven by GxEPD2's
// GxEPD2_750_GDEY075T7 over the ESP32-S3's hardware SPI (the global SPI
// object, remapped onto the CrowPanel's panel pins after GxEPD2's init()).
// None of the 5.79" panel's peculiarities apply any more: no second
// controller, no 8-column seam, no bit-banged bus.
//
// TODO(hw): the CrowPanel's booster ballast resistor is fitted for the
// SSD1683 panel it shipped with, and UC8179 panels usually want the other
// value (the one a DESPI-C02 adapter's switch selects). A faint, blotchy or
// half-refreshed image points at that resistor before it points at firmware.
// ---------------------------------------------------------------------------
constexpr int kEpdSck  = 12;   // hardware SPI (FSPI), remapped
constexpr int kEpdMosi = 11;
constexpr int kEpdCs   = 45;
constexpr int kEpdDc   = 46;
constexpr int kEpdRst  = 47;
constexpr int kEpdBusy = 48;   // UC8179: LOW while busy

constexpr uint16_t kWidth        = 800;
constexpr uint16_t kHeight       = 480;
constexpr uint16_t kStrideBytes  = kWidth / 8;                    // 100
constexpr uint32_t kFramebufferBytes =
    static_cast<uint32_t>(kStrideBytes) * kHeight;                // 48000

static_assert(kWidth % 8 == 0, "framebuffer rows must be whole bytes");

// Display orientation, as an Adafruit GFX / GxEPD2 setRotation() value
// (quarter turns). Only 0 and 2 keep the 800x480 landscape geometry every
// ui:: screen is laid out for.
// TODO(hw): confirm against the physical enclosure; 2 if the image is
// upside down.
constexpr uint8_t kDisplayRotation = 0;
static_assert(kDisplayRotation == 0 || kDisplayRotation == 2,
              "screens are laid out for landscape 800x480");

// ---------------------------------------------------------------------------
// Touch: the -T01 panel's GT911 capacitive layer  [GD:T01]
//
// The touch layer has its own 6-pin FPC, separate from the display's 24-pin
// one. Its pin order, all signals 3.3 V  [GD:T01p5]:
//   1 GND   2 VCC   3 RESET   4 INT   5 SDA   6 SCL
// Check it against the drawing before powering up.
// The CrowPanel has no socket for it, so it is hand-wired through an FPC
// breakout to the 2x10 header.
//
// The four GPIO numbers are OUR CHOICE, not from any datasheet: picked from
// kHeaderGpio below, clear of the panel, SD, rail and input pins, and all
// RTC-capable so INT can wake from deep sleep later. Change them to match
// however the breakout is actually wired.
//
// GT911 I2C address: 0x5D or 0x14, chosen by INT's level while RST rises.
// The touch test straps 0x5D and, if nothing answers, re-straps for 0x14.
//
// TODO(hw): the orientation flags are unknown until touched on the real
// panel: the touch_test build (src/touch_test.cpp) cycles through all eight
// on EXIT and prints the one that makes the corner targets line up.
// ---------------------------------------------------------------------------
constexpr int kTouchSda = 15;
constexpr int kTouchScl = 16;
constexpr int kTouchInt = 17;
constexpr int kTouchRst = 18;
// 100 kHz, not the GT911's 400 kHz maximum. The bus is hand-wired and has no
// pull-up resistors of its own, so it runs on the ESP32's internal ones
// (~45k). Against ~50 pF of wiring that is roughly a 1.8 us rise -- over the
// 300 ns that 400 kHz allows, and the first board run (2026-09-19) did report
// I2C read errors after the first panel refresh. At 100 kHz the same rise
// eats 18% of a bit instead of 72%.
//
// This is margin, not a fix: 4.7k from SDA and from SCL to 3V3 brings the
// rise to ~0.2 us and would be in spec even at 400 kHz. If errors persist
// with this setting, fit them -- TouchGt911::errorCount() is the counter to
// watch.
constexpr uint32_t kTouchI2cHz = 100000;

constexpr bool kTouchSwapXY  = false;  // TODO(hw)
constexpr bool kTouchInvertX = false;  // TODO(hw)
constexpr bool kTouchInvertY = false;  // TODO(hw)

// ---------------------------------------------------------------------------
// Power rails  [E:PWR] [E:TF]
//
// Both rails are ACTIVE HIGH. They are driven LOW before deep sleep.
// ---------------------------------------------------------------------------
constexpr int kPanelPowerEn = 7;    // must be HIGH before any EPD GPIO activity
constexpr int kSdPowerEn    = 42;   // must be HIGH before SD SPI begin()
constexpr int kPowerLed     = 41;   // active HIGH

// ---------------------------------------------------------------------------
// MicroSD / TF  [E:TF]
//
// The SD card sits on its OWN SPI bus (HSPI). It does not share pins with
// the panel, so there is no cross-peripheral CS ordering constraint.
// ---------------------------------------------------------------------------
constexpr int kSdSck  = 39;
constexpr int kSdMiso = 13;
constexpr int kSdMosi = 40;
constexpr int kSdCs   = 10;
// Elecrow's demo asks for 80 MHz; 40 MHz is the conservative default here.
// See docs/architecture.md "SD clock".
constexpr uint32_t kSdSpiHz = 40000000;

// ---------------------------------------------------------------------------
// Front-panel input  [E:key]
//
// All five inputs are ACTIVE LOW and idle HIGH via on-board pull-ups
// (Elecrow's example uses plain INPUT with no internal pull-up). We still
// request INPUT_PULLUP so a floating pin cannot produce phantom presses.
//
// Elecrow labels these HOME / EXIT / PRV / NEXT / OK. Physically, PRV+NEXT
// are the two rotary-encoder channels and OK is the encoder push switch.
// PRV/NEXT are two independent direction-pulse lines, not a quadrature pair
// -- confirmed on hardware, see bring-up-log.md section 2.1.
// ---------------------------------------------------------------------------
constexpr int kBtnMenu    = 2;   // Elecrow "HOME"
constexpr int kBtnExit    = 1;   // Elecrow "EXIT"
constexpr int kEncoderA   = 6;   // Elecrow "PRV"
constexpr int kEncoderB   = 4;   // Elecrow "NEXT"
constexpr int kEncoderSw  = 5;   // Elecrow "OK"  (encoder push)

constexpr bool kInputActiveLow = true;

// ---------------------------------------------------------------------------
// Deep-sleep wake sources
//
// ESP32-S3 EXT1 wake is only available on RTC-capable GPIOs, which are
// GPIO0..GPIO21. All five inputs (1,2,4,5,6) are inside that range, and all
// five idle HIGH and go LOW when actuated, so a single
// ESP_EXT1_WAKEUP_ANY_LOW mask covers buttons AND encoder with no conflict.
//
// The static_asserts are the guard: if a pin is ever moved out of the RTC
// range the build fails instead of silently losing wake capability.
// ---------------------------------------------------------------------------
constexpr int kMaxRtcGpio = 21;

constexpr uint64_t PinMask(int pin) { return 1ULL << pin; }

constexpr uint64_t kWakeMask = PinMask(kBtnMenu) | PinMask(kBtnExit) |
                               PinMask(kEncoderSw) | PinMask(kEncoderA) |
                               PinMask(kEncoderB);

static_assert(kBtnMenu   >= 0 && kBtnMenu   <= kMaxRtcGpio, "MENU not RTC-capable");
static_assert(kBtnExit   >= 0 && kBtnExit   <= kMaxRtcGpio, "EXIT not RTC-capable");
static_assert(kEncoderSw >= 0 && kEncoderSw <= kMaxRtcGpio, "ENC SW not RTC-capable");
static_assert(kEncoderA  >= 0 && kEncoderA  <= kMaxRtcGpio, "ENC A not RTC-capable");
static_assert(kEncoderB  >= 0 && kEncoderB  <= kMaxRtcGpio, "ENC B not RTC-capable");

// ---------------------------------------------------------------------------
// Battery
//
// The board exposes a 2-pin BAT JST with an on-board charger, but Elecrow
// publishes no battery-sense GPIO and their support has declined to state
// one. Battery reporting is therefore NOT implemented in Milestone 1.
// ---------------------------------------------------------------------------
// TODO(hw): battery sense ADC channel unknown. Do not invent one; resolve it
// from the schematic (Eagle_SCH&PCB in the Elecrow repo) before implementing
// IPower::batteryMillivolts().
constexpr int kBatteryAdcPin = -1;   // -1 == unavailable

// ---------------------------------------------------------------------------
// Free GPIO broken out on the 2x10 header  [E:GPIO]
// Listed so future peripherals (Phase 3 microphone) are chosen from here.
// 15, 16, 17 and 18 are taken by the touch panel (kTouch* above).
// ---------------------------------------------------------------------------
constexpr int kHeaderGpio[] = {8, 3, 14, 9, 16, 15, 18, 17, 20, 19, 38, 21};
constexpr int kHeaderGpioCount = 12;

}  // namespace board
