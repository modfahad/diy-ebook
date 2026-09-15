// epd750_display.h -- hal::IDisplay implementation for a Good Display
// GDEY075T7 (7.5", 800x480, UC8179) fitted to the CrowPanel ESP32-S3 board.
//
// The panel is driven through GxEPD2_4G (GxEPD2_750_GDEY075T7), which adds
// 4-grey refreshes to GxEPD2's black/white ones. UI code still draws on
// gfx::Canvas; the flush calls copy that canvas into GxEPD2_4G's buffers and
// let it run the refresh. Keeping gfx::Canvas as the drawing surface is what
// keeps every ui:: screen and the glyph blitter host-testable -- GxEPD2_4G
// needs Arduino.
//
// Three ways to push a frame:
//   flush()        black/white, full or partial -- every screen but home
//   flushGrey()    full 4-grey refresh: the canvas plus a 2bpp photo; flashes
//   flushWindow()  fast black/white update of one rectangle only, leaving the
//                  rest of the glass (a grey photo included) undriven -- the
//                  home screen's minute tick
//
// GxEPD2_4G headers are kept out of this file on purpose: main.cpp includes
// it, and the host test build must never see them.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "hal/display.h"
#include "hal/storage.h"

namespace drivers {

class Epd750Display : public hal::IDisplay {
 public:
  bool begin() override;
  void end() override;
  bool ready() const override { return ready_; }

  uint16_t width() const override;
  uint16_t height() const override;

  gfx::Canvas& canvas() override { return canvas_; }
  bool flush(hal::RefreshMode mode) override;
  hal::RefreshMode lastRefreshMode() const override { return last_mode_; }

  // --- concrete-only controls -------------------------------------------

  // Full 4-grey refresh: the canvas in black and white, plus `pixels` -- a
  // w x h 2bpp image in net::PhotoStore's layout (4 per byte, MSB first,
  // 0 = black .. 3 = white) -- drawn at (x, y). `pixels` may be null for a
  // grey frame of the canvas alone. Takes a few seconds and flashes.
  bool flushGrey(const uint8_t* pixels, int16_t x, int16_t y, int16_t w, int16_t h);

  // Fast black/white update of one rectangle of the canvas; everything outside
  // it stays as it is on the glass. x and w must be multiples of 8 (the
  // controller addresses RAM in whole bytes). Chosen on the real panel with
  // src/grey_test.cpp (variant B1) because it leaves grey photos intact.
  bool flushWindow(int16_t x, int16_t y, int16_t w, int16_t h);

  // Whether the last full frame on the glass was a grey one.
  bool greyOnGlass() const { return grey_on_glass_; }

  // Tells the next begin() to try restoring `path` from `storage` as the
  // panel's "previous image" with no refresh triggered, instead of starting
  // from a cleared controller: the glass keeps showing whatever was last
  // pushed to it, since E-Ink holds an image with no power. One-shot --
  // cleared by the begin() it applies to, whether or not it succeeds.
  //
  // Falls back to a normal initial begin() (GxEPD2 `initial = true`: clear
  // the controller and make the first refresh a full one) if `storage` is
  // null, the file is absent, or its size does not match framebufferBytes().
  // Call only on a real wake: on a cold boot the glass content is unknown.
  // See docs/architecture.md 4.2.
  void setRestorePath(hal::IStorage* storage, const char* path) {
    restore_storage_ = storage;
    restore_path_ = path;
  }

  // True if the begin() that just ran restored a frame. The caller's first
  // flush() can then be RefreshMode::kPartial instead of kFull.
  bool restoredFrame() const { return restored_from_storage_; }

  uint32_t framebufferBytes() const;
  bool framebufferInPsram() const { return fb_in_psram_; }
  const uint8_t* framebuffer() const { return framebuffer_; }
  // The frame as it is on the glass (canvas orientation, framebufferBytes()),
  // so a caller can tell which parts of a new canvas actually changed.
  const uint8_t* pushedFrame() const { return pushed_; }
  uint16_t partialsSinceFullRefresh() const { return partials_since_full_; }

  // --- flush timing, split ------------------------------------------------
  //
  // copy = canvas -> GxEPD2_4G buffer (CPU only); panel = the SPI transfer
  // plus the waveform. A grey frame interleaves the two page by page, so it
  // reports its whole time as panel time and 0 copy time.
  uint32_t lastFrameWriteMs() const { return last_write_ms_; }
  uint32_t lastWaveformMs() const { return last_waveform_ms_; }

 private:
  bool allocateFramebuffer();
  bool allocatePanel();
  bool loadRestoreFrame();
  bool writeRestoredFrame();
  bool panelFinished();

  gfx::Canvas canvas_;
  uint8_t* framebuffer_ = nullptr;
  // The frame as it is on the glass, in canvas orientation: the "old" plane
  // flushWindow() needs. Updated by every successful flush.
  uint8_t* pushed_ = nullptr;
  bool fb_in_psram_ = false;
  bool ready_ = false;
  bool grey_on_glass_ = false;

  uint16_t partials_since_full_ = 0;
  // What the last flush actually did, which is not always what it was asked
  // for -- see hal::IDisplay::lastRefreshMode().
  hal::RefreshMode last_mode_ = hal::RefreshMode::kFull;
  uint32_t last_write_ms_ = 0;
  uint32_t last_waveform_ms_ = 0;

  hal::IStorage* restore_storage_ = nullptr;
  const char* restore_path_ = nullptr;
  bool restored_from_storage_ = false;
};

}  // namespace drivers
