#include "ui/bookmarks_screen.h"

#include <stdio.h>

namespace ui {
namespace {

constexpr uint8_t kTitleScale = 3;
constexpr uint8_t kBodyScale = 2;
constexpr int kCharW = 12;  // (5 + 1) * kBodyScale

constexpr int kRuleX = 24;
constexpr int kRuleW = 752;
constexpr int kHeaderRuleY = 72;
constexpr int kFooterRuleY = 446;
constexpr int kFooterTextY = 454;
constexpr int kRowLeftX = 30;
constexpr int kRowPlaceX = 520;
constexpr int kRowTop = 84;
constexpr int kRowStep = 28;

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale, gfx::kBlack);
}

// Cuts `text` to `max_px` wide at kBodyScale, ending in ".." when it was cut.
void Fit(const char* text, int max_px, char* out, size_t capacity) {
  if (gfx::Canvas::textWidth(text, kBodyScale) <= max_px) {
    snprintf(out, capacity, "%s", text);
    return;
  }
  int keep = (max_px - 2 * kCharW) / kCharW;
  if (keep < 0) keep = 0;
  snprintf(out, capacity, "%.*s..", keep, text);
}

}  // namespace

int32_t BookmarksScreen::rowAt(const BookmarksState& state, int16_t x, int16_t y) {
  const uint16_t total = rowCount(state);
  if (total == 0) return -1;
  if (x < kRowLeftX - 14 || x >= kRuleX + kRuleW) return -1;
  if (y < kRowTop || y >= kRowTop + kBookmarksVisibleRows * kRowStep) return -1;
  const int32_t line = (y - kRowTop) / kRowStep;
  const int32_t row = static_cast<int32_t>(scrollTop(state)) + line;
  if (row < 0 || row >= static_cast<int32_t>(total)) return -1;
  return row;
}

uint16_t BookmarksScreen::rowCount(const BookmarksState& state) {
  const uint16_t marks = state.bookmarks != nullptr ? state.bookmarks->count() : 0;
  return static_cast<uint16_t>(marks + (state.has_last_read ? 1 : 0));
}

BookmarkRow BookmarksScreen::rowKind(const BookmarksState& state, uint16_t row,
                                     uint16_t* bookmark_index) {
  if (bookmark_index != nullptr) *bookmark_index = 0;
  if (row >= rowCount(state)) return BookmarkRow::kNone;
  if (state.has_last_read) {
    if (row == 0) return BookmarkRow::kLastRead;
    --row;
  }
  if (bookmark_index != nullptr) *bookmark_index = row;
  return BookmarkRow::kBookmark;
}

uint16_t BookmarksScreen::scrollTop(const BookmarksState& state) {
  return state.selected < kBookmarksVisibleRows
             ? 0
             : static_cast<uint16_t>(state.selected - kBookmarksVisibleRows + 1);
}

void BookmarksScreen::describe(const net::Bookmark& mark, char* out, uint32_t capacity) {
  switch (mark.kind) {
    case net::BookmarkKind::kQuran:
      snprintf(out, capacity, "Surah %u, screen %u", static_cast<unsigned>(mark.a),
               static_cast<unsigned>(mark.c));
      return;
    case net::BookmarkKind::kPageBook:
      snprintf(out, capacity, "page %u", static_cast<unsigned>(mark.a + 1));
      return;
    case net::BookmarkKind::kTextBook:
      // b is the surah for a translation or tafsir, 0 for an ordinary book.
      if (mark.b > 0) {
        snprintf(out, capacity, "Surah %u, page %u", static_cast<unsigned>(mark.b),
                 static_cast<unsigned>(mark.a));
      } else {
        snprintf(out, capacity, "page %u", static_cast<unsigned>(mark.a));
      }
      return;
  }
  snprintf(out, capacity, "?");
}

void BookmarksScreen::render(gfx::Canvas& canvas, const BookmarksState& state) {
  char buf[96];
  canvas.clear(gfx::kWhite);
  canvas.drawRect(4, 4, 792, 472, gfx::kBlack);

  CenterText(canvas, 14, "Bookmarks", kTitleScale);
  const uint16_t marks = state.bookmarks != nullptr ? state.bookmarks->count() : 0;
  if (marks == 0) {
    snprintf(buf, sizeof(buf), "None yet -- hold OK on a page to add one");
  } else {
    snprintf(buf, sizeof(buf), "%u saved", static_cast<unsigned>(marks));
  }
  CenterText(canvas, 48, buf, kBodyScale);
  canvas.drawHLine(kRuleX, kHeaderRuleY, kRuleW, gfx::kBlack);

  const uint16_t total = rowCount(state);
  const uint16_t top = scrollTop(state);
  for (uint8_t line = 0; line < kBookmarksVisibleRows; ++line) {
    const uint16_t row = static_cast<uint16_t>(top + line);
    if (row >= total) break;
    const int y = kRowTop + line * kRowStep;
    canvas.drawText(kRowLeftX - 14, y, row == state.selected ? ">" : " ", kBodyScale, gfx::kBlack);

    uint16_t index = 0;
    const BookmarkRow kind = rowKind(state, row, &index);
    const net::Bookmark* mark = kind == BookmarkRow::kLastRead ? &state.last_read
                                                               : state.bookmarks->at(index);
    if (mark == nullptr) continue;

    const net::LibraryEntry* entry =
        state.index != nullptr ? state.index->find(mark->content_id) : nullptr;
    const char* title = entry != nullptr ? entry->title : "(book no longer on the card)";
    char label[96];
    if (kind == BookmarkRow::kLastRead) {
      snprintf(label, sizeof(label), "Continue: %.80s", title);
    } else {
      snprintf(label, sizeof(label), "%.80s", title);
    }
    char fitted[96];
    Fit(label, kRowPlaceX - kRowLeftX - 16, fitted, sizeof(fitted));
    canvas.drawText(kRowLeftX, y, fitted, kBodyScale, gfx::kBlack);

    char place[40];
    describe(*mark, place, sizeof(place));
    canvas.drawText(kRowPlaceX, y, place, kBodyScale, gfx::kBlack);
  }

  canvas.drawHLine(kRuleX, kFooterRuleY, kRuleW, gfx::kBlack);
  if (state.status_message != nullptr && state.status_message[0] != 0) {
    CenterText(canvas, kFooterTextY, state.status_message, kBodyScale);
  } else {
    CenterText(canvas, kFooterTextY, "WHEEL=move OK=open  hold OK=delete  EXIT=back", kBodyScale);
  }
}

}  // namespace ui
