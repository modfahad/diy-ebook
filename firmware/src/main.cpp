// main.cpp -- Phase 1, Milestone 1: hardware bring-up self-test.
//
// Flow (spec section 6):
//
//   wake -> read wake cause -> bring up SD + panel -> render -> wait for
//   input -> repaint -> idle -> drop rails -> deep sleep
//
// SD comes up before the panel (Milestone 4 addition): a real wake can
// restore the last frame from SD into the panel's RAM before begin() decides
// whether to start from a cleared controller, so that decision needs
// storage mounted first. See Epd750Display::setRestorePath.
//
// Nothing here knows a GPIO number; everything goes through hal:: interfaces
// so Milestone 2 can drop a QPK reader in beside this screen without touching
// the drivers.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app/app_config.h"
#include "app/surah_names.h"
#include "util/ascii_fold.h"
#include "drivers/serial_log.h"
#include "board/board_crowpanel_579.h"
#include "drivers/ble_provisioning.h"
#include "drivers/epd750_display.h"
#include "drivers/http_server.h"
#include "drivers/input_manager.h"
#include "drivers/net_credentials.h"
#include "drivers/power_manager.h"
#include "drivers/sd_storage.h"
#include "drivers/system_clock.h"
#include "drivers/touch_gt911.h"
#include "hal/display.h"
#include "hal/input.h"
#include "hal/power.h"
#include "hal/storage.h"
#include "net/library_index.h"
#include "net/photo_store.h"
#include "net/protocol.h"
#include "net/time_zone.h"
#include "net/provisioning_state.h"
#include "net/reading_progress.h"
#include "net/reset_log.h"
#include "net/upload_manager.h"
#include "qpk/book_reader.h"
#include "qpk/byte_order.h"
#include "qpk/glyph_blitter.h"
#include "qpk/qpk_reader.h"
#include "qpk/translation_text.h"
#include "net/verified_packages.h"
#include "ui/home_screen.h"
#include "ui/library_screen.h"
#include "ui/options_menu.h"
#include "ui/page_image_screen.h"
#include "ui/bookmarks_screen.h"
#include "net/bookmarks.h"
#include "ui/quran_screen.h"
#include "ui/reader_screen.h"
#include "ui/selftest_screen.h"
#include "ui/setup_screen.h"
#include "ui/surah_picker_screen.h"
#include "util/battery.h"
#include "util/idle_policy.h"
#include "util/screen_setup.h"
#include "util/touch.h"

