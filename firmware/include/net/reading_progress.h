// reading_progress.h -- /USER/progress.bin: where the Quran reader left off.
//
// architecture.md section 6 puts reading progress at /USER/progress.bin, not
// /DEVICE/state.bin -- separate from device state so a package can be
// replaced without touching it, and so a backup is a small, well-defined set
// of files. Keyed on content_id, not a library index position or a path:
// net::LibraryIndex::rebuild()/prune() can renumber or drop-and-reinsert
// entries at any boot, and a repacked package keeps the same content_id.
//
// Same LIB1-style discipline as net::LibraryIndex::save() -- magic, version,
// a CRC over the record, .tmp-then-rename so a power loss never leaves a
// half-written file -- because this is written far more often (every EXIT
// from the reader, see main.cpp), so the atomicity matters at least as much.
//
// One record only: the device remembers one reading position, not a history
// or per-book bookmarks (that is architecture.md's separate, unbuilt
// /USER/bookmarks.bin). Opening a different Quran package overwrites it -- a
// deliberate simplification, not an oversight.

#pragma once

#include <stdint.h>

#include "hal/storage.h"

namespace net {

struct ReadingProgress {
  uint8_t content_id[16] = {0};
  uint16_t surah_id = 0;
  uint32_t first_ayah_index = 0;
  uint16_t screen_number = 0;
};

/** False (and *out left default) if the file is absent, short or corrupt. */
bool LoadReadingProgress(hal::IStorage* storage, ReadingProgress* out);

/** Writes via .tmp-then-rename. */
bool SaveReadingProgress(hal::IStorage* storage, const ReadingProgress& progress);

// /USER/pages.bin: the page a picture book (docs/qpk-format.md 9d) was left
// at. A separate file from progress.bin rather than a new field in it, so
// reading a PDF never forgets where the Quran was, and progress.bin's format
// stays untouched. Same one-record, CRC, .tmp-then-rename discipline.
struct PageProgress {
  uint8_t content_id[16] = {0};
  uint32_t page = 0;  // 0-based
};

/** False (and *out left default) if the file is absent, short or corrupt. */
bool LoadPageProgress(hal::IStorage* storage, PageProgress* out);

/** Writes via .tmp-then-rename. */
bool SavePageProgress(hal::IStorage* storage, const PageProgress& progress);

}  // namespace net
