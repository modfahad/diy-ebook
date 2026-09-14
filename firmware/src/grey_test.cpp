// grey_test.cpp -- 4-grey refresh experiment for the GDEY075T7 (env:grey_test).
//
// NOT the product build. It answers one question on the real panel before the
// home screen is built on the answer: can a 4-grey photo stay on the glass
// while the clock next to it updates every minute without a full-screen
// flash?
//
// GxEPD2_4G alone says no. In its GDEY075T7 driver the first black/white
// write after a grey refresh fills both controller RAM planes with white and
// forces a full black/white refresh (_writeImage -> _Force_Init_Full), and its
// grey partial refresh never sends "partial in" (usePartialUpdateWindow_4G is
// false: "greys out remainder"). This sketch drives the controller directly
// for the clock window and tries four variants:
//
//   A1  grey window    4-grey LUTs, partial-in around the clock, gates scan all
//   A2  grey window    same, gates scan only inside the window
//   B1  b/w window     fast OTP partial waveform, partial-in, gates scan all
//   B2  b/w window     same, gates scan only inside the window
//
// Each variant gets a fresh full 4-grey frame (test photo on the right, grey
// swatches, big clock) and then kUpdatesPerVariant clock updates
// kUpdateIntervalMs apart. The variant is written on the glass and in the log.
// What to watch during the clock updates: does the photo on the right keep
// its four greys, does anything outside the clock box flash or fade, and is
// the new time inside the box clean?

#include <Adafruit_GFX.h>
#include <Arduino.h>
#include <GxEPD2_4G_4G.h>
#include <SPI.h>
#include <stdarg.h>

#include "board/board_crowpanel_579.h"

