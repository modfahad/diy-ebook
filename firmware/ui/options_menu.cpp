#include "ui/options_menu.h"

#include <stdio.h>

namespace ui {

namespace {

constexpr uint8_t kTitleScale = 3;
constexpr uint8_t kBodyScale = 2;

// Layout for the 800x480 panel, same border and rules as every other screen.
constexpr int kBorderX = 4;
constexpr int kBorderY = 4;
constexpr int kBorderW = 792;
constexpr int kBorderH = 472;

constexpr int kRuleX = 24;
constexpr int kRuleW = 752;
constexpr int kHeaderRuleY = 72;
constexpr int kFooterRuleY = 446;
constexpr int kFooterTextY = 454;

// Rows are the full width of the rules so a finger has the whole line to aim
// at, not just the text.
constexpr int kRowX = kRuleX;
constexpr int kRowW = kRuleW;
constexpr int kRowTop = 84;
constexpr int kRowStep = 40;          // 8 * 40 = 320; 84 + 320 = 404 < 446
constexpr int kRowTextInset = 22;     // past the ">" marker
constexpr int kRowTextDrop = 12;      // text baseline inside the 40 px row

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale,
                  gfx::kBlack);
}

void Add(OptionsMenuState* state, MenuAction action, const char* label) {
  if (state->count >= kOptionsMaxItems) return;
  state->items[state->count].action = action;
  state->items[state->count].label = label;
  ++state->count;
}

// "Sleep" is not offered on a device that never sleeps (table-clock mode);
// "Close this menu" always is, so no list is ever a dead end. The library's
// fullest list already reaches kOptionsMaxItems exactly, so when the list is
// full the last row is REPLACED rather than dropped: losing an action off the
// end is recoverable, losing the way out is not.
void AddTail(OptionsMenuState* state, const MenuContext& context) {
  if (!context.table_clock) Add(state, MenuAction::kSleep, "Sleep now");
  if (state->count >= kOptionsMaxItems) {
    state->items[kOptionsMaxItems - 1].action = MenuAction::kBack;
    state->items[kOptionsMaxItems - 1].label = "Close this menu";
    return;
  }
  Add(state, MenuAction::kBack, "Close this menu");
}

const char* TransferLabel(const MenuContext& context) {
  return context.transfer_on ? "Stop transfer mode" : "Transfer mode (Wi-Fi)";
}

}  // namespace

void OptionsMenu::build(const MenuContext& context, OptionsMenuState* state) {
  if (state == nullptr) return;
  *state = OptionsMenuState();

  switch (context.screen) {
    case MenuScreen::kHome:
      state->title = "Options";
      Add(state, MenuAction::kOpenLibrary, "Library");
      if (context.has_bookmarks) {
        Add(state, MenuAction::kContinueReading, "Continue reading");
      }
      Add(state, MenuAction::kTransferMode, TransferLabel(context));
      Add(state, MenuAction::kRedraw, "Redraw the screen");
      break;

    case MenuScreen::kLibrary:
      state->title = "Library options";
      Add(state, MenuAction::kOpenSelected,
          context.library_in_category ? "Open this book" : "Open this shelf");
      if (context.library_in_category) {
        Add(state, MenuAction::kLeaveCategory, "Back to all shelves");
      }
      if (context.has_bookmarks) {
        Add(state, MenuAction::kContinueReading, "Continue reading");
        Add(state, MenuAction::kBookmarks, "Saved places");
      }
      Add(state, MenuAction::kTransferMode, TransferLabel(context));
      Add(state, MenuAction::kSelfTest, "Hardware test");
      break;

    case MenuScreen::kSelfTest:
      state->title = "Hardware test options";
      Add(state, MenuAction::kTransferMode, TransferLabel(context));
      Add(state, MenuAction::kOpenLibrary, "Library");
      Add(state, MenuAction::kFactoryReset, "Factory reset");
      break;

    case MenuScreen::kReader:
      state->title = context.translation ? "Translation options" : "Book options";
      Add(state, MenuAction::kTextSize, "Text size");
      if (context.translation) {
        Add(state, MenuAction::kChooseSurah, "Choose surah");
      }
      Add(state, MenuAction::kBookmarkHere, "Save this place");
      if (context.has_bookmarks) {
        Add(state, MenuAction::kBookmarks, "Saved places");
      }
      Add(state, MenuAction::kCloseBook, "Close this book");
      break;

    case MenuScreen::kPages:
      state->title = "Book options";
      if (context.page_jump) {
        Add(state, MenuAction::kNextChapter, "Jump to next chapter");
        Add(state, MenuAction::kGoToPage, "Stop going to a page");
      } else {
        Add(state, MenuAction::kGoToPage, "Go to page");
      }
      Add(state, MenuAction::kBookmarkHere, "Save this place");
      if (context.has_bookmarks) {
        Add(state, MenuAction::kBookmarks, "Saved places");
      }
      Add(state, MenuAction::kCloseBook, "Close this book");
      break;

    case MenuScreen::kQuran:
      state->title = "Quran options";
      Add(state, MenuAction::kBookmarkHere, "Save this place");
      Add(state, MenuAction::kChooseSurah, "Choose surah");
      if (context.has_bookmarks) {
        Add(state, MenuAction::kBookmarks, "Saved places");
      }
      Add(state, MenuAction::kCloseBook, "Close the Quran");
      break;

    case MenuScreen::kSurahPicker:
      state->title = "Surah list options";
      Add(state, MenuAction::kOpenSelected, "Open this surah");
      Add(state, MenuAction::kCloseBook, "Close");
      break;

    case MenuScreen::kBookmarks:
      state->title = "Saved places options";
      if (context.bookmark_selected) {
        Add(state, MenuAction::kOpenSelected, "Open this place");
        Add(state, MenuAction::kDeleteBookmark, "Delete this place");
      }
      Add(state, MenuAction::kOpenLibrary, "Library");
      break;
  }

  AddTail(state, context);
}