namespace {

drivers::PowerManager g_power;
drivers::Epd750Display g_display;
drivers::SdStorage g_storage;
drivers::SystemClock g_clock;
drivers::InputManager g_input;
// The panel's touch layer. Additive: if the GT911 does not answer -- not
// fitted, not wired, ribbon the wrong way round -- begin() says so once and
// the front panel behaves exactly as it always has.
drivers::TouchGt911 g_touch;
util::IdlePolicy g_idle;
ui::SelfTestState g_state;

// Milestone 4: transport plumbing. See docs/protocol.md and
// docs/provisioning.md for the wire specs these serve.
drivers::NetCredentials g_net_credentials;
net::ProvisioningState g_provisioning;
drivers::BleProvisioning g_ble;
net::LibraryIndex g_library_index;
net::UploadManager g_upload_manager;
drivers::HttpServer g_http;
net::PhotoStore g_photos;

// Transfer mode: a provisioned device bringing Wi-Fi + the HTTP API up on
// request, per packages/protocol/src/index.ts's "Wi-Fi is on-demand" rule.
// Triggered by a long-press on the encoder switch -- a real menu action is
// Milestone 5's job, but the trigger itself does not have to wait for one;
// see ToggleTransferMode.
enum class TransferState : uint8_t { kOff, kConnecting, kOn };
TransferState g_transfer_state = TransferState::kOff;
uint32_t g_transfer_connect_started_ms = 0;
uint32_t g_transfer_session_started_ms = 0;
bool g_clock_sync_logged = false;
char g_transfer_address[24] = {0};

// Table-clock mode keeps Wi-Fi up (app::kTableClockMode). g_wifi_wanted is
// what the user last asked for -- on at boot, off after a long-press turns
// transfer mode off -- so a dropped connection is retried only while wanted.
bool g_wifi_wanted = app::kTableClockMode;
uint32_t g_wifi_retry_ms = 0;
uint32_t g_wifi_lost_ms = 0;
constexpr uint32_t kWifiLostGraceMs = 15000;

// Kept in RTC memory so the checklist survives deep sleep. Seeing these
// persist across a wake IS the deep-sleep test.
RTC_DATA_ATTR bool g_seen_menu = false;
RTC_DATA_ATTR bool g_seen_exit = false;
RTC_DATA_ATTR bool g_seen_rotary = false;
RTC_DATA_ATTR bool g_seen_switch = false;
RTC_DATA_ATTR int32_t g_counter = 0;
RTC_DATA_ATTR uint32_t g_refresh_count = 0;

// Cumulative real elapsed time, in ms, since the first boot -- NOT millis(),
// which resets on every deep-sleep wake. Advanced two ways, both exactly
// known rather than guessed: by app::kBatterySampleIntervalMs whenever a
// timer-only sampling wake completes its full sleep, and by millis() itself
// at GoToSleep(), which is exactly how long this session stayed awake. See
// util::BatteryEstimator for what it feeds.
RTC_DATA_ATTR uint64_t g_battery_clock_ms = 0;
RTC_DATA_ATTR util::BatteryEstimator g_battery_estimator;

// hal::WakeInfo::boot_count (PowerManager::begin()) counts every reset,
// sampling wakes included -- exactly right for a hardware/diagnostic count,
// but development.md's manual test ("press MENU while asleep, Boots
// increments by 1") stops meaning anything once a background timer can bump
// it too. This counts only the wakes setup() treats as real, so the
// self-test screen's "Boots:" row stays a meaningful manual-test signal.
RTC_DATA_ATTR uint32_t g_real_wake_count = 0;

char g_sd_detail[40] = "not mounted";
bool g_dirty = true;
bool g_force_full_refresh = true;   // first paint after init
uint32_t g_last_poll_ms = 0;

// Milestone 5's on-device menu (docs/pending.md). In table-clock mode the
// device boots to the home screen (clock, calendar, photos) and MENU cycles
// home -> library -> hardware self-test -> home; otherwise it boots to the
// library and MENU switches between the library and the self-test.
//
// Not RTC_DATA_ATTR on purpose: a real deep-sleep wake should land back on
// the start screen, not wherever the screen happened to be left before
// sleeping.
enum class ScreenMode : uint8_t {
  kHome, kLibrary, kSelfTest, kReader, kQuran, kSurahPicker,
  kPages,      // a book shown as page pictures (ui::PageImageScreen)
  kBookmarks,  // continue reading, and saved places (ui::BookmarksScreen)
  kScreenSetup,  // which way up, and which way round touch is (ui::SetupScreen)
};
ScreenMode g_screen_mode = app::kTableClockMode ? ScreenMode::kHome : ScreenMode::kLibrary;

// The options menu (ui::OptionsMenu). It is a modal over whatever screen is
// showing rather than a ScreenMode of its own: the screen underneath keeps
// its entire state, so closing the menu is a repaint, not a re-entry. Hold
// OK opens it anywhere; the wheel and OK work it, and so does a tap.
bool g_menu_open = false;
ui::OptionsMenuState g_menu;

// The screen-and-touch setup screen, and the screen to go back to when it
// closes. Its two settings are applied the moment they change -- turning the
// picture while looking at it is the only way to tell it is the right way up
// -- and written to the card on the way out.
ui::SetupState g_setup;
ScreenMode g_setup_return_to = ScreenMode::kSelfTest;

// The Quran reader. Unlike the book reader, which loads all its text into
// g_book_text and closes the file, this keeps the package OPEN for as long as
// the screen is showing: every glyph is a readAsset() off the card during the
// repaint, which is the streaming design the format exists for. The file
// handle is therefore a resource with a lifetime, closed by CloseQuran().
qpk::Reader g_quran_reader;
hal::IFile* g_quran_file = nullptr;
// PSRAM copy of the open package's glyph table (qpk::Reader::buildGlyphIndex).
// Allocated on first use and kept across packages. 8,192 entries is ~131 KB
// against the full Quran's 1,793 glyphs; a bigger atlas just keeps the scan.
constexpr uint32_t kGlyphIndexCapacity = 8192;
qpk::GlyphRecord* g_glyph_index_storage = nullptr;
ui::QuranState g_quran_state;
// Start indices of the screens already shown, so EXIT-free backward paging is
// possible. Laying out forward is cheap; laying out *backward* is not, because
// line breaks depend on measured glyph widths -- there is no way to know where
// the previous screen began without having laid it out. So remember.
uint32_t g_quran_history[ui::kQuranHistoryDepth] = {0};
uint8_t g_quran_history_depth = 0;

// The surah picker (docs/pending.md's biggest usability gap): OpenQuran()
// lands here instead of surah 1 unless a saved net::ReadingProgress matches
// the package just opened, and EXIT from ui::QuranScreen comes back here
// rather than all the way out to the library -- see the EXIT handler below.
// Reads g_quran_reader directly, the same way g_library_state.index reads
// g_library_index directly, so there is nothing to keep in sync here beyond
// selected/scroll_top.
ui::SurahPickerState g_surah_picker_state;
uint16_t g_surah_picker_selected = 0;
uint16_t g_surah_picker_scroll_top = 0;

ui::LibraryState g_library_state;
// Which level of the browser is showing, and (in the item level) which type.
// Plain globals like g_library_selected, not RTC_DATA_ATTR: a deep-sleep wake
// deliberately returns to the top of the library rather than resuming a
// half-navigated menu. Reading position is a separate, still-open item.
ui::LibraryView g_library_view = ui::LibraryView::kCategories;
uint16_t g_library_category = 0;
uint16_t g_library_selected = 0;
uint16_t g_library_scroll_top = 0;
char g_library_status[48] = "";
char g_quran_status[48] = "";
// Where the next screen starts, as reported by the last render(). Paging reads
// this rather than recomputing: only the renderer knows how many ayahs fit.
uint32_t g_quran_next_ayah_index = 0;

// The layout path's paging cursor. Parallel to g_quran_next_ayah_index, not a
// replacement: a package without LAYOUT_DATA still pages by ayah, exactly as
// the hardware-confirmed build does.
uint32_t g_quran_next_line_index = 0;
bool g_quran_used_layout = false;

// The book reader: BOOK packages only (plain Latin text, drawn with the
// same 5x7 font as the rest of the UI). QURAN/TRANSLATION/TAFSIR
// packages need Arabic shaping -- no on-device glyph atlas exists yet, see
// docs/pending.md -- so OpenBook() below refuses those with a status
// message rather than attempting them.
//
// A fixed, static budget rather than a dynamic allocation, matching
// qpk_reader.h's "no dynamic allocation" rule: generous for the one real
// BOOK package on the card today (a few KB), refusing anything bigger with
// BookLoadError::kTruncated instead of overrunning RAM.
constexpr uint32_t kBookTextBufferBytes = 48 * 1024;
char g_book_text[kBookTextBufferBytes];

// What the text reader (ReaderScreen + g_book_pager) is showing: a book's
// text in g_book_text, or one surah of a translation in g_trans_text.
const char* g_reader_body = g_book_text;
uint32_t g_reader_body_length = 0;

// Translation / Tafsir reader: one surah at a time as numbered verses,
// through the text reader. Surah boundaries come from the Quran package the
// translation is aligned to (its metadata key 9); without that package on
// the card the whole translation reads as one run of numbered ayahs.
constexpr uint32_t kTranslationTextBytes = 256 * 1024;  // Al-Baqarah fits
char* g_trans_text = nullptr;                             // PSRAM
uint32_t g_trans_length = 0;
hal::IFile* g_trans_file = nullptr;
qpk::Reader g_trans_reader;
bool g_reader_is_translation = false;
uint16_t g_trans_surah = 1;
uint16_t g_trans_surah_count = 0;  // 0: no aligned Quran, one run
struct TransSurah {
  uint16_t ayah_count = 0;
  uint32_t first_ayah_index = 0;
};
TransSurah g_trans_surahs[114];

// The surah list is shared with the Quran reader (ScreenMode::kSurahPicker);
// this says it was opened from a translation, so OK, EXIT and MENU there act
// on the translation instead of the Quran package.
bool g_surah_picker_translation = false;
uint16_t g_trans_ayah_counts[114];

// Buttons in a translation (as asked on the board, 2026-09-14): OK = surah
// list, hold OK = text size, MENU = bookmark. Text books keep OK = size and
// hold OK = bookmark.
constexpr const char* kTranslationHint = "OK=surah hold=size MENU=mark";
// A one-off footer notice in the text reader ("Bookmarked ..."), cleared by
// the next page turn or size change.
char g_reader_notice[48] = "";

// A surah's stretch of TRANSLATION_DATA, read in one go
// (qpk::AppendTranslationVerses). Al-Baqarah's is about 75 KB; a longer
// tafsir surah that does not fit falls back to a read per verse.
constexpr uint32_t kTranslationScratchBytes = 192 * 1024;
char* g_trans_scratch = nullptr;  // PSRAM, allocated on first use

// Packages whose index checksums already passed (/DEVICE/verified.bin).
net::VerifiedPackages g_verified;
char g_trans_name[net::kTitleMaxBytes] = "";
uint32_t g_book_text_length = 0;
char g_book_title[net::kTitleMaxBytes] = "";

// /USER/bookmarks.bin, loaded at boot (see the Bookmarks section below).
net::Bookmarks g_bookmarks;

// Page-picture books (docs/qpk-format.md 9d). The package stays open while it
// is read, and the page on screen plus its neighbours are kept decoded in
// PSRAM (app::kPageCacheSlots), so a page turn is a redraw, not a card read.
struct PageCacheSlot {
  int32_t page = -1;  // 0-based; -1 = empty
  uint8_t* bits = nullptr;
};
PageCacheSlot g_page_cache[app::kPageCacheSlots];
uint8_t* g_page_scratch = nullptr;  // one page's compressed bytes while decoding
hal::IFile* g_page_file = nullptr;
qpk::Reader g_page_reader;
uint32_t g_page_count = 0;
uint32_t g_page_index = 0;              // 0-based, the page on screen
int32_t g_page_prefetch_failed = -1;    // a page that would not decode; not retried
// Where the last page-picture book was left, so reopening it resumes there.
uint8_t g_page_resume_id[16] = {0};
uint32_t g_page_resume_index = 0;
// The position is written to /USER/pages.bin a few seconds after the last
// page turn (not on every turn: an SD write per turn costs latency), and on
// every way out of the book.
bool g_page_progress_dirty = false;
uint32_t g_page_turn_ms = 0;
// OK while reading opens "Go to page": the wheel moves the target
// ui::kPageJumpStep pages a detent, OK goes there, EXIT stays put.
bool g_page_jump_mode = false;
uint32_t g_page_jump_target = 0;  // 0-based
constexpr uint32_t kPageProgressSaveDelayMs = 3000;
util::TextPager g_book_pager;
util::LineSlice g_book_lines[ui::kReaderMaxLines];
uint16_t g_book_page = 1;
uint8_t g_book_scale = 4;  // OK in the text reader cycles it (CycleBookTextSize)

// Forward-declared so GoToSleep() -- defined below but unguarded, since it
// runs for every build variant -- can save the reading position before
// sleeping. The real definition is with the rest of the Quran helpers, near
// CloseQuran().
void SaveQuranProgress();


// A no-op on this board revision until board::kBatteryAdcPin is resolved
// (batteryAvailable() is false, so nothing is added to the estimator) -- see
// util::battery.h and board_crowpanel_579.h. Safe to call often: it is one
// ADC read, no display or storage I/O.
void SampleBattery() {
  if (!g_power.batteryAvailable()) return;
  const uint8_t percent =
      util::BatteryPercentFromMillivolts(g_power.batteryMillivolts());
  g_battery_estimator.addSample(percent, g_battery_clock_ms);
}

ui::TestResult Flag(bool ok) {
  return ok ? ui::TestResult::kPass : ui::TestResult::kUntested;
}

void RefreshState() {
  g_state.rotary = Flag(g_seen_rotary);
  g_state.menu = Flag(g_seen_menu);
  g_state.exit = Flag(g_seen_exit);
  g_state.encoder_switch = Flag(g_seen_switch);
  g_state.input = g_input.diagnostics();
  g_state.wake = g_power.wakeInfo();
  g_state.real_wake_count = g_real_wake_count;
  g_state.wake_reason_name =
      drivers::PowerManager::wakeReasonName(g_state.wake.reason);
  g_state.firmware_version = app::kFirmwareVersion;
  g_state.ui_counter = g_counter;
  g_state.refresh_count = g_refresh_count;
  g_state.sd_detail = g_sd_detail;
  g_state.transfer_connecting = (g_transfer_state == TransferState::kConnecting);
  g_state.transfer_mode = (g_transfer_state == TransferState::kOn);
  g_state.transfer_address = g_transfer_address;
  g_state.pairing_mode = g_ble.active();

  g_state.battery_available = g_power.batteryAvailable();
  if (g_state.battery_available && g_battery_estimator.hasSamples()) {
    g_state.battery_percent = g_battery_estimator.latestPercent();
    util::FormatRemaining(g_battery_estimator.remainingMs(),
                          g_state.battery_remaining,
                          sizeof(g_state.battery_remaining));
  }

  // The library index is already in RAM (BringUpNetState loaded or rebuilt it
  // at boot), so this costs nothing and is the only place an installed
  // package is visible until the reader lands in Milestone 5.
  g_state.library_count = g_library_index.count();
  const net::LibraryEntry* first = g_library_index.at(0);
  g_state.library_title = (first != nullptr) ? first->title : "";
}

// Covers for the page of the Books shelf on screen, read straight from each
// package (qpk::ReadCoverFromFile: no checksum sweep) and cached by
// content_id, so moving within a page of covers costs no card reads at all.
struct ShelfCover {
  uint8_t id[16] = {0};
  bool loaded = false;
  bool present = false;
  uint8_t* pixels = nullptr;  // qpk::kCoverPixelBytes, in PSRAM
};
ShelfCover g_shelf[ui::kLibraryShelfPageTiles];

void LoadShelfCovers() {
  const uint16_t start = ui::LibraryScreen::shelfPageStart(g_library_state);
  for (uint8_t slot = 0; slot < ui::kLibraryShelfPageTiles; ++slot) {
    uint16_t ordinal = 0;
    if (ui::LibraryScreen::rowKind(g_library_state, static_cast<uint16_t>(start + slot),
                                   &ordinal) != ui::LibraryRow::kItem) {
      continue;
    }
    const net::LibraryEntry* entry =
        ui::LibraryScreen::entryOfType(g_library_state, g_library_state.category, ordinal);
    if (entry == nullptr) continue;

    ShelfCover& cover = g_shelf[slot];
    if (!cover.loaded || memcmp(cover.id, entry->content_id, sizeof(cover.id)) != 0) {
      memcpy(cover.id, entry->content_id, sizeof(cover.id));
      cover.loaded = true;
      cover.present = false;
      if (cover.pixels == nullptr) {
        cover.pixels = static_cast<uint8_t*>(ps_malloc(qpk::kCoverPixelBytes));
      }
      char path[72];
      if (cover.pixels != nullptr && net::BuildPackagePath(*entry, path, sizeof(path))) {
        if (hal::IFile* file = g_storage.open(path)) {
          cover.present = qpk::ReadCoverFromFile(file, cover.pixels, qpk::kCoverPixelBytes) ==
                          qpk::Error::kOk;
          file->close();
        }
      }
    }
    if (cover.present) g_library_state.shelf_covers[slot] = cover.pixels;
  }
}

void RefreshLibraryState() {
  g_library_state.index = &g_library_index;
  g_library_state.view = g_library_view;
  g_library_state.category = g_library_category;
  g_library_state.selected = g_library_selected;
  g_library_state.scroll_top = g_library_scroll_top;
  g_library_state.transfer_ready = g_provisioning.ready();
  g_library_state.transfer_connecting =
      (g_transfer_state == TransferState::kConnecting);
  g_library_state.transfer_mode = (g_transfer_state == TransferState::kOn);
  g_library_state.transfer_address = g_transfer_address;
  g_library_state.status_message = g_library_status;
  g_library_state.bookmark_count = g_bookmarks.count();
  for (const uint8_t*& cover : g_library_state.shelf_covers) cover = nullptr;
  if (ui::LibraryScreen::isShelf(g_library_state) && g_storage.mounted()) LoadShelfCovers();
  g_library_state.grey_covers =
      app::kShelfGreyCovers && ui::LibraryScreen::isShelf(g_library_state);
}

// --- Books shelf in greys (app::kShelfGreyCovers) ------------------------------
//
// A shelf page goes to the panel as one 4-grey refresh: the canvas in black
// and white plus every cover as grey pixels. Moving the selection within that
// page changes only the title strips (the selected title is drawn inverted),
// so those alone are updated with flushWindow(), which leaves the covers' greys
// on the glass. Anything else that changed -- a new page, a status line, the
// glass showing some other frame -- gets a fresh grey refresh.

void PersistFramebuffer();  // below, with the other flush helpers

uint8_t* g_shelf_grey = nullptr;     // ui::kShelfGreyBytes, allocated on first use
int32_t g_shelf_grey_page = -1;      // shelf page start last drawn in grey, -1 none
uint16_t g_shelf_grey_selected = 0;  // the selection that frame showed

// True if the new canvas differs from the glass only inside the title bands of
// shelf rows `row_a` and `row_b`; `*changed` says whether it differs at all.
bool OnlyShelfTitlesChanged(uint8_t row_a, uint8_t row_b, bool* changed) {
  const uint8_t* now = g_display.framebuffer();
  const uint8_t* was = g_display.pushedFrame();
  *changed = false;
  if (now == nullptr || was == nullptr) return false;
  const uint32_t stride = g_display.width() / 8;
  const uint32_t rows = g_display.framebufferBytes() / stride;
  int ax = 0, ay = 0, aw = 0, ah = 0, bx = 0, by = 0, bw = 0, bh = 0;
  ui::LibraryScreen::shelfTitleBand(row_a, &ax, &ay, &aw, &ah);
  ui::LibraryScreen::shelfTitleBand(row_b, &bx, &by, &bw, &bh);
  for (uint32_t y = 0; y < rows; ++y) {
    const bool in_band = (static_cast<int>(y) >= ay && static_cast<int>(y) < ay + ah) ||
                         (static_cast<int>(y) >= by && static_cast<int>(y) < by + bh);
    for (uint32_t b = 0; b < stride; ++b) {
      if (now[y * stride + b] == was[y * stride + b]) continue;
      *changed = true;
      // Both bands span the same columns (the whole shelf).
      const bool in_columns =
          static_cast<int>(b * 8) >= ax && static_cast<int>(b * 8) < ax + aw;
      if (!in_band || !in_columns) return false;
    }
  }
  return true;
}

void FlushShelfGrey() {
  const uint16_t page = ui::LibraryScreen::shelfPageStart(g_library_state);
  const uint16_t selected = g_library_state.selected;
  const auto shelf_row = [](uint16_t row) {
    return static_cast<uint8_t>((row % ui::kLibraryShelfPageTiles) / ui::kLibraryShelfColumns);
  };
  const uint8_t old_row = shelf_row(g_shelf_grey_selected);
  const uint8_t new_row = shelf_row(selected);
  const uint32_t start = millis();

  bool changed = true;
  bool ok = true;
  if (!g_force_full_refresh && g_display.greyOnGlass() &&
      g_shelf_grey_page == static_cast<int32_t>(page) &&
      OnlyShelfTitlesChanged(old_row, new_row, &changed)) {
    if (changed) {
      int x = 0, y = 0, w = 0, h = 0;
      ui::LibraryScreen::shelfTitleBand(old_row, &x, &y, &w, &h);
      ok = g_display.flushWindow(x, y, w, h);
      if (new_row != old_row) {
        ui::LibraryScreen::shelfTitleBand(new_row, &x, &y, &w, &h);
        ok = g_display.flushWindow(x, y, w, h) && ok;
      }
      drivers::Logf("[library] shelf selection %u -> %u: %u title strip(s) flush=%d in %lums",
                    static_cast<unsigned>(g_shelf_grey_selected),
                    static_cast<unsigned>(selected), new_row != old_row ? 2u : 1u, ok ? 1 : 0,
                    static_cast<unsigned long>(millis() - start));
    }
  } else {
    if (g_shelf_grey == nullptr) {
      g_shelf_grey = static_cast<uint8_t*>(ps_malloc(ui::kShelfGreyBytes));
    }
    if (g_shelf_grey != nullptr) {
      ui::LibraryScreen::composeShelfGrey(g_library_state, g_shelf_grey);
    }
    // Without the buffer the covers stay blank, but the shelf still works.
    ok = g_display.flushGrey(g_shelf_grey, ui::kShelfGreyX, ui::kShelfGreyY,
                             ui::kShelfGreyW, ui::kShelfGreyH);
    g_shelf_grey_page = ok ? static_cast<int32_t>(page) : -1;
    drivers::Logf("[library] shelf page %u in grey%s flush=%d in %lums",
                  static_cast<unsigned>(page), g_shelf_grey ? "" : " (no buffer: blank covers)",
                  ok ? 1 : 0, static_cast<unsigned long>(millis() - start));
  }
  g_shelf_grey_selected = selected;
  if (changed) {
    if (ok) {
      ++g_refresh_count;
      PersistFramebuffer();
    } else {
      drivers::LogLine("[epd] shelf flush failed (panel BUSY timeout?)");
    }
  }
}

// ---------------------------------------------------------------------------
// Home screen (table clock): clock and calendar on the left, a photo on the
// right. PollHomeScreen() owns this screen's refreshes rather than Repaint():
//   * a full 4-grey frame when the screen is entered, when the date changes,
//     when the clock first syncs, and every app::kPhotoIntervalMinutes with
//     the next photo;
//   * on every other minute, a fast black/white update of the clock digits
//     alone (Epd750Display::flushWindow), which leaves the photo's greys be.
// ---------------------------------------------------------------------------

ui::HomeState g_home_state;
char g_home_status[96] = "";
bool g_home_full_pending = true;
bool g_home_photos_dirty = true;   // reload the photo list before the next frame
bool g_home_ever_drawn = false;
bool g_home_shown_valid = false;   // whether the glass shows a real time
int g_home_shown_minute = -1;
int g_home_shown_yday = -1;
uint32_t g_home_last_grey_ms = 0;

// A burst of uploads from the desktop app must not become a burst of
// multi-second, flashing grey refreshes.
constexpr uint32_t kHomeMinGreyGapMs = 15000;

net::PhotoName* g_photo_names = nullptr;  // app::kMaxPhotos, allocated on first use
uint8_t* g_photo_pixels = nullptr;        // net::kPhotoPixelBytes, in PSRAM
uint16_t g_photo_count = 0;
uint16_t g_photo_next = 0;                // index of the photo to show next
uint16_t g_photo_shown = 0;               // 1-based; 0 = none on screen
bool g_photo_loaded = false;

// Which way up the picture is, and which way round touch is. Both start as
// the board header's build-time defaults and are overridden by whatever the
// setup screen last saved, so a device that sits upside down in its case, or
// a touch layer wired with its axes swapped, is a setting rather than a
// rebuild. A missing or unreadable file means "use the defaults".
// Seeded from the board header rather than left at zero: LoadScreenSetup()
// only runs when the card mounted, and ApplyScreenSetup() runs either way.
util::ScreenSetup g_screen_setup = {
    board::kDisplayRotation,
    static_cast<uint8_t>((board::kTouchSwapXY ? 1 : 0) |
                         (board::kTouchInvertX ? 2 : 0) |
                         (board::kTouchInvertY ? 4 : 0)),
};

void LoadScreenSetup() {
  g_screen_setup.rotation = board::kDisplayRotation;
  g_screen_setup.touch_orientation = static_cast<uint8_t>(
      (board::kTouchSwapXY ? 1 : 0) | (board::kTouchInvertX ? 2 : 0) |
      (board::kTouchInvertY ? 4 : 0));

  char text[util::kScreenSetupMaxChars + 1] = {0};
  const int32_t n =
      g_storage.read(app::kFileScreenSetup, 0, text, sizeof(text) - 1);
  if (n > 0) {
    text[n] = '\0';
    if (!util::ParseScreenSetup(text, &g_screen_setup)) {
      drivers::Logf("[setup] ignoring unreadable %s\n", app::kFileScreenSetup);
    }
  }
  drivers::Logf("[setup] rotation=%u touch=%u%s\n",
                static_cast<unsigned>(g_screen_setup.rotation),
                static_cast<unsigned>(g_screen_setup.touch_orientation),
                n > 0 ? "" : " (defaults, nothing saved yet)");
}

// Applies what is in g_screen_setup to the panel and to the input manager.
void ApplyScreenSetup() {
  g_display.setRotation(g_screen_setup.rotation);
  g_input.setTouchOrientation(g_screen_setup.touch_orientation);
}

bool SaveScreenSetup() {
  char text[util::kScreenSetupMaxChars + 1] = {0};
  const uint8_t n =
      util::FormatScreenSetup(g_screen_setup, text, sizeof(text) - 1);
  if (n == 0) return false;
  if (!g_storage.mounted()) {
    drivers::LogLine("[setup] no card -- the setting holds until the next restart");
    return false;
  }
  const bool ok = g_storage.writeAll(app::kFileScreenSetup, text, n);
  drivers::Logf("[setup] %s %s: %s", ok ? "saved" : "FAILED to save",
                app::kFileScreenSetup, text);
  return ok;
}

// Applies the time zone the desktop app last sent (POST /api/device/time).
void LoadTimeZone() {
  char tz[net::kTimeZoneMaxChars + 1] = {0};
  const int32_t n = g_storage.read(app::kFileTimeZone, 0, tz, sizeof(tz) - 1);
  if (n <= 0) {
    drivers::LogLine("[time] no time zone saved yet, the clock shows UTC");
    return;
  }
  tz[n] = '\0';
  for (int32_t i = n - 1; i >= 0 && (tz[i] == '\n' || tz[i] == '\r' || tz[i] == ' '); --i) {
    tz[i] = '\0';
  }
  if (!net::ValidTimeZone(tz)) {
    drivers::Logf("[time] ignoring invalid saved time zone \"%s\"", tz);
    return;
  }
  g_clock.setTimeZone(tz);
  drivers::Logf("[time] time zone \"%s\"", tz);
}

// Moves to the next photo in name order (wrapping) and loads its pixels.
void AdvancePhoto() {
  if (g_photo_names == nullptr) g_photo_names = new net::PhotoName[app::kMaxPhotos];
  if (g_photo_pixels == nullptr) {
    g_photo_pixels = static_cast<uint8_t*>(ps_malloc(net::kPhotoPixelBytes));
  }
  g_photo_loaded = false;
  g_photo_shown = 0;
  if (g_photo_pixels == nullptr || !g_storage.mounted()) {
    g_photo_count = 0;
    return;
  }
  g_photo_count = g_photos.list(g_photo_names, app::kMaxPhotos);
  if (g_photo_count == 0) return;
  if (g_photo_next >= g_photo_count) g_photo_next = 0;
  const char* name = g_photo_names[g_photo_next].name;
  g_photo_loaded = g_photos.load(name, g_photo_pixels, net::kPhotoPixelBytes);
  g_photo_shown = g_photo_loaded ? static_cast<uint16_t>(g_photo_next + 1) : 0;
  drivers::Logf("[home] photo %u/%u \"%s\" %s", static_cast<unsigned>(g_photo_next + 1),
                static_cast<unsigned>(g_photo_count), name,
                g_photo_loaded ? "loaded" : "could not be read");
  g_photo_next = static_cast<uint16_t>((g_photo_next + 1) % g_photo_count);
}

bool HomeLocalTime(struct tm* local) {
  if (!g_clock.synced()) return false;
  const time_t now = time(nullptr);
  localtime_r(&now, local);
  return true;
}

void FillHomeState(bool valid, const struct tm& local) {
  g_home_state.time_valid = valid;
  if (valid) {
    g_home_state.year = static_cast<uint16_t>(local.tm_year + 1900);
    g_home_state.month = static_cast<uint8_t>(local.tm_mon + 1);
    g_home_state.day = static_cast<uint8_t>(local.tm_mday);
    g_home_state.hour = static_cast<uint8_t>(local.tm_hour);
    g_home_state.minute = static_cast<uint8_t>(local.tm_min);
  }
  g_home_state.has_photo = g_photo_loaded;

  const char* wifi = g_transfer_state == TransferState::kOn ? g_transfer_address
                     : g_transfer_state == TransferState::kConnecting ? "connecting"
                                                                      : "offline";
  if (g_photo_count > 0) {
    snprintf(g_home_status, sizeof(g_home_status), "Wi-Fi %s  %s  photo %u/%u", wifi,
             valid ? "time synced" : "time not synced",
             static_cast<unsigned>(g_photo_shown), static_cast<unsigned>(g_photo_count));
  } else {
    snprintf(g_home_status, sizeof(g_home_status), "Wi-Fi %s  %s  no photos", wifi,
             valid ? "time synced" : "time not synced");
  }
  g_home_state.status = g_home_status;
}

void HomeGreyFrame(bool valid, const struct tm& local) {
  FillHomeState(valid, local);
  ui::HomeScreen::render(g_display.canvas(), g_home_state);
  const unsigned long clipped = g_display.canvas().clippedPixels();
  const uint32_t start = millis();
  const bool ok = g_display.flushGrey(g_photo_loaded ? g_photo_pixels : nullptr,
                                      ui::kHomePhotoX, ui::kHomePhotoY,
                                      ui::kHomePhotoW, ui::kHomePhotoH);
  drivers::Logf("[home] grey frame %02u:%02u%s photo=%u/%u clipped=%lu flush=%d in %lums",
                static_cast<unsigned>(g_home_state.hour),
                static_cast<unsigned>(g_home_state.minute), valid ? "" : " (not synced)",
                static_cast<unsigned>(g_photo_shown), static_cast<unsigned>(g_photo_count),
                clipped, ok ? 1 : 0, static_cast<unsigned long>(millis() - start));
  if (ok) {
    ++g_refresh_count;
  } else {
    drivers::LogLine("[home] grey frame failed (panel BUSY timeout?)");
  }
  g_home_shown_valid = valid;
  g_home_shown_minute = valid ? local.tm_min : -1;
  g_home_shown_yday = valid ? local.tm_yday : -1;
  g_home_last_grey_ms = millis();
  g_home_full_pending = false;
  g_home_ever_drawn = true;
}

void HomeClockTick(const struct tm& local) {
  FillHomeState(true, local);
  ui::HomeScreen::renderClock(g_display.canvas(), g_home_state);
  const bool ok = g_display.flushWindow(ui::kHomeClockX, ui::kHomeClockY,
                                        ui::kHomeClockW, ui::kHomeClockH);
  drivers::Logf("[home] clock %02d:%02d window flush=%d (copy=%lums panel=%lums)",
                local.tm_hour, local.tm_min, ok ? 1 : 0,
                static_cast<unsigned long>(g_display.lastFrameWriteMs()),
                static_cast<unsigned long>(g_display.lastWaveformMs()));
  if (ok) ++g_refresh_count;
  g_home_shown_minute = local.tm_min;
}

// Called every loop() pass; decides whether the home screen needs anything.
void PollHomeScreen(uint32_t now) {
  if (g_screen_mode != ScreenMode::kHome || !g_display.ready()) return;

  struct tm local = {};
  const bool valid = HomeLocalTime(&local);

  if (g_home_photos_dirty) {
    g_home_photos_dirty = false;
    AdvancePhoto();
    g_home_full_pending = true;
  }

  bool full = g_home_full_pending;
  if (!full && valid) {
    if (!g_home_shown_valid || local.tm_yday != g_home_shown_yday) {
      full = true;
    } else if (local.tm_min != g_home_shown_minute) {
      if (local.tm_min % app::kPhotoIntervalMinutes == 0) {
        AdvancePhoto();
        full = true;
      } else {
        HomeClockTick(local);
        return;
      }
    }
  } else if (!full && !valid &&
             now - g_home_last_grey_ms >= app::kPhotoIntervalMinutes * 60000UL) {
    // No clock yet: the photos still rotate, on uptime.
    AdvancePhoto();
    full = true;
  }
  if (!full) return;

  if (g_home_ever_drawn && now - g_home_last_grey_ms < kHomeMinGreyGapMs) {
    g_home_full_pending = true;  // drawn once the gap has passed
    return;
  }
  HomeGreyFrame(valid, local);
}

// Saves the frame just pushed to the panel so a future wake can restore it
// instead of flashing white -- see Epd750Display::setRestorePath. Written
// once per real repaint, not once per sleep: a device sitting idle with
// nothing drawn has nothing new worth persisting.
void PersistFramebuffer() {
  if (g_state.sd != ui::TestResult::kPass) return;
  g_storage.writeAll(app::kFileFramebuffer, g_display.framebuffer(),
                     g_display.framebufferBytes());
}

#ifdef EPD_SMOKE_TEST
// Bring-up aid. Drives the panel to solid black, then solid white, holding
// each long enough to be unmistakable.
//
// It exists because "the screen is blank" is an unreliable observation: a
// text layout seen through the shipping film, or glanced at before the
// ~2 s refresh finishes, looks exactly like a panel that never drew. A full
// black fill does not. Build with -DEPD_SMOKE_TEST=1; it is not in the
// product build.
void RunPanelSmokeTest() {
  struct Step {
    gfx::Color fill;
    const char* name;
  };
  static const Step kSteps[] = {{gfx::kBlack, "black"}, {gfx::kWhite, "white"}};

  for (const Step& step : kSteps) {
    // Through the canvas rather than memset: it goes down the same path a
    // real screen does, seam mapping included.
    g_display.canvas().clear(step.fill);
    const uint32_t start = millis();
    const bool ok = g_display.flush(hal::RefreshMode::kFull);
    drivers::Logf("[epd] smoke fill=%s flush=%d in %lums\n", step.name,
                  ok ? 1 : 0, static_cast<unsigned long>(millis() - start));
    delay(3000);
  }
}
#endif




bool EnsurePageBuffers() {
  if (g_page_scratch == nullptr) {
    g_page_scratch = static_cast<uint8_t*>(ps_malloc(qpk::kPageImageMaxCompressedBytes));
  }
  bool ok = g_page_scratch != nullptr;
  for (PageCacheSlot& slot : g_page_cache) {
    if (slot.bits == nullptr) slot.bits = static_cast<uint8_t*>(ps_malloc(qpk::kPageImageBytes));
    ok = ok && slot.bits != nullptr;
  }
  return ok;
}

bool PageCached(uint32_t index) {
  for (const PageCacheSlot& slot : g_page_cache) {
    if (slot.page == static_cast<int32_t>(index)) return true;
  }
  return false;
}

// Decoded page `index`, from memory or the card. A card read replaces the
// cached page farthest from the one on screen. nullptr if it cannot be read.
const uint8_t* PageBits(uint32_t index) {
  for (const PageCacheSlot& slot : g_page_cache) {
    if (slot.page == static_cast<int32_t>(index)) return slot.bits;
  }

  PageCacheSlot* victim = &g_page_cache[0];
  uint32_t farthest = 0;
  for (PageCacheSlot& slot : g_page_cache) {
    if (slot.page < 0) {
      victim = &slot;
      break;
    }
    const uint32_t page = static_cast<uint32_t>(slot.page);
    const uint32_t distance = page > g_page_index ? page - g_page_index : g_page_index - page;
    if (distance >= farthest) {
      farthest = distance;
      victim = &slot;
    }
  }

  const uint32_t start = millis();
  qpk::PageImageRecord record;
  qpk::Error err = g_page_reader.getPageImage(index, &record);
  if (err == qpk::Error::kOk &&
      (record.width != qpk::kPageImageWidth || record.height != qpk::kPageImageHeight)) {
    err = qpk::Error::kSectionSizeMismatch;
  }
  if (err == qpk::Error::kOk) {
    err = g_page_reader.readPageImage(index, g_page_scratch, qpk::kPageImageMaxCompressedBytes,
                                      victim->bits, qpk::kPageImageBytes);
  }
  if (err != qpk::Error::kOk) {
    victim->page = -1;
    drivers::Logf("[pages] page %u could not be read: %s", static_cast<unsigned>(index + 1),
                  qpk::ErrorText(err));
    return nullptr;
  }
  victim->page = static_cast<int32_t>(index);
  // Only slow reads are worth a line: one per page turn flooded the serial
  // log (2026-09-14) and hid everything else in it.
  const uint32_t elapsed = millis() - start;
  if (elapsed >= 150) {
    drivers::Logf("[pages] slow read: page %u (%u KB) took %lums", static_cast<unsigned>(index + 1),
                  static_cast<unsigned>(record.length / 1024), static_cast<unsigned long>(elapsed));
  }
  return victim->bits;
}

void SavePageProgress() {
  if (!g_page_progress_dirty || !g_page_reader.isOpen() || !g_storage.mounted()) return;
  net::PageProgress progress;
  memcpy(progress.content_id, g_page_resume_id, sizeof(progress.content_id));
  progress.page = g_page_index;
  if (net::SavePageProgress(&g_storage, progress)) {
    g_page_progress_dirty = false;
  } else {
    drivers::LogLine("[pages] could not save the reading position");
  }
}

// One not-yet-decoded neighbour of the page on screen per loop pass, while
// nothing is waiting to be drawn: the next page first, where reading goes.
void PollPagePrefetch() {
  if (g_screen_mode != ScreenMode::kPages || g_dirty || !g_page_reader.isOpen()) return;
  if (g_page_progress_dirty && millis() - g_page_turn_ms >= kPageProgressSaveDelayMs) {
    SavePageProgress();
    return;
  }
  const uint32_t next = g_page_index + 1;
  if (next < g_page_count && !PageCached(next) &&
      g_page_prefetch_failed != static_cast<int32_t>(next)) {
    if (PageBits(next) == nullptr) g_page_prefetch_failed = static_cast<int32_t>(next);
    return;
  }
  if (g_page_index == 0) return;
  const uint32_t previous = g_page_index - 1;
  if (!PageCached(previous) && g_page_prefetch_failed != static_cast<int32_t>(previous)) {
    if (PageBits(previous) == nullptr) g_page_prefetch_failed = static_cast<int32_t>(previous);
  }
}

void ClosePageBook() {
  SavePageProgress();  // before the reader closes
  if (g_page_reader.isOpen()) g_page_reader.close();
  if (g_page_file != nullptr) {
    g_page_file->close();
    g_page_file = nullptr;
  }
  for (PageCacheSlot& slot : g_page_cache) slot.page = -1;
  g_page_count = 0;
}

// Takes over `file` and g_page_reader, which OpenBook() has already opened on
// it and found to carry page pictures -- opening a package verifies its index
// checksums, so doing it twice doubled the wait.
void OpenPageBook(const net::LibraryEntry& entry, hal::IFile* file, const char* path) {
  if (!EnsurePageBuffers()) {
    g_page_reader.close();
    file->close();
    snprintf(g_library_status, sizeof(g_library_status), "Not enough memory for page pictures");
    g_dirty = true;
    return;
  }

  g_page_file = file;
  g_page_count = g_page_reader.pageImageCount();
  g_page_prefetch_failed = -1;
  g_page_progress_dirty = false;
  g_page_jump_mode = false;
  bool same_book = memcmp(g_page_resume_id, entry.content_id, sizeof(g_page_resume_id)) == 0;
  if (!same_book && g_storage.mounted()) {
    // After a restart the position is only on the card.
    net::PageProgress saved;
    if (net::LoadPageProgress(&g_storage, &saved) &&
        memcmp(saved.content_id, entry.content_id, sizeof(saved.content_id)) == 0) {
      g_page_resume_index = saved.page;
      same_book = true;
    }
  }
  g_page_index = same_book && g_page_resume_index < g_page_count ? g_page_resume_index : 0;
  memcpy(g_page_resume_id, entry.content_id, sizeof(g_page_resume_id));
  g_page_resume_index = g_page_index;
  snprintf(g_book_title, sizeof(g_book_title), "%s", entry.title);

  g_screen_mode = ScreenMode::kPages;
  g_force_full_refresh = true;
  g_library_status[0] = 0;
  g_dirty = true;
  drivers::Logf("[pages] opened %s \"%s\": %u page pictures, at page %u", path, entry.title,
                static_cast<unsigned>(g_page_count), static_cast<unsigned>(g_page_index + 1));
}

// --- Bookmarks ------------------------------------------------------------------
// Hold OK while reading to keep the place; the Bookmarks tile lists them after
// "Continue" (the last place read). Opening one goes through each reader's
// own resume path, so a bookmark lands exactly where resuming would.

ui::BookmarksState g_bookmarks_state;
uint16_t g_bookmarks_selected = 0;
char g_bookmarks_status[48] = "";
uint8_t g_book_content_id[16] = {0};  // the text book OpenBook() last loaded

// The place on screen as a bookmark; *ok false on a screen that is not a reader.
net::Bookmark CurrentPlace(bool* ok) {
  net::Bookmark mark;
  *ok = true;
  if (g_screen_mode == ScreenMode::kPages) {
    memcpy(mark.content_id, g_page_resume_id, sizeof(mark.content_id));
    mark.kind = net::BookmarkKind::kPageBook;
    mark.a = g_page_index;
  } else if (g_screen_mode == ScreenMode::kReader) {
    memcpy(mark.content_id, g_book_content_id, sizeof(mark.content_id));
    mark.kind = net::BookmarkKind::kTextBook;
    mark.a = g_book_page;
    mark.b = g_reader_is_translation ? g_trans_surah : 0;
    mark.c = g_book_scale;
  } else if (g_screen_mode == ScreenMode::kQuran && g_quran_reader.isOpen()) {
    memcpy(mark.content_id, g_quran_reader.header().content_id, sizeof(mark.content_id));
    mark.kind = net::BookmarkKind::kQuran;
    mark.a = g_quran_state.surah_id;
    mark.b = g_quran_state.first_ayah_index;
    mark.c = g_quran_state.screen_number;
  } else {
    *ok = false;
  }
  if (g_clock.synced()) mark.created_unix = g_clock.nowUnix();
  return mark;
}

void AddBookmarkHere() {
  bool ok = false;
  const net::Bookmark mark = CurrentPlace(&ok);
  if (!ok) return;
  g_bookmarks.add(mark);
  const bool saved = g_storage.mounted() && g_bookmarks.save(&g_storage);
  char place[40];
  ui::BookmarksScreen::describe(mark, place, sizeof(place));
  drivers::Logf("[bookmarks] saved %s%s", place, saved ? "" : " (could not write the card)");
  if (g_screen_mode == ScreenMode::kQuran) {
    snprintf(g_quran_status, sizeof(g_quran_status), "Bookmarked %.32s", place);
  } else if (g_screen_mode == ScreenMode::kReader) {
    snprintf(g_reader_notice, sizeof(g_reader_notice), "Bookmarked %.32s", place);
  }
  g_dirty = true;
}

void EnterBookmarks() {
  g_bookmarks_state = ui::BookmarksState();
  g_bookmarks_state.index = &g_library_index;
  g_bookmarks_state.bookmarks = &g_bookmarks;
  if (g_storage.mounted()) {
    net::PageProgress page;
    net::ReadingProgress quran;
    if (net::LoadPageProgress(&g_storage, &page) &&
        g_library_index.find(page.content_id) != nullptr) {
      g_bookmarks_state.has_last_read = true;
      memcpy(g_bookmarks_state.last_read.content_id, page.content_id, 16);
      g_bookmarks_state.last_read.kind = net::BookmarkKind::kPageBook;
      g_bookmarks_state.last_read.a = page.page;
    } else if (net::LoadReadingProgress(&g_storage, &quran) &&
               g_library_index.find(quran.content_id) != nullptr) {
      g_bookmarks_state.has_last_read = true;
      memcpy(g_bookmarks_state.last_read.content_id, quran.content_id, 16);
      g_bookmarks_state.last_read.kind = net::BookmarkKind::kQuran;
      g_bookmarks_state.last_read.a = quran.surah_id;
      g_bookmarks_state.last_read.b = quran.first_ayah_index;
      g_bookmarks_state.last_read.c = quran.screen_number;
    }
  }
  g_bookmarks_selected = 0;
  g_bookmarks_status[0] = 0;
  g_screen_mode = ScreenMode::kBookmarks;
  g_force_full_refresh = true;
  g_dirty = true;
}

void RenderBookmarks() {
  g_bookmarks_state.index = &g_library_index;
  g_bookmarks_state.bookmarks = &g_bookmarks;
  g_bookmarks_state.selected = g_bookmarks_selected;
  g_bookmarks_state.status_message = g_bookmarks_status;
  ui::BookmarksScreen::render(g_display.canvas(), g_bookmarks_state);
}

// --- Translation / Tafsir reader ---------------------------------------------------

// Opens a package for reading. The index checksum sweep (seconds of card
// reads for a big book) runs only the first time this exact package is
// opened; after it passes, the package's fingerprint goes into
// /DEVICE/verified.bin and later opens skip it. See net::VerifiedPackages.
qpk::Error OpenPackage(qpk::Reader& reader, hal::IFile* file, const char* path) {
  const uint32_t start = millis();
  qpk::Error err = reader.open(file, /*verify_index_checksums=*/false);
  if (err != qpk::Error::kOk) return err;
  const uint32_t parsed_ms = millis() - start;

  const qpk::Header& header = reader.header();
  net::PackageFingerprint fingerprint;
  memcpy(fingerprint.content_id, header.content_id, sizeof(fingerprint.content_id));
  fingerprint.header_crc32 = header.header_crc32;
  fingerprint.payload_crc32 = header.payload_crc32;
  fingerprint.package_size = header.package_size;
  if (g_verified.contains(fingerprint)) {
    drivers::Logf("[qpk] %s opened in %lums (checksums passed on an earlier open)", path,
                  static_cast<unsigned long>(parsed_ms));
    return qpk::Error::kOk;
  }

  const uint32_t verify_start = millis();
  err = reader.verifyIndexChecksums();
  drivers::Logf("[qpk] %s opened in %lums, index checksums %s in %lums", path,
                static_cast<unsigned long>(parsed_ms),
                err == qpk::Error::kOk ? "passed" : "FAILED",
                static_cast<unsigned long>(millis() - verify_start));
  if (err != qpk::Error::kOk) {
    reader.close();
    return err;
  }
  g_verified.add(fingerprint);
  if (g_storage.mounted() && !g_verified.save(&g_storage)) {
    drivers::LogLine("[qpk] could not save /DEVICE/verified.bin");
  }
  return qpk::Error::kOk;
}

void CloseTranslation() {
  if (g_trans_reader.isOpen()) g_trans_reader.close();
  if (g_trans_file != nullptr) {
    g_trans_file->close();
    g_trans_file = nullptr;
  }
  g_reader_is_translation = false;
}

// The aligned Quran's SURAH_INDEX, copied into RAM so paging from one surah
// to the next never reopens it. False when that package is not on the card.
bool LoadAlignedSurahs() {
  g_trans_surah_count = 0;
  char hex[33] = {0};
  uint32_t hex_length = 0;
  if (!g_trans_reader.metadata(qpk::MetadataKey::kAlignedContentId, hex, 32, &hex_length) ||
      hex_length != 32) {
    return false;
  }
  for (int i = 0; i < 32; ++i) {
    const char c = hex[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
  }
  uint8_t id[16] = {0};
  net::HexToContentId(hex, id);
  const net::LibraryEntry* quran = g_library_index.find(id);
  char path[72];
  if (quran == nullptr || !net::BuildPackagePath(*quran, path, sizeof(path))) return false;
  hal::IFile* file = g_storage.open(path);
  if (file == nullptr) return false;

  qpk::Reader reader;
  if (OpenPackage(reader, file, path) == qpk::Error::kOk) {
    const uint32_t count = reader.recordCount(qpk::SectionId::kSurahIndex);
    for (uint16_t surah_id = 1; surah_id <= count && surah_id <= 114; ++surah_id) {
      qpk::SurahRecord surah;
      if (reader.getSurah(surah_id, &surah) != qpk::Error::kOk) break;
      g_trans_surahs[surah_id - 1].ayah_count = surah.ayah_count;
      g_trans_surahs[surah_id - 1].first_ayah_index = surah.first_ayah_index;
      g_trans_surah_count = surah_id;
    }
    reader.close();
  }
  file->close();
  return g_trans_surah_count > 0;
}

// `surah` (1-based) as numbered verses in g_trans_text, folded to ASCII for
// the 5x7 font. Without an aligned Quran it is every ayah, numbered through.
void BuildTranslationText(uint16_t surah) {
  uint32_t first = 0;
  uint32_t count = g_trans_reader.recordCount(qpk::SectionId::kTranslationIndex);
  uint32_t length = 0;
  const auto put = [&](const char* text, uint32_t n) {
    const uint32_t room = kTranslationTextBytes - 1 - length;
    if (n > room) n = room;
    memcpy(g_trans_text + length, text, n);
    length += n;
  };

  char line[96];
  if (g_trans_surah_count > 0) {
    first = g_trans_surahs[surah - 1].first_ayah_index;
    count = g_trans_surahs[surah - 1].ayah_count;
    const int n = snprintf(line, sizeof(line), "Surah %u: %s\n\n", static_cast<unsigned>(surah),
                           app::kSurahNames[surah - 1]);
    if (n > 0) put(line, static_cast<uint32_t>(n));
  }

  length += qpk::AppendTranslationVerses(g_trans_reader, first, count, g_trans_text + length,
                                         kTranslationTextBytes - 1 - length, g_trans_scratch,
                                         g_trans_scratch != nullptr ? kTranslationScratchBytes : 0);
  g_trans_text[length] = 0;
  g_trans_length = length;
}

void ShowTranslationSurah(uint16_t surah, bool at_end) {
  g_trans_surah = surah;
  const uint32_t start = millis();
  BuildTranslationText(surah);
  g_reader_body = g_trans_text;
  g_reader_body_length = g_trans_length;
  g_reader_is_translation = true;
  g_book_pager.reset(g_reader_body, g_reader_body_length,
                     ui::ReaderCharsPerLine(g_display.canvas().width(), g_book_scale),
                     ui::ReaderLinesPerPage(g_book_scale));
  g_book_page = at_end ? g_book_pager.pageCount() : 1;
  if (g_trans_surah_count > 0) {
    snprintf(g_book_title, sizeof(g_book_title), "%.28s - %u %.20s", g_trans_name,
             static_cast<unsigned>(surah), app::kSurahNames[surah - 1]);
  } else {
    snprintf(g_book_title, sizeof(g_book_title), "%s", g_trans_name);
  }
  g_force_full_refresh = true;
  g_dirty = true;
  drivers::Logf("[translation] surah %u: %u bytes, %u pages, built in %lums",
                static_cast<unsigned>(surah), static_cast<unsigned>(g_trans_length),
                static_cast<unsigned>(g_book_pager.pageCount()),
                static_cast<unsigned long>(millis() - start));
}

// Opens a TRANSLATION or TAFSIR package at `surah` and `page` (1-based).
void OpenTranslation(const net::LibraryEntry& entry, uint16_t surah, uint16_t page) {
  CloseTranslation();
  ClosePageBook();
  if (g_trans_text == nullptr) {
    g_trans_text = static_cast<char*>(ps_malloc(kTranslationTextBytes));
  }
  if (g_trans_scratch == nullptr) {
    // Optional: without it each verse is read on its own, just slower.
    g_trans_scratch = static_cast<char*>(ps_malloc(kTranslationScratchBytes));
  }
  char path[72];
  if (g_trans_text == nullptr || !net::BuildPackagePath(entry, path, sizeof(path)) ||
      (g_trans_file = g_storage.open(path)) == nullptr) {
    snprintf(g_library_status, sizeof(g_library_status), "Could not open this translation");
    g_dirty = true;
    return;
  }
  const qpk::Error err = OpenPackage(g_trans_reader, g_trans_file, path);
  if (err != qpk::Error::kOk) {
    snprintf(g_library_status, sizeof(g_library_status), "%s", qpk::ErrorText(err));
    CloseTranslation();
    g_dirty = true;
    return;
  }

  snprintf(g_trans_name, sizeof(g_trans_name), "%s", entry.title);
  memcpy(g_book_content_id, entry.content_id, sizeof(g_book_content_id));  // for bookmarks
  const bool aligned = LoadAlignedSurahs();
  const uint16_t last = g_trans_surah_count > 0 ? g_trans_surah_count : 1;
  ShowTranslationSurah(surah >= 1 && surah <= last ? surah : 1, false);
  if (page > 1 && page <= g_book_pager.pageCount()) g_book_page = page;

  g_screen_mode = ScreenMode::kReader;
  g_library_status[0] = 0;
  drivers::Logf("[translation] opened %s: %u ayahs, %s", path,
                static_cast<unsigned>(g_trans_reader.recordCount(qpk::SectionId::kTranslationIndex)),
                aligned ? "surahs from the aligned Quran" : "no aligned Quran on the card, one run");
}

// Chapters for "Go to page". A picture book's CHAPTER_INDEX counts pages the
// same way its pictures do (both are the PDF's own pages, 1-based), and its
// records ascend by first_page, so the chapter covering a page is a binary
// search: a handful of record reads, not a scan.
bool ChapterForPage(uint32_t page, uint32_t* index_out, qpk::ChapterRecord* out) {
  const uint32_t count = g_page_reader.recordCount(qpk::SectionId::kChapterIndex);
  if (count == 0) return false;
  uint32_t lo = 0;
  uint32_t hi = count;  // ends at the first chapter starting after `page`
  qpk::ChapterRecord record;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (g_page_reader.getChapter(mid, &record) != qpk::Error::kOk) return false;
    if (record.first_page <= page) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0 || g_page_reader.getChapter(lo - 1, out) != qpk::Error::kOk) return false;
  *index_out = lo - 1;
  return true;
}

char g_page_jump_chapter[48] = "";

void UpdateJumpChapter() {
  g_page_jump_chapter[0] = 0;
  uint32_t index = 0;
  qpk::ChapterRecord chapter;
  if (!ChapterForPage(g_page_jump_target + 1, &index, &chapter)) return;
  const int32_t got = g_page_reader.readText(chapter.title_offset, chapter.title_length,
                                             g_page_jump_chapter, sizeof(g_page_jump_chapter) - 1);
  g_page_jump_chapter[got > 0 ? got : 0] = 0;
}

// MENU in "Go to page": the target moves to where the next chapter starts,
// wrapping from the last chapter back to the first.
void JumpTargetToNextChapter() {
  const uint32_t count = g_page_reader.recordCount(qpk::SectionId::kChapterIndex);
  if (count == 0) return;
  uint32_t index = 0;
  qpk::ChapterRecord chapter;
  uint32_t next = ChapterForPage(g_page_jump_target + 1, &index, &chapter) ? index + 1 : 0;
  if (next >= count) next = 0;
  if (g_page_reader.getChapter(next, &chapter) != qpk::Error::kOk) return;
  uint32_t target = chapter.first_page > 0 ? chapter.first_page - 1 : 0;
  if (g_page_count > 0 && target >= g_page_count) target = g_page_count - 1;
  g_page_jump_target = target;
  g_dirty = true;
}

void RenderPageBook() {
  const uint32_t start = millis();
  const bool cached = PageCached(g_page_index);
  ui::PageImageState state;
  state.bits = PageBits(g_page_index);
  state.width = qpk::kPageImageWidth;
  state.height = qpk::kPageImageHeight;
  state.turn = app::kPortraitClockwise ? ui::PortraitTurn::kClockwise
                                       : ui::PortraitTurn::kCounterClockwise;
  state.page = g_page_index + 1;
  state.page_count = g_page_count;
  state.jump_mode = g_page_jump_mode;
  state.jump_target = g_page_jump_target + 1;
  if (g_page_jump_mode) UpdateJumpChapter();
  state.jump_chapter = g_page_jump_chapter;
  bool is_reader = false;
  const net::Bookmark here = CurrentPlace(&is_reader);
  state.bookmarked = is_reader && g_bookmarks.contains(here);
  if (!ui::PageImageScreen::render(g_display.canvas(), state)) {
    g_display.canvas().drawText(24, 24, "This page could not be read", 3, gfx::kBlack);
  }
  // Every tenth page, or any page that had to come off the card, is enough to
  // follow a reading session without flooding the log.
  if (!cached || (g_page_index + 1) % 10 == 0) {
    drivers::Logf("[pages] page %u/%u %s, drawn in %lums", static_cast<unsigned>(g_page_index + 1),
                  static_cast<unsigned>(g_page_count), cached ? "from memory" : "from the card",
                  static_cast<unsigned long>(millis() - start));
  }
}

// OK in the text reader: the next text size (x2, x3, x4, then x2 again),
// re-paginated so the reader lands at about the same place in the book.
constexpr uint8_t kBookScales[] = {2, 3, 4};

void CycleBookTextSize() {
  uint8_t next = kBookScales[0];
  for (size_t i = 0; i < sizeof(kBookScales); ++i) {
    if (kBookScales[i] == g_book_scale) {
      next = kBookScales[(i + 1) % sizeof(kBookScales)];
      break;
    }
  }
  const uint32_t old_count = g_book_pager.pageCount();
  const uint32_t old_page = g_book_page;

  g_book_scale = next;
  g_reader_notice[0] = 0;
  g_book_pager.reset(g_reader_body, g_reader_body_length,
                     ui::ReaderCharsPerLine(g_display.canvas().width(), g_book_scale),
                     ui::ReaderLinesPerPage(g_book_scale));
  const uint32_t count = g_book_pager.pageCount();
  uint32_t page = 1;
  if (old_count > 1 && count > 1 && old_page > 1) {
    page = 1 + (old_page - 1) * (count - 1) / (old_count - 1);
  }
  if (page > count) page = count > 0 ? count : 1;

  g_book_page = static_cast<uint16_t>(page);
  g_force_full_refresh = true;  // every line moves: clear the ghosting too
  g_dirty = true;
  drivers::Logf("[reader] text size x%u, page %u of %u", static_cast<unsigned>(g_book_scale),
                static_cast<unsigned>(page), static_cast<unsigned>(count));
}

// Renders whatever OpenBook() last loaded: a BOOK package's text from the
// card (via LoadBookText), paginated by TextPager and drawn by ReaderScreen.
void RenderBookReader() {
  ui::ReaderState state;
  state.title = g_book_title;
  state.body = g_reader_body;
  state.scale = g_book_scale;
  state.chars_per_line =
      ui::ReaderCharsPerLine(g_display.canvas().width(), g_book_scale);
  state.lines_per_page = ui::ReaderLinesPerPage(g_book_scale);
  state.from_flash = false;
  state.page = g_book_page;
  state.page_count = g_book_pager.pageCount();
  state.line_count =
      g_book_pager.linesForPage(g_book_page, g_book_lines, ui::kReaderMaxLines);
  state.lines = g_book_lines;
  if (g_reader_notice[0] != 0) {
    state.hint = g_reader_notice;
  } else if (g_reader_is_translation) {
    state.hint = kTranslationHint;
  }
  ui::ReaderScreen::render(g_display.canvas(), state);
}

void Repaint() {
  if (g_screen_mode == ScreenMode::kHome && !g_menu_open) {
    // The home screen pushes its own frames (PollHomeScreen): grey ones and
    // clock windows, never this black/white flush.
    g_home_full_pending = true;
    g_dirty = false;
    return;
  }
  RefreshState();
  // Force busy for the render itself (never sleep mid-flush), then restore
  // whatever busy meant before -- not unconditionally false. A sustained
  // session (BLE pairing, transfer mode) sets busy for its whole duration;
  // a status repaint during one of those must not cancel that.
  const bool was_busy = g_idle.busy();
  g_idle.setBusy(true);
  if (g_menu_open) {
    // Over the home screen this is the one black/white frame that screen
    // otherwise never gets; closing it hands the grey photo back (see
    // CloseOptionsMenu).
    g_menu.touch_hint = g_input.diagnostics().touch_present;
    ui::OptionsMenu::render(g_display.canvas(), g_menu);
  } else if (g_screen_mode == ScreenMode::kLibrary) {
    RefreshLibraryState();
    ui::LibraryScreen::render(g_display.canvas(), g_library_state);
    if (g_library_state.grey_covers) {
      FlushShelfGrey();
      g_force_full_refresh = false;
      g_dirty = false;
      g_idle.setBusy(was_busy);
      g_idle.noteActivity(millis());
      return;
    }
  } else if (g_screen_mode == ScreenMode::kReader) {
    RenderBookReader();
  } else if (g_screen_mode == ScreenMode::kPages) {
    RenderPageBook();
  } else if (g_screen_mode == ScreenMode::kScreenSetup) {
    g_setup.touch_present = g_input.diagnostics().touch_present;
    ui::SetupScreen::render(g_display.canvas(), g_setup);
  } else if (g_screen_mode == ScreenMode::kBookmarks) {
    RenderBookmarks();
  } else if (g_screen_mode == ScreenMode::kSurahPicker) {
    if (g_surah_picker_translation) {
      g_surah_picker_state.reader = nullptr;
      g_surah_picker_state.list_count = g_trans_surah_count;
      g_surah_picker_state.list_ayah_counts = g_trans_ayah_counts;
    } else {
      g_surah_picker_state.reader = &g_quran_reader;
      g_surah_picker_state.list_count = 0;
      g_surah_picker_state.list_ayah_counts = nullptr;
    }
    g_surah_picker_state.selected = g_surah_picker_selected;
    g_surah_picker_state.scroll_top = g_surah_picker_scroll_top;
    ui::SurahPickerScreen::render(g_display.canvas(), g_surah_picker_state);
  } else if (g_screen_mode == ScreenMode::kQuran) {
    g_quran_state.status_message = g_quran_status;
    const uint32_t render_start = millis();
    const ui::QuranRenderResult r =
        ui::QuranScreen::render(g_display.canvas(), g_quran_state);
    const uint32_t render_ms = millis() - render_start;
    g_quran_next_ayah_index = r.next_ayah_index;
    g_quran_next_line_index = r.next_line_index;
    g_quran_used_layout = r.used_layout;
    // The mushaf's own flow runs straight through a surah boundary, so on the
    // layout path the surah on screen is a result, not an input -- follow it
    // so the header and the saved reading position stay truthful.
    if (r.used_layout && r.first_surah_id != 0 &&
        r.first_surah_id != g_quran_state.surah_id) {
      const uint32_t line = g_quran_state.first_line_index;
      const uint32_t anchor = g_quran_state.first_ayah_index;
      const uint16_t screen = g_quran_state.screen_number;
      ui::QuranScreen::loadSurah(&g_quran_state, r.first_surah_id);
      // loadSurah resets both cursors to the surah's start; only the surah
      // LABEL was wrong, so put the position back exactly as it was.
      g_quran_state.first_line_index = line;
      g_quran_state.first_ayah_index = anchor;
      g_quran_state.screen_number = screen;
    }
    drivers::Logf(
        "[quran] surah=%u from %s=%u drew %u ayahs, %u lines, %u glyphs "
        "in %lums%s\n",
        static_cast<unsigned>(g_quran_state.surah_id),
        r.used_layout ? "line_index" : "ayah_index",
        static_cast<unsigned>(r.used_layout ? g_quran_state.first_line_index
                                            : g_quran_state.first_ayah_index),
        static_cast<unsigned>(r.ayahs_drawn),
        static_cast<unsigned>(r.lines_drawn),
        static_cast<unsigned>(r.glyphs_drawn),
        static_cast<unsigned long>(render_ms),
        r.unsupported ? " (UNSUPPORTED PACKAGE)" : "");
  } else {
    ui::SelfTestScreen::render(g_display.canvas(), g_state);
  }
  const hal::RefreshMode mode = g_force_full_refresh
                                    ? hal::RefreshMode::kFull
                                    : hal::RefreshMode::kPartial;
  // Bring-up diagnostic, and worth keeping: it separates "the renderer drew
  // nothing" from "the renderer drew but the panel did not show it", which
  // are otherwise the same symptom -- a white screen and no error.
  const uint8_t* fb = g_display.framebuffer();
  uint32_t ink_bytes = 0;
  for (uint32_t i = 0; i < g_display.framebufferBytes(); ++i) {
    if (fb[i] != 0xFF) ++ink_bytes;
  }
  // The elapsed time is the other half of the diagnostic, and the more
  // decisive half: a real full refresh on this panel takes seconds. A flush
  // that "succeeds" in a few milliseconds means BUSY was never observed
  // going high, so nothing was actually driven onto the glass -- which looks
  // exactly like a working device that draws nothing.
  const uint32_t flush_start = millis();
  const bool flushed = g_display.flush(mode);
  const uint32_t flush_ms = millis() - flush_start;
  // The mode the display ACTUALLY used, not the one it was asked for: a
  // kPartial silently escalates to a full refresh every
  // kPartialsBeforeFullRefresh flushes. Logging the request made that
  // invisible, and a 2,096ms "partial" in the log was a full refresh all
  // along -- only the duration gave it away (docs/pending.md).
  const hal::RefreshMode actual = g_display.lastRefreshMode();
  // copy= is the canvas going into GxEPD2's buffer (CPU only); panel= is
  // GxEPD2's display(), the SPI transfer plus the waveform actually driving
  // the glass. One combined figure hid which half was slow, and they fail for
  // completely different reasons. clipped= is ink the screen tried to draw
  // off the 800x480 canvas: anything but 0 is a layout that does not fit.
  drivers::Logf(
      "[epd] repaint mode=%s%s ink=%lu/%lu clipped=%lu flush=%d in %lums "
      "(copy=%lums panel=%lums)\n",
      actual == hal::RefreshMode::kFull   ? "full"
      : actual == hal::RefreshMode::kFast ? "fast"
                                          : "partial",
      actual != mode ? " (escalated)" : "",
      static_cast<unsigned long>(ink_bytes),
      static_cast<unsigned long>(g_display.framebufferBytes()),
      static_cast<unsigned long>(g_display.canvas().clippedPixels()),
      flushed ? 1 : 0, static_cast<unsigned long>(flush_ms),
      static_cast<unsigned long>(g_display.lastFrameWriteMs()),
      static_cast<unsigned long>(g_display.lastWaveformMs()));

  if (flushed) {
    ++g_refresh_count;
    PersistFramebuffer();
  } else {
    drivers::LogLine("[epd] flush failed (panel BUSY timeout?)");
  }
  g_force_full_refresh = false;
  g_dirty = false;
  g_idle.setBusy(was_busy);
  g_idle.noteActivity(millis());
}

void GoToSleep() {
  // This session's own awake time is exactly known (millis() since this
  // boot/wake started) -- fold it into the same real-time clock the
  // battery-sample timer wake advances, so awake power draw is not invisible
  // to the drain-rate estimate. See g_battery_clock_ms's declaration.
  g_battery_clock_ms += millis();

  drivers::Logf("[power] idle, sleeping. boot=%lu counter=%ld\n",
                static_cast<unsigned long>(g_power.wakeInfo().boot_count),
                static_cast<long>(g_counter));
  Serial.flush();
  SaveQuranProgress();  // before storage unmounts below
  SavePageProgress();
  g_display.end();
  g_storage.end();
  // Timer-armed, not indefinite: this is what lets a battery-sample wake
  // fire even if the user never touches the device (see setup()'s kTimer
  // handling). EXT1 input wake stays armed too -- whichever fires first wins.
  // Only worth arming once there is a battery to sample -- kBatteryAdcPin is
  // still -1/unresolved (board_crowpanel_579.h), so batteryAvailable() is
  // false today and this stays the plain indefinite sleep it always was.
  // Otherwise the device would wake itself every kBatterySampleIntervalMs
  // forever to sample a battery it cannot read, for no benefit.
  if (g_power.batteryAvailable()) {
    g_power.deepSleepFor(app::kBatterySampleIntervalMs);  // does not return
  }
  g_power.deepSleep();  // does not return
}

void BringUpStorage() {
  if (!g_storage.begin()) {
    g_state.sd = ui::TestResult::kFail;
    snprintf(g_sd_detail, sizeof(g_sd_detail), "%s", g_storage.lastError());
    drivers::Logf("[sd] %s\n", g_storage.lastError());
    return;
  }

  const hal::StorageInfo info = g_storage.info();
  g_state.sd_capacity_mb = info.capacity_bytes / (1024ULL * 1024ULL);

  // Prove the card is writable, not merely readable: create the directory
  // layout from app_config.h and append one self-test line.
  const bool layout_ok = g_storage.ensureLayout();
  char line[96];
  snprintf(line, sizeof(line), "boot=%lu wake=%s fw=%s",
           static_cast<unsigned long>(g_power.wakeInfo().boot_count),
           drivers::PowerManager::wakeReasonName(g_power.wakeInfo().reason),
           app::kFirmwareVersion);
  const bool write_ok = g_storage.appendLine(app::kFileSelfTest, line);

  if (layout_ok && write_ok) {
    g_state.sd = ui::TestResult::kPass;
    snprintf(g_sd_detail, sizeof(g_sd_detail), "rw ok");
  } else {
    g_state.sd = ui::TestResult::kFail;
    snprintf(g_sd_detail, sizeof(g_sd_detail), "read-only?");
  }
  drivers::Logf("[sd] %s %llu MB, layout=%d write=%d\n", info.type,
                info.capacity_bytes / (1024ULL * 1024ULL), layout_ok ? 1 : 0,
                write_ok ? 1 : 0);
}

const char* WifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "no-ssid";
    case WL_SCAN_COMPLETED:  return "scan-done";
    case WL_CONNECTED:       return "connected";
    case WL_CONNECT_FAILED:  return "connect-failed";
    case WL_CONNECTION_LOST: return "connection-lost";
    case WL_DISCONNECTED:    return "disconnected";
    default:                 return "?";
  }
}

