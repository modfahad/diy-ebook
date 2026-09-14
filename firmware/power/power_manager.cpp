#include "drivers/power_manager.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "board/board_crowpanel_579.h"

namespace drivers {

// Survives deep sleep in RTC slow memory.
RTC_DATA_ATTR static uint32_t g_boot_count = 0;

bool PowerManager::begin() {
  ++g_boot_count;

  // Pad holds latched before the previous deep sleep must be released, or the
  // panel rail stays stuck at whatever level we parked it at.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(board::kPanelPowerEn));

  wake_ = hal::WakeInfo();
  wake_.boot_count = g_boot_count;

  switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
      wake_.reason = hal::WakeReason::kInput;
      wake_.pin_mask = esp_sleep_get_ext1_wakeup_status();
      wake_.gpio = wake_.pin_mask == 0
                       ? -1
                       : static_cast<int>(__builtin_ctzll(wake_.pin_mask));
      break;
    }
    case ESP_SLEEP_WAKEUP_TIMER:
      wake_.reason = hal::WakeReason::kTimer;
      break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      wake_.reason = hal::WakeReason::kColdBoot;
      break;
    default:
      wake_.reason = hal::WakeReason::kOther;
      break;
  }

  pinMode(board::kPowerLed, OUTPUT);
  digitalWrite(board::kPowerLed, LOW);
  return true;
}

bool PowerManager::batteryAvailable() const {
  return board::kBatteryAdcPin >= 0;
}

uint32_t PowerManager::batteryMillivolts() const {
  // TODO(hw): no documented battery-sense GPIO on this board revision.
  return 0;
}

void PowerManager::setStatusLed(bool on) {
  digitalWrite(board::kPowerLed, on ? HIGH : LOW);
}

void PowerManager::prepareRailsForSleep() {
  setStatusLed(false);

  pinMode(board::kPanelPowerEn, OUTPUT);
  digitalWrite(board::kPanelPowerEn, LOW);
  pinMode(board::kSdPowerEn, OUTPUT);
  digitalWrite(board::kSdPowerEn, LOW);

  // Only RTC-capable pads keep their level through deep sleep. kPanelPowerEn
  // (GPIO7) is RTC-capable, so it can be latched low.
  gpio_hold_en(static_cast<gpio_num_t>(board::kPanelPowerEn));
  gpio_deep_sleep_hold_en();
  // KNOWN LIMITATION: kSdPowerEn (GPIO42) is a plain digital pad and cannot
  // be held through deep sleep, so it floats. Measured impact is unknown
  // until hardware is in hand -- see docs/architecture.md.

  // Configure EXT1: every front-panel input idles HIGH and is pulled LOW when
  // actuated, so one ANY_LOW mask covers buttons and encoder alike.
  //
  // ANY_LOW is genuinely "any", not "all", on this target. The original ESP32
  // only had ALL_LOW/ANY_HIGH; from the S2 onwards the enum is
  //   ESP_EXT1_WAKEUP_ANY_LOW = 0, ESP_EXT1_WAKEUP_ANY_HIGH = 1
  // with ALL_LOW kept as a deprecated alias of ANY_LOW. Verified in
  // framework-arduinoespressif32/tools/sdk/esp32s3/include/esp_hw_support/
  // include/esp_sleep.h. Getting this wrong would mean the device sleeps once
  // and never wakes, so do not "simplify" it to ALL_LOW.
  esp_sleep_enable_ext1_wakeup(board::kWakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
}

void PowerManager::deepSleep() {
  prepareRailsForSleep();
  esp_deep_sleep_start();
}

void PowerManager::deepSleepFor(uint64_t ms) {
  prepareRailsForSleep();
  esp_sleep_enable_timer_wakeup(ms * 1000ULL);
  esp_deep_sleep_start();
}

const char* PowerManager::wakeReasonName(hal::WakeReason reason) {
  switch (reason) {
    case hal::WakeReason::kColdBoot: return "cold boot";
    case hal::WakeReason::kInput:    return "button/rotary";
    case hal::WakeReason::kTimer:    return "timer";
    default:                         return "other";
  }
}

}  // namespace drivers
