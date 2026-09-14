// surah_picker_screen.h -- jump straight to a surah instead of paging from 1.
//
// docs/pending.md's "biggest usability gap": the Quran reader used to open at
// surah 1 and only page forward, so reaching Al-Kahf took roughly 1,500
// wheel detents. This screen sits between the library browser and
// ui::QuranScreen -- OK on a Quran package (main.cpp's OpenQuran) opens the
// package and lands here instead of jumping straight into surah 1; EXIT from
// the reader comes back here rather than all the way out to the library, so
// picking a different surah does not mean re-opening the package.
//
// Pure render, like LibraryScreen/ReaderScreen: state in, canvas out. Rows
// are read live off the open qpk::Reader with getSurah() rather than caching
// every record in static RAM -- SURAH_INDEX is addressed [id-1] (see
// quran_screen.cpp's loadSurah), so each of the kSurahPickerMaxVisibleRows
// visible rows is one direct-index lookup, not a scan.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "qpk/qpk_reader.h"

namespace ui {

// 12 rows at a 28 px pitch fill the 800x480 panel, same as the library list.
constexpr uint8_t kSurahPickerMaxVisibleRows = 12;

struct SurahPickerState {
  /** Must be open and carry SURAH_INDEX; nullptr renders an empty list. */
  qpk::Reader* reader = nullptr;

  uint16_t selected = 0;    // 0-based index into SURAH_INDEX
  uint16_t scroll_top = 0;
};

class SurahPickerScreen {
 public:
  static void render(gfx::Canvas& canvas, const SurahPickerState& state);

  /** recordCount(kSurahIndex) off the reader; 0 if none is open. */
  static uint16_t rowCount(const SurahPickerState& state);
};

}  // namespace ui