// Blocks until Wi-Fi associates or app::kWifiConnectTimeoutMs passes, printing
// progress once a second. These waits run inside setup(), before loop()'s
// heartbeat exists, and a silent 15 s pause there is indistinguishable from a
// hung board on the serial monitor.
bool WaitForWifi(const char* tag) {
  const uint32_t started = millis();
  uint32_t last_report = started;
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < app::kWifiConnectTimeoutMs) {
    delay(100);
    if (millis() - last_report >= 1000) {
      last_report = millis();
      drivers::Logf("%s waiting for Wi-Fi: %lus/%lus status=%s\n", tag,
                    static_cast<unsigned long>((last_report - started) / 1000),
                    static_cast<unsigned long>(app::kWifiConnectTimeoutMs / 1000),
                    WifiStatusName(WiFi.status()));
    }
  }
  return WiFi.status() == WL_CONNECTED;
}

// Pulls one `"key":"value"` string out of a small JSON-ish blob. No escape
// handling, no nesting -- adequate for the two flat fields
// TryWifiBypassFromSdCard reads and nothing more.
bool ExtractJsonString(const char* json, const char* key, char* out,
                       size_t capacity) {
  char needle[40];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* key_pos = strstr(json, needle);
  if (!key_pos) return false;
  const char* colon = strchr(key_pos + strlen(needle), ':');
  if (!colon) return false;
  const char* quote_start = strchr(colon, '"');
  if (!quote_start) return false;
  quote_start++;
  const char* quote_end = strchr(quote_start, '"');
  if (!quote_end) return false;
  size_t length = static_cast<size_t>(quote_end - quote_start);
  if (length >= capacity) length = capacity - 1;
  memcpy(out, quote_start, length);
  out[length] = 0;
  return true;
}

