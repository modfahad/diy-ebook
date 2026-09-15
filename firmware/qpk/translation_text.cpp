#include "qpk/translation_text.h"

#include <stdio.h>
#include <string.h>

#include "qpk/byte_order.h"
#include "util/ascii_fold.h"

namespace qpk {
namespace {

// Static rather than on the stack: one ayah's text can run to a few KB.
char g_raw[4096];
char g_folded[4096];

// TRANSLATION_INDEX records read per batch: 512 x 8 bytes. Al-Baqarah's 286
// fit in one.
constexpr uint32_t kRecordBatch = 512;
uint8_t g_records[kRecordBatch * kTranslationRecordSize];

}  // namespace

uint32_t AppendTranslationVerses(const Reader& reader, uint32_t first_ayah_index, uint32_t count,
                                 char* out, uint32_t capacity, char* scratch,
                                 uint32_t scratch_capacity) {
  if (out == nullptr || capacity == 0) return 0;
  uint32_t length = 0;
  // False once `out` is full: the caller's buffer is the limit, not an error.
  const auto put = [&](const char* text, uint32_t n) {
    const uint32_t room = capacity - length;
    const bool fits = n <= room;
    const uint32_t take = fits ? n : room;
    memcpy(out + length, text, take);
    length += take;
    return fits;
  };

  char number[16];
  uint32_t done = 0;
  while (done < count) {
    const uint32_t want = count - done < kRecordBatch ? count - done : kRecordBatch;
    const uint32_t got = reader.readRecordRange(SectionId::kTranslationIndex,
                                                first_ayah_index + done, want, g_records,
                                                sizeof(g_records));
    if (got == 0) break;

    // The stretch of TRANSLATION_DATA this batch points into, read once when
    // it fits the scratch buffer.
    uint32_t span_start = 0xFFFFFFFFu;
    uint32_t span_end = 0;
    for (uint32_t i = 0; i < got; ++i) {
      const uint8_t* r = g_records + i * kTranslationRecordSize;
      const uint32_t offset = Read32(r);
      const uint32_t end = offset + Read32(r + 4);
      if (end < offset) continue;  // overflowing record: the per-verse read rejects it
      if (offset < span_start) span_start = offset;
      if (end > span_end) span_end = end;
    }
    bool have_span = false;
    if (scratch != nullptr && span_end > span_start && span_end - span_start <= scratch_capacity) {
      const uint32_t span = span_end - span_start;
      have_span = reader.readTranslationText(span_start, span, scratch, scratch_capacity) ==
                  static_cast<int32_t>(span);
    }

    for (uint32_t i = 0; i < got; ++i) {
      const uint8_t* r = g_records + i * kTranslationRecordSize;
      const uint32_t offset = Read32(r);
      const uint32_t text_length = Read32(r + 4);
      const char* text = nullptr;
      uint32_t text_bytes = 0;
      if (have_span && offset >= span_start && offset + text_length <= span_end &&
          offset + text_length >= offset) {
        text = scratch + (offset - span_start);
        // The same cap the per-verse read has.
        text_bytes = text_length < sizeof(g_raw) ? text_length : sizeof(g_raw);
      } else {
        const int32_t read = reader.readTranslationText(offset, text_length, g_raw, sizeof(g_raw));
        if (read < 0) return length;
        text = g_raw;
        text_bytes = static_cast<uint32_t>(read);
      }
      const uint32_t folded = util::FoldToAscii(text, text_bytes, g_folded, sizeof(g_folded));
      const int n = snprintf(number, sizeof(number), "%u. ", static_cast<unsigned>(done + i + 1));
      if (!put(number, n > 0 ? static_cast<uint32_t>(n) : 0) || !put(g_folded, folded) ||
          !put("\n\n", 2)) {
        return length;
      }
    }
    done += got;
    if (got < want) break;  // the section ended
  }
  return length;
}

}  // namespace qpk
