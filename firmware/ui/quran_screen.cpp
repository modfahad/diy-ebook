#include "ui/quran_screen.h"

#include <stdio.h>
#include <string.h>

#include "app/app_config.h"
#include "gfx/font5x7.h"
#include "qpk/byte_order.h"
#include "qpk/glyph_blitter.h"
#include "qpk/qpk_format.h"

namespace ui {

namespace {

constexpr uint8_t kBodyScale = 2;

// Layout for the 800x480 panel. The atlas is rasterized at 36px
// (tools/arabic-pager/shape_glyph_atlas.py's default), so kLineHeight has to
// clear that plus the descenders harakat push below the baseline. Seven
// mushaf rows fit between the header rule and the footer rule (baselines 78
// through 390), where the 272 px panel fit four.
constexpr int kMarginX = 18;
constexpr int kHeaderY = 12;
constexpr int kRuleY = 34;
constexpr int kFirstBaseline = 78;
constexpr int kLineHeight = 52;
constexpr int kFooterRuleY = 446;
constexpr int kFooterTextY = 452;     // 14 px tall at kBodyScale -> 465
constexpr int kSpaceWidth = 10;
// The last baseline that still leaves the glyphs clear of the footer rule.
constexpr int kMaxBaseline = kFooterRuleY - 14;

// A word longer than this is not laid out. The longest word in the corpus is
// well under it; the cap exists so a corrupt WORD_INDEX cannot overrun the
// stack buffer below.
constexpr uint32_t kMaxGlyphsPerWord = 64;

// A mushaf line holds at most 21 items across the whole real corpus
// (measured over all 8,817 lines); the cap is well above that so real content
// never reaches it, and exists so a corrupt LAYOUT_DATA cannot overrun the
// per-line stack arrays in RenderLayoutPath.
constexpr uint32_t kMaxWordsPerLine = 40;

// What the real Uthmani marker glyph costs horizontally: it advances 28px at
// the atlas's 36px, the same for all 286 of them.
//
// NOT an upper bound over both paths, which an earlier version of this
// comment wrongly claimed. The Latin fallback is WIDER: gfx::Canvas::
// textWidth is n * (5 + 1) * scale - scale, so "(7)" at scale 2 is 34px and
// "(286)" is 58px. MarkerSlotWidth() below picks the right one, because a
// line measured 28px short would wrap differently than it draws.
constexpr int kMarkerGlyphAdvance = 28;

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale,
                  gfx::kBlack);
}

/**
 * Where a package's end-of-ayah marker glyphs live, resolved once per render.
 *
 * `available` false means "draw the Latin fallback" -- an older package with
 * no kAyahMarkerGlyphs key, or a malformed one. Kept as a small value rather
 * than re-read per ayah because Reader::ayahMarkerGlyphs() walks METADATA,
 * and that is an SD read this screen already does enough of.
 */
struct MarkerAtlas {
  bool available = false;
  uint16_t first_glyph_id = 0;
  uint16_t count = 0;
};

/**
 * Draws an ayah-number marker, right-aligned to `right_x`. Returns its width.
 *
 * The real Uthmani marker is one atlas glyph, not a run. KFGQPC Uthmanic
 * Script composes the whole ornate circle-with-digits from the Arabic-Indic
 * digits alone (U+0660..U+0669, no U+06DD -- prefixing U+06DD shapes to two
 * glyphs and would draw a second, empty circle over it), so the shaper mints
 * one glyph per ayah NUMBER and this resolves it by arithmetic:
 * first_glyph_id + (n - 1). See tools/arabic-pager/shape_glyph_atlas.py's
 * marker pass and docs/qpk-format.md 9b.
 *
 * The Latin "(7)" stays as the fallback rather than being deleted. It is what
 * a package built before markers existed still draws, and it is the honest
 * answer when the atlas cannot produce the glyph -- an empty gap where a
 * verse boundary should be would be worse than an obviously-Latin stand-in.
 */
