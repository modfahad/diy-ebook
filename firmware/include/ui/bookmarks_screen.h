// bookmarks_screen.h -- the Bookmarks tile: where reading left off, then the
// places the reader saved.
//
// The first row is "Continue", the last place read (from pages.bin or
// progress.bin), when there is one; the rest are net::Bookmarks, newest
// first. Pure render, like every ui:: screen: main.cpp owns the lists and
// the selection, this draws them.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "net/bookmarks.h"
#include "net/library_index.h"

namespace ui {

constexpr uint8_t kBookmarksVisibleRows = 12;

enum class BookmarkRow : uint8_t {
  kLastRead,
  kBookmark,
  kNone,
};

struct BookmarksState {
  const net::LibraryIndex* index = nullptr;
  const net::Bookmarks* bookmarks = nullptr;

  bool has_last_read = false;
  net::Bookmark last_read;

  uint16_t selected = 0;
  const char* status_message = "";
};

class BookmarksScreen {
 public:
  static void render(gfx::Canvas& canvas, const BookmarksState& state);

  static uint16_t rowCount(const BookmarksState& state);

  /** What row `row` is; for kBookmark, its index into state.bookmarks. */
  static BookmarkRow rowKind(const BookmarksState& state, uint16_t row, uint16_t* bookmark_index);

  /** The first row drawn, so that `selected` is always on screen. */
  static uint16_t scrollTop(const BookmarksState& state);

  /** "page 212", "Surah 18, screen 3" -- the place, without the book. */
  static void describe(const net::Bookmark& mark, char* out, uint32_t capacity);
};

}  // namespace ui