// DEV-ONLY Wi-Fi bypass. Reads /DEVICE/wifi.json --
// {"ssid":"...","passphrase":"..."} in plaintext -- and provisions straight
// from it, skipping BLE entirely.
//
// This exists because BLE pairing, while now proven working on real
// hardware (docs/provisioning.md's "never run" caveat no longer holds: a
// real Just Works handshake completes, see [ble] pairing ok in the serial
// log), is slow to test from a desktop repeatedly while iterating on the
// client side. It is NOT how the real product should provision a device --
// a plaintext Wi-Fi password sitting on a removable SD card is a real
// downgrade from BLE's encrypted-link requirement. One-shot by design: the
// file is removed after use whether the attempt succeeds or fails, so the
// password never lingers and a bad attempt does not retry every boot.
bool TryWifiBypassFromSdCard(hal::IStorage* storage,
                             net::ProvisioningState* state) {
  constexpr const char* kBypassPath = "/DEVICE/wifi.json";
  if (!storage->exists(kBypassPath)) return false;

  char buf[256] = {0};
  const int64_t file_size = storage->size(kBypassPath);
  if (file_size <= 0 || file_size >= static_cast<int64_t>(sizeof(buf))) {
    drivers::LogLine("[wifi-bypass] wifi.json missing or too large, ignoring");
    storage->remove(kBypassPath);
    return false;
  }
  const int32_t bytes_read =
      storage->read(kBypassPath, 0, buf, static_cast<uint32_t>(file_size));
  storage->remove(kBypassPath);  // one-shot, regardless of outcome below
  if (bytes_read != file_size) {
    drivers::LogLine("[wifi-bypass] wifi.json read failed");
    return false;
  }
  buf[file_size] = 0;

  char ssid[net::kSsidMaxBytes + 1] = {0};
  char passphrase[net::kPassphraseMaxBytes + 1] = {0};
  char token[net::kTokenMaxChars + 1] = {0};
  if (!ExtractJsonString(buf, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
    drivers::LogLine("[wifi-bypass] no \"ssid\" found in wifi.json");
    return false;
  }
  ExtractJsonString(buf, "passphrase", passphrase, sizeof(passphrase));
  // Optional. Without it the device provisions Wi-Fi and stays unpaired,
  // exactly as before -- every endpoint but GET /api/device/info then
  // refuses, which is auth.h working, not a fault.
  ExtractJsonString(buf, "token", token, sizeof(token));

  state->reset();
  if (state->setSsid(ssid) != net::ProvisioningError::kOk) return false;
  if (passphrase[0] != 0 &&
      state->setPassphrase(passphrase) != net::ProvisioningError::kOk) {
    return false;
  }
  // Set before beginCommit(), because state->reset() above cleared it and
  // NetCredentials::save() persists whatever pairingToken() holds at the
  // moment the commit succeeds.
  if (token[0] != 0 &&
      state->setPairingToken(token) != net::ProvisioningError::kOk) {
    drivers::LogLine("[wifi-bypass] \"token\" rejected (over 64 chars?)");
    return false;
  }
  if (state->beginCommit() != net::ProvisioningError::kOk) return false;

  drivers::Logf("[wifi-bypass] connecting to \"%s\"...\n", ssid);
  if (passphrase[0] != 0) {
    WiFi.begin(ssid, passphrase);
  } else {
    WiFi.begin(ssid);
  }
  if (!WaitForWifi("[wifi-bypass]")) {
    drivers::LogLine("[wifi-bypass] connect failed/timed out");
    WiFi.disconnect(/*wifioff=*/true);
    state->commitFailed();
    return false;
  }

  // Commit-then-verify-then-persist, same rule BLE's flow follows -- see
  // provisioning_state.h. WiFi.status() has already reported association
  // above, so this is reachable only once the connection has actually
  // worked.
  state->commitSucceeded();
  // The token's presence is worth logging; its value never is -- it is the
  // bearer credential for every destructive endpoint, and the serial console
  // is not a private channel.
  drivers::Logf("[wifi-bypass] connected, pairing token %s\n",
               token[0] != 0 ? "set" : "not supplied (device stays unpaired)");
  return true;
}

#if DEV_AUTOPROVISION
#include "app/dev_secrets.h"

// DEV-ONLY. Provisions from credentials compiled into the image, so a test
// device comes up on Wi-Fi and already paired with no SD card shuffling and
// no BLE client. Exists for the same reason the wifi.json bypass does -- the
// Windows BLE client cannot finish the GATT flow (docs/pending.md section 3)
// -- and carries the same warning, one step louder: the pairing token is the
// bearer credential for every destructive endpoint and this puts it in the
// firmware image. Never ship a build with this flag set.
//
// Runs only when the device would otherwise be stuck: no saved credentials,
// or credentials but no token. That way a device provisioned properly over
// BLE is never quietly overwritten by a compiled-in default.
bool TryDevAutoProvision(net::ProvisioningState* state) {
  if (state->ready() && state->pairingToken()[0] != 0) {
    // Say so. A silent skip here is indistinguishable from the flag not
    // being compiled in, which cost a whole flash cycle to work out once.
    drivers::LogLine(
        "[dev-autoprovision] already provisioned and has a token; leaving NVS alone");
    return false;
  }
  drivers::Logf("[dev-autoprovision] state: ready=%d has_token=%d\n",
               state->ready() ? 1 : 0, state->pairingToken()[0] != 0 ? 1 : 0);

  drivers::LogLine("[dev-autoprovision] DEV BUILD: provisioning from compiled-in credentials");
  state->reset();
  if (state->setSsid(DEV_WIFI_SSID) != net::ProvisioningError::kOk) return false;
  if (state->setPassphrase(DEV_WIFI_PASSPHRASE) != net::ProvisioningError::kOk) {
    return false;
  }
  if (state->setPairingToken(DEV_PAIRING_TOKEN) != net::ProvisioningError::kOk) {
    drivers::LogLine("[dev-autoprovision] DEV_PAIRING_TOKEN rejected (over 64 chars?)");
    return false;
  }
  if (state->beginCommit() != net::ProvisioningError::kOk) return false;

  WiFi.begin(DEV_WIFI_SSID, DEV_WIFI_PASSPHRASE);
  if (!WaitForWifi("[dev-autoprovision]")) {
    drivers::LogLine("[dev-autoprovision] connect failed/timed out");
    WiFi.disconnect(/*wifioff=*/true);
    state->commitFailed();
    return false;
  }
  // Same commit-then-verify-then-persist rule as every other path.
  state->commitSucceeded();
  drivers::LogLine("[dev-autoprovision] connected, pairing token set");
  return true;
}
#endif  // DEV_AUTOPROVISION

// Brings the library index up from whatever is on disk, and the upload
// manager online. Called once storage is confirmed mounted.
void BringUpNetState() {
  if (!g_library_index.load(&g_storage)) {
    g_library_index.rebuild(&g_storage);
    g_library_index.save(&g_storage);
  } else if (const uint16_t dropped = g_library_index.prune(&g_storage)) {
    // An install that lost power between "remove the old package" and
    // "rename the new one in" leaves the index naming a file that is gone.
    // FAT cannot make those two steps atomic, so the guarantee is repaired
    // here at boot rather than prevented there. See library_index.h::prune.
    drivers::Logf("[library] pruned %u stale index entries\n", dropped);
    g_library_index.save(&g_storage);
  }
  g_upload_manager.begin(&g_storage, &g_library_index);
  g_upload_manager.sweepOrphans();

  bool has_saved_credentials = g_net_credentials.load(&g_provisioning);
  // A wifi.json on the card now wins over already-saved credentials, where it
  // used to be skipped whenever any were present. Two reasons: the file is
  // one-shot (removed on read, so this cannot loop or linger), and topping up
  // a *pairing token* on a device that is already Wi-Fi provisioned is
  // precisely what it is now for -- under the old gate that was impossible
  // without erasing NVS first.
  bool provisioned_now = TryWifiBypassFromSdCard(&g_storage, &g_provisioning);
#if DEV_AUTOPROVISION
  // The card still wins: an explicit wifi.json is a deliberate act, and the
  // compiled-in credentials are only a fallback for when nothing else has
  // provisioned this device.
  if (!provisioned_now) {
    if (has_saved_credentials) g_net_credentials.load(&g_provisioning);
    provisioned_now = TryDevAutoProvision(&g_provisioning);
  }
#endif
  if (provisioned_now) {
    if (g_net_credentials.save(g_provisioning)) {
      has_saved_credentials = true;
    } else {
      drivers::LogLine("[wifi-bypass] connected but failed to persist credentials");
    }
  } else if (has_saved_credentials) {
    // The bypass calls state->reset() before it tries. If no file was there
    // this reload is a harmless no-op; if the attempt failed, in-memory state
    // is now empty while NVS still holds the good credentials, so put them
    // back rather than boot into a needlessly unprovisioned device.
    has_saved_credentials = g_net_credentials.load(&g_provisioning);
  }
  if (!has_saved_credentials) {
    // First boot, or never successfully paired: advertise for setup.
    // Deep sleep turns both radios off (architecture.md's power model), so
    // pairing suppresses the idle timer for as long as it is in progress --
    // a 20-second idle window is nowhere near enough time to pair over BLE.
    g_ble.begin(&g_provisioning, app::kProductName);
    g_idle.setBusy(true);
  }
  // A provisioned device that is not mid-pairing brings up neither radio at
  // boot -- see ToggleTransferMode for how one comes up on request instead.
}

// Ends the BLE pairing session once it resolves, whichever way. Called every
// loop() iteration while it's active.
void CheckProvisioningOutcome() {
  if (!g_ble.active()) return;
  if (g_provisioning.status() != net::ProvisioningStatus::kProvisioned) return;

  g_ble.end();
  // Only Wi-Fi's connectivity was needed, to verify the credentials before
  // NetCredentials::save() persisted them (see provisioning_state.h's
  // commit-then-verify-then-persist rule) -- not an open transfer session.
  WiFi.disconnect(/*wifioff=*/true);
  g_idle.setBusy(false);
  g_idle.noteActivity(millis());
  // A table clock goes straight on to the network it was just given.
  if (app::kTableClockMode) {
    g_wifi_wanted = true;
    g_wifi_retry_ms = 0;
  }
}

// Wipes saved Wi-Fi credentials and re-enters BLE pairing, same as an
// unprovisioned first boot. Triggered by a long-press on EXIT -- distinct
// from the encoder switch's long-press (transfer mode), and guarded against
// firing during either radio session so a reset can't also orphan a
// transfer or double up on BLE.
void EndTransferMode();  // defined below; FactoryReset() may end a session

void FactoryReset() {
  if (g_ble.active()) return;
  if (g_transfer_state != TransferState::kOff) {
    // A table clock is always online, so waiting for Wi-Fi to be off would
    // make the reset unreachable; end the session instead.
    if (!app::kTableClockMode) return;
    EndTransferMode();
  }
  g_wifi_wanted = false;

  drivers::LogLine("[provisioning] factory reset: clearing saved credentials");
  g_net_credentials.clear();
  g_provisioning.reset();
  g_ble.begin(&g_provisioning, app::kProductName);
  g_idle.setBusy(true);
  g_dirty = true;
}

// Leaves transfer mode from kConnecting or kOn, either way. Shared by
// ToggleTransferMode (user asked), PollTransferMode (timeouts), and EXIT
// (architecture.md 4.3: EXIT ends the session rather than being ignored).
void EndTransferMode() {
  if (g_http.active()) g_http.end();
  WiFi.disconnect(/*wifioff=*/true);
  g_transfer_state = TransferState::kOff;
  g_wifi_retry_ms = millis();
  g_wifi_lost_ms = 0;
  g_idle.setBusy(false);
  g_idle.noteActivity(millis());
  g_dirty = true;
}

// Starts or stops transfer mode. Triggered by a long-press on the encoder
// switch -- see the enum comment by TransferState for why this, and not a
// menu, is the trigger for now.
void ToggleTransferMode() {
  if (g_ble.active()) return;  // mid-pairing; don't also try to go online

  if (g_transfer_state == TransferState::kOff) {
    if (!g_provisioning.ready()) {
      // Used to `return` silently, which made the menu row look broken rather
      // than unavailable -- the exact failure this screen already avoids for
      // "nothing to open yet". The row's own label says "(no saved Wi-Fi)",
      // but a press has to answer too, and a long-press from anywhere else
      // gets no label at all.
      snprintf(g_library_status, sizeof(g_library_status),
               "No saved Wi-Fi -- provision the device first");
      drivers::LogLine("[transfer] refused: no saved Wi-Fi credentials");
      g_dirty = true;
      return;
    }
    WiFi.begin(g_provisioning.ssid(), g_provisioning.passphraseForRadioUseOnly());
    g_wifi_wanted = true;
    drivers::Logf("[wifi] connecting to \"%s\"", g_provisioning.ssid());
    g_transfer_state = TransferState::kConnecting;
    g_transfer_connect_started_ms = millis();
    g_idle.setBusy(true);
    g_dirty = true;
    return;
  }

  g_wifi_wanted = false;  // asked for off: do not reconnect on our own
  EndTransferMode();  // kConnecting or kOn: turn it back off either way.
}

// Advances a pending Wi-Fi connect, and enforces the session timeout once
// connected (architecture.md 4.3 -- setBusy(true) defeats the normal idle
// timer for as long as this lasts, on purpose, so something has to bound
// it). Called every loop() iteration.
void PollTransferMode() {
  if (g_transfer_state == TransferState::kOff) {
    // Table-clock mode: stay online. Retry at an interval rather than in a
    // tight loop, so a network that is simply gone does not starve the clock.
    if (app::kTableClockMode && g_wifi_wanted && g_provisioning.ready() &&
        !g_ble.active() && millis() - g_wifi_retry_ms >= app::kWifiReconnectIntervalMs) {
      g_wifi_retry_ms = millis();
      ToggleTransferMode();
    }
    return;
  }

  if (g_transfer_state == TransferState::kConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      g_transfer_state = TransferState::kOn;
      g_transfer_session_started_ms = millis();
      g_clock_sync_logged = false;
      g_wifi_lost_ms = 0;
      snprintf(g_transfer_address, sizeof(g_transfer_address), "%s:%u",
               WiFi.localIP().toString().c_str(), net::kDefaultPort);
      drivers::Logf("[wifi] connected, API at %s", g_transfer_address);
      g_clock.begin(app::kNtpServer);
      g_http.begin(&g_storage, &g_power, &g_library_index, &g_upload_manager,
                  &g_provisioning, app::kProductName, &g_clock, &g_photos);
      g_dirty = true;
    } else if (millis() - g_transfer_connect_started_ms >
               app::kWifiConnectTimeoutMs) {
      drivers::Logf("[wifi] connect timed out after %lus",
                    static_cast<unsigned long>(app::kWifiConnectTimeoutMs / 1000));
      EndTransferMode();
    }
    return;
  }

  // kOn. A connection that stays down past a short grace period (the core
  // reconnects on its own for brief drops) ends the session; table-clock mode
  // then reconnects from kOff.
  if (WiFi.status() != WL_CONNECTED) {
    if (g_wifi_lost_ms == 0) {
      g_wifi_lost_ms = millis();
    } else if (millis() - g_wifi_lost_ms > kWifiLostGraceMs) {
      drivers::LogLine("[wifi] connection lost");
      EndTransferMode();
      return;
    }
  } else {
    g_wifi_lost_ms = 0;
  }

  // Logged once per session, purely so a serial log can confirm SNTP
  // actually landed on real hardware -- installed_at silently staying 0
  // otherwise looks identical to "no upload happened yet".
  if (!g_clock_sync_logged && g_clock.synced()) {
    drivers::Logf("[clock] synced now_unix=%lu\n",
                  static_cast<unsigned long>(g_clock.nowUnix()));
    g_clock_sync_logged = true;
  }

  if (!app::kTableClockMode &&
      millis() - g_transfer_session_started_ms > app::kTransferSessionTimeoutMs) {
    drivers::LogLine("[transfer] session timeout, closing");
    EndTransferMode();
  }
}