bool OptionsMenu::buildConfirm(MenuAction action, OptionsMenuState* state) {
  if (state == nullptr) return false;
  if (action != MenuAction::kFactoryReset) return false;

  *state = OptionsMenuState();
  state->title = "Factory reset";
  state->question =
      "Erase saved places, Wi-Fi and settings? Books on the card stay.";
  state->confirming = true;
  Add(state, MenuAction::kBack, "No, keep everything");
  Add(state, MenuAction::kFactoryResetConfirm, "Yes, erase");
  // The destructive row is never the one under the highlight when the list
  // opens: choosing it has to be deliberate.
  state->selected = 0;
  return true;
}

void OptionsMenu::move(OptionsMenuState* state, int16_t delta) {
  if (state == nullptr || state->count == 0) return;
  int32_t index = static_cast<int32_t>(state->selected) + delta;
  const int32_t count = static_cast<int32_t>(state->count);
  index %= count;
  if (index < 0) index += count;
  state->selected = static_cast<uint8_t>(index);
}

MenuAction OptionsMenu::selectedAction(const OptionsMenuState& state) {
  if (state.count == 0 || state.selected >= state.count) return MenuAction::kNone;
  return state.items[state.selected].action;
}

int OptionsMenu::rowAt(const OptionsMenuState& state, int16_t x, int16_t y) {
  if (state.count == 0) return -1;
  if (x < kRowX || x >= kRowX + kRowW) return -1;
  if (y < kRowTop) return -1;
  const int row = (y - kRowTop) / kRowStep;
  if (row < 0 || row >= static_cast<int>(state.count)) return -1;
  return row;
}

void OptionsMenu::render(gfx::Canvas& canvas, const OptionsMenuState& state) {
  canvas.clear(gfx::kWhite);
  canvas.drawRect(kBorderX, kBorderY, kBorderW, kBorderH, gfx::kBlack);

  CenterText(canvas, 14, state.title, kTitleScale);
  if (state.confirming && state.question[0] != '\0') {
    CenterText(canvas, 48, state.question, kBodyScale);
  }
  canvas.drawHLine(kRuleX, kHeaderRuleY, kRuleW, gfx::kBlack);

  for (uint8_t row = 0; row < state.count; ++row) {
    const int y = kRowTop + row * kRowStep;
    const bool chosen = (row == state.selected);
    if (chosen) {
      // A box, not inverted text: the 5x7 face has no white-on-black mode,
      // and a box survives the panel's ghosting better than a filled row.
      canvas.drawRect(kRowX, y, kRowW, kRowStep - 4, gfx::kBlack);
      canvas.drawRect(kRowX + 1, y + 1, kRowW - 2, kRowStep - 6, gfx::kBlack);
    }
    canvas.drawText(kRowX + 6, y + kRowTextDrop, chosen ? ">" : " ", kBodyScale,
                    gfx::kBlack);
    canvas.drawText(kRowX + kRowTextInset, y + kRowTextDrop, state.items[row].label,
                    kBodyScale, gfx::kBlack);
  }

  canvas.drawHLine(kRuleX, kFooterRuleY, kRuleW, gfx::kBlack);
  CenterText(canvas, kFooterTextY,
             state.touch_hint ? "TAP a line, or WHEEL=move OK=choose EXIT=close"
                              : "WHEEL=move OK=choose EXIT=close",
             kBodyScale);
}

}  // namespace ui
