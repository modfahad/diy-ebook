// book_reader.h -- loads a BOOK package's text for util::TextPager.
//
// The desktop converter pre-wraps a book's text at a device-independent
// 72-column reference grid (tools note: see
// desktop/converter/src/sources/paginate.ts's DEFAULT_GRID, a 768x768
// square canvas unrelated to this panel's actual 792x272 strip), one
// TEXT_DATA record per desktop-computed page, each holding that page's
// lines joined by real '\n' bytes. Re-showing those bytes as-is would
// double-wrap: this panel's real character width is not 72 columns, so the
// desktop's line breaks land at the wrong place on real hardware.
//
// LoadBookText undoes that: each record's internal '\n's become spaces
// (rejoining the desktop's wrapped lines into one line per page, since
// wrapText() only breaks at word boundaries, so no bytes are lost -- only
// the desktop's incidental line breaks), and a blank line goes between
// records so util::TextPager sees each desktop page as its own paragraph.
//
// A real, distinct gap this does NOT fix: buildBookPackage's
// paginateBlocks only inserts a blank-line spacer around a *heading*
// block, not between consecutive body paragraphs (see paginate.ts) -- so
// several plain paragraphs that landed on the same desktop page read back
// here as one run-on block. Fixing that means changing what the desktop
// pipeline stores, not this loader.

#pragma once

#include <stdint.h>

#include "qpk/qpk_reader.h"

namespace qpk {

enum class BookLoadError : uint8_t {
  kOk = 0,
  kNotABook,     // header.package_type != kBook
  kReaderError,  // a record or text read failed
  kTruncated,    // more text existed than `capacity` could hold
};

// `out` is always NUL-terminated on return, even for kTruncated -- callers
// can page through whatever fit rather than showing nothing.
BookLoadError LoadBookText(Reader* reader, char* out, uint32_t capacity,
                           uint32_t* out_length);

}  // namespace qpk
