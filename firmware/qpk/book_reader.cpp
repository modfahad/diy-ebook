#include "qpk/book_reader.h"

namespace qpk {

BookLoadError LoadBookText(Reader* reader, char* out, uint32_t capacity,
                           uint32_t* out_length) {
  *out_length = 0;
  if (capacity == 0) return BookLoadError::kTruncated;
  out[0] = 0;

  if (reader == nullptr || !reader->isOpen()) return BookLoadError::kReaderError;
  if (reader->type() != PackageType::kBook) return BookLoadError::kNotABook;

  const uint32_t record_count = reader->recordCount(SectionId::kTextIndex);
  uint32_t written = 0;
  bool truncated = false;

  for (uint32_t i = 0; i < record_count && !truncated; ++i) {
    TextRecord rec;
    if (reader->getTextRecord(i, &rec) != Error::kOk) {
      return BookLoadError::kReaderError;
    }

    // A blank line between desktop pages -- see the header comment for why
    // that's the only structural break this format actually preserves.
    if (i > 0) {
      if (written + 2 >= capacity) {
        truncated = true;
        break;
      }
      out[written++] = '\n';
      out[written++] = '\n';
    }

    if (written + 1 >= capacity) {
      truncated = true;
      break;
    }
    const uint32_t space = capacity - written - 1;  // room for the NUL
    const int32_t n =
        reader->readText(rec.text_offset, rec.text_length, out + written, space);
    if (n < 0) return BookLoadError::kReaderError;

    // Undo the desktop's device-independent hard wrap (see the header
    // comment) so util::TextPager re-flows for this panel's real width
    // instead of double-wrapping already-wrapped lines.
    for (int32_t j = 0; j < n; ++j) {
      if (out[written + j] == '\n') out[written + j] = ' ';
    }
    written += static_cast<uint32_t>(n);
    if (static_cast<uint32_t>(n) < rec.text_length) truncated = true;
  }

  out[written] = 0;
  *out_length = written;
  return truncated ? BookLoadError::kTruncated : BookLoadError::kOk;
}

}  // namespace qpk
