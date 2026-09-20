// library_screen.h -- Milestone 5's on-device menu: the library browser.
//
// Two levels, not one. The top level is a fixed set of four categories
// (Quran, Books, Translations, Tafsir) plus the synthetic "Transfer Mode"
// action; selecting a category drills into just that type's packages. A flat
// list was fine for six test packages and stops being fine the moment a real
// library arrives -- scrolling past a hundred books to reach a surah is not
// a menu, and on a 6-row screen with a wheel it is a long way past unusable.
//
// The categories are fixed rather than derived from what happens to be
// installed, so the menu does not change shape as content comes and goes: an
// empty category still says "Quran   0", which tells you where Quran content
// would go. That matters on a device whose whole library arrives by copying
// files onto a card.
//
// Pure render: state in, canvas out, no hardware -- same pattern as
// SelfTestScreen and ReaderScreen. `index` is read directly (LibraryIndex
// only depends on hal::IStorage, no Arduino, so this is exercisable on the
// host too) rather than copied into the state struct, to avoid materializing
// up to kMaxLibraryEntries rows on every repaint just to draw six of them.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "net/library_index.h"

namespace ui {

// 12 rows at a 28 px pitch fill the 800x480 panel between the header rule and
// the scroll indicator (library_screen.cpp's layout constants).
constexpr uint8_t kLibraryMaxVisibleRows = 12;

/** Quran, Books, Translations, Tafsir -- the four /LIBRARY subdirectories. */
constexpr uint8_t kLibraryCategoryCount = 4;

enum class LibraryView : uint8_t {
  kCategories,  // the four types, plus Transfer Mode
  kItems,       // one type's packages, plus a Back row
};

/**
 * What a given row is. Returned by rowKind() so main.cpp switches on a name
 * instead of re-deriving "is this the last row?" arithmetic that has to agree
 * with the renderer's.
 */
enum class LibraryRow : uint8_t {
  kCategory,
  kTransferMode,
  kBack,
  kItem,
  kNone,  // out of range: the index changed under us
  kClock,      // home tile: the table clock
  kDevice,     // home tile: the hardware self-test / diagnostics screen
  kBookmarks,  // home tile: continue reading, and saved places
};

// The categories view is a launcher, like a phone's home screen: eight icon
// tiles, four to a row -- Quran, Books, Translations, Tafsir, then Wi-Fi
// transfer, Clock, Device and Bookmarks. The wheel still moves through them
// in that order, so rowKind() and main.cpp's OK handler work by row number.
constexpr uint8_t kLibraryHomeTileCount = kLibraryCategoryCount + 4;
constexpr int kLibraryTileColumns = 4;
constexpr int kLibraryTileW = 180;
constexpr int kLibraryTileH = 170;
constexpr int kLibraryTileGap = 8;
constexpr int kLibraryTileLeft = 24;  // 24 + 4 * 180 + 3 * 8 = 768
constexpr int kLibraryTileTop = 88;   // 88 + 2 * 170 + 8 = 436, above the footer rule

// Books are a shelf of covers rather than a list: two rows of five, "< Back"
// as the first tile, the wheel moving through them in order. main.cpp hands
// in the covers for the page of tiles being drawn, so the screen stays pure.
constexpr uint8_t kLibraryShelfColumns = 5;
constexpr uint8_t kLibraryShelfRows = 2;
constexpr uint8_t kLibraryShelfPageTiles = kLibraryShelfColumns * kLibraryShelfRows;

struct LibraryState {
  const net::LibraryIndex* index = nullptr;

  LibraryView view = LibraryView::kCategories;
  /** Which type kItems is showing; a qpk::PackageType value. */
  uint16_t category = 0;

  // Selection is over whatever rowCount() says the current view has.
  // main.cpp owns clamping and scrolling this against input.
  uint16_t selected = 0;
  uint16_t scroll_top = 0;

  bool transfer_ready = false;  // provisioning has saved Wi-Fi credentials
  bool transfer_connecting = false;
  bool transfer_mode = false;
  const char* transfer_address = "";  // "host:port", set once connected