/**
 * How much horizontal room this ayah's marker needs, before drawing it.
 *
 * Mirrors DrawAyahMarker's own branch: the atlas glyph when the package can
 * supply one, the Latin stand-in's text width otherwise. Measuring a line
 * with the wrong one is not cosmetic -- the measure pass decides where the
 * line wraps, and the draw pass would then wrap somewhere else.
 */
int MarkerSlotWidth(const MarkerAtlas& markers, uint16_t ayah_number) {
  if (markers.available && ayah_number >= 1 && ayah_number <= markers.count) {
    return kMarkerGlyphAdvance;
  }
  char buf[10];
  snprintf(buf, sizeof(buf), "(%u)", static_cast<unsigned>(ayah_number));
  return gfx::Canvas::textWidth(buf, kBodyScale);
}

int DrawAyahMarker(gfx::Canvas& canvas, const qpk::Reader& reader,
                   const MarkerAtlas& markers, int right_x, int baseline_y,
                   uint16_t ayah_number, uint32_t* glyphs_drawn) {
  if (markers.available && ayah_number >= 1 && ayah_number <= markers.count) {
    const uint16_t glyph_id =
        static_cast<uint16_t>(markers.first_glyph_id + ayah_number - 1);
    int width = 0;
    if (qpk::MeasureGlyphRun(reader, &glyph_id, 1, &width) ==
            qpk::BlitError::kOk &&
        width > 0 && right_x - width >= 0 &&
        qpk::BlitGlyphRun(reader, &canvas, &glyph_id, 1, right_x - width,
                          baseline_y, nullptr) == qpk::BlitError::kOk) {
      if (glyphs_drawn != nullptr) ++*glyphs_drawn;
      return width;
    }
    // Fall through: a package that claims markers but cannot draw this one
    // gets the Latin stand-in for that ayah, not a hole in the line.
  }

  char buf[10];
  snprintf(buf, sizeof(buf), "(%u)", static_cast<unsigned>(ayah_number));
  const int w = gfx::Canvas::textWidth(buf, kBodyScale);
  // Sit the small Latin text roughly on the Arabic baseline.
  canvas.drawText(right_x - w, baseline_y - 12, buf, kBodyScale, gfx::kBlack);
  return w;
}

/** The footer rule and its text, identical for both layout paths. */
void DrawFooter(gfx::Canvas& canvas, const QuranState& state, char* buf,
                size_t buf_size) {
  canvas.drawHLine(kMarginX, kFooterRuleY,
                   static_cast<int>(canvas.width()) - 2 * kMarginX, gfx::kBlack);
  if (state.status_message[0] != 0) {
    CenterText(canvas, kFooterTextY, state.status_message, kBodyScale);
  } else {
    snprintf(buf, buf_size, "screen %u   WHEEL=page  EXIT=back",
             static_cast<unsigned>(state.screen_number));
    CenterText(canvas, kFooterTextY, buf, kBodyScale);
  }
}

/** Reads one word's glyph run out of ASSETS. False if it cannot be laid out. */
bool ReadWordRun(const qpk::Reader& reader, uint32_t word_index,
                 uint16_t* glyph_ids, uint32_t* glyph_count, int* width) {
  qpk::WordRecord word;
  if (reader.getWordByIndex(word_index, &word) != qpk::Error::kOk) return false;
  const uint32_t count = word.text_length / 2;
  if (count == 0 || count > kMaxGlyphsPerWord) return false;

  uint8_t raw[kMaxGlyphsPerWord * 2];
  const int32_t n = reader.readAsset(word.text_offset, word.text_length, raw,
                                     sizeof(raw));
  if (n < 0 || static_cast<uint32_t>(n) != word.text_length) return false;
  for (uint32_t g = 0; g < count; ++g) glyph_ids[g] = qpk::Read16(raw + g * 2);

  if (qpk::MeasureGlyphRun(reader, glyph_ids, count, width) !=
      qpk::BlitError::kOk) {
    return false;
  }
  *glyph_count = count;
  return true;
}