// Moves the library browser's selection by `delta` rows (a WHEEL detent is
// +-1), clamped to [0, book_count] -- the last position is the synthetic
// Transfer Mode row, see ui::LibraryScreen::rowCount. Keeps the visible
// window (ui::kLibraryMaxVisibleRows) following the selection, same as any
// scrolling list.
void MoveLibrarySelection(int32_t delta) {
  const uint16_t total = ui::LibraryScreen::rowCount(g_library_state);
  int32_t next = static_cast<int32_t>(g_library_selected) + delta;
  if (next < 0) next = 0;
  if (next >= total) next = total - 1;
  if (static_cast<uint16_t>(next) == g_library_selected) return;

  g_library_selected = static_cast<uint16_t>(next);
  if (g_library_selected < g_library_scroll_top) {
    g_library_scroll_top = g_library_selected;
  } else if (g_library_selected >=
             g_library_scroll_top + ui::kLibraryMaxVisibleRows) {
    g_library_scroll_top =
        g_library_selected - ui::kLibraryMaxVisibleRows + 1;
  }
  g_library_status[0] = 0;
  g_dirty = true;
}

// Opens a BOOK package and switches into the reader. Only ever called for
// an entry already known to be kBook -- QURAN/TRANSLATION/TAFSIR still need
// Arabic shaping (no on-device glyph atlas exists yet) and are refused by
// the caller before this runs.
void OpenBook(const net::LibraryEntry& entry) {
  char path[72];
  if (!net::BuildPackagePath(entry, path, sizeof(path))) {
    snprintf(g_library_status, sizeof(g_library_status), "Bad library entry");
    g_dirty = true;
    return;
  }

  hal::IFile* file = g_storage.open(path);
  if (file == nullptr) {
    snprintf(g_library_status, sizeof(g_library_status), "Could not open %.32s", path);
    g_dirty = true;
    return;
  }

  // Opened once, with the page-picture reader: a picture book keeps it open,
  // a text book is loaded from it and closed again.
  CloseTranslation();
  ClosePageBook();
  const uint32_t open_start = millis();
  qpk::Reader& reader = g_page_reader;
  const qpk::Error open_err = OpenPackage(reader, file, path);
  drivers::Logf("[library] opened %s in %lums", path,
                static_cast<unsigned long>(millis() - open_start));
  if (open_err != qpk::Error::kOk) {
    snprintf(g_library_status, sizeof(g_library_status), "%s",
             qpk::ErrorText(open_err));
    file->close();
    g_dirty = true;
    return;
  }

  if (reader.pageImageCount() > 0) {
    // Converted with "keep the PDF's page layout": show the page pictures.
    OpenPageBook(entry, file, path);
    return;
  }

  const qpk::BookLoadError load_err = qpk::LoadBookText(
      &reader, g_book_text, sizeof(g_book_text), &g_book_text_length);
  reader.close();
  file->close();

  if (load_err == qpk::BookLoadError::kReaderError ||
      load_err == qpk::BookLoadError::kNotABook) {
    snprintf(g_library_status, sizeof(g_library_status), "Could not read this book");
    g_dirty = true;
    return;
  }
  if (load_err == qpk::BookLoadError::kTruncated) {
    // Not a status-message case: the reader still opens with whatever fit,
    // this is purely a bring-up diagnostic for a book that outgrew
    // kBookTextBufferBytes.
    drivers::Logf("[reader] %s: text truncated at %u bytes\n", path,
                  static_cast<unsigned>(g_book_text_length));
  }

  snprintf(g_book_title, sizeof(g_book_title), "%s", entry.title);
  memcpy(g_book_content_id, entry.content_id, sizeof(g_book_content_id));
  const uint16_t chars =
      ui::ReaderCharsPerLine(g_display.canvas().width(), g_book_scale);
  const uint8_t lines = ui::ReaderLinesPerPage(g_book_scale);
  g_reader_body = g_book_text;
  g_reader_body_length = g_book_text_length;
  g_reader_is_translation = false;
  g_book_pager.reset(g_reader_body, g_reader_body_length, chars, lines);
  g_book_page = 1;

  g_screen_mode = ScreenMode::kReader;
  g_force_full_refresh = true;
  g_library_status[0] = 0;
  g_dirty = true;
  drivers::Logf("[reader] opened %s (%u bytes, %u pages)\n", path,
                static_cast<unsigned>(g_book_text_length),
                static_cast<unsigned>(g_book_pager.pageCount()));
}

// OK on the library browser: the Transfer Mode row acts, same as the
// encoder switch's long-press (ToggleTransferMode is unchanged and still
// works from anywhere -- this just gives it a real menu entry too, per
// docs/pending.md's Milestone 5 note). A BOOK entry opens for real; any
// other type can only report why it doesn't yet -- there is no on-device
// QPK blitter for Quran/translation content (the glyph atlas item, also
// still open).
// Moves between the browser's two levels. Selection and scroll reset on every
// transition: carrying a row number from a 5-row category menu into a
// 200-chapter list (or back) would land the cursor somewhere arbitrary.
void EnterLibraryCategory(uint16_t type) {
  g_library_view = ui::LibraryView::kItems;
  g_library_category = type;
  g_library_selected = 0;
  g_library_scroll_top = 0;
  g_library_status[0] = 0;
  g_dirty = true;
}

void LeaveLibraryCategory() {
  if (g_library_view == ui::LibraryView::kCategories) return;
  g_library_view = ui::LibraryView::kCategories;
  g_library_selected = 0;
  g_library_scroll_top = 0;
  g_library_status[0] = 0;
  g_dirty = true;
}

void CloseQuran() {
  if (g_quran_reader.isOpen()) g_quran_reader.close();
  if (g_quran_file != nullptr) {
    g_quran_file->close();
    g_quran_file = nullptr;
  }
  g_quran_state.reader = nullptr;
  g_quran_history_depth = 0;
}

// Persists where the reader is now, keyed on the open package's content_id
// (net::ReadingProgress -- see that header for why content_id, not a path).
// Called from every way of leaving ui::QuranScreen -- EXIT to the picker,
// MENU to the library, GoToSleep() (forward-declared above, near
// g_book_page) -- and MUST run before CloseQuran(), which drops the reader
// this reads header().content_id off of. Not called on every page turn: an
// SD write on the tail of every 622ms partial refresh would cost real
// latency for no benefit, since those three points already cover every way
// the position could be lost.
void SaveQuranProgress() {
  if (!g_quran_reader.isOpen() || !g_storage.mounted()) return;
  net::ReadingProgress progress;
  memcpy(progress.content_id, g_quran_reader.header().content_id, 16);
  progress.surah_id = g_quran_state.surah_id;
  progress.first_ayah_index = g_quran_state.first_ayah_index;
  progress.screen_number = g_quran_state.screen_number;
  if (!net::SaveReadingProgress(&g_storage, progress)) {
    drivers::LogLine("[quran] could not save reading position");
  }
}

// Enters the surah picker with `selected` (0-based SURAH_INDEX position)
// highlighted and scrolled into view.
void EnterSurahPicker(uint16_t selected) {
  const uint32_t total = g_quran_reader.recordCount(qpk::SectionId::kSurahIndex);
  g_surah_picker_selected =
      (total == 0 || selected >= total) ? 0 : selected;
  g_surah_picker_scroll_top = 0;
  if (g_surah_picker_selected >= ui::kSurahPickerMaxVisibleRows) {
    g_surah_picker_scroll_top = static_cast<uint16_t>(
        g_surah_picker_selected - ui::kSurahPickerMaxVisibleRows + 1);
  }
  g_surah_picker_translation = false;
  g_screen_mode = ScreenMode::kSurahPicker;
  g_force_full_refresh = true;
  g_dirty = true;
}

