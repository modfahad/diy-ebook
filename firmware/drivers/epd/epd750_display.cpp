#include "drivers/epd750_display.h"

#include <Arduino.h>
#include <GxEPD2_4G_4G.h>
#include <GxEPD2_4G_BW.h>
#include <SPI.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "app/app_config.h"
#include "board/board_crowpanel_579.h"
#include "drivers/serial_log.h"

namespace drivers {

namespace {

// GxEPD2_4G's panel class plus the one refresh it lacks: a fast black/white
// update of a single window with "partial in" (0x91) around it, so the
// controller drives only that window and a 4-grey photo beside it survives.
// The library alone, on the first b/w write after a grey refresh, fills both
// RAM planes with white and forces a full b/w refresh (_writeImage ->
// _Force_Init_Full). The command sequence mirrors the library's own private
// _setPartialRamArea / _PowerOn / OTP fast-partial _Init_Part; it was run on
// the real panel as variant B1 of src/grey_test.cpp.
class PanelDriver : public GxEPD2_750_GDEY075T7 {
 public:
  PanelDriver(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_750_GDEY075T7(cs, dc, rst, busy) {}

  // The next grey frame reloads the grey LUTs; bwWindow() replaces them
  // without the library knowing.
  void forceGreyReinit() { _init_4G_done = false; }

  // `old_bits`: what the glass shows in the window now; `new_bits`: what it
  // should show. 1bpp, 1 = white, w/8 bytes per row, panel-native order.
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
    _writeCommand(0x04);  // power on
    _waitWhileBusy("window power on", power_on_time);
    _writeCommand(0x12);  // display refresh
    _waitWhileBusy("window refresh", partial_refresh_time);
    _writeCommand(0x92);  // partial out
    _writeCommand(0x02);  // power off
    _waitWhileBusy("window power off", power_off_time);
    _power_is_on = false;
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
    _writeData(0x01);     // gates scan inside and outside the window
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
};

// GxEPD2_4G indexes its buffers with 16-bit counters, so each page must stay
// under 65,536 bytes: b/w is one 480-row page of 48,000 bytes, grey two
// 240-row pages of 48,000 bytes. The grey object shares the b/w object's
// panel driver. Both live in PSRAM (allocatePanel), not in internal RAM.
using PanelBw = GxEPD2_4G_BW<PanelDriver, 480>;
using PanelGrey = GxEPD2_4G_4G_R<PanelDriver, 240>;

PanelBw* g_bw = nullptr;
PanelGrey* g_grey = nullptr;

// init()'s serial rate, the same as the rest of the firmware's logging.
// GxEPD2_4G's own timing prints are compiled out (DISABLE_DIAGNOSTIC_OUTPUT in
// platformio.ini) because they cannot carry a timestamp; the flush calls log
// the panel timings themselves.
constexpr uint32_t kSerialDiagBaud = 115200;
constexpr uint16_t kResetPulseMs = 2;
constexpr uint32_t kPanelPowerSettleMs = 100;

void* AllocateLarge(size_t bytes, bool* in_psram = nullptr) {
#if defined(BOARD_HAS_PSRAM)
  if (void* p = ps_malloc(bytes)) {
    if (in_psram != nullptr) *in_psram = true;
    return p;
  }
#endif
  if (in_psram != nullptr) *in_psram = false;
  return malloc(bytes);
}

uint8_t ReverseBits(uint8_t b) {
  b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
  b = static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
  return b;
}

}  // namespace

uint16_t Epd750Display::width() const { return board::kWidth; }
uint16_t Epd750Display::height() const { return board::kHeight; }
uint32_t Epd750Display::framebufferBytes() const {
  return board::kFramebufferBytes;
}

bool Epd750Display::allocateFramebuffer() {
  if (framebuffer_ != nullptr) return true;

  // Spec section 13: framebuffers belong in PSRAM. Fall back to the internal
  // heap only so a board with PSRAM disabled still boots far enough to say so.
  framebuffer_ = static_cast<uint8_t*>(AllocateLarge(board::kFramebufferBytes, &fb_in_psram_));
  pushed_ = static_cast<uint8_t*>(AllocateLarge(board::kFramebufferBytes));
  if (framebuffer_ == nullptr || pushed_ == nullptr) return false;

  // Rotation 0 and no seam: the canvas is in logical (screen) coordinates,
  // and GxEPD2_4G's setRotation() maps them onto the panel.
  canvas_ = gfx::Canvas(framebuffer_, board::kWidth, board::kHeight,
                        board::kWidth, board::kHeight, board::kStrideBytes,
                        /*rotation=*/0, /*seam_x=*/0);
  canvas_.clear(gfx::kWhite);
  memset(pushed_, 0xFF, board::kFramebufferBytes);
  return true;
}

bool Epd750Display::allocatePanel() {
  if (g_bw != nullptr) return true;
  void* bw = AllocateLarge(sizeof(PanelBw));
  void* grey = AllocateLarge(sizeof(PanelGrey));
  if (bw == nullptr || grey == nullptr) {
    free(bw);
    free(grey);
    return false;
  }
  g_bw = new (bw) PanelBw(PanelDriver(board::kEpdCs, board::kEpdDc,
                                      board::kEpdRst, board::kEpdBusy));
  g_grey = new (grey) PanelGrey(g_bw->epd2);
  return true;
}

// Reads restore_path_ from restore_storage_ straight into framebuffer_. One-
// shot: always clears restore_storage_/restore_path_ before returning, so a
// failed attempt falls straight through to the normal cold-boot init.
bool Epd750Display::loadRestoreFrame() {
  hal::IStorage* storage = restore_storage_;
  const char* path = restore_path_;
  restore_storage_ = nullptr;
  restore_path_ = nullptr;
  if (storage == nullptr || path == nullptr) return false;

  const int64_t size = storage->size(path);
  if (size != static_cast<int64_t>(board::kFramebufferBytes)) {
    // Includes a 27,200-byte frame saved by the old 5.79" build.
    if (size >= 0) {
      Logf("[epd] saved frame is %lld bytes, want %lu -- not restoring",
           static_cast<long long>(size),
           static_cast<unsigned long>(board::kFramebufferBytes));
    }
    return false;
  }
  const int32_t n =
      storage->read(path, 0, framebuffer_, board::kFramebufferBytes);
  return n == static_cast<int32_t>(board::kFramebufferBytes);
}

// Writes the restored frame into both controller RAM planes, so the first
// partial refresh after a wake diffs against what the glass really shows.
// The frame is in canvas (logical) orientation and the controller wants
// panel-native: identical at rotation 0, and at rotation 2 a 180-degree turn,
// which for a width that is a multiple of 8 is exactly "reverse the byte order
// and the bits within each byte".
bool Epd750Display::writeRestoredFrame() {
  if (board::kDisplayRotation == 0) {
    g_bw->epd2.writeImageForFullRefresh(framebuffer_, 0, 0, board::kWidth,
                                        board::kHeight);
    return true;
  }
  uint8_t* native = static_cast<uint8_t*>(AllocateLarge(board::kFramebufferBytes));
  if (native == nullptr) return false;
  for (uint32_t i = 0; i < board::kFramebufferBytes; ++i) {
    native[board::kFramebufferBytes - 1 - i] = ReverseBits(framebuffer_[i]);
  }
  g_bw->epd2.writeImageForFullRefresh(native, 0, 0, board::kWidth, board::kHeight);
  free(native);
  return true;
}

bool Epd750Display::begin() {
  // The panel rail must be up before anything touches the display.
  pinMode(board::kPanelPowerEn, OUTPUT);
  digitalWrite(board::kPanelPowerEn, HIGH);
  delay(kPanelPowerSettleMs);
  LogLine("[epd] panel power (GPIO7) HIGH");

  if (!allocateFramebuffer() || !allocatePanel()) {
    LogLine("[epd] framebuffer or panel buffer allocation failed");
    ready_ = false;
    return false;
  }

  // Read before init(), because whether there is a frame decides init()'s
  // `initial` argument. initial=true -- the normal path -- clears controller
  // RAM on the first write and forces the first refresh to be full.
  // initial=false is used only when a saved frame is about to go into the
  // controller, so the first repaint after a wake can be partial instead of
  // flashing white (architecture.md 4.2).
  bool restoring = loadRestoreFrame();

  // init() calls Serial.begin() again, which drops anything still queued in
  // the UART -- without this the "[epd] panel power" line above is cut off.
  Serial.flush();
  g_bw->init(kSerialDiagBaud, /*initial=*/!restoring, kResetPulseMs,
             /*pulldown_rst_mode=*/false);

  // init() started the global SPI on the ESP32-S3's default pins; move it
  // onto the CrowPanel's panel pins. This has to come after init().
  SPI.end();
  SPI.begin(board::kEpdSck, -1, board::kEpdMosi, board::kEpdCs);

  g_bw->setRotation(board::kDisplayRotation);
  g_grey->setRotation(board::kDisplayRotation);
  Logf("[epd] GxEPD2_4G init initial=%d, SPI SCK=%d MOSI=%d CS=%d, "
       "rotation=%u, %ux%u",
       restoring ? 0 : 1, board::kEpdSck, board::kEpdMosi, board::kEpdCs,
       static_cast<unsigned>(board::kDisplayRotation),
       static_cast<unsigned>(g_bw->width()), static_cast<unsigned>(g_bw->height()));

  if (restoring && !writeRestoredFrame()) {
    // No scratch buffer for the rotated copy. The controller now holds no
    // valid previous image, so report "not restored": the caller's first
    // flush is then a full refresh, which does not depend on it.
    LogLine("[epd] restore failed (no memory for rotated frame)");
    restoring = false;
  }
  restored_from_storage_ = restoring;
  if (restoring) memcpy(pushed_, framebuffer_, board::kFramebufferBytes);

  partials_since_full_ = 0;
  grey_on_glass_ = false;
  last_mode_ = hal::RefreshMode::kFull;
  ready_ = true;
  return true;
}

void Epd750Display::end() {
  if (ready_) {
    // Panel driving voltages off, controller into deep sleep. The image stays
    // on the glass; the next begin()'s init() resets the controller awake.
    g_bw->hibernate();
  }
  digitalWrite(board::kPanelPowerEn, LOW);
  ready_ = false;
  // The buffers are deliberately left allocated: they cost nothing across a
  // deep sleep (RAM is lost anyway) and avoid a realloc on a warm path.
}

// GxEPD2_4G waits on BUSY with its own 10 s timeout and prints "Busy Timeout!"
// when it gives up, but reports nothing back. BUSY still asserted (LOW on the
// UC8179) once the refresh has returned is the failure visible here.
bool Epd750Display::panelFinished() {
  if (digitalRead(board::kEpdBusy) == LOW) {
    LogLine("[epd] BUSY still asserted after refresh -- panel did not finish");
    return false;
  }
  return true;
}

bool Epd750Display::flush(hal::RefreshMode mode) {
  if (!ready_ || framebuffer_ == nullptr) return false;

  // Partial refreshes accumulate ghosting, so every
  // kPartialsBeforeFullRefresh-th one is escalated to a full refresh. After a
  // grey frame the library runs a full b/w refresh whatever it is asked for,
  // so that is escalated here too, and said so. kFast has no separate
  // waveform on this panel, so it runs, and is reported, as full.
  const bool partial =
      (mode == hal::RefreshMode::kPartial && !grey_on_glass_ &&
       partials_since_full_ < app::kPartialsBeforeFullRefresh);

  const uint32_t copy_start = millis();
  if (partial) {
    g_bw->setPartialWindow(0, 0, g_bw->width(), g_bw->height());
  } else {
    g_bw->setFullWindow();
  }
  // gfx::Canvas stores 1 = white, 0 = black, which is exactly what
  // drawInvertedBitmap() expects: it paints `color` wherever a bit is 0.
  g_bw->fillScreen(GxEPD_WHITE);
  g_bw->drawInvertedBitmap(0, 0, framebuffer_, board::kWidth, board::kHeight,
                           GxEPD_BLACK);
  last_write_ms_ = millis() - copy_start;

  // Full: setFullWindow() + display(). Partial: setPartialWindow() +
  // display(true).
  const uint32_t panel_start = millis();
  g_bw->display(partial);
  last_waveform_ms_ = millis() - panel_start;

  if (partial) {
    ++partials_since_full_;
    last_mode_ = hal::RefreshMode::kPartial;
  } else {
    partials_since_full_ = 0;
    last_mode_ = hal::RefreshMode::kFull;
  }
  grey_on_glass_ = false;
  memcpy(pushed_, framebuffer_, board::kFramebufferBytes);
  return panelFinished();
}

bool Epd750Display::flushGrey(const uint8_t* pixels, int16_t x, int16_t y,
                              int16_t w, int16_t h) {
  if (!ready_ || framebuffer_ == nullptr) return false;
  if (pixels != nullptr && (w <= 0 || h <= 0 || w % 4 != 0)) return false;

  const uint32_t start = millis();
  // The grey LUTs must be loaded afresh: a window update since the last grey
  // frame replaced them behind the library's back.
  g_grey->epd2.forceGreyReinit();
  g_grey->setFullWindow();
  g_grey->firstPage();
  do {
    // Paged: GxEPD2_4G calls this once per 240-row page and clips to it.
    g_grey->drawInvertedBitmap(0, 0, framebuffer_, board::kWidth, board::kHeight,
                               GxEPD_BLACK);
    if (pixels != nullptr) {
      const uint16_t stride = static_cast<uint16_t>(w / 4);
      for (int16_t row = 0; row < h; ++row) {
        const uint8_t* line = pixels + static_cast<uint32_t>(row) * stride;
        for (int16_t col = 0; col < w; ++col) {
          const uint8_t grey = (line[col >> 2] >> (6 - 2 * (col & 3))) & 0x03;
          // The page starts white; white pixels need no work.
          if (grey != 0x03) {
            g_grey->drawGreyPixel(x + col, y + row, static_cast<uint8_t>(grey << 6));
          }
        }
      }
    }
  } while (g_grey->nextPage());  // the last page runs the 4-grey refresh

  last_write_ms_ = 0;
  last_waveform_ms_ = millis() - start;
  last_mode_ = hal::RefreshMode::kGrey;
  partials_since_full_ = 0;
  grey_on_glass_ = true;
  memcpy(pushed_, framebuffer_, board::kFramebufferBytes);
  return panelFinished();
}

bool Epd750Display::flushWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!ready_ || framebuffer_ == nullptr || pushed_ == nullptr) return false;
  if (x < 0 || y < 0 || w <= 0 || h <= 0 || x % 8 != 0 || w % 8 != 0 ||
      x + w > board::kWidth || y + h > board::kHeight) {
    Logf("[epd] flushWindow(%d,%d,%d,%d) is not a byte-aligned on-panel window",
         x, y, w, h);
    return false;
  }

