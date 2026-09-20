#include "ui/surah_picker_screen.h"

#include <stddef.h>
#include <stdio.h>

#include "app/surah_names.h"
#include "qpk/qpk_format.h"

namespace ui {

namespace {

constexpr uint8_t kTitleScale = 3;
constexpr uint8_t kBodyScale = 2;

// Layout for the 800x480 panel. Text is the 5x7 face: (5 + 1) * scale px per
// character and 7 * scale px tall, with `y` at the top of the glyph cell.
constexpr int kBorderX = 4;
constexpr int kBorderY = 4;
constexpr int kBorderW = 792;
constexpr int kBorderH = 472;

constexpr int kRuleX = 24;
constexpr int kRuleW = 752;           // 24..775
constexpr int kHeaderRuleY = 72;
constexpr int kFooterRuleY = 446;
constexpr int kFooterTextY = 454;

constexpr int kRowLeftX = 30;
constexpr int kRowRightX = 620;       // "286 ayahs" is 106 px -> ends at 725
constexpr int kRowTop = 84;
constexpr int kRowStep = 28;
// 84 + 12 * 28 = 420; the scroll indicator below it ends at 436, clear of
// the footer rule at 446.
constexpr int kListBottom = kRowTop + kSurahPickerMaxVisibleRows * kRowStep;

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale,
                  gfx::kBlack);
}

/** kSurahNames[surah_id - 1], or a numeric fallback for an id out of range. */
const char* SurahName(uint16_t surah_id, char* fallback, size_t capacity) {
  if (surah_id >= 1 && surah_id <= static_cast<uint16_t>(app::kSurahNameCount)) {
    return app::kSurahNames[surah_id - 1];
  }
  snprintf(fallback, capacity, "Surah %u", static_cast<unsigned>(surah_id));
  return fallback;
}

}  // namespace

uint16_t SurahPickerScreen::rowCount(const SurahPickerState& state) {
  if (state.reader == nullptr) {
    return state.list_ayah_counts != nullptr ? state.list_count : 0;
  }
  if (!state.reader->isOpen()) return 0;
  return static_cast<uint16_t>(
      state.reader->recordCount(qpk::SectionId::kSurahIndex));
}

int32_t SurahPickerScreen::rowAt(const SurahPickerState& state, int16_t x, int16_t y) {
  const uint16_t total = rowCount(state);
  if (total == 0) return -1;
  if (x < kRowLeftX - 14 || x >= kRuleX + kRuleW) return -1;
  if (y < kRowTop || y >= kListBottom) return -1;
  const int32_t line = (y - kRowTop) / kRowStep;
  const int32_t row = static_cast<int32_t>(state.scroll_top) + line;
  if (row < 0 || row >= static_cast<int32_t>(total)) return -1;
  return row;
}

void SurahPickerScreen::render(gfx::Canvas& canvas, const SurahPickerState& state) {
  char buf[80];

  canvas.clear(gfx::kWhite);
  canvas.drawRect(kBorderX, kBorderY, kBorderW, kBorderH, gfx::kBlack);

  const uint16_t total = rowCount(state);
  CenterText(canvas, 14, "Jump to Surah", kTitleScale);
  if (total > 0) {
    snprintf(buf, sizeof(buf), "%u surahs", static_cast<unsigned>(total));
  } else {
    snprintf(buf, sizeof(buf), "No SURAH_INDEX in this package");
  }
  CenterText(canvas, 48, buf, kBodyScale);
  canvas.drawHLine(kRuleX, kHeaderRuleY, kRuleW, gfx::kBlack);

  for (uint8_t row = 0; row < kSurahPickerMaxVisibleRows; ++row) {
    const uint16_t item = static_cast<uint16_t>(state.scroll_top + row);
    if (item >= total) break;

    const int y = kRowTop + row * kRowStep;
    canvas.drawText(kRowLeftX - 14, y, item == state.selected ? ">" : " ",
                    kBodyScale, gfx::kBlack);

    // item is 0-based; a well-formed SURAH_INDEX has ids starting at 1,
    // addressed [id-1], so item + 1 is the id to ask for -- true for slot
    // *contents* by the format's own contiguous-ids rule, but getSurah()
    // separately enforces SURAH_INDEX[id-1].surah_id == id (rule 14's
    // "surah index slot must hold that surah") and that specific check is
    // NOT part of open()'s validation, only this lazy lookup -- so a package
    // that opened successfully can still fail getSurah() here. Draw a
    // fallback row rather than skip: the row count (rowCount(), used for
    // scrolling/clamping) must never disagree with what actually gets
    // drawn, the same principle as ui::LibraryScreen's "index changed under
    // us" handling.
    qpk::SurahRecord surah;
    char name_fallback[16];
    if (state.reader == nullptr) {
      // A list handed in by the caller (the translation reader).
      const uint16_t id = static_cast<uint16_t>(item + 1);
      snprintf(buf, sizeof(buf), "%u. %s", static_cast<unsigned>(id),
               SurahName(id, name_fallback, sizeof(name_fallback)));
      canvas.drawText(kRowLeftX, y, buf, kBodyScale, gfx::kBlack);
      snprintf(buf, sizeof(buf), "%u ayahs", static_cast<unsigned>(state.list_ayah_counts[item]));
      canvas.drawText(kRowRightX, y, buf, kBodyScale, gfx::kBlack);
    } else if (state.reader->getSurah(static_cast<uint16_t>(item + 1), &surah) ==
               qpk::Error::kOk) {
      snprintf(buf, sizeof(buf), "%u. %s", static_cast<unsigned>(surah.surah_id),
               SurahName(surah.surah_id, name_fallback, sizeof(name_fallback)));
      canvas.drawText(kRowLeftX, y, buf, kBodyScale, gfx::kBlack);

      snprintf(buf, sizeof(buf), "%u ayahs", static_cast<unsigned>(surah.ayah_count));
      canvas.drawText(kRowRightX, y, buf, kBodyScale, gfx::kBlack);
    } else {
      snprintf(buf, sizeof(buf), "%u. (unavailable)", static_cast<unsigned>(item + 1));
      canvas.drawText(kRowLeftX, y, buf, kBodyScale, gfx::kBlack);
    }
  }

  if (total > kSurahPickerMaxVisibleRows) {
    snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(state.selected + 1),
             static_cast<unsigned>(total));
    canvas.drawText(kRowRightX + 60, kListBottom + 2, buf, kBodyScale,
                    gfx::kBlack);
  }

  canvas.drawHLine(kRuleX, kFooterRuleY, kRuleW, gfx::kBlack);
  CenterText(canvas, kFooterTextY, "WHEEL=move OK=open EXIT=back", kBodyScale);
}

}  // namespace ui
