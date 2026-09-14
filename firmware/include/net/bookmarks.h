// bookmarks.h -- /USER/bookmarks.bin: places the reader chose to keep.
//
// architecture.md 6 reserves /USER/bookmarks.bin beside progress.bin. Keyed on
// content_id like every other reading record, so a book that is re-uploaded
// or moved on the card keeps its bookmarks. One file, rewritten whole via
// .tmp-then-rename with a CRC, like progress.bin: bookmarks change rarely and
// a torn write must never lose all of them.
//
// A bookmark's position is kind-specific:
//   kQuran      a = surah_id, b = first_ayah_index, c = screen_number
//   kPageBook   a = page (0-based picture page)
//   kTextBook   a = page (1-based TextPager page at the size saved in c),
//               b = surah for a translation or tafsir, 0 for a book

#pragma once

#include <stdint.h>

#include "hal/storage.h"

namespace net {

constexpr uint16_t kMaxBookmarks = 64;

enum class BookmarkKind : uint8_t {
  kQuran = 1,
  kPageBook = 2,
  kTextBook = 3,
};

struct Bookmark {
  uint8_t content_id[16] = {0};
  BookmarkKind kind = BookmarkKind::kPageBook;
  uint32_t a = 0;
  uint32_t b = 0;
  uint32_t c = 0;
  uint32_t created_unix = 0;  // 0 when the clock was not synced
};

class Bookmarks {
 public:
  /** Loads the file; an absent or damaged file is an empty list. */
  void load(hal::IStorage* storage);
  bool save(hal::IStorage* storage) const;

  uint16_t count() const { return count_; }
  const Bookmark* at(uint16_t index) const;

  /**
   * Adds `bookmark` at the front (newest first). An identical place (same
   * book, kind and position) is moved to the front rather than duplicated.
   * When full, the oldest is dropped.
   */
  void add(const Bookmark& bookmark);

  bool removeAt(uint16_t index);

  /** True if this exact place is bookmarked. */
  bool contains(const Bookmark& bookmark) const;

 private:
  Bookmark items_[kMaxBookmarks];
  uint16_t count_ = 0;
};

}  // namespace net
