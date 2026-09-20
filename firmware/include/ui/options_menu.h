// options_menu.h -- the actions of the current screen, listed by name.
//
// Why this exists: OK meant something different on every screen and the glass
// never said what. Click was "open" on the library, "text size" in a book,
// "go to page" in a picture book; hold was "bookmark", or "delete this
// bookmark", or "turn on transfer mode", and holding EXIT wiped the device.
// None of it was written anywhere the reader could see.
//
// So: hold OK (or tap the menu) anywhere and this opens, listing what that
// screen can do. The wheel moves, OK chooses, EXIT closes -- and because the
// rows are big enough to hit with a finger, a tap chooses too. One mechanism
// for both input paths, and nothing hidden.
//
// Pure logic and pure rendering, like every other ui:: screen: build() turns
// a context into a list of labelled actions, render() draws it, rowAt() says
// which row a finger landed on. main.cpp does the acting.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"

namespace ui {

// Eight rows at a 40 px pitch fill the list area, and 40 px is about 2.5 mm
// short of a fingertip -- comfortably hittable without aiming.
constexpr uint8_t kOptionsMaxItems = 8;

// Which screen the menu was opened over. Mirrors main.cpp's ScreenMode, which
// is file-local to main.cpp and cannot be reached from here (and should not
// be: this list is a UI concern, not an app-state one).
enum class MenuScreen : uint8_t {
  kHome,
  kLibrary,
  kSelfTest,
  kReader,       // a book's text, or a translation (see MenuContext)
  kPages,        // a book shown as page pictures
  kQuran,
  kSurahPicker,
  kBookmarks,
  kScreenSetup,        // the setup screen has its own short menu
};

enum class MenuAction : uint8_t {
  kNone = 0,
  kOpenLibrary,
  kOpenSelected,       // the highlighted library row / bookmark / surah
  kLeaveCategory,      // up from a category to the category list
  kContinueReading,
  kBookmarks,          // open the bookmarks list
  kBookmarkHere,       // save this place
  kDeleteBookmark,
  kTextSize,
  kChooseSurah,
  kGoToPage,
  kNextChapter,
  kCloseBook,          // back to the library, releasing whatever is open
  kSelfTest,
  kScreenSetup,        // ui::SetupScreen: picture and touch orientation
  kTransferMode,       // toggles: the label says which way
  kRedraw,
  kFactoryReset,       // asks first -- see BuildConfirm()
  kFactoryResetConfirm,
  kSleep,
  kBack,               // close the menu, change nothing
};

struct MenuItem {
  MenuAction action = MenuAction::kNone;
  const char* label = "";
};

// Everything the list depends on. All of it is already known to main.cpp;
// passing it in keeps build() a pure function of its input, which is what
// makes the whole menu host-testable.
struct MenuContext {
  MenuScreen screen = MenuScreen::kHome;
  bool translation = false;        // the reader is showing a translation
  bool transfer_on = false;        // transfer mode is running
  bool page_jump = false;          // "Go to page" is active
  bool has_bookmarks = false;      // there is at least one saved place
  bool bookmark_selected = false;  // the bookmarks list has a row under it
  // The library is two levels: a list of categories, and the books inside
  // one. OK opens whichever row is highlighted either way, so the row has to
  // say which -- and inside a category there is a level to go back up to.
  bool library_in_category = false;
  bool table_clock = false;        // app::kTableClockMode: the device never sleeps
};

struct OptionsMenuState {
  MenuItem items[kOptionsMaxItems];
  uint8_t count = 0;
  uint8_t selected = 0;
  const char* title = "Options";
  // A confirmation list (Factory reset). Rendered with the question as the
  // subtitle; kept separate so an accidental OK on a destructive row cannot
  // be the last thing between the user and a wiped device.
  bool confirming = false;
  const char* question = "";
  bool touch_hint = false;         // show "TAP" in the footer
};

class OptionsMenu {
 public:
  // Fills `state` with the actions `context` allows. Never empty: every list
  // ends with something that closes the menu.
  static void build(const MenuContext& context, OptionsMenuState* state);

  // Replaces the list with the confirmation for `action`. Only destructive
  // actions have one; anything else leaves the state untouched and returns
  // false (the caller then just runs it).
  static bool buildConfirm(MenuAction action, OptionsMenuState* state);

  // Moves the highlight by `delta` rows, wrapping at both ends -- a wheel
  // with no stops should not have a list with stops.
  static void move(OptionsMenuState* state, int16_t delta);

  static MenuAction selectedAction(const OptionsMenuState& state);

  // Which row a touch at (x, y) is on, or -1 for none. Display pixels.
  static int rowAt(const OptionsMenuState& state, int16_t x, int16_t y);

  static void render(gfx::Canvas& canvas, const OptionsMenuState& state);
};

}  // namespace ui