/**
 * Draws whole mushaf lines from LAYOUT_DATA, starting at first_line_index.
 *
 * The difference from the fill-and-wrap path is where a line ENDS: there, a
 * line breaks wherever the words stopped fitting; here it breaks where the
 * Madinah mushaf breaks it, and the device only decides how to fit that line
 * onto the panel.
 *
 * 2.78% of the mushaf's 8,817 lines were wider than the 5.79" panel's 756
 * usable pixels at the atlas's 36px -- measured across the whole corpus, not
 * guessed -- so those wrap onto a second panel row. The 800 px panel has 764
 * usable pixels; that share has not been re-measured, and the wrap handles
 * it either way. The alternative was
 * re-rasterizing the atlas smaller, which the same measurement rules out:
 * the widest line (1,305px) is still over at 28px, so a global downscale
 * would cost legibility on every line and still not fix the outliers.
 *
 * A line is measured before any of it is drawn, so a line that cannot fit in
 * the space left is deferred whole to the next screen rather than sliced.
 */
void RenderLayoutPath(gfx::Canvas& canvas, const QuranState& state,
                      const MarkerAtlas& markers, QuranRenderResult* result) {
  const qpk::Reader& reader = *state.reader;
  result->used_layout = true;

  qpk::LayoutHeader layout;
  if (reader.getLayoutHeader(&layout) != qpk::Error::kOk) {
    result->unsupported = true;
    return;
  }
  const uint32_t total_words = reader.recordCount(qpk::SectionId::kWordIndex);
  const int right_edge = static_cast<int>(canvas.width()) - kMarginX;

  uint32_t line_index = state.first_line_index;
  result->next_line_index = line_index;

  // The ayah the first word belongs to, then advanced as words are consumed
  // -- one binary search instead of a lookup per word.
  uint32_t ayah_index = 0;
  qpk::AyahRecord ayah;
  bool have_ayah = false;
  {
    qpk::LineRecord first;
    if (reader.getLine(line_index, &first) == qpk::Error::kOk) {
      have_ayah = reader.findAyahByWordIndex(first.first_word_index,
                                             &ayah_index, &ayah) ==
                  qpk::Error::kOk;
      if (have_ayah) {
        result->first_surah_id = ayah.surah_id;
        result->next_ayah_index = ayah_index;
      }
    }
  }

  int baseline = kFirstBaseline;
  while (line_index < layout.line_count_total && baseline <= kMaxBaseline) {
    qpk::LineRecord line;
    if (reader.getLine(line_index, &line) != qpk::Error::kOk) break;

    uint32_t line_end = total_words;
    if (line_index + 1 < layout.line_count_total) {
      qpk::LineRecord next;
      if (reader.getLine(line_index + 1, &next) != qpk::Error::kOk) break;
      line_end = next.first_word_index;
    }
    if (line_end <= line.first_word_index) {  // empty or malformed
      ++line_index;
      result->next_line_index = line_index;
      continue;
    }

    const uint32_t span = line_end - line.first_word_index;
    if (span > kMaxWordsPerLine) break;  // corrupt LAYOUT_DATA, not real data

    // --- measure the whole line before drawing any of it -------------------
    int word_width[kMaxWordsPerLine];
    bool word_ok[kMaxWordsPerLine];
    bool marker_after[kMaxWordsPerLine];
    uint16_t marker_ayah[kMaxWordsPerLine];
    int needed_rows = 1;
    int pen = right_edge;
    {
      uint32_t probe_index = ayah_index;
      qpk::AyahRecord probe = ayah;
      bool probe_have = have_ayah;
      for (uint32_t i = 0; i < span; ++i) {
        const uint32_t word_index = line.first_word_index + i;
        uint16_t glyph_ids[kMaxGlyphsPerWord];
        uint32_t glyph_count = 0;
        int width = 0;
        word_ok[i] = ReadWordRun(reader, word_index, glyph_ids, &glyph_count,
                                 &width);
        word_width[i] = word_ok[i] ? width : 0;
        marker_after[i] = false;
        marker_ayah[i] = 0;

        if (probe_have && probe.word_count > 0 &&
            word_index >= probe.first_word_index + probe.word_count - 1u) {
          marker_after[i] = true;
          marker_ayah[i] = probe.ayah_number;
          if (reader.getAyahByIndex(++probe_index, &probe) != qpk::Error::kOk) {
            probe_have = false;
          }
        }

        if (word_ok[i]) {
          if (pen - word_width[i] < kMarginX && pen != right_edge) {
            ++needed_rows;
            pen = right_edge;
          }
          pen -= word_width[i] + kSpaceWidth;
        }
        if (marker_after[i]) {
          const int slot = MarkerSlotWidth(markers, marker_ayah[i]);
          if (pen - slot < kMarginX && pen != right_edge) {
            ++needed_rows;
            pen = right_edge;
          }
          pen -= slot + kSpaceWidth;
        }
      }
    }

    // A line is never sliced across screens: half a mushaf line on each is
    // worse than a screen with one fewer line on it.
    const int last_baseline = baseline + (needed_rows - 1) * kLineHeight;
    if (last_baseline > kMaxBaseline && result->lines_drawn > 0) break;

    // --- draw it ------------------------------------------------------------
    pen = right_edge;
    for (uint32_t i = 0; i < span; ++i) {
      const uint32_t word_index = line.first_word_index + i;
      if (word_ok[i]) {
        if (pen - word_width[i] < kMarginX && pen != right_edge) {
          baseline += kLineHeight;
          pen = right_edge;
        }
        uint16_t glyph_ids[kMaxGlyphsPerWord];
        uint32_t glyph_count = 0;
        int width = 0;
        if (baseline <= kMaxBaseline &&
            ReadWordRun(reader, word_index, glyph_ids, &glyph_count, &width) &&
            qpk::BlitGlyphRun(reader, &canvas, glyph_ids, glyph_count,
                              pen - width, baseline,
                              nullptr) == qpk::BlitError::kOk) {
          result->glyphs_drawn += glyph_count;
        }
        pen -= word_width[i] + kSpaceWidth;
      }
      if (marker_after[i]) {
        if (pen - MarkerSlotWidth(markers, marker_ayah[i]) < kMarginX &&
            pen != right_edge) {
          baseline += kLineHeight;
          pen = right_edge;
        }
        if (baseline <= kMaxBaseline) {
          pen -= DrawAyahMarker(canvas, reader, markers, pen, baseline,
                                marker_ayah[i], &result->glyphs_drawn) +
                 kSpaceWidth;
        }
        ++result->ayahs_drawn;
        if (have_ayah &&
            reader.getAyahByIndex(++ayah_index, &ayah) != qpk::Error::kOk) {
          have_ayah = false;
        }
        result->next_ayah_index = ayah_index;
      }
    }

    baseline += kLineHeight;
    ++line_index;
    ++result->lines_drawn;
    result->next_line_index = line_index;
  }

  // Nothing fit at all -- advance anyway, or paging would stick forever on a
  // line the panel cannot show.
  if (result->lines_drawn == 0 && line_index < layout.line_count_total) {
    result->next_line_index = line_index + 1;
  }
}

}  // namespace

