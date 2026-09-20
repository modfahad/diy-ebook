// app_config.h -- tunables that are policy, not hardware.
//
// Hardware facts live in board/board_crowpanel_579.h. Everything here is a
// product decision and is expected to change.

#pragma once

#include <stdint.h>

namespace app {

constexpr const char* kFirmwareVersion = "0.1.0-m1";
constexpr const char* kProductName = "Quran Reader";

// --- input timing ----------------------------------------------------------
constexpr uint16_t kButtonDebounceMs = 25;
constexpr uint16_t kEncoderDebounceMs = 2;   // encoder channels need to be fast
constexpr uint16_t kLongPressMs = 700;
constexpr uint16_t kInputPollIntervalMs = 2;

// Touch. The same long-press feel as the buttons, so "hold" means one thing
// whichever way it is done. The slop is how far a finger may slide and still
// count as a tap: 24 px is about 1.5 mm on this 800x480 7.5" panel, enough
// for the wobble of a finger landing and not enough to cross a menu row.
constexpr uint16_t kTouchLongPressMs = kLongPressMs;
constexpr uint16_t kTouchSlopPx = 24;
// The GT911 reports a frame on every scan while a finger is on the glass, so
// silence this long with a finger supposedly still down means the bus failed
// mid-gesture, not that the finger is resting. Without this the tracker would
// believe the finger is down forever, and hal::IInput::anyHeld() would block
// deep sleep for good -- on a battery device, a flat battery.
constexpr uint16_t kTouchStaleMs = 750;
// Tapping the left or right edge of a reading screen turns the page. 240 px
// of an 800 px panel each side leaves a 320 px band down the middle that does
// nothing, for the thumb of whoever is holding it.
constexpr uint16_t kTouchPageEdgePx = 240;

// What the touch layer does while the device sleeps. These two are the same
// decision from opposite ends, and cannot both be had:
//
//   kTouchSleepWithScreen  the GT911 is told to sleep (~100 uA instead of the
//                          ~8 mA it draws scanning). It notices nothing until
//                          the device is woken by a button.
//   kWakeOnTouch           the GT911 keeps scanning so a tap on the glass can
//                          pull its INT line and wake the device through
//                          EXT1 (board::kWakeMaskWithTouch).
//
// Sleeping wins by default. Waking on a tap is off until two things are
// confirmed on the bench, both of which the setup screen now shows: that INT
// idles HIGH and pulses LOW when touched (EXT1 here is ANY_LOW, so the wrong
// polarity means a device that wakes instantly and forever), and that the
// extra draw is acceptable on a battery this device does not have a gauge
// for yet. See board-test-checklist.md.
constexpr bool kTouchSleepWithScreen = true;
constexpr bool kWakeOnTouch = false;

// --- rotary coalescing (spec section 14) -----------------------------------
// Do not repaint until the user has paused this long, or this many detents
// have accumulated.
constexpr uint16_t kRotaryQuietMs = 140;
constexpr int16_t kRotaryMaxPending = 6;

// --- power -----------------------------------------------------------------
// How long the device stays awake with no input before returning to deep
// sleep. The E-Ink image survives, so this can be short.
constexpr uint32_t kIdleSleepMs = 20000;

// The device otherwise sleeps indefinitely (wakes only on input), and has no
// wall clock (no RTC, no NTP -- see net/protocol.h kSessionExpirySeconds).
// Without one, it cannot know how much real time passed during a multi-day
// sleep. This timer wake is what makes that knowable: it fires this often
// even with no input, takes one battery reading, and goes straight back to
// sleep (main.cpp, before storage/display come up) -- see util::battery.h
// for why the resulting (percent, elapsed) history, not an assumed current
// draw, is what the runtime estimate is built from.
constexpr uint32_t kBatterySampleIntervalMs = 30ul * 60ul * 1000ul;

// Informational only -- the remaining-runtime estimate (util::battery.h) is
// derived purely from observed percent-per-hour, which already normalises
// for pack size. Nothing in that formula reads this constant; it exists so
// the number is recorded somewhere once the pack is actually fitted.
constexpr uint32_t kBatteryCapacityMah = 3000;

// Refresh policy: a partial refresh every time accumulates ghosting, so force
// a full refresh every N partials.
constexpr uint16_t kPartialsBeforeFullRefresh = 8;

// --- networking (Milestone 4) -----------------------------------------------
// How long a BLE-commanded Wi-Fi connect attempt is given before it is
// reported back to the provisioning state machine as failed.
constexpr uint32_t kWifiConnectTimeoutMs = 15000;

// WiFiClient::setTimeout()'s parameter is named `seconds` on this Arduino
// core (WiFiClient.cpp: `tv.tv_sec = seconds;`) despite every other timeout
// in this codebase being milliseconds -- HttpServer::ReadChunkedBody used to
// pass 5000 expecting 5 seconds and got ~83 minutes instead (docs/pending.md,
// "Still unknown" note on the upload chunk-PUT investigation). Named here so
// the unit mismatch cannot recur silently at a second call site.
constexpr uint32_t kChunkedBodyReadTimeoutSec = 5;

// Transfer mode holds IdlePolicy busy for its whole duration (architecture.md
// 4.3), which defeats the normal 20 s idle timer by design -- this is the
// backstop against a session left open indefinitely (forgotten, or the
// client vanished) draining the battery. Not an activity timer: a session
// this long already implies minutes-long uploads are expected.
constexpr uint32_t kTransferSessionTimeoutMs = 30ul * 60ul * 1000ul;

// SNTP server for stamping LibraryEntry::installed_at during a transfer
// session (see HttpServer::HandleUploadFinish). Best-effort: if this
// doesn't resolve or the sync doesn't land before the entry is written,
// installed_at stays 0 -- "clock unset" -- exactly as it always has.
constexpr const char* kNtpServer = "pool.ntp.org";

// --- storage layout (spec sections 15/16) ----------------------------------
// Declared here in Milestone 1 so that later milestones do not each invent
// their own paths. Only the directories are created at this stage.
constexpr const char* kDirDevice = "/DEVICE";
constexpr const char* kDirUser = "/USER";
constexpr const char* kDirLibrary = "/LIBRARY";
constexpr const char* kDirQuran = "/LIBRARY/QURAN";
constexpr const char* kDirBooks = "/LIBRARY/BOOKS";
constexpr const char* kDirTranslations = "/LIBRARY/TRANSLATIONS";
constexpr const char* kDirTafsir = "/LIBRARY/TAFSIR";

constexpr const char* kFileState = "/DEVICE/state.bin";
constexpr const char* kFileSettings = "/DEVICE/settings.bin";
constexpr const char* kFileLibraryIndex = "/LIBRARY/library_index.bin";
constexpr const char* kFileSelfTest = "/DEVICE/selftest.log";
// Reading position (net::ReadingProgress) -- under /USER, not /DEVICE, so a
// package can be replaced without touching where the reader left off.
constexpr const char* kFileProgress = "/USER/progress.bin";
// The last frame actually pushed to the panel, written after every flush.
// Restored into the panel's RAM on a real wake so it doesn't have to flash
// white first -- see Epd750Display::setRestorePath and architecture.md 4.2.
constexpr const char* kFileFramebuffer = "/DEVICE/framebuffer.bin";

// --- home screen / table clock -------------------------------------------------
// The device is a USB-powered table clock: it boots to the home screen, keeps
// Wi-Fi and the HTTP API up so the desktop app can send photos and the time
// zone at any time, and never deep-sleeps. With this false it is the battery
// reader again: transfer mode on request, idle deep sleep.
constexpr bool kTableClockMode = true;

// Which way the page-picture reader (ui::PageImageScreen) turns a page so the
// device reads held upright. TODO(hw): confirm against how it is actually
// held -- if pages come out upside down, flip this.
constexpr bool kPortraitClockwise = true;

// Page pictures kept decoded in PSRAM: the page on screen and its neighbours,
// so turning one page never waits on the card. 48 KB each.
constexpr uint8_t kPageCacheSlots = 3;

// Books shelf covers in real greys: a shelf page is one 4-grey refresh, and
// moving the selection within it updates only the title strips (flushWindow),
// leaving the covers' greys on the glass. false: covers dithered to black and
// white and a framed selection, all fast partial refreshes. TODO(hw): confirm
// the window updates leave the covers intact (docs/board-test-checklist.md).
constexpr bool kShelfGreyCovers = true;

// A new photo, as a full 4-grey refresh, this often. Between them the clock
// updates each minute with a fast black/white window refresh.
constexpr uint8_t kPhotoIntervalMinutes = 10;

// Photos from the desktop app (net::PhotoStore).
constexpr const char* kDirPhotos = "/PHOTOS";
constexpr uint16_t kMaxPhotos = 200;

// POSIX TZ rule, written by POST /api/device/time. Absent means UTC.
constexpr const char* kFileTimeZone = "/DEVICE/timezone.txt";

// Which way up the picture is and which way round touch is, written by the
// setup screen (util::ScreenSetup). Absent means the board header's defaults.
constexpr const char* kFileScreenSetup = "/DEVICE/screen.txt";

// If Wi-Fi is lost in table-clock mode, try to reconnect this often.
constexpr uint32_t kWifiReconnectIntervalMs = 30000;

}  // namespace app