// OK in a translation: the surah list, with the surah being read selected.
// Needs the aligned Quran's surahs; without them the translation is one run
// and there is nothing to pick.
void EnterTranslationSurahPicker() {
  if (g_trans_surah_count == 0) {
    snprintf(g_reader_notice, sizeof(g_reader_notice), "No surah list: aligned Quran not on card");
    g_dirty = true;
    return;
  }
  for (uint16_t i = 0; i < g_trans_surah_count; ++i) {
    g_trans_ayah_counts[i] = static_cast<uint16_t>(g_trans_surahs[i].ayah_count);
  }
  g_surah_picker_selected =
      g_trans_surah >= 1 && g_trans_surah <= g_trans_surah_count ? g_trans_surah - 1 : 0;
  g_surah_picker_scroll_top = 0;
  if (g_surah_picker_selected >= ui::kSurahPickerMaxVisibleRows) {
    g_surah_picker_scroll_top =
        static_cast<uint16_t>(g_surah_picker_selected - ui::kSurahPickerMaxVisibleRows + 1);
  }
  g_surah_picker_translation = true;
  g_reader_notice[0] = 0;
  g_screen_mode = ScreenMode::kSurahPicker;
  g_force_full_refresh = true;
  g_dirty = true;
}

// Opens a QURAN package. The package stays open until CloseQuran(); see the
// globals' comment for why. Lands on the surah picker unless a saved
// net::ReadingProgress's content_id matches this exact package, in which
// case it resumes straight into ui::QuranScreen at the remembered position --
// EXIT from there comes back to the picker (not the library), so resuming
// never removes the ability to jump to a different surah.
void OpenQuran(const net::LibraryEntry& entry) {
  CloseQuran();  // never leak a handle by opening twice

  char path[72];
  if (!net::BuildPackagePath(entry, path, sizeof(path))) {
    snprintf(g_library_status, sizeof(g_library_status), "Bad library entry");
    g_dirty = true;
    return;
  }

  g_quran_file = g_storage.open(path);
  if (g_quran_file == nullptr) {
    snprintf(g_library_status, sizeof(g_library_status), "Could not open %.32s", path);
    g_dirty = true;
    return;
  }

  const qpk::Error open_err = OpenPackage(g_quran_reader, g_quran_file, path);
  if (open_err != qpk::Error::kOk) {
    snprintf(g_library_status, sizeof(g_library_status), "%s",
             qpk::ErrorText(open_err));
    CloseQuran();
    g_dirty = true;
    return;
  }

  g_quran_state = ui::QuranState();
  g_quran_state.reader = &g_quran_reader;
  g_quran_history_depth = 0;

  // Without this every glyph lookup scans FONT_METADATA on the card: ~236,000
  // SD reads and ~6 s for one 800x480 screen, measured on the full Quran.
  if (g_glyph_index_storage == nullptr) {
    g_glyph_index_storage = static_cast<qpk::GlyphRecord*>(
        ps_malloc(kGlyphIndexCapacity * sizeof(qpk::GlyphRecord)));
  }
  if (g_glyph_index_storage != nullptr) {
    const uint32_t glyph_start = millis();
    const uint32_t indexed = g_quran_reader.buildGlyphIndex(
        g_glyph_index_storage, kGlyphIndexCapacity);
    drivers::Logf("[quran] glyph index: %u of %u glyphs in %lums\n",
                  static_cast<unsigned>(indexed),
                  static_cast<unsigned>(
                      g_quran_reader.recordCount(qpk::SectionId::kFontMetadata)),
                  static_cast<unsigned long>(millis() - glyph_start));
  } else {
    drivers::LogLine("[quran] glyph index: no PSRAM, lookups will scan the card");
  }

  const uint32_t surah_total = g_quran_reader.recordCount(qpk::SectionId::kSurahIndex);
  if (surah_total == 0) {
    snprintf(g_library_status, sizeof(g_library_status), "No SURAH_INDEX in this package");
    CloseQuran();
    g_dirty = true;
    return;
  }

  net::ReadingProgress progress;
  const bool has_progress =
      g_storage.mounted() && net::LoadReadingProgress(&g_storage, &progress) &&
      memcmp(progress.content_id, g_quran_reader.header().content_id, 16) == 0;

  // loadSurah() succeeding only proves the surah exists; it says nothing
  // about whether progress.first_ayah_index still lands inside it -- a CRC-
  // clean but semantically stale record (e.g. written under a different
  // AYAH_INDEX layout) would otherwise resume onto a dead page with no way
  // back but EXIT. getAyahByIndex + a surah_id match is the same direct
  // check getSurah() itself does for its own consistency rule.
  qpk::AyahRecord probe;
  const bool position_valid =
      has_progress &&
      ui::QuranScreen::loadSurah(&g_quran_state, progress.surah_id) &&
      g_quran_reader.getAyahByIndex(progress.first_ayah_index, &probe) ==
          qpk::Error::kOk &&
      probe.surah_id == progress.surah_id;

  if (position_valid) {
    // seekToAyah() sets first_ayah_index either way, and additionally points
    // the line cursor at the mushaf line holding it when the package carries
    // LAYOUT_DATA. That indirection is what lets net::ReadingProgress keep
    // storing an ayah index -- its on-disk format, confirmed on hardware
    // 2026-09-01, does not change to gain line-driven paging.
    ui::QuranScreen::seekToAyah(&g_quran_state, progress.first_ayah_index);
    g_quran_state.screen_number =
        progress.screen_number > 0 ? progress.screen_number : 1;
    g_screen_mode = ScreenMode::kQuran;
    g_force_full_refresh = true;
    g_dirty = true;
    drivers::Logf("[quran] opened %s, resuming surah=%u ayah_index=%u\n", path,
                  static_cast<unsigned>(progress.surah_id),
                  static_cast<unsigned>(progress.first_ayah_index));
  } else {
    EnterSurahPicker(0);
    drivers::Logf("[quran] opened %s, %u surahs, %u ayahs\n", path,
                  static_cast<unsigned>(surah_total),
                  static_cast<unsigned>(
                      g_quran_reader.recordCount(qpk::SectionId::kAyahIndex)));
  }
}

// Moves the surah picker's selection by `delta` rows, same clamp-and-scroll
// shape as MoveLibrarySelection.
uint32_t SurahPickerTotal() {
  return g_surah_picker_translation ? g_trans_surah_count
                                    : g_quran_reader.recordCount(qpk::SectionId::kSurahIndex);
}

void MoveSurahPickerSelection(int32_t delta) {
  const uint32_t total = SurahPickerTotal();
  if (total == 0) return;
  int32_t next = static_cast<int32_t>(g_surah_picker_selected) + delta;
  if (next < 0) next = 0;
  if (next >= static_cast<int32_t>(total)) next = static_cast<int32_t>(total) - 1;
  if (static_cast<uint16_t>(next) == g_surah_picker_selected) return;

  g_surah_picker_selected = static_cast<uint16_t>(next);
  if (g_surah_picker_selected < g_surah_picker_scroll_top) {
    g_surah_picker_scroll_top = g_surah_picker_selected;
  } else if (g_surah_picker_selected >=
             g_surah_picker_scroll_top + ui::kSurahPickerMaxVisibleRows) {
    g_surah_picker_scroll_top = static_cast<uint16_t>(
        g_surah_picker_selected - ui::kSurahPickerMaxVisibleRows + 1);
  }
  g_dirty = true;
}

// OK on the surah picker: load the selected surah and enter ui::QuranScreen.
// Does nothing if the selection is stale (index changed under us).
void OpenSelectedSurah() {
  if (g_surah_picker_translation) {
    if (g_surah_picker_selected >= g_trans_surah_count) return;
    ShowTranslationSurah(static_cast<uint16_t>(g_surah_picker_selected + 1), false);
    g_screen_mode = ScreenMode::kReader;
    drivers::Logf("[translation] picked surah=%u", static_cast<unsigned>(g_trans_surah));
    return;
  }
  const uint32_t total = g_quran_reader.recordCount(qpk::SectionId::kSurahIndex);
  if (g_surah_picker_selected >= total) return;
  const uint16_t surah_id = static_cast<uint16_t>(g_surah_picker_selected + 1);
  if (!ui::QuranScreen::loadSurah(&g_quran_state, surah_id)) return;
  // loadSurah() sets first_ayah_index to the surah's first ayah; this points
  // the line cursor at the mushaf line that ayah begins on, when the package
  // has one. Without it the layout path would open every surah at line 0.
  ui::QuranScreen::seekToAyah(&g_quran_state, g_quran_state.first_ayah_index);

  g_quran_history_depth = 0;
  g_quran_status[0] = 0;
  g_screen_mode = ScreenMode::kQuran;
  g_force_full_refresh = true;
  g_dirty = true;
  drivers::Logf("[quran] picked surah=%u\n", static_cast<unsigned>(surah_id));
}

// Re-derives the ayah cursor from the line cursor, on the layout path.
//
// It has to be derived, not carried over from the render result: on the
// layout path render() only advances next_ayah_index when an ayah ENDS on
// screen, so a screen showing the middle of a long ayah (2:282 runs for
// pages) reports the ayah the screen STARTED in. Persisting that and
// resuming from it would reopen the reader several screens back -- silently,
// and only for long ayahs. net::ReadingProgress stores an ayah index, so
// this is what keeps the stored position honest.
void QuranSyncAyahToLine() {
  qpk::LineRecord line;
  qpk::AyahRecord ayah;
  uint32_t ayah_index = 0;
  if (g_quran_reader.getLine(g_quran_state.first_line_index, &line) ==
          qpk::Error::kOk &&
      g_quran_reader.findAyahByWordIndex(line.first_word_index, &ayah_index,
                                         &ayah) == qpk::Error::kOk) {
    g_quran_state.first_ayah_index = ayah_index;
    if (ayah.surah_id != g_quran_state.surah_id) {
      const uint32_t keep_line = g_quran_state.first_line_index;
      const uint16_t keep_screen = g_quran_state.screen_number;
      ui::QuranScreen::loadSurah(&g_quran_state, ayah.surah_id);
      g_quran_state.first_line_index = keep_line;
      g_quran_state.first_ayah_index = ayah_index;
      g_quran_state.screen_number = keep_screen;
    }
  }
}

// Paging is driven by what render() reported last, not recomputed here: the
// screen is the only thing that knows how many ayahs actually fit.
void QuranPageForward(uint32_t next_index) {
  // Layout path: the cursor is a mushaf line, and stepping back is just
  // subtraction, so no history stack is needed here at all.
  if (g_quran_used_layout) {
    qpk::LayoutHeader layout;
    if (g_quran_reader.getLayoutHeader(&layout) == qpk::Error::kOk &&
        g_quran_next_line_index >= layout.line_count_total) {
      snprintf(g_quran_status, sizeof(g_quran_status), "End of the Quran");
      g_dirty = true;
      return;
    }
    if (g_quran_next_line_index == g_quran_state.first_line_index) {
      snprintf(g_quran_status, sizeof(g_quran_status), "End of the Quran");
      g_dirty = true;
      return;
    }
    if (g_quran_history_depth < ui::kQuranHistoryDepth) {
      g_quran_history[g_quran_history_depth++] = g_quran_state.first_line_index;
    }
    g_quran_state.first_line_index = g_quran_next_line_index;
    ++g_quran_state.screen_number;
    // Symmetric with QuranPageBack below: the line is the cursor, and the
    // ayah anchor follows from it. `next_index` is informational here.
    (void)next_index;
    QuranSyncAyahToLine();
    g_quran_status[0] = 0;
    g_dirty = true;
    return;
  }

  const uint32_t total = g_quran_reader.recordCount(qpk::SectionId::kAyahIndex);
  if (next_index >= total || next_index == g_quran_state.first_ayah_index) {
    snprintf(g_quran_status, sizeof(g_quran_status), "End of the Quran");
    g_dirty = true;
    return;
  }
  if (g_quran_history_depth < ui::kQuranHistoryDepth) {
    g_quran_history[g_quran_history_depth++] = g_quran_state.first_ayah_index;
  }
  g_quran_state.first_ayah_index = next_index;
  ++g_quran_state.screen_number;
  g_quran_status[0] = 0;

  // Paging naturally runs off the end of a surah, because AYAH_INDEX is one
  // contiguous run of all 6236 ayahs. Follow it into the next surah rather
  // than stopping -- that is what makes the whole Quran readable by paging.
  qpk::AyahRecord ayah;
  if (g_quran_reader.getAyahByIndex(next_index, &ayah) == qpk::Error::kOk &&
      ayah.surah_id != g_quran_state.surah_id) {
    const uint16_t screen = g_quran_state.screen_number;
    ui::QuranScreen::loadSurah(&g_quran_state, ayah.surah_id);
    g_quran_state.first_ayah_index = next_index;  // loadSurah reset it
    g_quran_state.screen_number = screen;
  }
  g_dirty = true;
}

void QuranPageBack() {
  if (g_quran_used_layout) {
    // The history stack still holds LINE indices on this path, so back is the
    // same pop -- but a stack that has run dry is recoverable here in a way it
    // never was for ayah paging: one line back is a legal position, always.
    if (g_quran_history_depth > 0) {
      g_quran_state.first_line_index = g_quran_history[--g_quran_history_depth];
    } else if (g_quran_state.first_line_index > 0) {
      --g_quran_state.first_line_index;
    } else {
      snprintf(g_quran_status, sizeof(g_quran_status), "Start of the Quran");
      g_dirty = true;
      return;
    }
    if (g_quran_state.screen_number > 1) --g_quran_state.screen_number;
    QuranSyncAyahToLine();
    g_quran_status[0] = 0;
    g_dirty = true;
    return;
  }

  if (g_quran_history_depth == 0) {
    snprintf(g_quran_status, sizeof(g_quran_status), "Start of the Quran");
    g_dirty = true;
    return;
  }
  const uint32_t previous = g_quran_history[--g_quran_history_depth];
  g_quran_state.first_ayah_index = previous;
  if (g_quran_state.screen_number > 1) --g_quran_state.screen_number;
  g_quran_status[0] = 0;

  qpk::AyahRecord ayah;
  if (g_quran_reader.getAyahByIndex(previous, &ayah) == qpk::Error::kOk &&
      ayah.surah_id != g_quran_state.surah_id) {
    const uint16_t screen = g_quran_state.screen_number;
    ui::QuranScreen::loadSurah(&g_quran_state, ayah.surah_id);
    g_quran_state.first_ayah_index = previous;
    g_quran_state.screen_number = screen;
  }
  g_dirty = true;
}

void OpenBookmark(const net::Bookmark& mark) {
  const net::LibraryEntry* entry = g_library_index.find(mark.content_id);
  if (entry == nullptr) {
    snprintf(g_bookmarks_status, sizeof(g_bookmarks_status), "That book is no longer on the card");
    g_dirty = true;
    return;
  }
  g_library_status[0] = 0;
  switch (mark.kind) {
    case net::BookmarkKind::kPageBook:
      // OpenPageBook() resumes from these when the book matches.
      memcpy(g_page_resume_id, mark.content_id, sizeof(g_page_resume_id));
      g_page_resume_index = mark.a;
      OpenBook(*entry);
      break;
    case net::BookmarkKind::kTextBook:
      if (mark.c == 2 || mark.c == 3 || mark.c == 4) g_book_scale = static_cast<uint8_t>(mark.c);
      if (entry->type == static_cast<uint16_t>(qpk::PackageType::kTranslation) ||
          entry->type == static_cast<uint16_t>(qpk::PackageType::kTafsir)) {
        OpenTranslation(*entry, static_cast<uint16_t>(mark.b), static_cast<uint16_t>(mark.a));
        break;
      }
      OpenBook(*entry);
      if (g_screen_mode == ScreenMode::kReader) {
        const uint32_t count = g_book_pager.pageCount();
        const uint32_t page = mark.a < 1 ? 1 : (mark.a > count ? count : mark.a);
        g_book_page = static_cast<uint16_t>(page);
      }
      break;
    case net::BookmarkKind::kQuran: {
      // OpenQuran() resumes from progress.bin, and checks the position still
      // exists in the package before trusting it.
      net::ReadingProgress progress;
      memcpy(progress.content_id, mark.content_id, sizeof(progress.content_id));
      progress.surah_id = static_cast<uint16_t>(mark.a);
      progress.first_ayah_index = mark.b;
      progress.screen_number = static_cast<uint16_t>(mark.c);
      if (g_storage.mounted()) net::SaveReadingProgress(&g_storage, progress);
      OpenQuran(*entry);
      break;
    }
  }
  if (g_screen_mode == ScreenMode::kBookmarks) {
    // Still here: it did not open, and the reader said why in the library status.
    snprintf(g_bookmarks_status, sizeof(g_bookmarks_status), "%s",
             g_library_status[0] != 0 ? g_library_status : "Could not open that book");
    g_dirty = true;
  }
}

void OpenSelectedBookmark() {
  uint16_t index = 0;
  switch (ui::BookmarksScreen::rowKind(g_bookmarks_state, g_bookmarks_selected, &index)) {
    case ui::BookmarkRow::kLastRead:
      OpenBookmark(g_bookmarks_state.last_read);
      break;
    case ui::BookmarkRow::kBookmark:
      if (const net::Bookmark* mark = g_bookmarks.at(index)) OpenBookmark(*mark);
      break;
    case ui::BookmarkRow::kNone:
      break;
  }
}

void DeleteSelectedBookmark() {
  uint16_t index = 0;
  if (ui::BookmarksScreen::rowKind(g_bookmarks_state, g_bookmarks_selected, &index) !=
      ui::BookmarkRow::kBookmark) {
    return;
  }
  g_bookmarks.removeAt(index);
  if (g_storage.mounted()) g_bookmarks.save(&g_storage);
  const uint16_t total = ui::BookmarksScreen::rowCount(g_bookmarks_state);
  if (total > 0 && g_bookmarks_selected >= total) g_bookmarks_selected = total - 1;
  snprintf(g_bookmarks_status, sizeof(g_bookmarks_status), "Bookmark deleted");
  g_dirty = true;
}

void HandleLibraryOk() {
  RefreshLibraryState();  // rowKind() reads view/category/index off the state
  uint16_t ordinal = 0;
  switch (ui::LibraryScreen::rowKind(g_library_state, g_library_selected,
                                     &ordinal)) {
    case ui::LibraryRow::kTransferMode:
      drivers::LogLine("[library] OK on Transfer Mode row");
      ToggleTransferMode();
      return;

    case ui::LibraryRow::kCategory: {
      const uint16_t type =
          ui::LibraryScreen::categoryType(static_cast<uint8_t>(ordinal));
      const uint16_t n = ui::LibraryScreen::countOfType(g_library_state, type);
      drivers::Logf("[library] OK on category %s (%u)\n",
                    ui::LibraryScreen::categoryLabel(static_cast<uint8_t>(ordinal)),
                    static_cast<unsigned>(n));
      if (n == 0) {
        // Entering an empty category would show a Back row and nothing else,
        // which reads as a broken menu. Say why instead.
        snprintf(g_library_status, sizeof(g_library_status),
                 "No %s content on the card yet",
                 ui::LibraryScreen::categoryLabel(static_cast<uint8_t>(ordinal)));
        g_dirty = true;
        return;
      }
      EnterLibraryCategory(type);
      return;
    }

    case ui::LibraryRow::kBack:
      drivers::LogLine("[library] OK on Back");
      LeaveLibraryCategory();
      return;

    case ui::LibraryRow::kClock:
      drivers::LogLine("[library] OK on Clock");
      g_screen_mode = ScreenMode::kHome;
      g_library_status[0] = 0;
      g_home_full_pending = true;
      return;

    case ui::LibraryRow::kBookmarks:
      drivers::LogLine("[library] OK on Bookmarks");
      EnterBookmarks();
      return;

    case ui::LibraryRow::kDevice:
      drivers::LogLine("[library] OK on Device");
      g_screen_mode = ScreenMode::kSelfTest;
      g_library_status[0] = 0;
      g_force_full_refresh = true;
      g_dirty = true;
      return;

    case ui::LibraryRow::kItem: {
      const net::LibraryEntry* entry = ui::LibraryScreen::entryOfType(
          g_library_state, g_library_category, ordinal);
      if (entry == nullptr) {
        drivers::Logf("[library] OK on item %u but the index has no entry there\n",
                      static_cast<unsigned>(ordinal));
        return;  // index changed under us; nothing to open
      }
      drivers::Logf("[library] OK on item %u: \"%s\" type=%s\n",
                    static_cast<unsigned>(ordinal), entry->title,
                    net::PackageTypeName(entry->type));
      if (entry->type == static_cast<uint16_t>(qpk::PackageType::kBook)) {
        OpenBook(*entry);
        return;
      }
      if (entry->type == static_cast<uint16_t>(qpk::PackageType::kQuran)) {
        OpenQuran(*entry);
        return;
      }
      if (entry->type == static_cast<uint16_t>(qpk::PackageType::kTranslation) ||
          entry->type == static_cast<uint16_t>(qpk::PackageType::kTafsir)) {
        OpenTranslation(*entry, 1, 1);
        return;
      }
      snprintf(g_library_status, sizeof(g_library_status),
               "No reader for %s yet", net::PackageTypeName(entry->type));
      g_dirty = true;
      return;
    }

    case ui::LibraryRow::kNone:
      return;
  }
}