bool QuranScreen::hasLayout(const QuranState& state) {
  if (state.reader == nullptr || !state.reader->isOpen()) return false;
  qpk::LayoutHeader header;
  return state.reader->getLayoutHeader(&header) == qpk::Error::kOk &&
         header.line_count_total > 0;
}

bool QuranScreen::seekToAyah(QuranState* state, uint32_t ayah_index) {
  if (state == nullptr) return false;
  state->first_ayah_index = ayah_index;
  if (!hasLayout(*state)) return false;

  qpk::AyahRecord ayah;
  if (state->reader->getAyahByIndex(ayah_index, &ayah) != qpk::Error::kOk) {
    return false;
  }
  uint32_t line_index = 0;
  qpk::LineRecord line;
  if (state->reader->findLineByWordIndex(ayah.first_word_index, &line_index,
                                         &line) != qpk::Error::kOk) {
    return false;
  }
  state->first_line_index = line_index;
  return true;
}

bool QuranScreen::loadSurah(QuranState* state, uint16_t surah_id) {
  if (state == nullptr || state->reader == nullptr) return false;

  qpk::SurahRecord surah;
  if (state->reader->getSurah(surah_id, &surah) != qpk::Error::kOk) return false;

  state->surah_id = surah_id;
  state->ayah_count = surah.ayah_count;
  state->first_ayah_index = surah.first_ayah_index;
  state->screen_number = 1;
  state->surah_name[0] = 0;

  // The name lives in TEXT_DATA as UTF-8 Arabic, which the ASCII face cannot
  // draw -- it is read anyway because the header falls back to it only when
  // there is nothing better, and a caller may want it for logging.
  const uint32_t take = surah.name_length < sizeof(state->surah_name) - 1
                            ? surah.name_length
                            : sizeof(state->surah_name) - 1;
  const int32_t n = state->reader->readText(surah.name_offset, take,
                                            state->surah_name, take + 1);
  if (n < 0) state->surah_name[0] = 0;
  return true;
}

