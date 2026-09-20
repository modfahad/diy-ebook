// tenergy_touch_test.cpp -- can a LILYGO T-Energy-S3 drive the GT911?
// (env:tenergy_touch_test)
//
// A standalone answer to one question, before any of this project moves to
// that board: does the touch layer of the GDEY075T7-T01 talk to it. There is
// no display in this sketch on purpose -- the panel is a second variable, and
// this is about the I2C bus and four wires.
//
// It reports over USB serial:
//   * what the chip is (16MB flash / 8MB PSRAM, to confirm the board),
//   * the battery on IO3, which is the reason for choosing this board,
//   * every address that answers on I2C,
//   * the GT911's identity, validated rather than believed,
//   * every touch, with the chip's own coordinates,
//   * a health line: frames read, I2C errors, INT level.
//
// Send any character over serial to re-scan and re-probe without resetting.
//
// Wiring (all four are OUR CHOICE -- override with -D at build time):
//   touch 1 GND   -> GND
//   touch 2 VCC   -> 3V3          (not 5V)
//   touch 3 RESET -> kRst  (GPIO 18 by default)
//   touch 4 INT   -> kInt  (GPIO 17)
//   touch 5 SDA   -> kSda  (GPIO 8, the Qwiic pair on this board)
//   touch 6 SCL   -> kScl  (GPIO 9)
// The ribbon's pin order is from the GDEY075T7-T01 spec sheet, page 5 --
// check it before powering up, since VCC and GND reversed kills the chip.
//
// USB note: this board has no USB-serial chip. The USB-C socket is the
// ESP32-S3's own USB, so the build turns on CDC-on-boot; on the CrowPanel
// (CH340 on UART0) that flag is off. Getting it wrong means no output at all.

#include <Arduino.h>
#include <Wire.h>
#include <stdarg.h>
#include <stdio.h>