const char* InputSourceName(hal::InputSource source) {
  switch (source) {
    case hal::InputSource::kMenu:          return "MENU";
    case hal::InputSource::kExit:          return "EXIT";
    case hal::InputSource::kEncoderSwitch: return "OK";
    case hal::InputSource::kEncoder:       return "WHEEL";
    case hal::InputSource::kTouch:         return "TOUCH";
    default:                               return "?";
  }
}

const char* InputActionName(hal::InputAction action) {
  switch (action) {
    case hal::InputAction::kClick:     return "click";
    case hal::InputAction::kLongPress: return "long";
    case hal::InputAction::kRotate:    return "rotate";
    case hal::InputAction::kDown:      return "down";
    case hal::InputAction::kUp:        return "up";
    default:                           return "?";
  }
}

void OpenOptionsMenu();  // defined with the options menu below

void EnterScreenSetup() {
  g_setup = ui::SetupState();
  g_setup.rotation = g_screen_setup.rotation;
  g_setup.touch_orientation = g_screen_setup.touch_orientation;
  g_setup.touch_present = g_input.diagnostics().touch_present;
  // Coming back to the clock screen from here would hide the result behind a
  // photo, so the hardware test screen is the fallback -- it is where this
  // screen belongs anyway.
  g_setup_return_to = g_screen_mode == ScreenMode::kHome ? ScreenMode::kSelfTest
                                                         : g_screen_mode;
  g_screen_mode = ScreenMode::kScreenSetup;
  g_force_full_refresh = true;
  g_dirty = true;
}

// Saves, applies and leaves. EXIT is the only way out that writes the card,
// which is also what makes it safe to try all eight orientations: nothing is
// remembered until it is left deliberately.
void LeaveScreenSetup() {
  g_screen_setup.rotation = g_setup.rotation;
  g_screen_setup.touch_orientation = g_setup.touch_orientation;
  ApplyScreenSetup();
  g_setup.saved = SaveScreenSetup();
  g_screen_mode = g_setup_return_to;
  g_force_full_refresh = true;
  g_dirty = true;
}

// The setup screen's own buttons. It is deliberately not a list: a wrong
// touch orientation is exactly the situation where a list cannot be used, so
// every control here is a physical button, and touch only ever reports where
// it thinks the finger went.
bool HandleScreenSetupEvent(const hal::InputEvent& ev) {
  if (g_screen_mode != ScreenMode::kScreenSetup) return false;
  switch (ev.source) {
    case hal::InputSource::kEncoderSwitch:
      if (ev.action == hal::InputAction::kClick) {
        ui::SetupScreen::nextTouchOrientation(&g_setup);
        g_input.setTouchOrientation(g_setup.touch_orientation);
        g_dirty = true;
      } else if (ev.action == hal::InputAction::kLongPress) {
        // Hold OK is the menu everywhere else; here it is the way out to a
        // normal screen if the wheel is all that works.
        OpenOptionsMenu();
      }
      break;

    case hal::InputSource::kEncoder:
      if (ev.action == hal::InputAction::kRotate) {
        for (int16_t i = 0; i < (ev.delta < 0 ? -ev.delta : ev.delta); ++i) {
          ui::SetupScreen::nextTouchOrientation(&g_setup);
        }
        g_input.setTouchOrientation(g_setup.touch_orientation);
        g_dirty = true;
      }
      break;

    case hal::InputSource::kMenu:
      if (ev.action == hal::InputAction::kClick) {
        ui::SetupScreen::flipPicture(&g_setup);
        // Applied at once: seeing it is the only way to know which way is up.
        g_display.setRotation(g_setup.rotation);
        g_force_full_refresh = true;
        g_dirty = true;
      }
      break;

    case hal::InputSource::kExit:
      if (ev.action == hal::InputAction::kClick) LeaveScreenSetup();
      break;

    case hal::InputSource::kTouch:
      if (ev.action == hal::InputAction::kClick) {
        ui::SetupScreen::noteTap(&g_setup, ev.x, ev.y);
        g_dirty = true;
      }
      break;

    default:
      break;
  }
  return true;
}

// ---------------------------------------------------------------------------
// The options menu (ui::OptionsMenu)
//
// Every screen's actions, by name, opened by holding OK. It replaced the
// hold-OK meanings that used to differ per screen and were written nowhere:
// hold = bookmark here, or delete that bookmark, or toggle transfer mode,
// and hold EXIT = wipe the device. All of them are rows in here now.
// ---------------------------------------------------------------------------

const char* ScreenName();  // defined with the other logging helpers below

ui::MenuScreen MenuScreenFor(ScreenMode mode) {
  switch (mode) {
    case ScreenMode::kHome:        return ui::MenuScreen::kHome;
    case ScreenMode::kLibrary:     return ui::MenuScreen::kLibrary;
    case ScreenMode::kSelfTest:    return ui::MenuScreen::kSelfTest;
    case ScreenMode::kReader:      return ui::MenuScreen::kReader;
    case ScreenMode::kPages:       return ui::MenuScreen::kPages;
    case ScreenMode::kQuran:       return ui::MenuScreen::kQuran;
    case ScreenMode::kSurahPicker: return ui::MenuScreen::kSurahPicker;
    case ScreenMode::kBookmarks:   return ui::MenuScreen::kBookmarks;
    case ScreenMode::kScreenSetup: return ui::MenuScreen::kScreenSetup;
  }
  return ui::MenuScreen::kLibrary;
}

ui::MenuContext CurrentMenuContext() {
  ui::MenuContext context;
  context.screen = MenuScreenFor(g_screen_mode);
  context.translation = g_reader_is_translation;
  context.transfer_on = g_transfer_state != TransferState::kOff;
  context.page_jump = g_page_jump_mode;
  context.table_clock = app::kTableClockMode;
  context.has_bookmarks = g_bookmarks.count() > 0;
  context.library_in_category = g_library_view == ui::LibraryView::kItems;
  if (g_screen_mode == ScreenMode::kBookmarks) {
    uint16_t ignored = 0;
    context.bookmark_selected =
        ui::BookmarksScreen::rowKind(g_bookmarks_state, g_bookmarks_selected,
                                     &ignored) != ui::BookmarkRow::kNone;
  }
  return context;
}

void OpenOptionsMenu() {
  ui::OptionsMenu::build(CurrentMenuContext(), &g_menu);
  g_menu_open = true;
  g_force_full_refresh = true;  // over a grey home photo, or ghosted text
  g_dirty = true;
  drivers::Logf("[menu] open over %s, %u options\n", ScreenName(),
                static_cast<unsigned>(g_menu.count));
}

void CloseOptionsMenu() {
  if (!g_menu_open) return;
  g_menu_open = false;
  g_force_full_refresh = true;
  g_dirty = true;
  // The home screen's own painter owns the glass; Repaint() hands it back.
  if (g_screen_mode == ScreenMode::kHome) g_home_full_pending = true;
}

// Back to the library from wherever we are, releasing whatever is open. The
// same sequence MENU does, as one callable thing.
void LeaveForLibrary() {
  if (g_screen_mode == ScreenMode::kReader) {
    CloseTranslation();
  } else if (g_screen_mode == ScreenMode::kPages) {
    ClosePageBook();
  } else if (g_screen_mode == ScreenMode::kSurahPicker && g_surah_picker_translation) {
    CloseTranslation();
  } else if (g_screen_mode == ScreenMode::kQuran ||
             g_screen_mode == ScreenMode::kSurahPicker) {
    if (g_screen_mode == ScreenMode::kQuran) SaveQuranProgress();
    CloseQuran();
  }
  g_screen_mode = ScreenMode::kLibrary;
  g_library_status[0] = 0;
  g_force_full_refresh = true;
  g_dirty = true;
}

// Runs one row. The menu is already closed when this runs, so an action that
// changes screens (or sleeps) lands on the new screen, not back in the menu.
void RunMenuAction(ui::MenuAction action) {
  switch (action) {
    case ui::MenuAction::kOpenLibrary:
      LeaveForLibrary();
      break;

    case ui::MenuAction::kLeaveCategory:
      if (g_screen_mode == ScreenMode::kLibrary) LeaveLibraryCategory();
      break;

    case ui::MenuAction::kOpenSelected:
      if (g_screen_mode == ScreenMode::kLibrary) {
        HandleLibraryOk();
      } else if (g_screen_mode == ScreenMode::kBookmarks) {
        OpenSelectedBookmark();
      } else if (g_screen_mode == ScreenMode::kSurahPicker) {
        OpenSelectedSurah();
      }
      break;

    case ui::MenuAction::kContinueReading:
      EnterBookmarks();
      // Row 0 is the "continue reading" row when there is one.
      if (g_bookmarks_state.has_last_read) OpenSelectedBookmark();
      break;

    case ui::MenuAction::kBookmarks:
      EnterBookmarks();
      break;

    case ui::MenuAction::kBookmarkHere:
      AddBookmarkHere();
      g_dirty = true;
      break;

    case ui::MenuAction::kDeleteBookmark:
      DeleteSelectedBookmark();
      break;

    case ui::MenuAction::kTextSize:
      CycleBookTextSize();
      g_dirty = true;
      break;

    case ui::MenuAction::kChooseSurah:
      if (g_screen_mode == ScreenMode::kReader && g_reader_is_translation) {
        EnterTranslationSurahPicker();
      } else if (g_screen_mode == ScreenMode::kQuran) {
        SaveQuranProgress();
        EnterSurahPicker(static_cast<uint16_t>(g_quran_state.surah_id - 1));
      }
      break;

    case ui::MenuAction::kGoToPage:
      if (g_screen_mode == ScreenMode::kPages) {
        g_page_jump_mode = !g_page_jump_mode;
        if (g_page_jump_mode) g_page_jump_target = g_page_index;
        g_dirty = true;
      }
      break;

    case ui::MenuAction::kNextChapter:
      if (g_screen_mode == ScreenMode::kPages && g_page_jump_mode) {
        JumpTargetToNextChapter();
      }
      break;

    case ui::MenuAction::kCloseBook:
      LeaveForLibrary();
      break;

    case ui::MenuAction::kSelfTest:
      g_screen_mode = ScreenMode::kSelfTest;
      g_force_full_refresh = true;
      g_dirty = true;
      break;

    case ui::MenuAction::kScreenSetup:
      EnterScreenSetup();
      break;

    case ui::MenuAction::kTransferMode:
      ToggleTransferMode();
      break;

    case ui::MenuAction::kRedraw:
      g_force_full_refresh = true;
      if (g_screen_mode == ScreenMode::kHome) g_home_full_pending = true;
      g_dirty = true;
      break;

    case ui::MenuAction::kFactoryReset:
      // Never reached: kFactoryReset turns into its confirmation list before
      // anything runs (ChooseMenuRow).
      break;

    case ui::MenuAction::kFactoryResetConfirm:
      FactoryReset();
      break;

    case ui::MenuAction::kSleep:
      g_dirty = true;
      Repaint();
      if (!g_ble.active()) GoToSleep();
      break;

    case ui::MenuAction::kBack:
    case ui::MenuAction::kNone:
    default:
      break;
  }
}

// Chooses the highlighted row: either it asks for confirmation and the menu
// stays open, or the menu closes and the action runs.
void ChooseMenuRow() {
  const ui::MenuAction action = ui::OptionsMenu::selectedAction(g_menu);
  drivers::Logf("[menu] chose %s\n",
                g_menu.selected < g_menu.count ? g_menu.items[g_menu.selected].label
                                               : "(nothing)");
  if (ui::OptionsMenu::buildConfirm(action, &g_menu)) {
    g_dirty = true;
    return;
  }
  CloseOptionsMenu();
  RunMenuAction(action);
}

// True when the event was the menu's. Everything the menu does not consume
// while it is open is swallowed rather than passed through: a MENU press that
// also switched screens behind the menu would leave the two disagreeing.
bool HandleMenuEvent(const hal::InputEvent& ev) {
  if (!g_menu_open) return false;
  switch (ev.source) {
    case hal::InputSource::kEncoder:
      if (ev.action == hal::InputAction::kRotate) {
        ui::OptionsMenu::move(&g_menu, ev.delta);
        g_dirty = true;
      }
      break;

    case hal::InputSource::kEncoderSwitch:
      // Only a click chooses. The hold that opened the menu must not also
      // pick whatever row happens to be under the highlight.
      if (ev.action == hal::InputAction::kClick) ChooseMenuRow();
      break;

    case hal::InputSource::kExit:
    case hal::InputSource::kMenu:
      if (ev.action == hal::InputAction::kClick) CloseOptionsMenu();
      break;

    case hal::InputSource::kTouch:
      if (ev.action == hal::InputAction::kClick) {
        const int row = ui::OptionsMenu::rowAt(g_menu, ev.x, ev.y);
        if (row >= 0) {
          g_menu.selected = static_cast<uint8_t>(row);
          ChooseMenuRow();
        }
        // A tap on the margins is ignored, not a close: with the touch
        // orientation still unconfirmed on hardware, a stray mapping should
        // cost nothing.
      }
      break;

    default:
      break;
  }
  // No Repaint() here on purpose: the main loop repaints when g_dirty, which
  // also coalesces a fast wheel spin into one refresh. Repainting per event
  // would cost a full panel refresh for every swallowed button release.
  return true;
}

// One wheel detent's worth of "next"/"previous", per screen. Extracted from
// HandleEvent so that a tap on a page edge turns the page by exactly this
// path rather than a second copy of it -- these clamps and surah-boundary
// rules were settled on real hardware and there should only ever be one of
// them.
void HandleRotate(int16_t delta) {
  g_seen_rotary = true;
  g_counter += delta;
  if (g_screen_mode == ScreenMode::kLibrary) {
    MoveLibrarySelection(delta);
  } else if (g_screen_mode == ScreenMode::kBookmarks) {
    const int32_t total = ui::BookmarksScreen::rowCount(g_bookmarks_state);
    int32_t next = static_cast<int32_t>(g_bookmarks_selected) + delta;
    if (next >= total) next = total - 1;
    if (next < 0) next = 0;
    if (static_cast<uint16_t>(next) != g_bookmarks_selected) {
      g_bookmarks_selected = static_cast<uint16_t>(next);
      g_bookmarks_status[0] = 0;
      g_dirty = true;
    }
  } else if (g_screen_mode == ScreenMode::kPages && g_page_jump_mode) {
    int64_t target = static_cast<int64_t>(g_page_jump_target) +
                     static_cast<int64_t>(delta) * ui::kPageJumpStep;
    if (target < 0) target = 0;
    if (g_page_count > 0 && target > static_cast<int64_t>(g_page_count) - 1) {
      target = static_cast<int64_t>(g_page_count) - 1;
    }
    if (static_cast<uint32_t>(target) != g_page_jump_target) {
      g_page_jump_target = static_cast<uint32_t>(target);
      g_dirty = true;
    }
  } else if (g_screen_mode == ScreenMode::kPages) {
    int64_t next = static_cast<int64_t>(g_page_index) + delta;
    if (next < 0) next = 0;
    if (g_page_count > 0 && next > static_cast<int64_t>(g_page_count) - 1) {
      next = static_cast<int64_t>(g_page_count) - 1;
    }
    if (static_cast<uint32_t>(next) != g_page_index) {
      g_page_index = static_cast<uint32_t>(next);
      g_page_resume_index = g_page_index;
      g_page_progress_dirty = true;
      g_page_turn_ms = millis();
      g_dirty = true;
    }
  } else if (g_screen_mode == ScreenMode::kReader && g_reader_is_translation) {
    g_reader_notice[0] = 0;
    // Past the last page of a surah into the next one, and back.
    const int32_t last = static_cast<int32_t>(g_book_pager.pageCount());
    int32_t next = static_cast<int32_t>(g_book_page) + delta;
    if (next > last && g_trans_surah < g_trans_surah_count) {
      ShowTranslationSurah(static_cast<uint16_t>(g_trans_surah + 1), false);
    } else if (next < 1 && g_trans_surah > 1 && g_trans_surah_count > 0) {
      ShowTranslationSurah(static_cast<uint16_t>(g_trans_surah - 1), true);
    } else {
      if (next < 1) next = 1;
      if (next > last) next = last;
      if (static_cast<uint16_t>(next) != g_book_page) {
        g_book_page = static_cast<uint16_t>(next);
        g_dirty = true;
      }
    }
  } else if (g_screen_mode == ScreenMode::kReader) {
    g_reader_notice[0] = 0;
    const int32_t last = static_cast<int32_t>(g_book_pager.pageCount());
    int32_t next = static_cast<int32_t>(g_book_page) + delta;
    if (next < 1) next = 1;
    if (next > last) next = last;
    if (static_cast<uint16_t>(next) != g_book_page) {
      g_book_page = static_cast<uint16_t>(next);
      g_dirty = true;
    }
  } else if (g_screen_mode == ScreenMode::kQuran) {
    // One detent, one screen. There is no page *count* to clamp against:
    // where a screen ends depends on measured glyph widths, so the only
    // way to know is to have drawn it. Forward uses what the last
    // render() reported; backward comes off the history stack.
    if (delta > 0) {
      QuranPageForward(g_quran_next_ayah_index);
    } else if (delta < 0) {
      QuranPageBack();
    }
  } else if (g_screen_mode == ScreenMode::kSurahPicker) {
    MoveSurahPickerSelection(delta);
  } else if (g_screen_mode == ScreenMode::kSelfTest) {
    // The self-test screen displays the raw counter, so every detent
    // is a real change there. The home screen shows nothing the wheel
    // changes.
    g_dirty = true;
  }
}

// Where a tap landed, and what that means on this screen. Taps only ever do
// what a button already does: open the highlighted thing, or turn a page.
void HandleTouchTap(int16_t x, int16_t y) {
  switch (g_screen_mode) {
    case ScreenMode::kLibrary: {
      const int32_t row = ui::LibraryScreen::rowAt(g_library_state, x, y);
      if (row < 0) break;
      // Opening on the first tap, not selecting and waiting for a second:
      // every extra step here is another half-second refresh, and EXIT
      // undoes a mis-tap.
      g_library_selected = static_cast<uint16_t>(row);
      RefreshLibraryState();
      HandleLibraryOk();
      break;
    }

    case ScreenMode::kBookmarks: {
      const int32_t row = ui::BookmarksScreen::rowAt(g_bookmarks_state, x, y);
      if (row < 0) break;
      g_bookmarks_selected = static_cast<uint16_t>(row);
      OpenSelectedBookmark();
      break;
    }

    case ScreenMode::kSurahPicker: {
      // The picker's state is rebuilt in Repaint(); the scroll position it
      // was last drawn with is the one in g_surah_picker_scroll_top, which is
      // what the hit test needs.
      g_surah_picker_state.selected = g_surah_picker_selected;
      g_surah_picker_state.scroll_top = g_surah_picker_scroll_top;
      const int32_t row = ui::SurahPickerScreen::rowAt(g_surah_picker_state, x, y);
      if (row < 0) break;
      g_surah_picker_selected = static_cast<uint16_t>(row);
      OpenSelectedSurah();
      break;
    }

    case ScreenMode::kReader:
    case ScreenMode::kPages:
    case ScreenMode::kQuran: {
      const int8_t delta =
          util::PageTapDelta(x, board::kWidth, app::kTouchPageEdgePx);
      if (delta != 0) HandleRotate(delta);
      break;
    }

    default:
      // Every other screen is driven by the wheel and the buttons for now.
      break;
  }
}