namespace {

constexpr uint32_t kUpdateIntervalMs = 10000;
constexpr uint8_t kUpdatesPerVariant = 3;

// GxEPD2_4G indexes its buffer with 16-bit counters (fillScreen, drawPixel),
// so a single 800x480 grey page (96,000 bytes) makes fillScreen loop forever.
// Two 240-row pages of 48,000 bytes each stay inside the counter.
constexpr uint16_t kPageHeight = 240;

// The clock box. x and w are multiples of 8, as the controller addresses RAM
// in whole bytes.
constexpr int16_t kWinX = 24;
constexpr int16_t kWinY = 176;
constexpr int16_t kWinW = 392;
constexpr int16_t kWinH = 128;

// The synthetic "photo" occupies the right side of the panel.
constexpr int16_t kPhotoX = 440;

enum class Variant : uint8_t { kA1, kA2, kB1, kB2, kCount };

const char* VariantName(Variant v) {
  switch (v) {
    case Variant::kA1: return "A1 grey window, gates scan all";
    case Variant::kA2: return "A2 grey window, gates inside window";
    case Variant::kB1: return "B1 b/w window, gates scan all";
    case Variant::kB2: return "B2 b/w window, gates inside window";
    default:           return "?";
  }
}

// Stamped log lines, so the monitor output lines up with what was on the glass.
void Log(const char* format, ...) {
  char buffer[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  const unsigned long ms = millis();
  Serial.printf("[+%lu.%03lu] %s\n", ms / 1000, ms % 1000, buffer);
}

// The library's panel class plus direct window refreshes. Subclassing gives
// access to GxEPD2_4G_EPD's protected command helpers; the sequences below
// mirror GxEPD2_750_GDEY075T7's own private _setPartialRamArea / _PowerOn /
// _Init_Part (OTP fast partial) with the one thing it never does for this
// panel: "partial in" (0x91) around the refresh itself.
class Gdey075Experiment : public GxEPD2_750_GDEY075T7 {
 public:
  Gdey075Experiment(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_750_GDEY075T7(cs, dc, rst, busy) {}

  // While set, refresh(x, y, w, h) -- which the paged drawing loop calls for a
  // partial window -- is a grey window refresh with partial-in. Off: the
  // library's own behaviour.
  bool grey_window = false;
  bool gates_scan_all = true;

  // The next full grey frame must reload the grey LUTs from scratch: the b/w
  // variant replaces them behind the library's back.
  void forceGreyReinit() { _init_4G_done = false; }

  void refresh(bool partial_update_mode = false) {
    GxEPD2_750_GDEY075T7::refresh(partial_update_mode);
  }

  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) {
    if (!grey_window) {
      GxEPD2_750_GDEY075T7::refresh(x, y, w, h);
      return;
    }
    _writeCommand(0x91);  // partial in
    area(x, y, w, h);
    panelPowerOn();
    _writeCommand(0x12);  // display refresh
    _waitWhileBusy("grey window", grey_refresh_time);
    _writeCommand(0x92);  // partial out
    panelPowerOff();
  }

  // Black/white fast partial update of one window. `old_bits` is what the
  // glass shows there now, `new_bits` what it should show; both 1bpp,
  // 1 = white, w/8 bytes per row.
  void bwWindow(const uint8_t* old_bits, const uint8_t* new_bits, int16_t x,
                int16_t y, int16_t w, int16_t h) {
    _writeCommand(0x00);  // panel setting
    _writeData(0x1f);     // LUT from OTP
    _writeCommand(0x50);  // VCOM and data interval
    _writeData(0x29);     // N2OCP: copy new to old
    _writeData(0x07);
    _writeCommand(0xE0);  // cascade setting
    _writeData(0x02);     // TSFIX
    _writeCommand(0xE5);  // force temperature
    _writeData(0x6E);     // 110 -> the fast partial waveform
    plane(0x10, old_bits, x, y, w, h);
    plane(0x13, new_bits, x, y, w, h);
    _writeCommand(0x91);  // partial in
    area(x, y, w, h);
    panelPowerOn();
    _writeCommand(0x12);
    _waitWhileBusy("b/w window", partial_refresh_time);
    _writeCommand(0x92);  // partial out
    panelPowerOff();
    forceGreyReinit();
  }

 private:
  void area(int16_t x, int16_t y, int16_t w, int16_t h) {
    const uint16_t xs = static_cast<uint16_t>(x) & 0xFFF8;
    const uint16_t xe = static_cast<uint16_t>(x + w - 1) | 0x0007;
    const uint16_t ys = static_cast<uint16_t>(y);
    const uint16_t ye = static_cast<uint16_t>(y + h - 1);
    _writeCommand(0x90);  // partial window
    _writeData(xs >> 8);
    _writeData(xs & 0xFF);
    _writeData(xe >> 8);
    _writeData(xe & 0xFF);
    _writeData(ys >> 8);
    _writeData(ys & 0xFF);
    _writeData(ye >> 8);
    _writeData(ye & 0xFF);
    _writeData(gates_scan_all ? 0x01 : 0x00);
  }

  void plane(uint8_t command, const uint8_t* bits, int16_t x, int16_t y,
             int16_t w, int16_t h) {
    _writeCommand(0x91);
    area(x, y, w, h);
    _writeCommand(command);
    _startTransfer();
    const uint32_t count = static_cast<uint32_t>(w / 8) * static_cast<uint32_t>(h);
    for (uint32_t i = 0; i < count; ++i) _transfer(bits[i]);
    _endTransfer();
    _writeCommand(0x92);
  }

  void panelPowerOn() {
    _writeCommand(0x04);
    _waitWhileBusy("power on", power_on_time);
    _power_is_on = true;
  }

  void panelPowerOff() {
    _writeCommand(0x02);
    _waitWhileBusy("power off", power_off_time);
    _power_is_on = false;
  }
};

GxEPD2_4G_4G<Gdey075Experiment, kPageHeight> display(
    Gdey075Experiment(board::kEpdCs, board::kEpdDc, board::kEpdRst,
                      board::kEpdBusy));

// 1bpp copies of the clock box, for the b/w variants' old/new planes.
GFXcanvas1 window_old(kWinW, kWinH);
GFXcanvas1 window_new(kWinW, kWinH);

const uint16_t kGreys[4] = {GxEPD_BLACK, GxEPD_DARKGREY, GxEPD_LIGHTGREY,
                            GxEPD_WHITE};

// 4x4 Bayer matrix, 0..15.
const uint8_t kBayer[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// The time is fixed by the caller, not read inside: paged drawing calls the
// draw code once per page (twice for a partial window), and every pass has to
// draw the same text.
void DrawClock(Adafruit_GFX& gfx, int16_t ox, int16_t oy, uint16_t white,
               uint16_t black, unsigned long seconds, uint8_t update) {
  char text[8];
  snprintf(text, sizeof(text), "%02lu:%02lu", (seconds / 60) % 100, seconds % 60);
  gfx.fillRect(ox, oy, kWinW, kWinH, white);
  gfx.setTextColor(black);
  gfx.setTextSize(10);  // 6x8 cell -> 60x80
  gfx.setCursor(ox + 46, oy + 12);
  gfx.print(text);
  gfx.setTextSize(2);
  gfx.setCursor(ox + 8, oy + kWinH - 20);
  gfx.printf("update %u", update);
}

void DrawTestPhoto() {
  // Four flat bars.
  for (int i = 0; i < 4; ++i) {
    display.fillRect(kPhotoX, i * 40, 800 - kPhotoX, 40, kGreys[i]);
  }
  // Ordered-dither gradient, black to white, left to right.
  for (int16_t y = 160; y < 320; ++y) {
    for (int16_t x = kPhotoX; x < 800; ++x) {
      const uint32_t v = static_cast<uint32_t>(x - kPhotoX) * 255u / (799u - kPhotoX);
      const uint32_t q = (v * 3u + kBayer[y % 4][x % 4] * 255u / 16u) / 255u;
      display.drawPixel(x, y, kGreys[q > 3 ? 3 : q]);
    }
  }
  // Soft shapes, like a photo's midtones.
  display.fillRect(kPhotoX, 320, 800 - kPhotoX, 160, GxEPD_LIGHTGREY);
  display.fillCircle(kPhotoX + 90, 400, 60, GxEPD_DARKGREY);
  display.fillCircle(kPhotoX + 240, 400, 60, GxEPD_BLACK);
  display.fillCircle(kPhotoX + 240, 400, 30, GxEPD_WHITE);
}

void DrawFullFrame(Variant variant, unsigned long seconds) {
  display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(3);
  display.setCursor(24, 24);
  display.print("GREY REFRESH TEST");
  display.setTextSize(2);
  display.setCursor(24, 72);
  display.print(VariantName(variant));
  display.setTextSize(1);
  display.setCursor(24, 104);
  display.print("Watch the right side while the clock box updates.");
  display.setCursor(24, 118);
  display.print("Should stay: 4 bars, smooth gradient, grey circles.");

  display.drawRect(kWinX - 4, kWinY - 4, kWinW + 8, kWinH + 8, GxEPD_BLACK);
  DrawClock(display, kWinX, kWinY, GxEPD_WHITE, GxEPD_BLACK, seconds, 0);

  for (int i = 0; i < 4; ++i) {
    display.fillRect(24 + i * 100, 340, 80, 80, kGreys[i]);
    display.drawRect(24 + i * 100, 340, 80, 80, GxEPD_BLACK);
  }
  display.setCursor(24, 430);
  display.print("black     dark grey    light grey   white");

  DrawTestPhoto();
}

void RunVariant(Variant variant) {
  Log("=== variant %s ===", VariantName(variant));

  display.epd2.forceGreyReinit();
  display.epd2.grey_window = false;
  unsigned long seconds = millis() / 1000;
  uint32_t start = millis();
  display.setFullWindow();
  display.firstPage();
  do {
    DrawFullFrame(variant, seconds);
  } while (display.nextPage());  // the last page runs the full 4-grey refresh
  Log("full grey frame in %lums", static_cast<unsigned long>(millis() - start));

  // What the glass shows in the clock box right now, for the b/w old plane.
  DrawClock(window_old, 0, 0, 1, 0, seconds, 0);

  const bool grey = (variant == Variant::kA1 || variant == Variant::kA2);
  display.epd2.gates_scan_all = (variant == Variant::kA1 || variant == Variant::kB1);

  for (uint8_t update = 1; update <= kUpdatesPerVariant; ++update) {
    delay(kUpdateIntervalMs);
    seconds = millis() / 1000;
    start = millis();
    if (grey) {
      display.epd2.grey_window = true;
      display.setPartialWindow(kWinX, kWinY, kWinW, kWinH);
      display.firstPage();
      do {
        DrawClock(display, kWinX, kWinY, GxEPD_WHITE, GxEPD_BLACK, seconds, update);
      } while (display.nextPage());
      display.epd2.grey_window = false;
    } else {
      DrawClock(window_new, 0, 0, 1, 0, seconds, update);
      display.epd2.bwWindow(window_old.getBuffer(), window_new.getBuffer(), kWinX,
                            kWinY, kWinW, kWinH);
      memcpy(window_old.getBuffer(), window_new.getBuffer(),
             static_cast<size_t>(kWinW / 8) * kWinH);
    }
    Log("%s: clock update %u in %lums", VariantName(variant), update,
        static_cast<unsigned long>(millis() - start));
  }
  delay(kUpdateIntervalMs);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Log("[grey-test] 4-grey refresh experiment, GDEY075T7 800x480");

  pinMode(board::kPanelPowerEn, OUTPUT);
  digitalWrite(board::kPanelPowerEn, HIGH);
  delay(100);
  Serial.flush();
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(board::kEpdSck, -1, board::kEpdMosi, board::kEpdCs);
  display.setRotation(board::kDisplayRotation);
  Log("[grey-test] panel up, clock box x=%d y=%d w=%d h=%d, update every %lus",
      kWinX, kWinY, kWinW, kWinH, static_cast<unsigned long>(kUpdateIntervalMs / 1000));
}

void loop() {
  for (uint8_t v = 0; v < static_cast<uint8_t>(Variant::kCount); ++v) {
    RunVariant(static_cast<Variant>(v));
  }
}
