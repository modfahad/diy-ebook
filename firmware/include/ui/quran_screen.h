// quran_screen.h -- Milestone 5's Quran reader.
//
// Draws real Quranic text on the panel by blitting pre-shaped glyph runs out
// of a QURAN package's atlas. Nothing here shapes Arabic: HarfBuzz ran on the
// desktop (tools/arabic-pager/shape_glyph_atlas.py) and the result is in the
// package's FONT_METADATA + ASSETS. This walks AYAH_INDEX -> WORD_INDEX ->
// ASSETS and lays the runs out right-to-left.
//
// WHY THIS IS NOT A PURE RENDERER, unlike SelfTestScreen/LibraryScreen/
// ReaderScreen: every glyph is an SD read (qpk::Reader::readAsset per glyph,
// via qpk::BlitGlyphRun). The bytes cannot be gathered into a state struct
// first without buffering an entire page of glyph bitmaps in RAM, which is
// exactly what the format's streaming design exists to avoid. So this takes
// the Reader and does I/O while drawing, the same way qpk::BlitGlyphRun
// already does.
//
// It requires a package with FLAG_SHAPED_TEXT_DATA. A plain text QURAN
// package carries UTF-8 Arabic in TEXT_DATA and the device has no Arabic
// font to draw that with -- gfx::font5x7 is a 5x7 ASCII face. render() says
// so on screen rather than drawing nothing.

#pragma once

#include <stdint.h>

#include "gfx/canvas.h"
#include "qpk/qpk_reader.h"

namespace ui {

/** Ayahs whose start positions are remembered for backward paging. */
constexpr uint8_t kQuranHistoryDepth = 32;

struct QuranState {
  /** The opened package. Must carry FLAG_SHAPED_TEXT_DATA to render. */
  qpk::Reader* reader = nullptr;

  uint16_t surah_id = 1;
  /** Index into AYAH_INDEX of the first ayah shown. */
  uint32_t first_ayah_index = 0;

  /**
   * Index into LAYOUT_DATA of the first mushaf line shown.
   *
   * Only meaningful when the package carries LAYOUT_DATA. It is the paging
   * cursor for the layout path, the way first_ayah_index is for the
   * fill-and-wrap path -- and it is derived from first_ayah_index, never
   * persisted, so net::ReadingProgress's on-disk format (hardware-confirmed
   * 2026-09-01) does not change. See QuranScreen::seekToAyah().
   */
  uint32_t first_line_index = 0;

  /** Surah name, copied out of TEXT_DATA once when the surah is opened. */
  char surah_name[64] = {0};
  uint16_t ayah_count = 0;
  /** 1-based position of this screen within the surah, for the footer. */
  uint16_t screen_number = 1;

  const char* status_message = "";
};

/** What render() managed to do -- main.cpp uses this to drive paging. */
struct QuranRenderResult {
  /** AYAH_INDEX index one past the last ayah that fit. */
  uint32_t next_ayah_index = 0;
  /**
   * Ayahs actually drawn.
   *
   * On the fill-and-wrap path, 0 means nothing fit, so do not advance. On the
   * layout path it counts ayahs that ENDED on this screen, so 0 is ordinary
   * -- a screen in the middle of a long ayah closes none. Use lines_drawn
   * there instead.
   */
  uint16_t ayahs_drawn = 0;
  /** Glyphs blitted -- a cheap sanity number for the serial log. */
  uint32_t glyphs_drawn = 0;
  /** True when the package cannot be rendered (no shaped atlas). */
  bool unsupported = false;

  /** True when this screen was laid out from LAYOUT_DATA, not fill-and-wrap. */
  bool used_layout = false;
  /** LAYOUT_DATA index the next screen starts at. Only set with used_layout. */
  uint32_t next_line_index = 0;
  /** Mushaf lines drawn. 0 with used_layout means nothing fit. */
  uint16_t lines_drawn = 0;
  /**
   * Surah of the first ayah on this screen.
   *
   * The layout path follows the mushaf's own flow, which runs straight
   * through a surah boundary -- so the surah on screen is a RESULT of where
   * the lines led, not an input the caller chose. The fill-and-wrap path
   * stops at the boundary and reports state.surah_id unchanged.
   */
  uint16_t first_surah_id = 0;
};

class QuranScreen {
 public:
  /**
   * Draws as many ayahs as fit, starting at `state.first_ayah_index`.
   *
   * Returns where the next screen should start. Laying out forward from a
   * known ayah is the only direction that is cheap: line breaks depend on
   * measured glyph widths, so there is no way to know where the *previous*
   * screen began without laying it out too. main.cpp keeps a small stack of
   * start indices instead.
   */
  static QuranRenderResult render(gfx::Canvas& canvas, const QuranState& state);

  /** Fills surah_name/ayah_count from the package. False if the id is absent. */
  static bool loadSurah(QuranState* state, uint16_t surah_id);

  /** True when the package can be laid out from LAYOUT_DATA. */
  static bool hasLayout(const QuranState& state);

  /**
   * Points `first_line_index` at the mushaf line holding `first_ayah_index`.
   *
   * The bridge between an ayah-addressed reading position -- what the surah
   * picker chooses and what net::ReadingProgress stores -- and the line
   * cursor the layout path pages with. A no-op returning false when the
   * package has no LAYOUT_DATA, which is what selects fill-and-wrap.
   */
  static bool seekToAyah(QuranState* state, uint32_t ayah_index);
};

}  // namespace ui