  // Footer feedback for the last OK press, e.g. "Reader not built yet
  // (Milestone 5)". Cleared by the caller on the next navigation, not by
  // this screen -- rendering does not mutate state.
  const char* status_message = "";

  // For the Bookmarks tile's "N saved".
  uint16_t bookmark_count = 0;

  // Books shelf: the covers for the page of tiles holding `selected`, by
  // position on that page (0 = its first tile). Each is qpk::kCoverPixelBytes
  // of 2bpp pixels (docs/qpk-format.md 9c), or nullptr to draw a plain cover.
  const uint8_t* shelf_covers[kLibraryShelfPageTiles] = {};

  // Covers go to the panel in greys (app::kShelfGreyCovers): the canvas
  // leaves each cover's inside white for composeShelfGrey()'s pixels, and the
  // selection is the book's title drawn white on black -- a strip under the
  // covers that can be updated alone -- instead of a frame round the cover.
  bool grey_covers = false;
};

// The shelf's grey layer: every cover on a page, as one 2bpp image (4 pixels
// a byte, MSB first, 0 black .. 3 white) for Epd750Display::flushGrey(). The
// width is a multiple of 4, as flushGrey requires.
constexpr int kShelfGreyX = 20;
constexpr int kShelfGreyY = 84;
constexpr int kShelfGreyW = 760;
constexpr int kShelfGreyH = 322;
constexpr uint32_t kShelfGreyBytes = (kShelfGreyW / 4) * kShelfGreyH;

class LibraryScreen {
 public:
  static void render(gfx::Canvas& canvas, const LibraryState& state);

  /** Selectable rows in the current view. main.cpp clamps `selected` to it. */
  static uint16_t rowCount(const LibraryState& state);

  /**
   * Classifies a row, and reports its ordinal within its kind: the category
   * number for kCategory, or the position within the filtered list for
   * kItem. `ordinal` may be null.
   */
  static LibraryRow rowKind(const LibraryState& state, uint16_t row,
                            uint16_t* ordinal);

  /** Where home tile `row` sits (categories view). */
  static void tileRect(uint16_t row, int* x, int* y, int* w, int* h);

  /** True when the current view is the Books shelf rather than a list. */
  static bool isShelf(const LibraryState& state);

  /**
   * Which row a tap at (x, y) is on, or -1 for none. Display pixels. Covers
   * both shapes this screen takes: the list of rows, and the shelf of book
   * covers, where the whole tile including its title counts as the target.
   */
  static int32_t rowAt(const LibraryState& state, int16_t x, int16_t y);

  /** The first row on the shelf page that holds `selected`. */
  static uint16_t shelfPageStart(const LibraryState& state);

  /** Top-left of the 108x144 cover for tile `slot` on a shelf page. */
  static void shelfCoverRect(uint8_t slot, int* x, int* y);

  /**
   * The strip holding the titles of shelf row `shelf_row` (0 or 1): below
   * that row's covers, clear of every cover's pixels, and byte-aligned
   * (x and w multiples of 8) for Epd750Display::flushWindow().
   */
  static void shelfTitleBand(uint8_t shelf_row, int* x, int* y, int* w, int* h);

  /**
   * Fills `out` (kShelfGreyBytes) with the covers of the shelf page being
   * shown, at their places within the kShelfGrey* rectangle, white elsewhere.
   */
  static void composeShelfGrey(const LibraryState& state, uint8_t* out);

  /** The package type behind category `i` (0..kLibraryCategoryCount-1). */
  static uint16_t categoryType(uint8_t i);

  /** "Quran" / "Books" / "Translations" / "Tafsir". */
  static const char* categoryLabel(uint8_t i);

  /** How many installed packages have this type. */
  static uint16_t countOfType(const LibraryState& state, uint16_t type);

  /**
   * The `nth` package of `type`, or nullptr. Linear in the index, which is
   * bounded by kMaxLibraryEntries and is walked at most six times per
   * repaint -- cheaper than keeping a second sorted structure in sync.
   */
  static const net::LibraryEntry* entryOfType(const LibraryState& state,
                                              uint16_t type, uint16_t nth);
};

}  // namespace ui
