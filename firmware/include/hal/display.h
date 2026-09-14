// display.h -- display hardware abstraction.
//
// Phase 2's renderers and Phase 1's UI only ever see this interface. The
// CrowPanel's dual-SSD1683 weirdness stays behind it.
//
// Lifecycle contract (important for the power model):
//   begin() is IDEMPOTENT and is called on EVERY wake from deep sleep, not
//   once at boot. The panel controller loses its state when it is put into
//   hibernate, so a driver that assumes a single init would break the whole
//   power design.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"

namespace hal {

enum class RefreshMode : uint8_t {
  kFull,     // slowest, best contrast, clears ghosting
  kPartial,  // fast, accumulates ghosting
  kFast,     // vendor "fast mode" full update
  kGrey,     // full 4-grey refresh (drivers::Epd750Display::flushGrey)
};

class IDisplay {
 public:
  virtual ~IDisplay() = default;

  // Powers the rail, resets and initialises the controller, allocates the
  // framebuffer if needed. Safe to call repeatedly. Returns false on failure.
  virtual bool begin() = 0;

  // Puts the controller into hibernate and drops the rail. The image stays
  // on the glass.
  virtual void end() = 0;

  virtual bool ready() const = 0;

  virtual uint16_t width() const = 0;
  virtual uint16_t height() const = 0;

  // The drawing surface. Only valid after a successful begin().
  virtual gfx::Canvas& canvas() = 0;

  // Pushes the framebuffer to the panel.
  virtual bool flush(RefreshMode mode) = 0;

  /**
   * The refresh mode the last flush() actually performed.
   *
   * Not the same as the mode it was ASKED for, which is the whole reason
   * this exists: an implementation may escalate -- Epd750Display promotes a
   * kPartial to a full refresh every kPartialsBeforeFullRefresh flushes, to
   * clear accumulated ghosting. Logging the requested mode instead made that
   * escalation invisible in serial and cost real debugging time on the
   * first-refresh-doesn't-commit investigation (docs/pending.md): a 2,096ms
   * "partial" in the log was a full refresh all along, and only the duration
   * gave it away.
   */
  virtual RefreshMode lastRefreshMode() const = 0;
};

}  // namespace hal
