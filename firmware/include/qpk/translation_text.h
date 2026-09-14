// translation_text.h -- a run of translation (or tafsir) ayahs as readable text.
//
// The device's translation reader shows one surah at a time through the
// ordinary text reader, so it needs the surah's verses as plain text:
// "1. ...\n\n2. ...", numbered from 1 within the run, folded to the ASCII
// the 5x7 font draws (util::FoldToAscii). Kept here, out of main.cpp, so the
// host tests can run it against a real package.

#pragma once

#include <stdint.h>

#include "qpk/qpk_reader.h"

namespace qpk {

// Appends ayahs [first_ayah_index, first_ayah_index + count) of `reader`'s
// TRANSLATION_INDEX to `out`, never writing more than `capacity` bytes (a
// verse that does not fit is cut, and nothing after it is written). Stops at
// the first ayah that cannot be read. Returns the bytes written; `out` is
// not NUL-terminated.
uint32_t AppendTranslationVerses(const Reader& reader, uint32_t first_ayah_index, uint32_t count,
                                 char* out, uint32_t capacity);

}  // namespace qpk