namespace {

// --- wiring ---------------------------------------------------------------
#ifndef TOUCH_SDA
#define TOUCH_SDA 8
#endif
#ifndef TOUCH_SCL
#define TOUCH_SCL 9
#endif
#ifndef TOUCH_INT
#define TOUCH_INT 17
#endif
#ifndef TOUCH_RST
#define TOUCH_RST 18
#endif
// The 18650's divider, per LilyGO's own documentation for this board.
#ifndef BATTERY_ADC
#define BATTERY_ADC 3
#endif

constexpr int kSda = TOUCH_SDA;
constexpr int kScl = TOUCH_SCL;
constexpr int kInt = TOUCH_INT;
constexpr int kRst = TOUCH_RST;
constexpr int kBatteryAdc = BATTERY_ADC;

// 100 kHz, not the GT911's 400 kHz maximum: a hand-wired bus with no pull-up
// resistors of its own runs on the ESP32's internal ones, and at 400 kHz the
// CrowPanel build saw read errors. Raise it once 4.7k resistors are fitted.
constexpr uint32_t kI2cHz = 100000;

constexpr uint8_t kAddrPrimary = 0x5D;  // INT low while RST rises
constexpr uint8_t kAddrAlt = 0x14;      // INT high

constexpr uint16_t kRegProductId = 0x8140;
constexpr uint16_t kRegStatus = 0x814E;
constexpr uint16_t kRegPoints = 0x814F;  // confirmed on the CrowPanel board
constexpr uint8_t kMaxPoints = 5;

constexpr uint32_t kHealthIntervalMs = 5000;

// --- state ----------------------------------------------------------------
uint8_t g_address = 0;         // 0 = nothing found
char g_product[5] = {0};
uint16_t g_firmware = 0;
uint16_t g_res_x = 0;
uint16_t g_res_y = 0;
uint32_t g_frames = 0;
uint32_t g_errors = 0;
uint32_t g_touches = 0;
uint32_t g_last_health_ms = 0;
bool g_last_int_high = true;
bool g_down = false;

void Log(const char* format, ...) {
  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  const unsigned long ms = millis();
  Serial.printf("[+%lu.%03lu] %s\n", ms / 1000, ms % 1000, buffer);
}

bool Ack(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool ReadRegister(uint16_t reg, uint8_t* out, uint8_t length) {
  if (g_address == 0) return false;
  Wire.beginTransmission(g_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<uint8_t>(g_address),
                       static_cast<uint8_t>(length)) != length) {
    return false;
  }
  for (uint8_t i = 0; i < length; ++i) out[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool WriteRegister(uint16_t reg, uint8_t value) {
  if (g_address == 0) return false;
  Wire.beginTransmission(g_address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// INT's level while RST rises picks the address; then INT is released so the
// chip can drive it.
void StrapAndReset(bool int_high) {
  pinMode(kRst, OUTPUT);
  pinMode(kInt, OUTPUT);
  digitalWrite(kRst, LOW);
  digitalWrite(kInt, int_high ? HIGH : LOW);
  delay(10);
  digitalWrite(kRst, HIGH);
  delay(10);
  digitalWrite(kInt, LOW);
  delay(50);
  pinMode(kInt, INPUT);
  delay(50);
}

void ScanBus() {
  uint8_t found = 0;
  Serial.print("[i2c] devices:");
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    if (!Ack(a)) continue;
    ++found;
    Serial.printf(" 0x%02X", a);
  }
  if (found == 0) Serial.print(" none");
  Serial.println();
}

// True only for something that really looks like a GT911. An undriven bus
// reads back as all ones, and taking that at face value is how the CrowPanel
// build once announced a chip that was not there (fw=0xFFFF, res=65535).
bool Identify(uint8_t address) {
  g_address = address;
  uint8_t id[10];
  if (!ReadRegister(kRegProductId, id, sizeof(id))) {
    g_address = 0;
    return false;
  }
  const uint16_t firmware = static_cast<uint16_t>(id[4] | (id[5] << 8));
  const uint16_t width = static_cast<uint16_t>(id[6] | (id[7] << 8));
  const uint16_t height = static_cast<uint16_t>(id[8] | (id[9] << 8));
  if (!(id[0] == '9' && id[1] == '1' && id[2] == '1') || firmware == 0xFFFF ||
      firmware == 0 || width > 4096 || height > 4096) {
    Log("0x%02X answered with nonsense: id=%02X %02X %02X %02X fw=0x%04X "
        "res=%ux%u -- not a GT911",
        address, id[0], id[1], id[2], id[3], firmware, width, height);
    g_address = 0;
    return false;
  }
  memcpy(g_product, id, 4);
  g_product[4] = 0;
  g_firmware = firmware;
  g_res_x = width;
  g_res_y = height;
  return true;
}

bool Probe() {
  g_address = 0;
  for (uint8_t attempt = 1; attempt <= 3; ++attempt) {
    StrapAndReset(false);
    if (Ack(kAddrPrimary) && Identify(kAddrPrimary)) break;
    StrapAndReset(true);
    if (Ack(kAddrAlt) && Identify(kAddrAlt)) break;
    Log("attempt %u: no answer at 0x%02X or 0x%02X", attempt, kAddrPrimary,
        kAddrAlt);
    delay(50);
  }
  if (g_address == 0) {
    Log("NO TOUCH CHIP on SDA%d/SCL%d -- check the ribbon, the 3V3 feed and "
        "the pin order",
        kSda, kScl);
    ScanBus();
    return false;
  }
  Log("GT911 at 0x%02X  id=\"%s\"  fw=0x%04X  res=%ux%u", g_address, g_product,
      g_firmware, g_res_x, g_res_y);
  return true;
}

void ReportBattery() {
  // A rough reading: the divider halves the cell, and the ESP32-S3's ADC is
  // not accurate without calibration. It is here to prove the pin is alive,
  // not to gauge a battery.
  const uint32_t mv = analogReadMilliVolts(kBatteryAdc) * 2;
  Log("battery on IO%d: ~%lu mV (raw x2, uncalibrated)", kBatteryAdc,
      static_cast<unsigned long>(mv));
}

void PollTouch() {
  if (g_address == 0) return;

  uint8_t status = 0;
  if (!ReadRegister(kRegStatus, &status, 1)) {
    ++g_errors;
    return;
  }
  if ((status & 0x80) == 0) return;

  uint8_t count = status & 0x0F;
  if (count > kMaxPoints) count = kMaxPoints;

  uint8_t raw[8 * kMaxPoints];
  bool ok = true;
  if (count > 0) ok = ReadRegister(kRegPoints, raw, static_cast<uint8_t>(8 * count));
  if (!WriteRegister(kRegStatus, 0)) ok = false;
  if (!ok) {
    ++g_errors;
    return;
  }
  ++g_frames;

  if (count == 0) {
    if (g_down) {
      g_down = false;
      Log("up");
    }
    return;
  }

  // Reject the two shapes a broken read takes: all zeros and all ones.
  bool all_zero = true;
  bool all_ones = true;
  for (uint8_t b = 0; b < 7; ++b) {
    if (raw[b] != 0x00) all_zero = false;
    if (raw[b] != 0xFF) all_ones = false;
  }
  if (all_zero || all_ones) {
    ++g_errors;
    Log("dropped an all-%s frame", all_zero ? "zero" : "ones");
    return;
  }

  Serial.printf("[+%lu.%03lu] touch n=%u", millis() / 1000, millis() % 1000,
                count);
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t* p = raw + 8 * i;
    const uint16_t x = static_cast<uint16_t>(p[1] | (p[2] << 8));
    const uint16_t y = static_cast<uint16_t>(p[3] | (p[4] << 8));
    const uint16_t size = static_cast<uint16_t>(p[5] | (p[6] << 8));
    Serial.printf("  [id=%u (%u,%u) size=%u]", p[0], x, y, size);
  }
  Serial.println();
  if (!g_down) ++g_touches;
  g_down = true;
}

void ReportHealth() {
  const bool int_high = digitalRead(kInt) != LOW;
  Log("health: chip=%s frames=%lu i2c_errors=%lu touches=%lu INT=%s",
      g_address != 0 ? "ok" : "MISSING", static_cast<unsigned long>(g_frames),
      static_cast<unsigned long>(g_errors),
      static_cast<unsigned long>(g_touches), int_high ? "HIGH" : "LOW");
  g_last_int_high = int_high;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // Native USB needs a moment to enumerate before anything is worth printing,
  // and there may be no host attached at all.
  const uint32_t start = millis();
  while (!Serial && millis() - start < 3000) delay(10);
  delay(200);

  Serial.println();
  Log("[t-energy-s3] GT911 touch test");
  Log("chip=%s rev%d cores=%d cpu=%luMHz", ESP.getChipModel(),
      ESP.getChipRevision(), ESP.getChipCores(),
      static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  Log("flash=%luMB psram=%lu bytes (free %lu) heap_free=%lu",
      static_cast<unsigned long>(ESP.getFlashChipSize() / (1024 * 1024)),
      static_cast<unsigned long>(ESP.getPsramSize()),
      static_cast<unsigned long>(ESP.getFreePsram()),
      static_cast<unsigned long>(ESP.getFreeHeap()));
  Log("wiring: SDA=%d SCL=%d INT=%d RST=%d at %lu Hz", kSda, kScl, kInt, kRst,
      static_cast<unsigned long>(kI2cHz));
  ReportBattery();

  Wire.begin(kSda, kScl, kI2cHz);
  ScanBus();
  Probe();
  Log("touch the glass. Send any character to probe again.");
  g_last_health_ms = millis();
}

void loop() {
  if (Serial.available() > 0) {
    while (Serial.available() > 0) Serial.read();
    Log("re-probing on request");
    ScanBus();
    Probe();
  }

  PollTouch();

  const bool int_high = digitalRead(kInt) != LOW;
  if (int_high != g_last_int_high) {
    g_last_int_high = int_high;
    Log("INT went %s", int_high ? "HIGH" : "LOW");
  }

  const uint32_t now = millis();
  if (static_cast<uint32_t>(now - g_last_health_ms) >= kHealthIntervalMs) {
    g_last_health_ms = now;
    ReportHealth();
  }
  delay(2);
}
