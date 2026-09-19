// touch_test.cpp -- touch bring-up for the GDEY075T7-T01 (env:touch_test).
//
// NOT the product build. It answers three questions on the real panel before
// any screen is built on touch:
//
//   1. Is the GT911 wired right? The chip's product id, firmware and
//      configured resolution go on the glass and in the log; if it does not
//      answer, the glass shows every address the I2C scan found instead.
//   2. Which way round is it? Touch coordinates can come out swapped or
//      mirrored relative to the display. EXIT steps through all eight
//      orientations; the right one is the one where tapping each of the five
//      numbered targets fills that target. Copy its three flags into
//      kTouchSwapXY / kTouchInvertX / kTouchInvertY in the board header.
//   3. Does it track a finger? Dragging draws a line; up to five fingers draw
//      at once. OK (encoder push) or MENU clears the glass.
//
// The glass is refreshed with the fast partial waveform, at most every
// kMinRefreshMs while a finger is down and once more when it lifts, so a
// drawn line lags the finger by a refresh -- that is the panel, not the touch.

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <GxEPD2_4G_BW.h>
#include <SPI.h>
#include <Wire.h>
#include <stdarg.h>

#include "board/board_crowpanel_579.h"

namespace {

constexpr uint32_t kMinRefreshMs = 400;
constexpr uint32_t kRetryProbeMs = 3000;
constexpr uint32_t kButtonDebounceMs = 40;

constexpr int16_t kW = board::kWidth;
constexpr int16_t kH = board::kHeight;

// GT911 registers (16-bit, big-endian on the wire).
constexpr uint16_t kRegProductId = 0x8140;  // 4 ASCII bytes, "911\0"
constexpr uint16_t kRegFirmware  = 0x8144;  // u16 LE
constexpr uint16_t kRegXRes      = 0x8146;  // u16 LE
constexpr uint16_t kRegYRes      = 0x8148;  // u16 LE
constexpr uint16_t kRegStatus    = 0x814E;  // bit7 ready, bits0-3 count
// 8 bytes per point: track id, x, y, size (u16 LE), reserved. NOT verified
// on this chip: some GT911 maps put point 1 at 0x8150 with the id at 0x814F.
// The "down" log line prints the raw bytes to settle it.
constexpr uint16_t kRegPoints    = 0x814F;
constexpr uint8_t kMaxPoints = 5;

constexpr uint8_t kAddrPrimary = 0x5D;
constexpr uint8_t kAddrAlt     = 0x14;

GxEPD2_4G_BW<GxEPD2_750_GDEY075T7, board::kHeight> display(
    GxEPD2_750_GDEY075T7(board::kEpdCs, board::kEpdDc, board::kEpdRst,
                         board::kEpdBusy));

void Log(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  const unsigned long ms = millis();
  Serial.printf("[+%lu.%03lu] %s\n", ms / 1000, ms % 1000, buffer);
}

// ---------------------------------------------------------------------------
// GT911
// ---------------------------------------------------------------------------

struct Chip {
  bool found = false;
  uint8_t addr = 0;
  char product[5] = {0};
  uint16_t firmware = 0;
  uint16_t x_res = kW;
  uint16_t y_res = kH;
};

Chip g_chip;

bool ReadReg(uint16_t reg, uint8_t* out, size_t n) {
  Wire.beginTransmission(g_chip.addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(g_chip.addr, static_cast<uint8_t>(n)) != n) return false;
  for (size_t i = 0; i < n; ++i) out[i] = Wire.read();
  return true;
}

bool WriteReg(uint16_t reg, uint8_t value) {
  Wire.beginTransmission(g_chip.addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool Ack(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// GT911 datasheet power-on timing: INT's level while RST rises picks the
// address (LOW -> 0x5D, HIGH -> 0x14), then INT is held LOW for 50 ms and
// released so the chip can drive it.
void ResetGt911(bool int_high) {
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

// An idle I2C bus sits HIGH. LOW on either line with the pull-ups on means
// the GT911 is unpowered (its pins clamp the bus) or a line is shorted to GND;
// every transaction then waits the driver's 1 s timeout, so a full scan would
// take two minutes. Check first and say so instead.
String BusStuck() {
  const bool sda = digitalRead(board::kTouchSda);
  const bool scl = digitalRead(board::kTouchScl);
  if (sda && scl) return String();
  char text[80];
  snprintf(text, sizeof(text), "bus held LOW (SDA=%d SCL=%d): check 3V3/GND to the touch FPC",
           sda, scl);
  return String(text);
}

// Every 7-bit address that ACKs, as text, for the "not found" screen.
String ScanBus() {
  const String stuck = BusStuck();
  if (stuck.length()) return stuck;
  String found;
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    if (!Ack(a)) continue;
    char hex[8];
    snprintf(hex, sizeof(hex), "0x%02X ", a);
    found += hex;
  }
  return found.length() ? found : String("nothing");
}

bool ProbeChip() {
  g_chip = Chip();
  if (BusStuck().length()) return false;
  ResetGt911(false);
  if (Ack(kAddrPrimary)) {
    g_chip.addr = kAddrPrimary;
  } else {
    ResetGt911(true);
    if (!Ack(kAddrAlt)) return false;
    g_chip.addr = kAddrAlt;
  }
  uint8_t info[10];
  if (!ReadReg(kRegProductId, info, sizeof(info))) return false;
  memcpy(g_chip.product, info, 4);
  g_chip.firmware = info[4] | (info[5] << 8);
  const uint16_t xr = info[6] | (info[7] << 8);
  const uint16_t yr = info[8] | (info[9] << 8);
  // A blank config reads 0; assume the panel's own size then.
  if (xr) g_chip.x_res = xr;
  if (yr) g_chip.y_res = yr;
  g_chip.found = true;
  static_assert(kRegFirmware == kRegProductId + 4 && kRegXRes == kRegProductId + 6 &&
                    kRegYRes == kRegProductId + 8,
                "one read covers id, firmware and resolution");
  return true;
}

struct Point {
  uint8_t bytes[8];  // as read, for the log
  uint8_t id;
  uint16_t raw_x, raw_y, size;
  int16_t x, y;  // mapped to the display
};

// Bit 0 swap x/y, bit 1 mirror x, bit 2 mirror y.
uint8_t g_orientation = (board::kTouchSwapXY ? 1 : 0) |
                        (board::kTouchInvertX ? 2 : 0) |
                        (board::kTouchInvertY ? 4 : 0);

void Map(Point& p) {
  uint32_t a = p.raw_x, b = p.raw_y;
  uint32_t range_a = g_chip.x_res, range_b = g_chip.y_res;
  if (g_orientation & 1) {
    a = p.raw_y; b = p.raw_x;
    range_a = g_chip.y_res; range_b = g_chip.x_res;
  }
  int32_t x = static_cast<int32_t>(a * kW / (range_a ? range_a : 1));
  int32_t y = static_cast<int32_t>(b * kH / (range_b ? range_b : 1));
  if (g_orientation & 2) x = kW - 1 - x;
  if (g_orientation & 4) y = kH - 1 - y;
  p.x = static_cast<int16_t>(constrain(x, 0, kW - 1));
  p.y = static_cast<int16_t>(constrain(y, 0, kH - 1));
}

constexpr int kBusError = -1;
constexpr int kNoFrame = -2;

// kBusError, kNoFrame (nothing new since the last read), or 0..5 points in a
// fresh frame -- 0 meaning every finger lifted. The status register must be
// cleared after every ready frame or the chip stops reporting.
int ReadPoints(Point* points) {
  uint8_t status;
  if (!ReadReg(kRegStatus, &status, 1)) return kBusError;
  if (!(status & 0x80)) return kNoFrame;
  uint8_t n = status & 0x0F;
  if (n > kMaxPoints) n = kMaxPoints;
  uint8_t raw[8 * kMaxPoints];
  if (n && !ReadReg(kRegPoints, raw, 8 * n)) {
    WriteReg(kRegStatus, 0);
    return kBusError;
  }
  WriteReg(kRegStatus, 0);
  for (uint8_t i = 0; i < n; ++i) {
    const uint8_t* r = raw + 8 * i;
    memcpy(points[i].bytes, r, 8);
    points[i].id = r[0];
    points[i].raw_x = r[1] | (r[2] << 8);
    points[i].raw_y = r[3] | (r[4] << 8);
    points[i].size = r[5] | (r[6] << 8);
    Map(points[i]);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

constexpr int16_t kTarget = 64;
constexpr int16_t kInset = 16;

struct Target {
  int16_t x, y;
  bool hit;
};

Target g_targets[5] = {
    {kInset, kInset, false},                                  // 1 top-left
    {kW - kInset - kTarget, kInset, false},                   // 2 top-right
    {kInset, kH - kInset - kTarget, false},                   // 3 bottom-left
    {kW - kInset - kTarget, kH - kInset - kTarget, false},    // 4 bottom-right
    {(kW - kTarget) / 2, (kH - kTarget) / 2, false},          // 5 centre
};

String g_scan_result;

void DrawTarget(uint8_t i) {
  const Target& t = g_targets[i];
  display.fillRect(t.x, t.y, kTarget, kTarget, t.hit ? GxEPD_BLACK : GxEPD_WHITE);
  display.drawRect(t.x, t.y, kTarget, kTarget, GxEPD_BLACK);
  const uint16_t ink = t.hit ? GxEPD_WHITE : GxEPD_BLACK;
  display.drawFastHLine(t.x + 8, t.y + kTarget / 2, kTarget - 16, ink);
  display.drawFastVLine(t.x + kTarget / 2, t.y + 8, kTarget - 16, ink);
  display.setTextColor(ink);
  display.setTextSize(2);
  display.setCursor(t.x + 4, t.y + 4);
  display.print(i + 1);
}

// Text lines sit between the top targets, clear of every target.
constexpr int16_t kTextX = kInset + kTarget + 16;
constexpr int16_t kStatusY = 110;
constexpr int16_t kStatusH = 20;

void DrawStatus(const char* text) {
  display.fillRect(kTextX, kStatusY, kW - 2 * kTextX, kStatusH, GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(2);
  display.setCursor(kTextX, kStatusY + 2);
  display.print(text);
}

void DrawFrame() {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(3);
  display.setCursor(kTextX, 20);
  display.print("TOUCH TEST  GT911");
  display.setTextSize(1);
  display.setCursor(kTextX, 56);
  if (g_chip.found) {
    display.printf("addr 0x%02X  id \"%s\"  fw 0x%04X  res %ux%u  pins SDA%d SCL%d INT%d RST%d",
                   g_chip.addr, g_chip.product, g_chip.firmware, g_chip.x_res,
                   g_chip.y_res, board::kTouchSda, board::kTouchScl,
                   board::kTouchInt, board::kTouchRst);
  } else {
    display.printf("GT911 NOT FOUND on SDA%d SCL%d INT%d RST%d - retrying",
                   board::kTouchSda, board::kTouchScl, board::kTouchInt,
                   board::kTouchRst);
    display.setCursor(kTextX, 70);
    display.print("I2C scan: ");
    display.print(g_scan_result);
  }
  display.setCursor(kTextX, 80);
  display.printf("orientation %u: swapXY=%s invertX=%s invertY=%s",
                 g_orientation, (g_orientation & 1) ? "true" : "false",
                 (g_orientation & 2) ? "true" : "false",
                 (g_orientation & 4) ? "true" : "false");
  display.setCursor(kTextX, 92);
  display.print("Tap 1-5. Wrong box fills? EXIT = next orientation. OK/MENU = clear.");
  for (uint8_t i = 0; i < 5; ++i) DrawTarget(i);
}

void FullRedraw() {
  const uint32_t start = millis();
  display.setFullWindow();
  DrawFrame();
  display.display(false);
  Log("full refresh in %lums", static_cast<unsigned long>(millis() - start));
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

struct Button {
  int pin;
  bool down;
  uint32_t changed_ms;
};

Button g_exit = {board::kBtnExit, false, 0};
Button g_ok = {board::kEncoderSw, false, 0};
Button g_menu = {board::kBtnMenu, false, 0};

bool Pressed(Button& b) {
  const bool down = digitalRead(b.pin) == (board::kInputActiveLow ? LOW : HIGH);
  const uint32_t now = millis();
  if (down == b.down || now - b.changed_ms < kButtonDebounceMs) return false;
  b.down = down;
  b.changed_ms = now;
  return down;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct Track {
  bool active;
  int16_t x, y;
};

Track g_tracks[16];  // indexed by GT911 track id (0..15)
bool g_dirty = false;
bool g_finger_down = false;
uint32_t g_last_refresh_ms = 0;
uint32_t g_last_probe_ms = 0;
uint32_t g_touch_count = 0;

void ResetDrawing() {
  for (Target& t : g_targets) t.hit = false;
  for (Track& t : g_tracks) t.active = false;
  g_dirty = false;
}

void HandlePoints(const Point* points, int n) {
  bool seen[16] = {false};
  for (int i = 0; i < n; ++i) {
    const Point& p = points[i];
    Track& t = g_tracks[p.id & 0x0F];
    seen[p.id & 0x0F] = true;
    if (!t.active) {
      ++g_touch_count;
      // The bytes are logged so a wrong point base register (0x814F vs
      // 0x8150; published GT911 maps differ) shows on the first tap.
      const uint8_t* b = p.bytes;
      Log("down id=%u raw=(%u,%u) size=%u -> (%d,%d) orient=%u "
          "bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
          p.id, p.raw_x, p.raw_y, p.size, p.x, p.y, g_orientation, b[0], b[1],
          b[2], b[3], b[4], b[5], b[6], b[7]);
      display.fillCircle(p.x, p.y, 4, GxEPD_BLACK);
      for (uint8_t k = 0; k < 5; ++k) {
        Target& tg = g_targets[k];
        if (!tg.hit && p.x >= tg.x && p.x < tg.x + kTarget && p.y >= tg.y &&
            p.y < tg.y + kTarget) {
          tg.hit = true;
          DrawTarget(k);
          Log("target %u hit", k + 1);
        }
      }
    } else {
      // Thick line: three offset strokes.
      for (int d = -1; d <= 1; ++d) {
        display.drawLine(t.x + d, t.y, p.x + d, p.y, GxEPD_BLACK);
        display.drawLine(t.x, t.y + d, p.x, p.y + d, GxEPD_BLACK);
      }
    }
    t.active = true;
    t.x = p.x;
    t.y = p.y;
    g_dirty = true;
  }
  for (uint8_t id = 0; id < 16; ++id) {
    if (g_tracks[id].active && !seen[id]) {
      g_tracks[id].active = false;
      Log("up   id=%u at (%d,%d)", id, g_tracks[id].x, g_tracks[id].y);
    }
  }
  if (n > 0) {
    char text[96];
    snprintf(text, sizeof(text), "%d finger%s  (%d,%d)  raw (%u,%u)  #%lu", n,
             n == 1 ? "" : "s", points[0].x, points[0].y, points[0].raw_x,
             points[0].raw_y, static_cast<unsigned long>(g_touch_count));
    DrawStatus(text);
  }
  g_finger_down = n > 0;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Log("[touch-test] GDEY075T7-T01 GT911 touch test");

  pinMode(board::kBtnExit, INPUT_PULLUP);
  pinMode(board::kEncoderSw, INPUT_PULLUP);
  pinMode(board::kBtnMenu, INPUT_PULLUP);

  pinMode(board::kPanelPowerEn, OUTPUT);
  digitalWrite(board::kPanelPowerEn, HIGH);
  delay(100);
  Serial.flush();
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(board::kEpdSck, -1, board::kEpdMosi, board::kEpdCs);
  display.setRotation(board::kDisplayRotation);

  Wire.begin(board::kTouchSda, board::kTouchScl, board::kTouchI2cHz);
  if (ProbeChip()) {
    Log("[touch-test] GT911 at 0x%02X id=\"%s\" fw=0x%04X res=%ux%u", g_chip.addr,
        g_chip.product, g_chip.firmware, g_chip.x_res, g_chip.y_res);
  } else {
    g_scan_result = ScanBus();
    Log("[touch-test] GT911 not found. I2C scan: %s", g_scan_result.c_str());
  }
  g_last_probe_ms = millis();
  Log("[touch-test] orientation %u. EXIT = next orientation, OK/MENU = clear",
      g_orientation);
  FullRedraw();
}

void loop() {
  if (Pressed(g_exit)) {
    g_orientation = (g_orientation + 1) & 7;
    Log("orientation -> %u: swapXY=%d invertX=%d invertY=%d", g_orientation,
        g_orientation & 1, (g_orientation >> 1) & 1, (g_orientation >> 2) & 1);
    ResetDrawing();
    FullRedraw();
  }
  if (Pressed(g_ok) || Pressed(g_menu)) {
    Log("clear");
    ResetDrawing();
    FullRedraw();
  }

  if (!g_chip.found) {
    if (millis() - g_last_probe_ms >= kRetryProbeMs) {
      g_last_probe_ms = millis();
      if (ProbeChip()) {
        Log("[touch-test] GT911 appeared at 0x%02X", g_chip.addr);
        FullRedraw();
      } else {
        const String scan = ScanBus();
        if (scan != g_scan_result) {
          g_scan_result = scan;
          Log("[touch-test] I2C scan: %s", scan.c_str());
          FullRedraw();
        }
      }
    }
    return;
  }

  Point points[kMaxPoints];
  const int n = ReadPoints(points);
  if (n == kBusError) {
    Log("[touch-test] I2C read failed");
    delay(100);
    return;
  }
  if (n != kNoFrame) HandlePoints(points, n);

  const uint32_t now = millis();
  if (g_dirty && (!g_finger_down || now - g_last_refresh_ms >= kMinRefreshMs)) {
    const uint32_t start = now;
    display.display(true);  // whole buffer, fast partial waveform
    g_last_refresh_ms = millis();
    g_dirty = false;
    Log("partial refresh in %lums", static_cast<unsigned long>(g_last_refresh_ms - start));
  }
  delay(10);
}