  const uint32_t copy_start = millis();
  const uint16_t stride = static_cast<uint16_t>(w / 8);
  const size_t bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
  uint8_t* old_bits = static_cast<uint8_t*>(AllocateLarge(bytes));
  uint8_t* new_bits = static_cast<uint8_t*>(AllocateLarge(bytes));
  if (old_bits == nullptr || new_bits == nullptr) {
    free(old_bits);
    free(new_bits);
    return false;
  }

  // Panel-native order: identical at rotation 0; at rotation 2 the window
  // sits mirrored in both axes and every byte is bit-reversed.
  const bool turned = (board::kDisplayRotation == 2);
  for (int16_t row = 0; row < h; ++row) {
    const uint32_t source_row =
        static_cast<uint32_t>(turned ? y + h - 1 - row : y + row) * board::kStrideBytes;
    const uint8_t* old_source = pushed_ + source_row + x / 8;
    const uint8_t* new_source = framebuffer_ + source_row + x / 8;
    uint8_t* old_row = old_bits + static_cast<uint32_t>(row) * stride;
    uint8_t* new_row = new_bits + static_cast<uint32_t>(row) * stride;
    for (uint16_t col = 0; col < stride; ++col) {
      if (turned) {
        old_row[stride - 1 - col] = ReverseBits(old_source[col]);
        new_row[stride - 1 - col] = ReverseBits(new_source[col]);
      } else {
        old_row[col] = old_source[col];
        new_row[col] = new_source[col];
      }
    }
  }
  const int16_t native_x = turned ? static_cast<int16_t>(board::kWidth - x - w) : x;
  const int16_t native_y = turned ? static_cast<int16_t>(board::kHeight - y - h) : y;
  last_write_ms_ = millis() - copy_start;

  const uint32_t panel_start = millis();
  g_bw->epd2.bwWindow(old_bits, new_bits, native_x, native_y, w, h);
  last_waveform_ms_ = millis() - panel_start;
  free(old_bits);
  free(new_bits);

  for (int16_t row = 0; row < h; ++row) {
    const uint32_t offset = static_cast<uint32_t>(y + row) * board::kStrideBytes + x / 8;
    memcpy(pushed_ + offset, framebuffer_ + offset, stride);
  }
  last_mode_ = hal::RefreshMode::kPartial;
  return panelFinished();
}

}  // namespace drivers
