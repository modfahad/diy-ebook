// text_pager.h -- word wrapping and pagination, header-only and pure.
//
// Given a body of text, a line width in characters and a number of lines per
// page, this answers the two questions a reader needs: how many pages is
// this, and which slices of the text are on page N.
//
// It allocates nothing and copies nothing: a line is an (offset, length) into
// the caller's buffer, so the text can live in flash and never be duplicated
// into RAM. That matters on a device whose whole point is holding books.
//
// Pure logic in util/ by the rule in architecture.md section 1: no hardware
// dependency, so it is unit-tested on the host.

#pragma once

#include <stdint.h>

namespace util {

struct LineSlice {
  uint32_t offset = 0;
  uint16_t length = 0;
};

class TextPager {
 public:
  void reset(const char* text, uint32_t length, uint16_t chars_per_line,
             uint8_t lines_per_page) {
    text_ = text;
    length_ = (text == nullptr) ? 0 : length;
    chars_per_line_ = chars_per_line == 0 ? 1 : chars_per_line;
    lines_per_page_ = lines_per_page == 0 ? 1 : lines_per_page;

    uint32_t cursor = 0;
    uint32_t lines = 0;
    LineSlice slice;
    while (nextLine(&cursor, &slice)) ++lines;
    line_count_ = lines;
    page_count_ = static_cast<uint16_t>((lines + lines_per_page_ - 1) /
                                        lines_per_page_);
    if (page_count_ == 0) page_count_ = 1;
  }

  uint16_t pageCount() const { return page_count_; }
  uint32_t lineCount() const { return line_count_; }

  /**
   * Fills up to `capacity` slices for 1-based `page`. Returns how many lines
   * that page actually has, which is fewer than lines_per_page on the last
   * page.
   */
  uint8_t linesForPage(uint16_t page, LineSlice* out, uint8_t capacity) const {
    if (out == nullptr || capacity == 0 || page == 0) return 0;

    const uint32_t skip = static_cast<uint32_t>(page - 1) * lines_per_page_;
    uint32_t cursor = 0;
    LineSlice slice;
    for (uint32_t i = 0; i < skip; ++i) {
      if (!nextLine(&cursor, &slice)) return 0;
    }

    uint8_t filled = 0;
    while (filled < lines_per_page_ && filled < capacity) {
      if (!nextLine(&cursor, &slice)) break;
      out[filled++] = slice;
    }
    return filled;
  }

 private:
  /**
   * Greedy wrap. Breaks at the last space that fits; a word longer than the
   * line is hard-split rather than allowed to overflow the glass. A newline
   * ends a line, and an empty line survives as a zero-length slice so
   * paragraph spacing is preserved rather than collapsed.
   */
  bool nextLine(uint32_t* cursor, LineSlice* out) const {
    if (text_ == nullptr || *cursor >= length_) return false;

    if (text_[*cursor] == '\n') {
      out->offset = *cursor;
      out->length = 0;
      ++(*cursor);
      return true;
    }

    const uint32_t start = *cursor;
    uint32_t at = start;
    uint32_t taken = 0;
    uint32_t last_space = 0;
    bool have_space = false;

    while (at < length_ && taken < chars_per_line_) {
      const char c = text_[at];
      if (c == '\n') break;
      if (c == ' ') {
        last_space = at;
        have_space = true;
      }
      ++at;
      ++taken;
    }

    uint32_t end;
    if (at >= length_ || text_[at] == '\n' || text_[at] == ' ') {
      // The line ended on a natural boundary -- end of text, a newline, or
      // exactly on a space. That last case matters: backing off to the
      // previous space here would push a word that fits perfectly onto the
      // next line, wasting up to a whole word of every line.
      end = at;
    } else if (have_space && last_space > start) {
      end = last_space;               // break at the last space that fits
    } else {
      end = at;                       // one long word: hard-split it
    }

    uint32_t trimmed = end;
    while (trimmed > start && text_[trimmed - 1] == ' ') --trimmed;

    out->offset = start;
    out->length = static_cast<uint16_t>(trimmed - start);

    *cursor = end;
    while (*cursor < length_ && text_[*cursor] == ' ') ++(*cursor);
    if (*cursor < length_ && text_[*cursor] == '\n') ++(*cursor);
    return true;
  }

  const char* text_ = nullptr;
  uint32_t length_ = 0;
  uint16_t chars_per_line_ = 1;
  uint8_t lines_per_page_ = 1;
  uint16_t page_count_ = 1;
  uint32_t line_count_ = 0;
};

}  // namespace util