void HandleEvent(const hal::InputEvent& ev) {
  if (ev.source == hal::InputSource::kTouch) {
    drivers::Logf("[input] TOUCH %s at (%d,%d)\n", InputActionName(ev.action),
                  static_cast<int>(ev.x), static_cast<int>(ev.y));
  } else {
    drivers::Logf("[input] %s %s delta=%d\n", InputSourceName(ev.source),
                  InputActionName(ev.action), static_cast<int>(ev.delta));
  }
  if (HandleMenuEvent(ev)) return;
  if (HandleScreenSetupEvent(ev)) return;
  switch (ev.source) {
    case hal::InputSource::kMenu:
      if (ev.action == hal::InputAction::kClick) {
        g_seen_menu = true;
        if (g_screen_mode == ScreenMode::kPages && g_page_jump_mode) {
          JumpTargetToNextChapter();
          break;
        }
        if (g_screen_mode == ScreenMode::kReader && g_reader_is_translation) {
          AddBookmarkHere();  // MENU = bookmark in a translation (see kTranslationHint)
          break;
        }
        // MENU switches between the library browser and the hardware
        // self-test screen -- see ScreenMode. From the reader it always
        // goes back to the library instead ("MENU opens the menu"), not toggling to the
        // self-test screen a reader session was never on. Same for the
        // Quran reader and its picker, which also release the open package
        // on the way out -- MENU used to skip straight to kLibrary without
        // closing it, leaking the handle CloseQuran() exists to release.
        if (g_screen_mode == ScreenMode::kReader) {
          CloseTranslation();
          g_screen_mode = ScreenMode::kLibrary;
        } else if (g_screen_mode == ScreenMode::kBookmarks) {
          g_screen_mode = ScreenMode::kLibrary;
        } else if (g_screen_mode == ScreenMode::kPages) {
          ClosePageBook();
          g_screen_mode = ScreenMode::kLibrary;
        } else if (g_screen_mode == ScreenMode::kSurahPicker && g_surah_picker_translation) {
          CloseTranslation();
          g_screen_mode = ScreenMode::kLibrary;
        } else if (g_screen_mode == ScreenMode::kQuran ||
                   g_screen_mode == ScreenMode::kSurahPicker) {
          if (g_screen_mode == ScreenMode::kQuran) SaveQuranProgress();
          CloseQuran();
          g_screen_mode = ScreenMode::kLibrary;
        } else if (g_screen_mode == ScreenMode::kHome) {
          g_screen_mode = ScreenMode::kLibrary;
        } else if (g_screen_mode == ScreenMode::kLibrary) {
          g_screen_mode = ScreenMode::kSelfTest;
        } else {
          g_screen_mode =
              app::kTableClockMode ? ScreenMode::kHome : ScreenMode::kLibrary;
        }
        g_library_status[0] = 0;
        g_force_full_refresh = true;
        g_dirty = true;
      }
      break;

    case hal::InputSource::kExit:
      if (ev.action == hal::InputAction::kClick) {
        g_seen_exit = true;
        if (g_transfer_state != TransferState::kOff && !app::kTableClockMode) {
          // architecture.md 4.3: EXIT ends the session rather than being
          // ignored. Safe to interrupt mid-upload -- UploadManager resumes
          // from the .part file's size on disk, same as any other reset.
          g_dirty = true;
          EndTransferMode();
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kBookmarks) {
          g_screen_mode = ScreenMode::kLibrary;
          g_force_full_refresh = true;
          g_dirty = true;
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kPages && g_page_jump_mode) {
          // Out of "Go to page", staying on the page that was open.
          g_page_jump_mode = false;
          g_dirty = true;
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kPages) {
          // Back to the library; the book resumes at this page if reopened.
          ClosePageBook();
          g_screen_mode = ScreenMode::kLibrary;
          g_force_full_refresh = true;
          g_dirty = true;
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kReader) {
          CloseTranslation();  // releases the package if a translation was open
          // "EXIT goes back" --
          // closing a book is not the same gesture as sleeping the device.
          g_screen_mode = ScreenMode::kLibrary;
          g_force_full_refresh = true;
          g_dirty = true;
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kQuran) {
          // Same gesture, one level: back to the picker, not all the way out
          // to the library, so switching surahs does not mean re-opening the
          // package. It stays open -- ui::SurahPickerScreen reads it too.
          // Highlight g_quran_state.surah_id, not the picker's last
          // selection: paging forward can cross a surah boundary
          // (QuranPageForward's loadSurah call) without ever going back
          // through the picker, so the two can disagree.
          SaveQuranProgress();
          EnterSurahPicker(static_cast<uint16_t>(g_quran_state.surah_id - 1));
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kSurahPicker && g_surah_picker_translation) {
          // Back to the translation, on the page it was showing.
          g_screen_mode = ScreenMode::kReader;
          g_force_full_refresh = true;
          g_dirty = true;
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kSurahPicker) {
          // One level further: leaving the picker releases the package, the
          // real resource that stays open the whole time either screen is up.
          CloseQuran();
          g_screen_mode = ScreenMode::kLibrary;
          g_force_full_refresh = true;
          g_dirty = true;
          Repaint();
          break;
        }
        if (g_screen_mode == ScreenMode::kLibrary &&
            g_library_view == ui::LibraryView::kItems) {
          // Same gesture, one level up: leaving a category is "back" too, and
          // sleeping the device instead would be a surprising thing for the
          // back button to do.
          LeaveLibraryCategory();
          Repaint();
          break;
        }
        if (app::kTableClockMode) {
          // A table clock does not sleep. EXIT from the top of any menu goes
          // home; on the home screen it redraws it, a manual ghost clear.
          g_screen_mode = ScreenMode::kHome;
          g_dirty = true;
          Repaint();
          break;
        }
        // EXIT also means "go to sleep now"; the checklist stays on the glass
        // and the next press wakes us. This is the manual deep-sleep test.
        g_dirty = true;
        Repaint();
        // ...unless BLE is mid-pairing: deep sleep cuts the radio
        // unconditionally, abandoning a pairing attempt the user is actively
        // in the middle of.
        if (!g_ble.active()) {
          GoToSleep();
        }
      } else if (ev.action == hal::InputAction::kLongPress) {
        g_seen_exit = true;
        // This used to factory reset, with nothing on the glass saying so and
        // no confirmation: one held button between a reader and a wiped
        // device. It is a row in the hardware test screen's options now, and
        // it asks first.
        drivers::LogLine(
            "[input] hold EXIT does nothing now -- hold OK for Options");
      }
      break;

    case hal::InputSource::kEncoderSwitch:
      if (ev.action == hal::InputAction::kClick) {
        g_seen_switch = true;
        // On the library browser this is a real selection (book, or the
        // Transfer Mode row); on the surah picker it opens the highlighted
        // surah; on the self-test screen there is nothing to select, only
        // the checklist to prove the button itself works.
        if (g_screen_mode == ScreenMode::kLibrary) {
          HandleLibraryOk();
        } else if (g_screen_mode == ScreenMode::kBookmarks) {
          OpenSelectedBookmark();
        } else if (g_screen_mode == ScreenMode::kReader) {
          if (g_reader_is_translation) {
            EnterTranslationSurahPicker();
          } else {
            CycleBookTextSize();
          }
        } else if (g_screen_mode == ScreenMode::kPages) {
          if (!g_page_jump_mode) {
            g_page_jump_mode = true;
            g_page_jump_target = g_page_index;
          } else {
            g_page_jump_mode = false;
            if (g_page_jump_target != g_page_index) {
              g_page_index = g_page_jump_target;
              g_page_resume_index = g_page_index;
              g_page_progress_dirty = true;
              g_page_turn_ms = millis();
              g_force_full_refresh = true;  // a whole new page: clear the ghosting too
            }
            drivers::Logf("[pages] jumped to page %u", static_cast<unsigned>(g_page_index + 1));
          }
          g_dirty = true;
        } else if (g_screen_mode == ScreenMode::kSurahPicker) {
          OpenSelectedSurah();
        } else if (g_screen_mode != ScreenMode::kHome) {
          // Not on home: a repaint there is a multi-second grey refresh.
          g_dirty = true;
        }
      } else if (ev.action == hal::InputAction::kLongPress) {
        g_seen_switch = true;
        // One meaning, on every screen: show me what this screen can do.
        // Bookmarking, deleting a bookmark, text size and transfer mode are
        // all named rows in there now, instead of things you had to already
        // know.
        OpenOptionsMenu();
      }
      break;

    case hal::InputSource::kEncoder:
      if (ev.action == hal::InputAction::kRotate) {
        HandleRotate(ev.delta);
      }
      break;

    case hal::InputSource::kTouch:
      // A tap acts where the finger landed; a hold is the options menu, and
      // that is handled by the button path so that resting a thumb on the
      // glass while reading cannot open it.
      if (ev.action == hal::InputAction::kClick) HandleTouchTap(ev.x, ev.y);
      break;

    default:
      break;
  }
}

const char* ResetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int-watchdog";
    case ESP_RST_TASK_WDT:  return "task-watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

// First thing on the serial port, before any peripheral is touched: if these
// lines appear, the UART, the CH340 and the monitor's baud rate are all fine,
// and whatever goes wrong afterwards is not a serial problem. A brownout or
// panic reset reason here also explains a board that "just restarts".
void PrintBootDiagnostics() {
  drivers::Logf("[boot] ===== %s fw=%s =====\n", app::kProductName,
                app::kFirmwareVersion);
  drivers::Logf("[boot] reset=%s chip=%s rev%u cores=%u cpu=%luMHz\n",
                ResetReasonName(esp_reset_reason()), ESP.getChipModel(),
                static_cast<unsigned>(ESP.getChipRevision()),
                static_cast<unsigned>(ESP.getChipCores()),
                static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  drivers::Logf("[boot] flash=%luMB psram=%lu (free %lu) heap_free=%lu\n",
                static_cast<unsigned long>(ESP.getFlashChipSize() / (1024UL * 1024UL)),
                static_cast<unsigned long>(ESP.getPsramSize()),
                static_cast<unsigned long>(ESP.getFreePsram()),
                static_cast<unsigned long>(ESP.getFreeHeap()));
  drivers::Logf("[boot] %s, panel %ux%u, built %s %s\n", board::kBoardName,
                static_cast<unsigned>(board::kWidth),
                static_cast<unsigned>(board::kHeight), __DATE__, __TIME__);
}

const char* ScreenName() {
  switch (g_screen_mode) {
    case ScreenMode::kHome:        return "home";
    case ScreenMode::kLibrary:     return "library";
    case ScreenMode::kSelfTest:    return "selftest";
    case ScreenMode::kReader:      return "reader";
    case ScreenMode::kPages:       return "pages";
    case ScreenMode::kBookmarks:   return "bookmarks";
    case ScreenMode::kQuran:       return "quran";
    case ScreenMode::kSurahPicker: return "surah-picker";
    case ScreenMode::kScreenSetup: return "screen-setup";
  }
  return "?";
}

const char* TransferStateName() {
  switch (g_transfer_state) {
    case TransferState::kOff:        return "off";
    case TransferState::kConnecting: return "connecting";
    case TransferState::kOn:         return "on";
  }
  return "?";
}

// One line every kHeartbeatMs from loop(), so a quiet serial monitor means
// "not running", never "running but idle". loop() itself never blocks on
// Wi-Fi (transfer mode's connect is polled), so this keeps ticking through a
// connect attempt; the only long gaps are a panel refresh and GxEPD2's own
// BUSY wait, which prints "Busy Timeout!" if the panel never answers.
constexpr uint32_t kHeartbeatMs = 10000;
uint32_t g_last_heartbeat_ms = 0;

// When /DEVICE/alive.bin was last written (net::SaveAliveTime). Once a
// minute, and only with a synced clock: after an outage, resets.log's next
// line says roughly when the device stopped.
uint32_t g_last_alive_save_ms = 0;
constexpr uint32_t kAliveSaveIntervalMs = 60000;

void PollHeartbeat(uint32_t now) {
  if (g_clock.synced() && g_storage.mounted() &&
      (g_last_alive_save_ms == 0 ||
       static_cast<uint32_t>(now - g_last_alive_save_ms) >= kAliveSaveIntervalMs)) {
    g_last_alive_save_ms = now == 0 ? 1 : now;
    net::SaveAliveTime(&g_storage, g_clock.nowUnix());
  }
  if (static_cast<uint32_t>(now - g_last_heartbeat_ms) < kHeartbeatMs) return;
  g_last_heartbeat_ms = now;
  drivers::Logf(
      "[hb] up=%lus screen=%s epd=%d refreshes=%lu dirty=%d busy=%d "
      "sleep_in=%lus transfer=%s ble=%d heap_free=%lu\n",
      static_cast<unsigned long>(now / 1000), ScreenName(),
      g_display.ready() ? 1 : 0, static_cast<unsigned long>(g_refresh_count),
      g_dirty ? 1 : 0, g_idle.busy() ? 1 : 0,
      static_cast<unsigned long>(g_idle.remainingMs(now) / 1000),
      TransferStateName(), g_ble.active() ? 1 : 0,
      static_cast<unsigned long>(ESP.getFreeHeap()));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(50);
  PrintBootDiagnostics();

  g_power.begin();
  drivers::Logf("[boot] %s fw=%s boot=%lu wake=%s gpio=%d\n",
                app::kProductName, app::kFirmwareVersion,
                static_cast<unsigned long>(g_power.wakeInfo().boot_count),
                drivers::PowerManager::wakeReasonName(
                    g_power.wakeInfo().reason),
                g_power.wakeInfo().gpio);

  // A timer wake with no matching input is this device waking itself purely
  // to sample the battery (GoToSleep() is what arms it -- see
  // app::kBatterySampleIntervalMs). The whole point is that it is cheap:
  // take the reading, log the exactly-known elapsed time, and go straight
  // back to sleep without bringing up storage, the panel, or input.
  if (g_power.wakeInfo().reason == hal::WakeReason::kTimer) {
    g_battery_clock_ms += app::kBatterySampleIntervalMs;
    SampleBattery();
    drivers::Logf("[battery] sample wake, clock=%llu ms\n",
                  static_cast<unsigned long long>(g_battery_clock_ms));
    Serial.flush();
    g_power.deepSleepFor(app::kBatterySampleIntervalMs);  // does not return
  }

  ++g_real_wake_count;  // see its declaration -- excludes the kTimer bail-out above

  // Input first: we want to know which pin is held before anything else runs.
  g_input.attachTouch(&g_touch);
  g_input.begin();
  SampleBattery();  // opportunistic: keeps the displayed % fresh on a real wake

  // Storage before the panel now (M1 had it the other way around): the
  // wake-without-flashing restore below needs SD mounted so it can hand
  // the display driver a path to try, before begin() decides whether to
  // clear or restore.
  BringUpStorage();
  const char* reset_reason = ResetReasonName(esp_reset_reason());
  g_http.setBootInfo(reset_reason, g_power.wakeInfo().boot_count);
  if (g_state.sd == ui::TestResult::kPass) {
    // Before anything else can fail: the reason this boot happened, and when
    // the previous run was last known alive (0 = never synced / unknown).
    const uint32_t last_alive = net::LoadAliveTime(&g_storage);
    net::AppendResetRecord(&g_storage, g_power.wakeInfo().boot_count, reset_reason, last_alive);
    g_bookmarks.load(&g_storage);
    g_verified.load(&g_storage);
    drivers::Logf("[boot] reset=%s last_alive=%lu (history in /DEVICE/resets.log)", reset_reason,
                  static_cast<unsigned long>(last_alive));
    BringUpNetState();
    g_photos.begin(&g_storage);
    LoadTimeZone();
    LoadScreenSetup();
    if (g_power.wakeInfo().reason != hal::WakeReason::kColdBoot) {
      // A real wake, not a cold boot: the panel's RAM contents are unknown
      // on a cold boot, but on a real wake the last frame this same device
      // wrote to SD is still the truth. See architecture.md 4.2.
      g_display.setRestorePath(&g_storage, app::kFileFramebuffer);
    }
  }

  // Before the panel comes up: begin() pushes the rotation into the
  // controller, and a restored frame is written in that same orientation.
  ApplyScreenSetup();
  const bool display_ok = g_display.begin();
  g_state.eink = display_ok ? ui::TestResult::kPass : ui::TestResult::kFail;
  g_state.framebuffer_in_psram = g_display.framebufferInPsram();
  g_state.psram = g_display.framebufferInPsram() ? ui::TestResult::kPass
                                                 : ui::TestResult::kFail;
  drivers::Logf("[epd] init=%d psram_fb=%d psram_total=%u restored=%d\n",
                display_ok ? 1 : 0, g_display.framebufferInPsram() ? 1 : 0,
                static_cast<unsigned>(ESP.getPsramSize()),
                g_display.restoredFrame() ? 1 : 0);
  if (display_ok && g_display.restoredFrame()) {
    // The panel already shows the last real frame; the first repaint below
    // only has to diff against it, not redraw from a flashed-white glass.
    g_force_full_refresh = false;
  }

  g_idle.begin(app::kIdleSleepMs, millis());
  g_last_poll_ms = millis();

  if (display_ok) {
#ifdef EPD_SMOKE_TEST
    RunPanelSmokeTest();
#endif
    Repaint();
  } else {
    // Without a panel there is nothing to show; report over serial and sleep
    // rather than spinning on a dead bus.
    drivers::LogLine("[epd] init failed -- check the panel power rail (GPIO7)");
  }
  if (app::kTableClockMode) {
    // Online from boot: the clock needs NTP and the desktop app needs the API.
    if (g_provisioning.ready() && !g_ble.active()) {
      ToggleTransferMode();
    } else {
      drivers::LogLine("[wifi] not provisioned yet -- pair over BLE or put "
                       "/DEVICE/wifi.json on the card");
    }
  }
  drivers::Logf("[boot] setup done in %lums, entering loop (heartbeat every %lus)\n",
                static_cast<unsigned long>(millis()),
                static_cast<unsigned long>(kHeartbeatMs / 1000));
}

void loop() {
  const uint32_t now = millis();

  PollHeartbeat(now);

  if (static_cast<uint32_t>(now - g_last_poll_ms) >=
      app::kInputPollIntervalMs) {
    g_last_poll_ms = now;

    g_input.poll(now);
  }

  hal::InputEvent ev;
  while (g_input.nextEvent(&ev)) {
    g_idle.noteActivity(now);
    HandleEvent(ev);
  }

  if (g_ble.active()) {
    g_ble.poll();
    CheckProvisioningOutcome();
  }
  PollTransferMode();
  if (g_http.active()) {
    g_http.poll();
    if (g_http.takePhotosChanged()) g_home_photos_dirty = true;
    if (g_http.takeTimeZoneChanged()) g_home_full_pending = true;
    if (g_http.takeRestartRequested()) {
      // A firmware update is installed and its reply has gone out: restart
      // into it, after a moment for the TCP send to finish.
      drivers::LogLine("[firmware] update installed, restarting");
      Serial.flush();
      delay(1000);
      ESP.restart();
    }
  }

  // Spec section 14: never repaint while more rotary steps are still landing.
  if (g_dirty && g_display.ready() && !g_input.rotaryPending()) {
    Repaint();
  }
  // Not while the options menu is up: PollHomeScreen owns the glass on the
  // clock screen and would paint the clock straight back over the menu on
  // the next minute tick. (PollPagePrefetch below only decodes, so it is
  // free to keep running.)
  if (!g_input.rotaryPending() && !g_menu_open) PollHomeScreen(millis());
  if (!g_input.rotaryPending()) PollPagePrefetch();

  // A table clock never sleeps; it is on USB power (app::kTableClockMode).
  if (!app::kTableClockMode && g_idle.shouldSleep(millis(), g_input.anyHeld())) {
    GoToSleep();
  }

  delay(1);
}
