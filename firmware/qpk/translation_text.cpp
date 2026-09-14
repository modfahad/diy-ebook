#include "qpk/translation_text.h"

#include <stdio.h>
#include <string.h>

#include "util/ascii_fold.h"

namespace qpk {
namespace {

// Static rather than on the stack: one ayah's text can run to a few KB.
char g_raw[4096];
char g_folded[4096];

}  // namespace

uint32_t AppendTranslationVerses(const Reader& reader, uint32_t first_ayah_index, uint32_t count,
                                 char* out, uint32_t capacity) {
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
  for (uint32_t i = 0; i < count; ++i) {
    TranslationRecord record;
    if (reader.getTranslation(first_ayah_index + i, &record) != Error::kOk) break;
    const int32_t got =
        reader.readTranslationText(record.text_offset, record.text_length, g_raw, sizeof(g_raw));
    if (got < 0) break;
    const uint32_t folded =
        util::FoldToAscii(g_raw, static_cast<uint32_t>(got), g_folded, sizeof(g_folded));
    const int n = snprintf(number, sizeof(number), "%u. ", static_cast<unsigned>(i + 1));
    if (!put(number, n > 0 ? static_cast<uint32_t>(n) : 0) || !put(g_folded, folded) ||
        !put("\n\n", 2)) {
      break;
    }
  }
  return length;
}

}  // namespace qpk