QuranRenderResult QuranScreen::render(gfx::Canvas& canvas,
                                      const QuranState& state) {
  QuranRenderResult result;
  result.next_ayah_index = state.first_ayah_index;

  canvas.clear(gfx::kWhite);

  char buf[80];
  if (state.reader == nullptr || !state.reader->isOpen()) {
    canvas.drawText(kMarginX, kHeaderY, "Quran", kBodyScale, gfx::kBlack);
    canvas.drawHLine(kMarginX, kRuleY,
                     static_cast<int>(canvas.width()) - 2 * kMarginX, gfx::kBlack);
    CenterText(canvas, 230, "No package open", kBodyScale);
    result.unsupported = true;
    return result;
  }

  // The header names the ayah this screen actually starts at, read from the
  // record rather than derived from the index -- the index is a position in
  // AYAH_INDEX, not an ayah number, and the two only coincide for surah 1.
  uint16_t first_ayah_number = 1;
  {
    qpk::AyahRecord probe;
    if (state.reader->getAyahByIndex(state.first_ayah_index, &probe) ==
        qpk::Error::kOk) {
      first_ayah_number = probe.ayah_number;
    }
  }
  snprintf(buf, sizeof(buf), "Surah %u   ayah %u of %u",
           static_cast<unsigned>(state.surah_id),
           static_cast<unsigned>(first_ayah_number),
           static_cast<unsigned>(state.ayah_count));
  canvas.drawText(kMarginX, kHeaderY, buf, kBodyScale, gfx::kBlack);
  canvas.drawHLine(kMarginX, kRuleY,
                   static_cast<int>(canvas.width()) - 2 * kMarginX, gfx::kBlack);

  // A package without a shaped atlas cannot be drawn: TEXT_DATA holds UTF-8
  // Arabic and gfx::font5x7 is a 5x7 ASCII face. Say which package to open
  // rather than rendering an empty screen.
  if (!state.reader->hasSection(qpk::SectionId::kFontMetadata) ||
      !state.reader->hasSection(qpk::SectionId::kAssets)) {
    CenterText(canvas, 215, "This Quran package has no shaped glyph atlas.",
               kBodyScale);
    CenterText(canvas, 245, "Open the shaped package instead.", kBodyScale);
    result.unsupported = true;
    return result;
  }

  MarkerAtlas markers;
  markers.available = state.reader->ayahMarkerGlyphs(&markers.first_glyph_id,
                                                     &markers.count);

  // The layout path when the package carries the mushaf's own line breaks;
  // fill-and-wrap otherwise. Kept as two paths on purpose: fill-and-wrap is
  // the one confirmed on real hardware (2026-09-01), and a package built
  // before LAYOUT_DATA existed still has to render exactly as it did.
  if (hasLayout(state)) {
    RenderLayoutPath(canvas, state, markers, &result);
    DrawFooter(canvas, state, buf, sizeof(buf));
    return result;
  }

  const uint32_t total_ayahs = state.reader->recordCount(qpk::SectionId::kAyahIndex);
  const uint32_t surah_end = state.first_ayah_index + state.ayah_count;

  int pen_x = static_cast<int>(canvas.width()) - kMarginX;
  int baseline = kFirstBaseline;
  uint32_t index = state.first_ayah_index;

  while (index < total_ayahs) {
    qpk::AyahRecord ayah;
    if (state.reader->getAyahByIndex(index, &ayah) != qpk::Error::kOk) break;
    if (ayah.surah_id != state.surah_id) break;  // ran off the end of the surah

    // Lay the whole ayah out; if it does not fit, stop before drawing any of
    // it only when nothing has been drawn on this screen yet -- otherwise the
    // partial ayah continues on the next screen, which is what a mushaf does.
    bool ayah_started = false;
    for (uint32_t w = 0; w < ayah.word_count; ++w) {
      qpk::WordRecord word;
      if (state.reader->getWordByIndex(ayah.first_word_index + w, &word) !=
          qpk::Error::kOk) {
        break;
      }
      const uint32_t glyph_count = word.text_length / 2;
      if (glyph_count == 0 || glyph_count > kMaxGlyphsPerWord) continue;

      uint8_t raw[kMaxGlyphsPerWord * 2];
      const int32_t n = state.reader->readAsset(word.text_offset, word.text_length,
                                                raw, sizeof(raw));
      if (n < 0 || static_cast<uint32_t>(n) != word.text_length) continue;

      uint16_t glyph_ids[kMaxGlyphsPerWord];
      for (uint32_t g = 0; g < glyph_count; ++g) {
        glyph_ids[g] = qpk::Read16(raw + g * 2);
      }

      int width = 0;
      if (qpk::MeasureGlyphRun(*state.reader, glyph_ids, glyph_count, &width) !=
          qpk::BlitError::kOk) {
        continue;
      }

      if (pen_x - width < kMarginX) {  // wrap
        pen_x = static_cast<int>(canvas.width()) - kMarginX;
        baseline += kLineHeight;
        if (baseline > kMaxBaseline) {
          // Out of room. Stop here; this ayah resumes on the next screen.
          result.next_ayah_index = ayah_started ? index : index;
          if (result.ayahs_drawn == 0 && !ayah_started) {
            // Not even one word fit on an empty screen: refuse to loop
            // forever by advancing past this ayah.
            result.next_ayah_index = index + 1;
          }
          goto done;
        }
      }

      const int start_x = pen_x - width;
      if (qpk::BlitGlyphRun(*state.reader, &canvas, glyph_ids, glyph_count,
                            start_x, baseline, nullptr) != qpk::BlitError::kOk) {
        continue;
      }
      result.glyphs_drawn += glyph_count;
      pen_x = start_x - kSpaceWidth;
      ayah_started = true;
    }

    // Ayah number, after its last word, on the same line.
    if (ayah_started) {
      if (pen_x - 40 < kMarginX) {
        pen_x = static_cast<int>(canvas.width()) - kMarginX;
        baseline += kLineHeight;
      }
      if (baseline <= kMaxBaseline) {
        pen_x -= DrawAyahMarker(canvas, *state.reader, markers, pen_x, baseline,
                                ayah.ayah_number, &result.glyphs_drawn) +
                 kSpaceWidth;
      }
      ++result.ayahs_drawn;
    }

    ++index;
    result.next_ayah_index = index;
    if (index >= surah_end) break;  // end of surah
  }

done:
  DrawFooter(canvas, state, buf, sizeof(buf));
  return result;
}

}  // namespace ui
