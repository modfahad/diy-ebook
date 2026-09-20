// Host unit tests for the ui:: screens -- all four of them, now that
// docs/pending.md's "no host C++ compiler" gap is resolved (2026-09-01).
// ui::SurahPickerScreen (the surah picker, docs/pending.md's "biggest
// usability gap") was the first one wired in, when a compiler was still a
// separate problem from these screens simply not having tests written
// against them; ui::LibraryScreen, ui::ReaderScreen and ui::SelfTestScreen
// follow the same pattern here.
//
// All four are pure render: state in, canvas out, no hardware -- each
// screen's own header says so. That is what makes them host-testable at
// all despite the ESP32-specific `hal::` types some of their state structs
// reference (WakeInfo, InputDiagnostics): those are plain structs with
// stdint.h fields only, no Arduino dependency.

#include <unity.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "app/library_icons.h"
#include "app/surah_names.h"
#include "board/board_crowpanel_579.h"
#include "gfx/canvas.h"
#include "net/library_index.h"
#include "net/protocol.h"
#include "qpk/memory_file.h"
#include "qpk/qpk_reader.h"
#include "test_qpk/qpk_test_package.h"
#include "ui/options_menu.h"
#include "ui/home_screen.h"
#include "ui/bookmarks_screen.h"
#include "ui/library_screen.h"
#include "ui/page_image_screen.h"
#include "ui/quran_screen.h"
#include "ui/reader_screen.h"
#include "ui/selftest_screen.h"
#include "ui/surah_picker_screen.h"
#include "ui/setup_screen.h"
#include "util/screen_setup.h"
#include "util/ascii_fold.h"
#include "util/text_pager.h"

namespace {

uint8_t g_buffer[board::kFramebufferBytes];

// Same shape as the real product build's canvas (g_display.canvas() in
// main.cpp): logical 800x480, rotation 0, no seam -- see
// test_logic/test_main.cpp's makeCanvas(). Rotation 0 is also what makes
// PixelIsBlack() below a direct (x, y) lookup.
gfx::Canvas MakeCanvas() {
  return gfx::Canvas(g_buffer, board::kWidth, board::kHeight, board::kWidth,
                     board::kHeight, board::kStrideBytes, /*rotation=*/0,
                     /*seam_x=*/0);
}

bool FramebufferIsAllWhite() {
  for (uint32_t i = 0; i < board::kFramebufferBytes; ++i) {
    if (g_buffer[i] != 0xFF) return false;
  }
  return true;
}

}  // namespace

void test_surah_picker_row_count_is_zero_with_no_package_open() {
  ui::SurahPickerState state;
  TEST_ASSERT_EQUAL_UINT16(0, ui::SurahPickerScreen::rowCount(state));

  qpk::Reader reader;  // never opened
  state.reader = &reader;
  TEST_ASSERT_EQUAL_UINT16(0, ui::SurahPickerScreen::rowCount(state));
}

void test_surah_picker_row_count_matches_the_package_not_a_hardcoded_114() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader.open(&file)));

  ui::SurahPickerState state;
  state.reader = &reader;
  TEST_ASSERT_EQUAL_UINT16(qpktest::MiniQuran::kSurahCount,
                           ui::SurahPickerScreen::rowCount(state));
}

void test_surah_picker_render_draws_something_for_a_real_package() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader.open(&file)));

  ui::SurahPickerState state;
  state.reader = &reader;
  state.selected = 1;

  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  TEST_ASSERT_TRUE(FramebufferIsAllWhite());
  ui::SurahPickerScreen::render(canvas, state);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

void test_surah_picker_render_with_no_package_does_not_crash() {
  ui::SurahPickerState state;  // reader == nullptr
  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  TEST_ASSERT_TRUE(FramebufferIsAllWhite());
  ui::SurahPickerScreen::render(canvas, state);  // must return, not crash
  // Still draws the border/title/footer chrome even with nothing to list.
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

void test_surah_picker_render_clamps_a_selection_past_the_end() {
  // "index changed under us" -- the same case ui::LibraryScreen::entryOfType
  // guards against -- must be skipped, not walked off the section.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader.open(&file)));

  ui::SurahPickerState state;
  state.reader = &reader;
  state.selected = 9999;
  state.scroll_top = 9999;

  gfx::Canvas canvas = MakeCanvas();
  ui::SurahPickerScreen::render(canvas, state);  // must return, not hang or crash
}

// ---------------------------------------------------------------------------
// QuranScreen -- the one screen with no host test until today
// (docs/pending.md), not because it was untestable but because no fixture
// carried a shaped glyph atlas. qpktest::BuildMiniShapedQuran() (new) fixes
// that with 3 synthetic glyphs -- not real Arabic, per this file's own rule.
// ---------------------------------------------------------------------------

void test_quran_screen_renders_real_glyphs_from_a_shaped_package() {
  qpktest::MiniShapedQuran mini = qpktest::BuildMiniShapedQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader.open(&file)));

  ui::QuranState state;
  state.reader = &reader;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state, 1));

  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  const ui::QuranRenderResult result = ui::QuranScreen::render(canvas, state);

  TEST_ASSERT_FALSE(result.unsupported);
  TEST_ASSERT_TRUE(result.ayahs_drawn > 0);
  TEST_ASSERT_TRUE(result.glyphs_drawn > 0);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

void test_quran_screen_pages_across_the_surah_boundary() {
  // Surah 1 holds ayah_index 0..2 (3 ayahs); the fixture is small enough
  // that a single screen's worth of glyphs covers all of it, so
  // next_ayah_index should already point past it -- exactly the boundary
  // main.cpp's QuranPageForward crosses by calling loadSurah(2) and
  // resuming at that index. This proves surah 2 renders too, not just
  // surah 1's opening screen.
  qpktest::MiniShapedQuran mini = qpktest::BuildMiniShapedQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  reader.open(&file);

  ui::QuranState state;
  state.reader = &reader;
  ui::QuranScreen::loadSurah(&state, 1);

  gfx::Canvas canvas = MakeCanvas();
  const ui::QuranRenderResult first = ui::QuranScreen::render(canvas, state);
  TEST_ASSERT_TRUE(first.ayahs_drawn > 0);

  if (first.next_ayah_index >= 3) {  // ran off the end of surah 1
    TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state, 2));
    state.first_ayah_index = first.next_ayah_index;
    canvas.clear(gfx::kWhite);
    const ui::QuranRenderResult second = ui::QuranScreen::render(canvas, state);
    TEST_ASSERT_TRUE(second.ayahs_drawn > 0);
    TEST_ASSERT_TRUE(second.glyphs_drawn > 0);
  }
}

void test_quran_screen_draws_a_real_ayah_marker_when_the_package_has_one() {
  // Counted rather than eyeballed: the marker is one atlas glyph per ayah, so
  // a package carrying kAyahMarkerGlyphs must blit exactly one more glyph per
  // ayah drawn than the same package without the key, which falls back to the
  // Latin "(n)" (drawn with the 5x7 face, never through the atlas).
  //
  // Both fixtures carry the identical atlas -- only the METADATA key differs
  // -- so nothing but the marker path can move this number.
  qpktest::MiniShapedQuran with_markers = qpktest::BuildMiniShapedQuran();
  qpktest::MiniShapedQuran without = qpktest::BuildMiniShapedQuran(nullptr);

  qpk::MemoryFile file_a;
  file_a.reset(with_markers.file.data(), with_markers.file.size());
  qpk::Reader reader_a;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader_a.open(&file_a)));

  qpk::MemoryFile file_b;
  file_b.reset(without.file.data(), without.file.size());
  qpk::Reader reader_b;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader_b.open(&file_b)));

  ui::QuranState state_a;
  state_a.reader = &reader_a;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state_a, 1));
  ui::QuranState state_b;
  state_b.reader = &reader_b;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state_b, 1));

  gfx::Canvas canvas = MakeCanvas();
  const ui::QuranRenderResult drawn = ui::QuranScreen::render(canvas, state_a);
  const ui::QuranRenderResult fallback =
      ui::QuranScreen::render(canvas, state_b);

  TEST_ASSERT_TRUE(drawn.ayahs_drawn > 0);
  TEST_ASSERT_EQUAL_UINT16(drawn.ayahs_drawn, fallback.ayahs_drawn);
  TEST_ASSERT_EQUAL_UINT32(fallback.glyphs_drawn + drawn.ayahs_drawn,
                           drawn.glyphs_drawn);
}

void test_quran_screen_falls_back_when_a_claimed_marker_is_not_in_the_atlas() {
  // A package can claim a marker range its atlas does not hold -- a
  // mismatched or truncated build. "900:3" parses fine and every id in it
  // resolves to nothing, so the screen must draw the Latin stand-in for every
  // ayah rather than leaving a hole in the line where a verse ends.
  //
  // Asserted against the no-key package: identical glyph counts means not one
  // marker came out of the atlas, and identical ayah counts means the
  // fall-through did not cost a verse.
  qpktest::MiniShapedQuran claims = qpktest::BuildMiniShapedQuran("900:3");
  qpktest::MiniShapedQuran without = qpktest::BuildMiniShapedQuran(nullptr);

  qpk::MemoryFile file_a;
  file_a.reset(claims.file.data(), claims.file.size());
  qpk::Reader reader_a;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader_a.open(&file_a)));
  qpk::MemoryFile file_b;
  file_b.reset(without.file.data(), without.file.size());
  qpk::Reader reader_b;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader_b.open(&file_b)));

  ui::QuranState state_a;
  state_a.reader = &reader_a;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state_a, 1));
  ui::QuranState state_b;
  state_b.reader = &reader_b;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state_b, 1));

  gfx::Canvas canvas = MakeCanvas();
  const ui::QuranRenderResult over_claimed =
      ui::QuranScreen::render(canvas, state_a);
  const ui::QuranRenderResult no_key = ui::QuranScreen::render(canvas, state_b);

  TEST_ASSERT_TRUE(no_key.ayahs_drawn > 0);
  TEST_ASSERT_EQUAL_UINT16(no_key.ayahs_drawn, over_claimed.ayahs_drawn);
  TEST_ASSERT_EQUAL_UINT32(no_key.glyphs_drawn, over_claimed.glyphs_drawn);
}

// ---------------------------------------------------------------------------
// QuranScreen, layout path -- LAYOUT_DATA-driven mushaf line breaks
// ---------------------------------------------------------------------------

namespace {

// Opens a fixture and leaves the reader alive for the caller. The reader
// holds a pointer into the file bytes and QuranScreen re-reads through it
// while drawing (quran_screen.h says why), so both have to outlive render().
struct ShapedFixture {
  qpktest::MiniShapedQuran mini;
  qpk::MemoryFile file;
  qpk::Reader reader;

  void open(bool with_layout) {
    mini = qpktest::BuildMiniShapedQuran("601:3", with_layout);
    file.reset(mini.file.data(), mini.file.size());
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader.open(&file)));
  }
};

}  // namespace

void test_quran_screen_uses_the_layout_path_only_when_layout_data_is_present() {
  // The whole additive claim in one test: the same content, the same atlas,
  // and the only difference is whether LAYOUT_DATA is there. A package built
  // before LAYOUT_DATA existed must still take the fill-and-wrap path that
  // was confirmed on hardware.
  ShapedFixture with_layout;
  with_layout.open(true);
  ShapedFixture without;
  without.open(false);

  ui::QuranState a;
  a.reader = &with_layout.reader;
  TEST_ASSERT_TRUE(ui::QuranScreen::hasLayout(a));
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&a, 1));
  TEST_ASSERT_TRUE(ui::QuranScreen::seekToAyah(&a, a.first_ayah_index));

  ui::QuranState b;
  b.reader = &without.reader;
  TEST_ASSERT_FALSE(ui::QuranScreen::hasLayout(b));
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&b, 1));
  // seekToAyah still sets the ayah cursor; it just has no line to point at.
  TEST_ASSERT_FALSE(ui::QuranScreen::seekToAyah(&b, 3));
  TEST_ASSERT_EQUAL_UINT32(3, b.first_ayah_index);
  // ...and ayah 3 is in surah 2, which the fill path refuses to draw under
  // surah 1 -- put it back to this surah's own first ayah before rendering.
  TEST_ASSERT_FALSE(ui::QuranScreen::seekToAyah(&b, 0));

  gfx::Canvas canvas = MakeCanvas();
  const ui::QuranRenderResult layout_result =
      ui::QuranScreen::render(canvas, a);
  TEST_ASSERT_TRUE(layout_result.used_layout);
  TEST_ASSERT_TRUE(layout_result.lines_drawn > 0);
  TEST_ASSERT_TRUE(layout_result.glyphs_drawn > 0);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());

  canvas.clear(gfx::kWhite);
  const ui::QuranRenderResult fill_result = ui::QuranScreen::render(canvas, b);
  TEST_ASSERT_FALSE(fill_result.used_layout);
  TEST_ASSERT_EQUAL_UINT16(0, fill_result.lines_drawn);
  TEST_ASSERT_TRUE(fill_result.glyphs_drawn > 0);
}

void test_seek_to_ayah_finds_the_line_the_ayah_starts_on() {
  // The bridge net::ReadingProgress depends on: an ayah index in, a line
  // cursor out. The fixture puts 3 words on each line and 2-3 words in each
  // ayah, so the two do not line up trivially -- ayah 3 starts at word 7,
  // which is on line 2 (words 6..8).
  ShapedFixture fixture;
  fixture.open(true);

  ui::QuranState state;
  state.reader = &fixture.reader;

  struct Case { uint32_t ayah_index; uint32_t expect_line; };
  const Case kCases[] = {
      {0, 0},  // words 0..1  -> line 0 (words 0..2)
      {1, 0},  // words 2..4  -> line 0
      {2, 1},  // words 5..6  -> line 1 (words 3..5)
      {3, 2},  // words 7..9  -> line 2 (words 6..8)
      {4, 3},  // words 10..11 -> line 3 (words 9..11)
  };
  for (uint32_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    TEST_ASSERT_TRUE(
        ui::QuranScreen::seekToAyah(&state, kCases[i].ayah_index));
    TEST_ASSERT_EQUAL_UINT32(kCases[i].ayah_index, state.first_ayah_index);
    TEST_ASSERT_EQUAL_UINT32(kCases[i].expect_line, state.first_line_index);
  }
}

void test_quran_screen_layout_paging_advances_by_whole_lines() {
  // next_line_index is the paging cursor, and it must move forward every
  // screen or the reader sticks. Walking it to the end also proves no line is
  // skipped and none is drawn twice.
  ShapedFixture fixture;
  fixture.open(true);

  ui::QuranState state;
  state.reader = &fixture.reader;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&state, 1));
  TEST_ASSERT_TRUE(ui::QuranScreen::seekToAyah(&state, 0));

  gfx::Canvas canvas = MakeCanvas();
  uint32_t seen_lines = 0;
  uint32_t guard = 0;
  while (state.first_line_index < qpktest::MiniShapedQuran::kLayoutLineCount &&
         guard++ < 16) {
    const ui::QuranRenderResult result = ui::QuranScreen::render(canvas, state);
    TEST_ASSERT_TRUE(result.used_layout);
    TEST_ASSERT_TRUE(result.next_line_index > state.first_line_index);
    seen_lines += result.lines_drawn;
    state.first_line_index = result.next_line_index;
  }
  TEST_ASSERT_TRUE(guard < 16);  // it terminated rather than spinning
  TEST_ASSERT_EQUAL_UINT32(qpktest::MiniShapedQuran::kLayoutLineCount, seen_lines);
}

void test_quran_screen_reports_unsupported_for_an_unshaped_package() {
  // "A package with no atlas says so" (quran_screen.h) -- confirmed against
  // the plain MiniQuran fixture, which carries no FONT_METADATA/ASSETS.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  reader.open(&file);

  ui::QuranState state;
  state.reader = &reader;
  ui::QuranScreen::loadSurah(&state, 1);

  gfx::Canvas canvas = MakeCanvas();
  const ui::QuranRenderResult result = ui::QuranScreen::render(canvas, state);
  TEST_ASSERT_TRUE(result.unsupported);
  TEST_ASSERT_EQUAL_UINT32(0, result.glyphs_drawn);
}

// ---------------------------------------------------------------------------
// LibraryScreen
// ---------------------------------------------------------------------------

namespace {

bool PixelIsBlackAt(int x, int y) {
  const uint32_t index = static_cast<uint32_t>(y) * board::kStrideBytes +
                         static_cast<uint32_t>(x / 8);
  return (g_buffer[index] & (0x80u >> (x & 7))) == 0;
}

net::LibraryEntry MakeEntry(uint8_t id_seed, qpk::PackageType type,
                            const char* title) {
  net::LibraryEntry entry;
  memset(entry.content_id, id_seed, sizeof(entry.content_id));
  entry.type = static_cast<uint16_t>(type);
  snprintf(entry.title, sizeof(entry.title), "%s", title);
  return entry;
}

}  // namespace

void test_library_screen_categories_row_count_is_fixed() {
  // Fixed regardless of what is installed -- see the header's "the menu does
  // not change shape as content comes and goes" comment -- so an empty or
  // even null index must not change it.
  ui::LibraryState state;
  state.view = ui::LibraryView::kCategories;
  TEST_ASSERT_EQUAL_UINT16(ui::kLibraryHomeTileCount,
                           ui::LibraryScreen::rowCount(state));

  net::LibraryIndex index;
  state.index = &index;
  TEST_ASSERT_EQUAL_UINT16(ui::kLibraryHomeTileCount,
                           ui::LibraryScreen::rowCount(state));
}

void test_library_screen_items_row_count_is_count_plus_back() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kBook, "For Bushra"));
  index.upsert(MakeEntry(2, qpk::PackageType::kBook, "Another Book"));
  index.upsert(MakeEntry(3, qpk::PackageType::kQuran, "The Holy Quran"));

  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  // 2 books + the Back row.
  TEST_ASSERT_EQUAL_UINT16(3, ui::LibraryScreen::rowCount(state));
}

void test_library_screen_row_kind_classifies_every_row() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kBook, "For Bushra"));

  ui::LibraryState categories;
  categories.index = &index;
  categories.view = ui::LibraryView::kCategories;
  uint16_t ordinal = 99;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::LibraryRow::kCategory),
                        static_cast<int>(
                            ui::LibraryScreen::rowKind(categories, 0, &ordinal)));
  TEST_ASSERT_EQUAL_UINT16(0, ordinal);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ui::LibraryRow::kTransferMode),
      static_cast<int>(ui::LibraryScreen::rowKind(
          categories, ui::kLibraryCategoryCount, &ordinal)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ui::LibraryRow::kClock),
      static_cast<int>(ui::LibraryScreen::rowKind(
          categories, ui::kLibraryCategoryCount + 1, nullptr)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ui::LibraryRow::kDevice),
      static_cast<int>(ui::LibraryScreen::rowKind(
          categories, ui::kLibraryCategoryCount + 2, nullptr)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ui::LibraryRow::kNone),
      static_cast<int>(ui::LibraryScreen::rowKind(
          categories, ui::kLibraryHomeTileCount, nullptr)));

  ui::LibraryState items;
  items.index = &index;
  items.view = ui::LibraryView::kItems;
  items.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::LibraryRow::kBack),
                        static_cast<int>(ui::LibraryScreen::rowKind(items, 0, nullptr)));
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ui::LibraryRow::kItem),
      static_cast<int>(ui::LibraryScreen::rowKind(items, 1, &ordinal)));
  TEST_ASSERT_EQUAL_UINT16(0, ordinal);
}

void test_library_screen_category_type_and_label_agree() {
  // rowKind()'s kCategory ordinal indexes into these two -- they must stay
  // in the same order the row loop and main.cpp's OK handler both assume.
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(qpk::PackageType::kQuran),
                           ui::LibraryScreen::categoryType(0));
  TEST_ASSERT_EQUAL_STRING("Quran", ui::LibraryScreen::categoryLabel(0));
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(qpk::PackageType::kBook),
                           ui::LibraryScreen::categoryType(1));
  TEST_ASSERT_EQUAL_STRING("Books", ui::LibraryScreen::categoryLabel(1));
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(qpk::PackageType::kTranslation),
                           ui::LibraryScreen::categoryType(2));
  TEST_ASSERT_EQUAL_STRING("Translations", ui::LibraryScreen::categoryLabel(2));
  TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(qpk::PackageType::kTafsir),
                           ui::LibraryScreen::categoryType(3));
  TEST_ASSERT_EQUAL_STRING("Tafsir", ui::LibraryScreen::categoryLabel(3));
}

void test_library_screen_entry_of_type_filters_by_type() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kQuran, "Quran A"));
  index.upsert(MakeEntry(2, qpk::PackageType::kBook, "Book A"));
  index.upsert(MakeEntry(3, qpk::PackageType::kBook, "Book B"));

  ui::LibraryState state;
  state.index = &index;

  TEST_ASSERT_EQUAL_UINT16(
      2, ui::LibraryScreen::countOfType(
             state, static_cast<uint16_t>(qpk::PackageType::kBook)));
  const net::LibraryEntry* first = ui::LibraryScreen::entryOfType(
      state, static_cast<uint16_t>(qpk::PackageType::kBook), 0);
  const net::LibraryEntry* second = ui::LibraryScreen::entryOfType(
      state, static_cast<uint16_t>(qpk::PackageType::kBook), 1);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_EQUAL_STRING("Book A", first->title);
  TEST_ASSERT_EQUAL_STRING("Book B", second->title);
  // Only two books exist; a third ordinal is "index changed under us".
  TEST_ASSERT_NULL(ui::LibraryScreen::entryOfType(
      state, static_cast<uint16_t>(qpk::PackageType::kBook), 2));
}

void test_library_screen_render_draws_something_in_both_views() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kBook, "For Bushra"));

  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kCategories;

  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  ui::LibraryScreen::render(canvas, state);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());

  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  canvas.clear(gfx::kWhite);
  ui::LibraryScreen::render(canvas, state);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

void test_library_screen_footer_shows_transfer_address_and_yields_to_status() {
  // The IP:port a desktop client needs used to be shown only on
  // SelfTestScreen, one MENU press away from where Transfer Mode is
  // actually toggled on (the library's trailing row) -- a real usability
  // gap now fixed. Two things worth locking in: the footer actually
  // changes when transfer_mode flips (not silently ignored), and
  // status_message still wins over it, same precedence as every other
  // footer branch here.
  ui::LibraryState state;
  state.view = ui::LibraryView::kCategories;

  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  ui::LibraryScreen::render(canvas, state);
  std::vector<uint8_t> idle_footer(g_buffer, g_buffer + board::kFramebufferBytes);

  state.transfer_mode = true;
  state.transfer_address = "10.102.150.184:8080";
  canvas.clear(gfx::kWhite);
  ui::LibraryScreen::render(canvas, state);
  std::vector<uint8_t> transfer_footer(g_buffer, g_buffer + board::kFramebufferBytes);
  TEST_ASSERT_FALSE(idle_footer == transfer_footer);

  state.status_message = "some other footer feedback";
  canvas.clear(gfx::kWhite);
  ui::LibraryScreen::render(canvas, state);
  std::vector<uint8_t> status_footer(g_buffer, g_buffer + board::kFramebufferBytes);
  TEST_ASSERT_FALSE(status_footer == transfer_footer);
}

void test_library_icons_are_not_blank() {
  // Guards against firmware/scripts/gen_library_icons.py silently producing
  // an all-white icon -- a coordinate typo that draws nothing outside the
  // canvas would compile fine and only show up as a missing icon on real
  // hardware otherwise.
  auto has_ink = [](const uint8_t icon[][2]) {
    for (uint8_t row = 0; row < app::kLibraryIconSize; ++row) {
      if (icon[row][0] != 0 || icon[row][1] != 0) return true;
    }
    return false;
  };
  TEST_ASSERT_TRUE(has_ink(app::kIconQuran));
  TEST_ASSERT_TRUE(has_ink(app::kIconBooks));
  TEST_ASSERT_TRUE(has_ink(app::kIconTranslations));
  TEST_ASSERT_TRUE(has_ink(app::kIconTafsir));
  TEST_ASSERT_TRUE(has_ink(app::kIconTransfer));
  TEST_ASSERT_TRUE(has_ink(app::kIconClock));
  TEST_ASSERT_TRUE(has_ink(app::kIconDevice));
}

void test_library_home_tiles_fit_and_only_the_selected_one_is_inverted() {
  int last_bottom = 0;
  for (uint16_t row = 0; row < ui::kLibraryHomeTileCount; ++row) {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    ui::LibraryScreen::tileRect(row, &x, &y, &w, &h);
    TEST_ASSERT_TRUE(x >= 4 && x + w <= 796);
    TEST_ASSERT_TRUE(y >= 76 && y + h <= 446);  // between the header and footer rules
    if (y + h > last_bottom) last_bottom = y + h;
  }
  TEST_ASSERT_TRUE(last_bottom <= 446);

  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kBook, "For Bushra"));
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kCategories;
  gfx::Canvas canvas = MakeCanvas();
  for (uint16_t selected = 0; selected < ui::kLibraryHomeTileCount; ++selected) {
    state.selected = selected;
    ui::LibraryScreen::render(canvas, state);
    TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
    for (uint16_t row = 0; row < ui::kLibraryHomeTileCount; ++row) {
      int x = 0;
      int y = 0;
      int w = 0;
      int h = 0;
      ui::LibraryScreen::tileRect(row, &x, &y, &w, &h);
      // A point just inside the corner clip: solid black only on the selected tile.
      TEST_ASSERT_EQUAL(row == selected, PixelIsBlackAt(x + 6, y + h / 2));
    }
  }
}

// ---------------------------------------------------------------------------
// ReaderScreen
// ---------------------------------------------------------------------------

void test_reader_screen_chars_and_lines_per_page_are_sane() {
  const uint16_t chars = ui::ReaderCharsPerLine(board::kWidth, 4);
  TEST_ASSERT_TRUE(chars > 0);
  const uint8_t lines = ui::ReaderLinesPerPage(4);
  TEST_ASSERT_TRUE(lines > 0);
  TEST_ASSERT_TRUE(lines <= ui::kReaderMaxLines);
  // A zero scale must not divide by zero -- both functions treat it as 1.
  TEST_ASSERT_TRUE(ui::ReaderCharsPerLine(board::kWidth, 0) > 0);
  TEST_ASSERT_TRUE(ui::ReaderLinesPerPage(0) > 0);
}

void test_reader_screen_render_draws_a_real_paginated_page() {
  static const char kBody[] =
      "In the beginning there was a note.\n\nIt had two paragraphs, so the "
      "pager would have something real to wrap.";
  const uint16_t chars_per_line = ui::ReaderCharsPerLine(board::kWidth, 4);
  const uint8_t lines_per_page = ui::ReaderLinesPerPage(4);

  util::TextPager pager;
  pager.reset(kBody, sizeof(kBody) - 1, chars_per_line, lines_per_page);

  util::LineSlice lines[ui::kReaderMaxLines];
  const uint8_t line_count = pager.linesForPage(1, lines, ui::kReaderMaxLines);
  TEST_ASSERT_TRUE(line_count > 0);

  ui::ReaderState state;
  state.title = "For Bushra";
  state.body = kBody;
  state.lines = lines;
  state.line_count = line_count;
  state.page = 1;
  state.page_count = pager.pageCount();
  state.scale = 4;
  state.chars_per_line = chars_per_line;
  state.lines_per_page = lines_per_page;

  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  ui::ReaderScreen::render(canvas, state);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

void test_reader_screen_render_with_no_lines_does_not_crash() {
  ui::ReaderState state;  // lines == nullptr, line_count == 0
  gfx::Canvas canvas = MakeCanvas();
  ui::ReaderScreen::render(canvas, state);  // must return, not crash
  // Header/footer chrome still draws even with an empty body.
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

// ---------------------------------------------------------------------------
// SelfTestScreen
// ---------------------------------------------------------------------------

void test_selftest_screen_result_text_maps_every_result() {
  TEST_ASSERT_EQUAL_STRING("OK", ui::SelfTestScreen::resultText(ui::TestResult::kPass));
  TEST_ASSERT_EQUAL_STRING("FAIL", ui::SelfTestScreen::resultText(ui::TestResult::kFail));
  TEST_ASSERT_EQUAL_STRING("--", ui::SelfTestScreen::resultText(ui::TestResult::kUntested));
}

void test_selftest_screen_render_draws_something_default_and_with_library() {
  ui::SelfTestState state;  // every field at its default (untested/empty)
  gfx::Canvas canvas = MakeCanvas();
  canvas.clear(gfx::kWhite);
  ui::SelfTestScreen::render(canvas, state);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());

  // The subtitle branches on library_count/library_title -- exercise that
  // path too, not just the "Hardware Test" default.
  state.library_count = 3;
  state.library_title = "The Holy Quran";
  state.sd = ui::TestResult::kPass;
  state.sd_capacity_mb = 59640;
  state.battery_available = true;
  state.battery_percent = 42;
  snprintf(state.battery_remaining, sizeof(state.battery_remaining), "14h");
  canvas.clear(gfx::kWhite);
  ui::SelfTestScreen::render(canvas, state);
  TEST_ASSERT_FALSE(FramebufferIsAllWhite());
}

// ---------------------------------------------------------------------------
// Layout fit on the 800x480 panel
//
// Worst-case content must neither run off the canvas (Canvas::clippedPixels,
// which also catches a centred line wider than the panel) nor, on the three
// framed screens, put ink outside the frame they draw at (4,4) 792x472.
// ---------------------------------------------------------------------------

namespace {

bool PixelIsBlack(int x, int y) {
  const uint32_t index = static_cast<uint32_t>(y) * board::kStrideBytes +
                         static_cast<uint32_t>(x / 8);
  return (g_buffer[index] & (0x80u >> (x & 7))) == 0;
}

bool InkOutsideFrame() {
  for (int y = 0; y < static_cast<int>(board::kHeight); ++y) {
    for (int x = 0; x < static_cast<int>(board::kWidth); ++x) {
      const bool inside = x >= 4 && x <= 795 && y >= 4 && y <= 475;
      if (!inside && PixelIsBlack(x, y)) return true;
    }
  }
  return false;
}

// A title at net::kTitleMaxBytes, in the widest glyph there is (the face is
// fixed-width, so any character is the widest).
const std::string kLongestTitle(net::kTitleMaxBytes - 1, 'W');
// What main.cpp's g_library_status[48] can hold.
const std::string kLongestStatus(47, 'W');
constexpr const char* kLongestAddress = "255.255.255.255:65535";

void AssertFramedScreenFits(const gfx::Canvas& canvas, const char* what) {
  char msg[96];
  snprintf(msg, sizeof(msg), "%s: %lu pixels clipped", what,
           static_cast<unsigned long>(canvas.clippedPixels()));
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, canvas.clippedPixels(), msg);
  snprintf(msg, sizeof(msg), "%s: ink outside the frame", what);
  TEST_ASSERT_FALSE_MESSAGE(InkOutsideFrame(), msg);
}

}  // namespace

void test_library_screen_fits_the_panel_with_worst_case_content() {
  net::LibraryIndex index;
  for (uint8_t i = 0; i < 20; ++i) {
    net::LibraryEntry entry = MakeEntry(static_cast<uint8_t>(i + 1),
                                        qpk::PackageType::kTranslation,
                                        kLongestTitle.c_str());
    entry.package_size = 999ull * 1024ull * 1024ull;  // "999.0MB"
    index.upsert(entry);
  }
  gfx::Canvas canvas = MakeCanvas();

  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kCategories;
  state.transfer_connecting = true;
  ui::LibraryScreen::render(canvas, state);
  AssertFramedScreenFits(canvas, "library categories, connecting");

  state.transfer_connecting = false;
  state.transfer_mode = true;
  state.transfer_address = kLongestAddress;
  ui::LibraryScreen::render(canvas, state);
  AssertFramedScreenFits(canvas, "library categories, transfer address");

  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kTranslation);
  state.selected = 20;
  state.scroll_top = 20 - ui::kLibraryMaxVisibleRows + 1;
  state.status_message = kLongestStatus.c_str();
  ui::LibraryScreen::render(canvas, state);
  AssertFramedScreenFits(canvas, "library items, scrolled, status");
}

void test_selftest_screen_fits_the_panel_with_worst_case_content() {
  ui::SelfTestState s;
  s.library_count = 96;
  s.library_title = kLongestTitle.c_str();
  s.wake_reason_name = "button/rotary";
  s.real_wake_count = 4294967295u;
  s.sd = ui::TestResult::kFail;
  s.sd_detail = "present, no FAT";
  s.input.pulse_position = -2147483647 - 1;
  s.ui_counter = -2147483647 - 1;
  s.refresh_count = 4294967295u;
  s.battery_available = true;
  s.battery_percent = 100;
  snprintf(s.battery_remaining, sizeof(s.battery_remaining), "999d, 23h");
  s.firmware_version = "0.1.0-m1";
  gfx::Canvas canvas = MakeCanvas();

  ui::SelfTestScreen::render(canvas, s);
  AssertFramedScreenFits(canvas, "selftest, default footer");

  s.library_count = 1;
  s.transfer_mode = true;
  s.transfer_address = kLongestAddress;
  ui::SelfTestScreen::render(canvas, s);
  AssertFramedScreenFits(canvas, "selftest, transfer footer");

  s.pairing_mode = true;
  ui::SelfTestScreen::render(canvas, s);
  AssertFramedScreenFits(canvas, "selftest, pairing footer");
}

void test_surah_picker_fits_the_panel() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile file;
  file.reset(mini.file.data(), mini.file.size());
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(reader.open(&file)));

  ui::SurahPickerState state;
  state.reader = &reader;
  gfx::Canvas canvas = MakeCanvas();
  ui::SurahPickerScreen::render(canvas, state);
  AssertFramedScreenFits(canvas, "surah picker");

  // The fixture has only a few surahs, so the real names are checked by
  // arithmetic: the longest "114. Name" row starts at surah_picker_screen.cpp's
  // kRowLeftX (30) and must stop short of its ayah column at kRowRightX (620).
  for (uint16_t id = 1; id <= app::kSurahNameCount; ++id) {
    char row[48];
    snprintf(row, sizeof(row), "%u. %s", static_cast<unsigned>(id),
             app::kSurahNames[id - 1]);
    TEST_ASSERT_TRUE_MESSAGE(30 + gfx::Canvas::textWidth(row, 2) < 620, row);
  }
}

void test_surah_picker_lists_a_translations_surahs_without_a_package() {
  static uint16_t counts[114];
  for (uint16_t i = 0; i < 114; ++i) counts[i] = static_cast<uint16_t>(3 + i * 2);

  ui::SurahPickerState state;  // no reader: the translation reader's list
  state.list_count = 114;
  TEST_ASSERT_EQUAL_UINT16(0, ui::SurahPickerScreen::rowCount(state));  // no counts yet
  state.list_ayah_counts = counts;
  TEST_ASSERT_EQUAL_UINT16(114, ui::SurahPickerScreen::rowCount(state));

  gfx::Canvas canvas = MakeCanvas();
  for (uint16_t selected : {0, 17, 113}) {
    state.selected = selected;
    state.scroll_top = selected >= ui::kSurahPickerMaxVisibleRows
                           ? static_cast<uint16_t>(selected - ui::kSurahPickerMaxVisibleRows + 1)
                           : 0;
    ui::SurahPickerScreen::render(canvas, state);
    AssertFramedScreenFits(canvas, "surah picker, translation list");
  }
}

void test_reader_screen_footer_takes_the_translation_hint_and_notices() {
  gfx::Canvas canvas = MakeCanvas();
  for (uint8_t scale = 3; scale <= 6; ++scale) {
    ui::ReaderState state;
    state.title = "Sahih International - 114 An-Nas";
    state.scale = scale;
    state.chars_per_line = ui::ReaderCharsPerLine(board::kWidth, scale);
    state.lines_per_page = ui::ReaderLinesPerPage(scale);
    state.page = 999;
    state.page_count = 999;
    for (const char* hint : {"OK=surah hold=size MENU=mark",
                             "Bookmarked Sahih International surah 2 p",
                             "No surah list: aligned Quran not on card"}) {
      state.hint = hint;
      ui::ReaderScreen::render(canvas, state);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, canvas.clippedPixels(), hint);
    }
  }
}

void test_reader_screen_fits_the_panel_at_every_scale() {
  const std::string title(net::kTitleMaxBytes - 1, 'W');
  gfx::Canvas canvas = MakeCanvas();
  for (uint8_t scale = 3; scale <= 6; ++scale) {
    const uint16_t chars = ui::ReaderCharsPerLine(board::kWidth, scale);
    const uint8_t lines_per_page = ui::ReaderLinesPerPage(scale);
    const std::string body(static_cast<size_t>(chars) * lines_per_page, 'W');

    util::LineSlice lines[ui::kReaderMaxLines];
    for (uint8_t i = 0; i < lines_per_page; ++i) {
      lines[i].offset = static_cast<uint32_t>(i) * chars;
      lines[i].length = chars;
    }

    ui::ReaderState state;
    state.title = title.c_str();
    state.body = body.c_str();
    state.lines = lines;
    state.line_count = lines_per_page;
    state.page = 999;
    state.page_count = 999;
    state.scale = scale;
    state.chars_per_line = chars;
    state.lines_per_page = lines_per_page;
    ui::ReaderScreen::render(canvas, state);

    char msg[48];
    snprintf(msg, sizeof(msg), "reader at x%u", static_cast<unsigned>(scale));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, canvas.clippedPixels(), msg);
  }
}

void test_quran_screen_fits_the_panel_on_both_paths() {
  ShapedFixture with_layout;
  with_layout.open(true);
  ShapedFixture without;
  without.open(false);
  gfx::Canvas canvas = MakeCanvas();

  ui::QuranState a;
  a.reader = &with_layout.reader;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&a, 1));
  TEST_ASSERT_TRUE(ui::QuranScreen::seekToAyah(&a, a.first_ayah_index));
  a.status_message = kLongestStatus.c_str();
  ui::QuranScreen::render(canvas, a);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());

  ui::QuranState b;
  b.reader = &without.reader;
  TEST_ASSERT_TRUE(ui::QuranScreen::loadSurah(&b, 1));
  ui::QuranScreen::render(canvas, b);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
}

// ---------------------------------------------------------------------------
// HomeScreen -- the table clock
// ---------------------------------------------------------------------------

namespace {

bool InkInRect(int x0, int y0, int w, int h) {
  for (int y = y0; y < y0 + h; ++y) {
    for (int x = x0; x < x0 + w; ++x) {
      if (PixelIsBlack(x, y)) return true;
    }
  }
  return false;
}

ui::HomeState HomeAt(uint16_t year, uint8_t month, uint8_t day, uint8_t hour,
                     uint8_t minute) {
  ui::HomeState s;
  s.time_valid = true;
  s.year = year;
  s.month = month;
  s.day = day;
  s.hour = hour;
  s.minute = minute;
  return s;
}

}  // namespace

void test_home_calendar_maths() {
  TEST_ASSERT_EQUAL_UINT8(1, ui::DayOfWeek(2026, 9, 14));   // a Monday
  TEST_ASSERT_EQUAL_UINT8(6, ui::DayOfWeek(2000, 1, 1));    // a Saturday
  TEST_ASSERT_EQUAL_UINT8(4, ui::DayOfWeek(2024, 2, 29));   // a Thursday
  TEST_ASSERT_EQUAL_UINT8(0, ui::DayOfWeek(2026, 3, 1));    // a Sunday
  TEST_ASSERT_EQUAL_UINT8(29, ui::DaysInMonth(2024, 2));
  TEST_ASSERT_EQUAL_UINT8(28, ui::DaysInMonth(2100, 2));
  TEST_ASSERT_EQUAL_UINT8(29, ui::DaysInMonth(2000, 2));
  TEST_ASSERT_EQUAL_UINT8(30, ui::DaysInMonth(2026, 9));
  TEST_ASSERT_EQUAL_UINT8(31, ui::DaysInMonth(2026, 12));
}

void test_home_clock_rect_is_byte_aligned() {
  // flushWindow() refuses anything else, so the minute tick would silently
  // never reach the glass.
  TEST_ASSERT_EQUAL_INT(0, ui::kHomeClockX % 8);
  TEST_ASSERT_EQUAL_INT(0, ui::kHomeClockW % 8);
  TEST_ASSERT_TRUE(ui::kHomeClockX + ui::kHomeClockW <= ui::kHomePhotoX);
}

void test_home_screen_fits_the_panel_in_every_month() {
  gfx::Canvas canvas = MakeCanvas();
  const std::string status(120, 'W');
  // Every month of a year with 6-row months (August 2026 starts on a
  // Saturday), on its last day, at both week starts, with and without a photo.
  for (uint8_t month = 1; month <= 12; ++month) {
    for (int monday = 0; monday <= 1; ++monday) {
      ui::HomeState s = HomeAt(2026, month, ui::DaysInMonth(2026, month), 23, 59);
      s.week_starts_monday = (monday == 1);
      s.status = status.c_str();
      s.has_photo = (month % 2 == 0);
      ui::HomeScreen::render(canvas, s);
      char msg[48];
      snprintf(msg, sizeof(msg), "home screen, month %u monday=%d",
               static_cast<unsigned>(month), monday);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, canvas.clippedPixels(), msg);
      if (s.has_photo) {
        // The driver draws the photo there; the canvas must leave it empty.
        TEST_ASSERT_FALSE_MESSAGE(
            InkInRect(ui::kHomePhotoX, ui::kHomePhotoY, ui::kHomePhotoW, ui::kHomePhotoH),
            msg);
      }
    }
  }

  ui::HomeState unsynced;
  unsynced.status = status.c_str();
  ui::HomeScreen::render(canvas, unsynced);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
}

void test_home_clock_update_touches_only_the_clock_rect() {
  gfx::Canvas canvas = MakeCanvas();
  ui::HomeState s = HomeAt(2026, 9, 14, 10, 59);
  s.has_photo = true;
  ui::HomeScreen::render(canvas, s);
  const std::vector<uint8_t> before(g_buffer, g_buffer + board::kFramebufferBytes);

  s.hour = 11;
  s.minute = 0;
  ui::HomeScreen::renderClock(canvas, s);

  bool changed_inside = false;
  for (int y = 0; y < static_cast<int>(board::kHeight); ++y) {
    for (int x = 0; x < static_cast<int>(board::kWidth); ++x) {
      const uint32_t i = static_cast<uint32_t>(y) * board::kStrideBytes + x / 8;
      const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
      const bool differs = ((g_buffer[i] ^ before[i]) & mask) != 0;
      const bool inside = x >= ui::kHomeClockX && x < ui::kHomeClockX + ui::kHomeClockW &&
                          y >= ui::kHomeClockY && y < ui::kHomeClockY + ui::kHomeClockH;
      if (differs && !inside) {
        char msg[64];
        snprintf(msg, sizeof(msg), "pixel (%d,%d) changed outside the clock rect", x, y);
        TEST_FAIL_MESSAGE(msg);
      }
      if (differs && inside) changed_inside = true;
    }
  }
  TEST_ASSERT_TRUE(changed_inside);
}

// PageImageScreen ---------------------------------------------------------------

bool PageTestInk(const gfx::Canvas& canvas, int x, int y) {
  // MakeCanvas() is rotation 0 with no seam: bit set = white.
  const uint32_t index = static_cast<uint32_t>(y) * ((canvas.width() + 7) / 8) + x / 8;
  return (canvas.buffer()[index] & (0x80 >> (x % 8))) == 0;
}

int PageTestInkCount(const gfx::Canvas& canvas) {
  int ink = 0;
  for (int y = 0; y < canvas.height(); ++y) {
    for (int x = 0; x < canvas.width(); ++x) ink += PageTestInk(canvas, x, y) ? 1 : 0;
  }
  return ink;
}

void test_page_image_screen_turns_the_page_upright_onto_the_canvas() {
  gfx::Canvas canvas = MakeCanvas();
  std::vector<uint8_t> bits(60 * 800, 0);
  bits[0] = 0x80;               // page pixel (0, 0), top-left
  bits[799 * 60 + 59] = 0x01;   // page pixel (479, 799), bottom-right

  ui::PageImageState state;
  state.bits = bits.data();
  state.width = 480;
  state.height = 800;
  state.turn = ui::PortraitTurn::kClockwise;
  TEST_ASSERT_TRUE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
  TEST_ASSERT_EQUAL_INT(2, PageTestInkCount(canvas));
  // Clockwise: the page's top edge runs down the canvas's right edge.
  TEST_ASSERT_TRUE(PageTestInk(canvas, 799, 0));
  TEST_ASSERT_TRUE(PageTestInk(canvas, 0, 479));

  state.turn = ui::PortraitTurn::kCounterClockwise;
  TEST_ASSERT_TRUE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_EQUAL_INT(2, PageTestInkCount(canvas));
  TEST_ASSERT_TRUE(PageTestInk(canvas, 0, 479));
  TEST_ASSERT_TRUE(PageTestInk(canvas, 799, 0));
  int cx = 0;
  int cy = 0;
  ui::PageImageScreen::mapPoint(canvas, state, 0, 0, &cx, &cy);
  TEST_ASSERT_EQUAL_INT(0, cx);
  TEST_ASSERT_EQUAL_INT(479, cy);
}

void test_page_image_screen_refuses_a_missing_or_oversized_page() {
  gfx::Canvas canvas = MakeCanvas();
  ui::PageImageState state;
  state.width = 480;
  state.height = 800;
  TEST_ASSERT_FALSE(ui::PageImageScreen::render(canvas, state));  // no bits

  std::vector<uint8_t> bits(60 * 900, 0xFF);
  state.bits = bits.data();
  state.height = 900;  // turned, taller than the canvas is wide
  TEST_ASSERT_FALSE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_EQUAL_INT(0, PageTestInkCount(canvas));
}

std::string Fold(const char* utf8) {
  char out[256];
  const uint32_t n = util::FoldToAscii(utf8, static_cast<uint32_t>(strlen(utf8)), out, sizeof(out));
  return std::string(out, n);
}

void test_ascii_fold_keeps_translation_text_readable() {
  TEST_ASSERT_EQUAL_STRING("plain ASCII stays", Fold("plain ASCII stays").c_str());
  TEST_ASSERT_EQUAL_STRING("\"It's\" -- he said...",
                           Fold("\xE2\x80\x9CIt\xE2\x80\x99s\xE2\x80\x9D \xE2\x80\x94 he said\xE2\x80\xA6").c_str());
  // Transliteration marks: a-macron, h-underdot, ayin.
  TEST_ASSERT_EQUAL_STRING("Qur'an, ahadith, 'Ali",
                           Fold("Qur\xE2\x80\x99\xC4\x81n, a\xE1\xB8\xA5\xC4\x81\x64\xC4\xABth, \xCA\xBF\x41l\xC4\xAB").c_str());
  TEST_ASSERT_EQUAL_STRING("cafe", Fold("caf\xC3\xA9").c_str());
  TEST_ASSERT_EQUAL_STRING("x?y", Fold("x\xE4\xB8\xADy").c_str());  // no ASCII form: '?'
  TEST_ASSERT_EQUAL_STRING("a?", Fold("a\xE2\x80").c_str());         // cut-off sequence
  TEST_ASSERT_EQUAL_STRING("ab", Fold("a\xE2\x80\x8B" "b").c_str());  // zero-width space dropped

  char small[4];
  TEST_ASSERT_EQUAL_UINT32(4, util::FoldToAscii("abc\xE2\x80\xA6", 6, small, sizeof(small)));  // never overruns
}

void test_bookmarks_screen_rows_places_and_fit() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kBook, kLongestTitle.c_str()));
  net::Bookmarks marks;
  for (uint32_t i = 0; i < 30; ++i) {
    net::Bookmark mark;
    memset(mark.content_id, (i % 2) != 0 ? 1 : 9, sizeof(mark.content_id));  // 9: not on the card
    mark.kind = i % 3 == 0 ? net::BookmarkKind::kQuran : net::BookmarkKind::kPageBook;
    mark.a = 100 + i;
    mark.c = 99;
    marks.add(mark);
  }

  ui::BookmarksState state;
  state.index = &index;
  state.bookmarks = &marks;
  TEST_ASSERT_EQUAL_UINT16(30, ui::BookmarksScreen::rowCount(state));
  state.has_last_read = true;
  memset(state.last_read.content_id, 1, sizeof(state.last_read.content_id));
  state.last_read.a = 211;
  TEST_ASSERT_EQUAL_UINT16(31, ui::BookmarksScreen::rowCount(state));

  uint16_t index_out = 99;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::BookmarkRow::kLastRead),
                        static_cast<int>(ui::BookmarksScreen::rowKind(state, 0, &index_out)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::BookmarkRow::kBookmark),
                        static_cast<int>(ui::BookmarksScreen::rowKind(state, 1, &index_out)));
  TEST_ASSERT_EQUAL_UINT16(0, index_out);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ui::BookmarkRow::kNone),
                        static_cast<int>(ui::BookmarksScreen::rowKind(state, 31, nullptr)));

  gfx::Canvas canvas = MakeCanvas();
  const uint16_t selections[] = {0, 11, 12, 30};
  for (uint16_t selected : selections) {
    state.selected = selected;
    ui::BookmarksScreen::render(canvas, state);
    AssertFramedScreenFits(canvas, "bookmarks");
    const uint16_t top = ui::BookmarksScreen::scrollTop(state);
    TEST_ASSERT_TRUE(top <= selected && selected < top + ui::kBookmarksVisibleRows);
  }

  char place[40];
  ui::BookmarksScreen::describe(state.last_read, place, sizeof(place));
  TEST_ASSERT_EQUAL_STRING("page 212", place);
  net::Bookmark quran;
  quran.kind = net::BookmarkKind::kQuran;
  quran.a = 18;
  quran.c = 3;
  ui::BookmarksScreen::describe(quran, place, sizeof(place));
  TEST_ASSERT_EQUAL_STRING("Surah 18, screen 3", place);

  net::Bookmarks none;
  ui::BookmarksState empty;
  empty.index = &index;
  empty.bookmarks = &none;
  ui::BookmarksScreen::render(canvas, empty);
  AssertFramedScreenFits(canvas, "bookmarks, empty");
}

void test_page_image_screen_ribbon_marks_a_bookmarked_page() {
  gfx::Canvas canvas = MakeCanvas();
  std::vector<uint8_t> bits(60 * 800, 0);
  ui::PageImageState state;
  state.bits = bits.data();
  state.width = 480;
  state.height = 800;
  int cx = 0;
  int cy = 0;
  ui::PageImageScreen::mapPoint(canvas, state, 480 - 20 - 12, 10, &cx, &cy);  // inside the ribbon

  TEST_ASSERT_TRUE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_FALSE(PageTestInk(canvas, cx, cy));
  state.bookmarked = true;
  TEST_ASSERT_TRUE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_TRUE(PageTestInk(canvas, cx, cy));
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
}

void test_library_books_shelf_fits_and_frames_the_selection() {
  net::LibraryIndex index;
  for (uint8_t i = 0; i < 23; ++i) {
    index.upsert(MakeEntry(static_cast<uint8_t>(i + 1), qpk::PackageType::kBook,
                           kLongestTitle.c_str()));
  }
  static uint8_t cover[qpk::kCoverPixelBytes];
  for (uint32_t i = 0; i < sizeof(cover); ++i) cover[i] = static_cast<uint8_t>(i * 37);

  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  TEST_ASSERT_TRUE(ui::LibraryScreen::isShelf(state));

  gfx::Canvas canvas = MakeCanvas();
  const uint16_t selections[] = {0, 9, 10, 23};
  for (uint16_t selected : selections) {
    state.selected = selected;
    for (uint8_t slot = 0; slot < ui::kLibraryShelfPageTiles; ++slot) {
      state.shelf_covers[slot] = (slot % 2) != 0 ? cover : nullptr;  // both kinds of cover
    }
    ui::LibraryScreen::render(canvas, state);
    AssertFramedScreenFits(canvas, "books shelf");
    TEST_ASSERT_EQUAL_UINT16(selected - selected % ui::kLibraryShelfPageTiles,
                             ui::LibraryScreen::shelfPageStart(state));
    int x = 0;
    int y = 0;
    ui::LibraryScreen::shelfCoverRect(
        static_cast<uint8_t>(selected % ui::kLibraryShelfPageTiles), &x, &y);
    TEST_ASSERT_TRUE(PixelIsBlackAt(x - 5, y + 20));  // the selection frame
  }

  state.category = static_cast<uint16_t>(qpk::PackageType::kTranslation);
  TEST_ASSERT_FALSE(ui::LibraryScreen::isShelf(state));
}

void test_library_books_shelf_grey_mode_leaves_covers_to_the_grey_layer() {
  net::LibraryIndex index;
  for (uint8_t i = 0; i < 12; ++i) {
    index.upsert(MakeEntry(static_cast<uint8_t>(i + 1), qpk::PackageType::kBook, "Book"));
  }
  static uint8_t cover[qpk::kCoverPixelBytes];
  for (uint32_t i = 0; i < sizeof(cover); ++i) cover[i] = static_cast<uint8_t>(i * 37 + 11);

  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  state.grey_covers = true;
  state.selected = 7;  // slot 7: second shelf row
  for (uint8_t slot = 1; slot < ui::kLibraryShelfPageTiles; ++slot) state.shelf_covers[slot] = cover;

  gfx::Canvas canvas = MakeCanvas();
  ui::LibraryScreen::render(canvas, state);
  AssertFramedScreenFits(canvas, "books shelf, grey covers");

  int x = 0;
  int y = 0;
  ui::LibraryScreen::shelfCoverRect(3, &x, &y);
  // Inside a cover: white on the canvas, the grey layer draws it.
  for (int r = 2; r < 142; r += 7) {
    for (int c = 2; c < 106; c += 5) TEST_ASSERT_FALSE(PixelIsBlackAt(x + c, y + r));
  }
  // No frame round the selected cover...
  ui::LibraryScreen::shelfCoverRect(7, &x, &y);
  TEST_ASSERT_FALSE(PixelIsBlackAt(x - 5, y + 20));
  // ...its title box is inverted instead, inside its row's title band.
  int bx = 0;
  int by = 0;
  int bw = 0;
  int bh = 0;
  ui::LibraryScreen::shelfTitleBand(1, &bx, &by, &bw, &bh);
  TEST_ASSERT_TRUE(PixelIsBlackAt(x - 16, y + 144 + 6));
  TEST_ASSERT_TRUE(y + 144 + 6 >= by && y + 144 + 6 < by + bh);

  // Bands: byte-aligned, on the panel, and clear of every cover's pixels.
  for (uint8_t shelf_row = 0; shelf_row < ui::kLibraryShelfRows; ++shelf_row) {
    ui::LibraryScreen::shelfTitleBand(shelf_row, &bx, &by, &bw, &bh);
    TEST_ASSERT_EQUAL_INT(0, bx % 8);
    TEST_ASSERT_EQUAL_INT(0, bw % 8);
    TEST_ASSERT_TRUE(bx >= 0 && bx + bw <= 800 && by + bh <= 480 && bh > 0);
    for (uint8_t slot = 0; slot < ui::kLibraryShelfPageTiles; ++slot) {
      ui::LibraryScreen::shelfCoverRect(slot, &x, &y);
      const bool overlaps = by < y + 144 && y < by + bh;
      TEST_ASSERT_FALSE(overlaps);
    }
  }

  // The grey layer: each present cover at its place, white where there is none.
  static uint8_t grey[ui::kShelfGreyBytes];
  ui::LibraryScreen::composeShelfGrey(state, grey);
  const auto level_at = [](int px, int py) {
    const int col = px - ui::kShelfGreyX;
    const int row = py - ui::kShelfGreyY;
    return (grey[row * (ui::kShelfGreyW / 4) + (col >> 2)] >> (6 - 2 * (col & 3))) & 3;
  };
  for (uint8_t slot = 1; slot < 4; ++slot) {
    ui::LibraryScreen::shelfCoverRect(slot, &x, &y);
    for (int r = 0; r < 144; r += 13) {
      for (int c = 0; c < 108; c += 11) {
        const int expected = (cover[r * 27 + (c >> 2)] >> (6 - 2 * (c & 3))) & 3;
        TEST_ASSERT_EQUAL_INT(expected, level_at(x + c, y + r));
      }
    }
  }
  ui::LibraryScreen::shelfCoverRect(0, &x, &y);  // "< Back": no cover
  TEST_ASSERT_EQUAL_INT(3, level_at(x + 50, y + 70));
  // "< Back" and 12 books are 13 rows: page 1 holds rows 10..12, so a cover
  // handed in for slot 3 (row 13) belongs to nothing and is left out.
  state.selected = 12;
  ui::LibraryScreen::composeShelfGrey(state, grey);
  ui::LibraryScreen::shelfCoverRect(3, &x, &y);  // row 13 does not exist
  TEST_ASSERT_EQUAL_INT(3, level_at(x + 50, y + 70));
}

void test_page_image_screen_draws_the_page_number_and_jump_box_upright() {
  gfx::Canvas canvas = MakeCanvas();
  std::vector<uint8_t> bits(60 * 800, 0);
  ui::PageImageState state;
  state.bits = bits.data();
  state.width = 480;
  state.height = 800;
  state.turn = ui::PortraitTurn::kClockwise;
  state.page = 487;
  state.page_count = 913;

  TEST_ASSERT_TRUE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
  const int footer_ink = PageTestInkCount(canvas);
  TEST_ASSERT_TRUE(footer_ink > 0);
  // The box's bottom-right corner sits 6 px in from the page's own corner.
  int cx = 0;
  int cy = 0;
  ui::PageImageScreen::mapPoint(canvas, state, 473, 793, &cx, &cy);
  TEST_ASSERT_TRUE(PageTestInk(canvas, cx, cy));
  // ...and nothing is drawn near the page's top-left.
  ui::PageImageScreen::mapPoint(canvas, state, 10, 10, &cx, &cy);
  TEST_ASSERT_FALSE(PageTestInk(canvas, cx, cy));

  state.jump_mode = true;
  state.jump_target = 500;
  TEST_ASSERT_TRUE(ui::PageImageScreen::render(canvas, state));
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
  TEST_ASSERT_TRUE(PageTestInkCount(canvas) > footer_ink);
}

void setUp() {}
void tearDown() {}


// ---------------------------------------------------------------------------
// Options menu (ui/options_menu.h)
//
// The menu exists because OK meant something different on every screen and
// nothing said so. These tests are about the promise that replaced it: every
// screen offers a named list, nothing destructive happens without a question
// first, and a finger lands on the row it is over.
// ---------------------------------------------------------------------------

namespace {

ui::OptionsMenuState BuildFor(ui::MenuScreen screen) {
  ui::MenuContext context;
  context.screen = screen;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(context, &state);
  return state;
}

bool MenuHas(const ui::OptionsMenuState& state, ui::MenuAction action) {
  for (uint8_t i = 0; i < state.count; ++i) {
    if (state.items[i].action == action) return true;
  }
  return false;
}

}  // namespace

void test_menu_every_screen_offers_a_way_out() {
  const ui::MenuScreen screens[] = {
      ui::MenuScreen::kHome,   ui::MenuScreen::kLibrary, ui::MenuScreen::kSelfTest,
      ui::MenuScreen::kReader, ui::MenuScreen::kPages,   ui::MenuScreen::kQuran,
      ui::MenuScreen::kSurahPicker, ui::MenuScreen::kBookmarks};
  // Both extremes: nothing available, and every optional row at once. The
  // library's fullest list reaches kOptionsMaxItems exactly, so the loaded
  // pass is where a way out would be squeezed off the end.
  for (int loaded = 0; loaded < 2; ++loaded) {
    for (ui::MenuScreen screen : screens) {
      ui::MenuContext context;
      context.screen = screen;
      if (loaded != 0) {
        context.translation = true;
        context.transfer_on = true;
        context.page_jump = true;
        context.has_bookmarks = true;
        context.bookmark_selected = true;
        context.library_in_category = true;
      }
      ui::OptionsMenuState state;
      ui::OptionsMenu::build(context, &state);

      TEST_ASSERT_TRUE(state.count > 0);
      TEST_ASSERT_TRUE(state.count <= ui::kOptionsMaxItems);
      TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kBack));
      for (uint8_t i = 0; i < state.count; ++i) {
        TEST_ASSERT_NOT_NULL(state.items[i].label);
        TEST_ASSERT_TRUE(state.items[i].label[0] != '\0');
        TEST_ASSERT_TRUE(state.items[i].action != ui::MenuAction::kNone);
      }
    }
  }
}

void test_menu_carries_the_actions_that_used_to_be_hidden_holds() {
  // Hold OK used to bookmark in a book, delete on the bookmarks list and
  // toggle transfer mode elsewhere. Each is now a row where it belongs.
  ui::MenuContext reading;
  reading.screen = ui::MenuScreen::kReader;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(reading, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kBookmarkHere));
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kTextSize));
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kChooseSurah));

  ui::MenuContext translation = reading;
  translation.translation = true;
  ui::OptionsMenu::build(translation, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kChooseSurah));

  ui::MenuContext bookmarks;
  bookmarks.screen = ui::MenuScreen::kBookmarks;
  bookmarks.bookmark_selected = true;
  ui::OptionsMenu::build(bookmarks, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kDeleteBookmark));

  // Nothing selected: no delete row to hit by accident.
  bookmarks.bookmark_selected = false;
  ui::OptionsMenu::build(bookmarks, &state);
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kDeleteBookmark));

  ui::MenuContext library;
  library.screen = ui::MenuScreen::kLibrary;
  ui::OptionsMenu::build(library, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kTransferMode));
}

void test_menu_library_rows_name_the_level_they_are_on() {
  // Two levels: shelves, and the books on one. The row that opens the
  // highlight has to say which, and only inside a shelf is there a way up.
  ui::MenuContext context;
  context.screen = ui::MenuScreen::kLibrary;
  ui::OptionsMenuState shelves;
  ui::OptionsMenu::build(context, &shelves);
  TEST_ASSERT_FALSE(MenuHas(shelves, ui::MenuAction::kLeaveCategory));

  context.library_in_category = true;
  ui::OptionsMenuState books;
  ui::OptionsMenu::build(context, &books);
  TEST_ASSERT_TRUE(MenuHas(books, ui::MenuAction::kLeaveCategory));

  const char* shelf_label = "";
  const char* book_label = "";
  for (uint8_t i = 0; i < shelves.count; ++i) {
    if (shelves.items[i].action == ui::MenuAction::kOpenSelected) {
      shelf_label = shelves.items[i].label;
    }
  }
  for (uint8_t i = 0; i < books.count; ++i) {
    if (books.items[i].action == ui::MenuAction::kOpenSelected) {
      book_label = books.items[i].label;
    }
  }
  TEST_ASSERT_TRUE(strstr(shelf_label, "shelf") != nullptr);
  TEST_ASSERT_TRUE(strstr(book_label, "book") != nullptr);
}

void test_menu_transfer_row_says_which_way_it_goes() {
  ui::MenuContext context;
  context.screen = ui::MenuScreen::kLibrary;
  ui::OptionsMenuState off;
  ui::OptionsMenu::build(context, &off);
  context.transfer_on = true;
  ui::OptionsMenuState on;
  ui::OptionsMenu::build(context, &on);

  const char* off_label = "";
  const char* on_label = "";
  for (uint8_t i = 0; i < off.count; ++i) {
    if (off.items[i].action == ui::MenuAction::kTransferMode) off_label = off.items[i].label;
  }
  for (uint8_t i = 0; i < on.count; ++i) {
    if (on.items[i].action == ui::MenuAction::kTransferMode) on_label = on.items[i].label;
  }
  TEST_ASSERT_TRUE(strstr(off_label, "Transfer") != nullptr);
  TEST_ASSERT_TRUE(strstr(on_label, "Stop") != nullptr);
}

void test_menu_hides_bookmark_rows_until_there_are_bookmarks() {
  ui::MenuContext context;
  context.screen = ui::MenuScreen::kLibrary;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kBookmarks));
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kContinueReading));

  context.has_bookmarks = true;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kBookmarks));
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kContinueReading));
}

void test_menu_omits_sleep_on_a_device_that_never_sleeps() {
  ui::MenuContext context;
  context.screen = ui::MenuScreen::kHome;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kSleep));

  context.table_clock = true;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kSleep));
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kBack));
}

void test_menu_page_jump_rows_follow_the_mode() {
  ui::MenuContext context;
  context.screen = ui::MenuScreen::kPages;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kGoToPage));
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kNextChapter));

  context.page_jump = true;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kNextChapter));
}

void test_menu_factory_reset_asks_first_and_defaults_to_no() {
  ui::MenuContext context;
  context.screen = ui::MenuScreen::kSelfTest;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(context, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kFactoryReset));

  TEST_ASSERT_TRUE(ui::OptionsMenu::buildConfirm(ui::MenuAction::kFactoryReset, &state));
  TEST_ASSERT_TRUE(state.confirming);
  TEST_ASSERT_TRUE(state.question[0] != '\0');
  // The highlight sits on the harmless row: erasing has to be chosen.
  TEST_ASSERT_EQUAL(ui::MenuAction::kBack, ui::OptionsMenu::selectedAction(state));
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kFactoryResetConfirm));

  // Nothing else asks: an ordinary row runs straight away.
  ui::OptionsMenuState untouched = state;
  TEST_ASSERT_FALSE(ui::OptionsMenu::buildConfirm(ui::MenuAction::kTextSize, &untouched));
  TEST_ASSERT_TRUE(untouched.confirming);  // left exactly as it was
}

void test_menu_selection_wraps_both_ways() {
  ui::OptionsMenuState state = BuildFor(ui::MenuScreen::kSurahPicker);
  const uint8_t last = static_cast<uint8_t>(state.count - 1);

  state.selected = 0;
  ui::OptionsMenu::move(&state, -1);
  TEST_ASSERT_EQUAL_UINT8(last, state.selected);
  ui::OptionsMenu::move(&state, 1);
  TEST_ASSERT_EQUAL_UINT8(0, state.selected);
  // A fast spin of more than a whole list still lands somewhere real.
  ui::OptionsMenu::move(&state, static_cast<int16_t>(state.count * 3 + 1));
  TEST_ASSERT_TRUE(state.selected < state.count);
}

void test_menu_rowAt_finds_the_row_under_a_finger() {
  const ui::OptionsMenuState state = BuildFor(ui::MenuScreen::kLibrary);
  // Row geometry (options_menu.cpp): first row at y = 84, 40 px pitch,
  // x from 24 to 776.
  TEST_ASSERT_EQUAL_INT(0, ui::OptionsMenu::rowAt(state, 400, 84));
  TEST_ASSERT_EQUAL_INT(0, ui::OptionsMenu::rowAt(state, 30, 120));
  TEST_ASSERT_EQUAL_INT(1, ui::OptionsMenu::rowAt(state, 400, 124));
  TEST_ASSERT_EQUAL_INT(state.count - 1,
                        ui::OptionsMenu::rowAt(state, 400, 84 + (state.count - 1) * 40));

  // Above the list, below the last row, and off to the sides: nothing.
  TEST_ASSERT_EQUAL_INT(-1, ui::OptionsMenu::rowAt(state, 400, 40));
  TEST_ASSERT_EQUAL_INT(-1, ui::OptionsMenu::rowAt(state, 400, 84 + state.count * 40));
  TEST_ASSERT_EQUAL_INT(-1, ui::OptionsMenu::rowAt(state, 10, 100));
  TEST_ASSERT_EQUAL_INT(-1, ui::OptionsMenu::rowAt(state, 790, 100));
}

void test_menu_renders_inside_the_panel() {
  gfx::Canvas canvas = MakeCanvas();

  ui::MenuContext context;
  context.screen = ui::MenuScreen::kLibrary;
  context.has_bookmarks = true;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(context, &state);
  state.touch_hint = true;
  ui::OptionsMenu::render(canvas, state);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());

  // The confirmation, with its longer subtitle, fits too.
  ui::OptionsMenu::buildConfirm(ui::MenuAction::kFactoryReset, &state);
  ui::OptionsMenu::render(canvas, state);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
}


// ---------------------------------------------------------------------------
// Screen and touch setup (ui/setup_screen.h)
//
// The screen that answers "which way round is the touch layer?" on the device
// instead of by reflashing. Its whole promise is that hitting both targets
// means the orientation is right -- so the tests are about the targets.
// ---------------------------------------------------------------------------

void test_setup_targets_are_apart_in_both_axes() {
  // One corner cannot tell a swapped pair of axes from a straight one, so the
  // second target must differ in x AND agree in y -- swapping then sends it
  // somewhere else entirely.
  TEST_ASSERT_EQUAL_INT(1, ui::SetupScreen::targetAt(ui::kSetupTarget1X + 10,
                                                     ui::kSetupTarget1Y + 10));
  TEST_ASSERT_EQUAL_INT(2, ui::SetupScreen::targetAt(ui::kSetupTarget2X + 10,
                                                     ui::kSetupTarget2Y + 10));
  TEST_ASSERT_EQUAL_INT(0, ui::SetupScreen::targetAt(400, 240));
  TEST_ASSERT_EQUAL_INT(0, ui::SetupScreen::targetAt(0, 0));
  TEST_ASSERT_TRUE(ui::kSetupTarget2X > ui::kSetupTarget1X + ui::kSetupTargetSize);
  TEST_ASSERT_EQUAL_INT(ui::kSetupTarget1Y, ui::kSetupTarget2Y);
}

void test_setup_records_taps_and_needs_both_targets() {
  ui::SetupState state;
  TEST_ASSERT_FALSE(ui::SetupScreen::bothTargetsHit(state));

  ui::SetupScreen::noteTap(&state, ui::kSetupTarget1X + 5, ui::kSetupTarget1Y + 5);
  TEST_ASSERT_TRUE(state.has_tap);
  TEST_ASSERT_TRUE(state.target1_hit);
  TEST_ASSERT_FALSE(ui::SetupScreen::bothTargetsHit(state));

  // A tap in the middle of the screen is recorded but hits nothing.
  ui::SetupScreen::noteTap(&state, 400, 300);
  TEST_ASSERT_EQUAL_INT16(400, state.tap_x);
  TEST_ASSERT_FALSE(state.target2_hit);

  ui::SetupScreen::noteTap(&state, ui::kSetupTarget2X + 5, ui::kSetupTarget2Y + 5);
  TEST_ASSERT_TRUE(ui::SetupScreen::bothTargetsHit(state));
  TEST_ASSERT_EQUAL_UINT32(3, state.tap_count);
}

void test_setup_changing_orientation_forgets_the_old_hits() {
  ui::SetupState state;
  ui::SetupScreen::noteTap(&state, ui::kSetupTarget1X + 5, ui::kSetupTarget1Y + 5);
  ui::SetupScreen::noteTap(&state, ui::kSetupTarget2X + 5, ui::kSetupTarget2Y + 5);
  TEST_ASSERT_TRUE(ui::SetupScreen::bothTargetsHit(state));

  // Those hits were under the old orientation; keeping them would claim the
  // job is done when the mapping just changed underneath.
  ui::SetupScreen::nextTouchOrientation(&state);
  TEST_ASSERT_EQUAL_UINT8(1, state.touch_orientation);
  TEST_ASSERT_FALSE(state.target1_hit);
  TEST_ASSERT_FALSE(state.target2_hit);
  TEST_ASSERT_FALSE(state.has_tap);
}

void test_setup_orientation_cycles_through_all_eight() {
  ui::SetupState state;
  for (uint8_t i = 0; i < util::kTouchOrientationCount; ++i) {
    TEST_ASSERT_EQUAL_UINT8(i, state.touch_orientation);
    ui::SetupScreen::nextTouchOrientation(&state);
  }
  TEST_ASSERT_EQUAL_UINT8(0, state.touch_orientation);
}

void test_setup_flip_toggles_between_the_two_landscape_rotations() {
  ui::SetupState state;
  TEST_ASSERT_EQUAL_UINT8(0, state.rotation);
  ui::SetupScreen::flipPicture(&state);
  TEST_ASSERT_EQUAL_UINT8(2, state.rotation);
  ui::SetupScreen::flipPicture(&state);
  TEST_ASSERT_EQUAL_UINT8(0, state.rotation);
}

void test_setup_turning_the_picture_forgets_the_old_hits_too() {
  // The trap this exists to stop: hit both boxes, then turn the picture the
  // right way up, and the screen would still claim the touch orientation was
  // settled -- while the flip has just moved every position 180 degrees under
  // a touch layer that did not move at all.
  ui::SetupState state;
  ui::SetupScreen::noteTap(&state, ui::kSetupTarget1X + 5, ui::kSetupTarget1Y + 5);
  ui::SetupScreen::noteTap(&state, ui::kSetupTarget2X + 5, ui::kSetupTarget2Y + 5);
  TEST_ASSERT_TRUE(ui::SetupScreen::bothTargetsHit(state));

  ui::SetupScreen::flipPicture(&state);
  TEST_ASSERT_FALSE(ui::SetupScreen::bothTargetsHit(state));
  TEST_ASSERT_FALSE(state.has_tap);
  // The orientation itself is untouched: only what was proven about it goes.
  TEST_ASSERT_EQUAL_UINT8(0, state.touch_orientation);
}

void test_setup_renders_inside_the_panel() {
  gfx::Canvas canvas = MakeCanvas();
  ui::SetupState state;
  state.touch_present = true;
  ui::SetupScreen::render(canvas, state);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());

  // With a tap mark at the very edge, both targets hit, and the longest
  // status line: still nothing off the canvas.
  state.touch_orientation = 7;
  ui::SetupScreen::noteTap(&state, 799, 479);
  state.target1_hit = true;
  state.target2_hit = true;
  ui::SetupScreen::render(canvas, state);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());

  // And with no touch panel at all.
  ui::SetupState plain;
  ui::SetupScreen::render(canvas, plain);
  TEST_ASSERT_EQUAL_UINT32(0, canvas.clippedPixels());
}

void test_menu_offers_the_setup_screen_where_it_can_be_reached() {
  ui::MenuContext home;
  home.screen = ui::MenuScreen::kHome;
  ui::OptionsMenuState state;
  ui::OptionsMenu::build(home, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kScreenSetup));

  ui::MenuContext self_test;
  self_test.screen = ui::MenuScreen::kSelfTest;
  ui::OptionsMenu::build(self_test, &state);
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kScreenSetup));

  // The setup screen's own menu is a way out and nothing else: offering it
  // again from inside itself would go nowhere.
  ui::MenuContext setup;
  setup.screen = ui::MenuScreen::kScreenSetup;
  ui::OptionsMenu::build(setup, &state);
  TEST_ASSERT_FALSE(MenuHas(state, ui::MenuAction::kScreenSetup));
  TEST_ASSERT_TRUE(MenuHas(state, ui::MenuAction::kBack));
}


// ---------------------------------------------------------------------------
// Which library row a finger is on (ui::LibraryScreen::rowAt)
// ---------------------------------------------------------------------------

void test_library_row_at_maps_list_rows_including_the_scroll_offset() {
  net::LibraryIndex index;
  for (int i = 1; i <= 20; ++i) {
    index.upsert(MakeEntry(static_cast<uint8_t>(i), qpk::PackageType::kQuran,
                           "A Package"));
  }
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kQuran);

  // Rows start at y = 84 with a 28 px pitch (library_screen.cpp).
  TEST_ASSERT_EQUAL_INT32(0, ui::LibraryScreen::rowAt(state, 300, 84));
  TEST_ASSERT_EQUAL_INT32(0, ui::LibraryScreen::rowAt(state, 300, 111));
  TEST_ASSERT_EQUAL_INT32(1, ui::LibraryScreen::rowAt(state, 300, 112));

  // Scrolled: the same place on the glass is a different row, which is the
  // whole reason this cannot be a plain division at the call site.
  state.scroll_top = 5;
  TEST_ASSERT_EQUAL_INT32(5, ui::LibraryScreen::rowAt(state, 300, 84));
  TEST_ASSERT_EQUAL_INT32(6, ui::LibraryScreen::rowAt(state, 300, 112));
}

void test_library_row_at_rejects_the_header_footer_and_empty_space() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kQuran, "The Holy Quran"));
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kQuran);

  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, 300, 40));   // title
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, 300, 460));  // footer
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, 5, 90));     // margin
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, 790, 90));

  // Below the last real row: 2 rows here (the package and Back), so the
  // third row's worth of glass is empty and must not open anything.
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, 300, 84 + 2 * 28));
}

void test_library_row_at_hits_shelf_tiles_including_their_titles() {
  net::LibraryIndex index;
  for (int i = 1; i <= 6; ++i) {
    index.upsert(MakeEntry(static_cast<uint8_t>(i), qpk::PackageType::kBook,
                           "A Book"));
  }
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  TEST_ASSERT_TRUE(ui::LibraryScreen::isShelf(state));

  int x = 0;
  int y = 0;
  ui::LibraryScreen::shelfCoverRect(0, &x, &y);
  const uint16_t start = ui::LibraryScreen::shelfPageStart(state);

  // The cover itself...
  TEST_ASSERT_EQUAL_INT32(start, ui::LibraryScreen::rowAt(
                                     state, static_cast<int16_t>(x + 10),
                                     static_cast<int16_t>(y + 10)));
  // ...and the title strip under it, which reads as part of the same tile.
  TEST_ASSERT_EQUAL_INT32(start, ui::LibraryScreen::rowAt(
                                     state, static_cast<int16_t>(x + 10),
                                     static_cast<int16_t>(y + 150)));

  int x1 = 0;
  int y1 = 0;
  ui::LibraryScreen::shelfCoverRect(1, &x1, &y1);
  const int32_t second = ui::LibraryScreen::rowAt(state, static_cast<int16_t>(x1 + 10),
                                                  static_cast<int16_t>(y1 + 10));
  TEST_ASSERT_EQUAL_INT32(start + 1, second);

  // Above the shelf: the header, not a tile.
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, static_cast<int16_t>(x + 10), 20));
}

void test_library_row_at_hits_the_home_tiles() {
  // The screen the device opens on, and the one place a wrong branch would
  // be found first: the top level is tiles, not 28 px rows.
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kBook, "For Bushra"));
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kCategories;

  const uint16_t total = ui::LibraryScreen::rowCount(state);
  TEST_ASSERT_EQUAL_UINT16(ui::kLibraryHomeTileCount, total);

  for (uint16_t row = 0; row < total; ++row) {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    ui::LibraryScreen::tileRect(row, &x, &y, &w, &h);
    // A finger in the middle of a tile hits that tile and no other.
    const int16_t cx = static_cast<int16_t>(x + w / 2);
    const int16_t cy = static_cast<int16_t>(y + h / 2);
    TEST_ASSERT_EQUAL_INT32(row, ui::LibraryScreen::rowAt(state, cx, cy));
    // ...and the corners belong to it too.
    TEST_ASSERT_EQUAL_INT32(row, ui::LibraryScreen::rowAt(
                                     state, static_cast<int16_t>(x),
                                     static_cast<int16_t>(y)));
    TEST_ASSERT_EQUAL_INT32(row, ui::LibraryScreen::rowAt(
                                     state, static_cast<int16_t>(x + w - 1),
                                     static_cast<int16_t>(y + h - 1)));
  }

  // The header above the tiles is not a tile.
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(state, 400, 20));
}

void test_library_row_at_is_minus_one_when_there_is_nothing_to_hit() {
  ui::LibraryState empty;  // no index at all
  empty.view = ui::LibraryView::kItems;
  empty.category = static_cast<uint16_t>(qpk::PackageType::kQuran);
  TEST_ASSERT_EQUAL_INT32(-1, ui::LibraryScreen::rowAt(empty, 300, 90));
}


// ---------------------------------------------------------------------------
// Tapping the other two lists (BookmarksScreen::rowAt, SurahPickerScreen::rowAt)
// ---------------------------------------------------------------------------

void test_bookmarks_row_at_follows_the_scroll_the_render_used() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kPageBook, "A Picture Book"));
  net::Bookmarks marks;
  for (uint32_t i = 0; i < 30; ++i) {
    net::Bookmark mark;
    memset(mark.content_id, 1, sizeof(mark.content_id));
    mark.kind = net::BookmarkKind::kPageBook;
    mark.a = 100 + i;
    marks.add(mark);
  }
  ui::BookmarksState state;
  state.index = &index;
  state.bookmarks = &marks;

  // Rows start at y = 84, 28 px apart (bookmarks_screen.cpp).
  TEST_ASSERT_EQUAL_INT32(0, ui::BookmarksScreen::rowAt(state, 300, 84));
  TEST_ASSERT_EQUAL_INT32(1, ui::BookmarksScreen::rowAt(state, 300, 112));

  // This list has no stored scroll: it is derived from the selection, so the
  // hit test has to ask scrollTop() rather than assume the top of the list.
  state.selected = 20;
  const uint16_t top = ui::BookmarksScreen::scrollTop(state);
  TEST_ASSERT_TRUE(top > 0);
  TEST_ASSERT_EQUAL_INT32(top, ui::BookmarksScreen::rowAt(state, 300, 84));
  TEST_ASSERT_EQUAL_INT32(top + 3, ui::BookmarksScreen::rowAt(state, 300, 84 + 3 * 28));

  // Off the list: header, footer, margins.
  TEST_ASSERT_EQUAL_INT32(-1, ui::BookmarksScreen::rowAt(state, 300, 40));
  TEST_ASSERT_EQUAL_INT32(-1, ui::BookmarksScreen::rowAt(state, 300, 460));
  TEST_ASSERT_EQUAL_INT32(-1, ui::BookmarksScreen::rowAt(state, 5, 100));

  // An empty list has nothing to hit.
  net::Bookmarks none;
  ui::BookmarksState empty;
  empty.index = &index;
  empty.bookmarks = &none;
  TEST_ASSERT_EQUAL_INT32(-1, ui::BookmarksScreen::rowAt(empty, 300, 90));
}

void test_bookmarks_row_at_stops_at_the_last_real_row() {
  net::LibraryIndex index;
  index.upsert(MakeEntry(1, qpk::PackageType::kPageBook, "A Picture Book"));
  net::Bookmarks marks;
  net::Bookmark mark;
  memset(mark.content_id, 1, sizeof(mark.content_id));
  mark.kind = net::BookmarkKind::kPageBook;
  mark.a = 7;
  marks.add(mark);

  ui::BookmarksState state;
  state.index = &index;
  state.bookmarks = &marks;
  TEST_ASSERT_EQUAL_UINT16(1, ui::BookmarksScreen::rowCount(state));
  TEST_ASSERT_EQUAL_INT32(0, ui::BookmarksScreen::rowAt(state, 300, 90));
  // The empty glass below the only row must not delete or open anything.
  TEST_ASSERT_EQUAL_INT32(-1, ui::BookmarksScreen::rowAt(state, 300, 84 + 28));
}

void test_surah_picker_row_at_is_zero_based_like_selected() {
  ui::SurahPickerState state;
  const uint16_t ayahs[] = {7, 286, 200, 176, 120};
  state.list_count = 5;
  state.list_ayah_counts = ayahs;
  TEST_ASSERT_EQUAL_UINT16(5, ui::SurahPickerScreen::rowCount(state));

  // Row 0 is surah 1: main.cpp puts this straight into the 0-based
  // g_surah_picker_selected, so an off-by-one here opens the wrong surah.
  TEST_ASSERT_EQUAL_INT32(0, ui::SurahPickerScreen::rowAt(state, 300, 84));
  TEST_ASSERT_EQUAL_INT32(1, ui::SurahPickerScreen::rowAt(state, 300, 112));
  TEST_ASSERT_EQUAL_INT32(4, ui::SurahPickerScreen::rowAt(state, 300, 84 + 4 * 28));

  // Past the end of a short list, and off to the sides.
  TEST_ASSERT_EQUAL_INT32(-1, ui::SurahPickerScreen::rowAt(state, 300, 84 + 5 * 28));
  TEST_ASSERT_EQUAL_INT32(-1, ui::SurahPickerScreen::rowAt(state, 5, 90));
  TEST_ASSERT_EQUAL_INT32(-1, ui::SurahPickerScreen::rowAt(state, 300, 40));

  // Scrolled down the full 114: the top row is whatever scroll_top says.
  state.scroll_top = 40;
  state.list_count = 114;
  TEST_ASSERT_EQUAL_INT32(40, ui::SurahPickerScreen::rowAt(state, 300, 84));
  TEST_ASSERT_EQUAL_INT32(51, ui::SurahPickerScreen::rowAt(state, 300, 84 + 11 * 28));
  // The 13th line is past the 12 this screen draws.
  TEST_ASSERT_EQUAL_INT32(-1, ui::SurahPickerScreen::rowAt(state, 300, 84 + 12 * 28));

  // No reader and no list: nothing to hit.
  ui::SurahPickerState nothing;
  TEST_ASSERT_EQUAL_INT32(-1, ui::SurahPickerScreen::rowAt(nothing, 300, 90));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_surah_picker_row_count_is_zero_with_no_package_open);
  RUN_TEST(test_surah_picker_row_count_matches_the_package_not_a_hardcoded_114);
  RUN_TEST(test_surah_picker_render_draws_something_for_a_real_package);
  RUN_TEST(test_surah_picker_render_with_no_package_does_not_crash);
  RUN_TEST(test_surah_picker_render_clamps_a_selection_past_the_end);

  RUN_TEST(test_quran_screen_renders_real_glyphs_from_a_shaped_package);
  RUN_TEST(test_quran_screen_pages_across_the_surah_boundary);
  RUN_TEST(test_quran_screen_draws_a_real_ayah_marker_when_the_package_has_one);
  RUN_TEST(test_quran_screen_falls_back_when_a_claimed_marker_is_not_in_the_atlas);
  RUN_TEST(test_quran_screen_reports_unsupported_for_an_unshaped_package);
  RUN_TEST(test_quran_screen_uses_the_layout_path_only_when_layout_data_is_present);
  RUN_TEST(test_seek_to_ayah_finds_the_line_the_ayah_starts_on);
  RUN_TEST(test_quran_screen_layout_paging_advances_by_whole_lines);

  RUN_TEST(test_library_screen_categories_row_count_is_fixed);
  RUN_TEST(test_library_screen_items_row_count_is_count_plus_back);
  RUN_TEST(test_library_screen_row_kind_classifies_every_row);
  RUN_TEST(test_library_screen_category_type_and_label_agree);
  RUN_TEST(test_library_screen_entry_of_type_filters_by_type);
  RUN_TEST(test_library_screen_render_draws_something_in_both_views);
  RUN_TEST(test_library_screen_footer_shows_transfer_address_and_yields_to_status);
  RUN_TEST(test_library_icons_are_not_blank);
  RUN_TEST(test_library_home_tiles_fit_and_only_the_selected_one_is_inverted);

  RUN_TEST(test_reader_screen_chars_and_lines_per_page_are_sane);
  RUN_TEST(test_reader_screen_render_draws_a_real_paginated_page);
  RUN_TEST(test_reader_screen_render_with_no_lines_does_not_crash);

  RUN_TEST(test_selftest_screen_result_text_maps_every_result);
  RUN_TEST(test_selftest_screen_render_draws_something_default_and_with_library);

  RUN_TEST(test_library_screen_fits_the_panel_with_worst_case_content);
  RUN_TEST(test_selftest_screen_fits_the_panel_with_worst_case_content);
  RUN_TEST(test_surah_picker_fits_the_panel);
  RUN_TEST(test_surah_picker_lists_a_translations_surahs_without_a_package);
  RUN_TEST(test_reader_screen_footer_takes_the_translation_hint_and_notices);
  RUN_TEST(test_reader_screen_fits_the_panel_at_every_scale);
  RUN_TEST(test_quran_screen_fits_the_panel_on_both_paths);

  RUN_TEST(test_home_calendar_maths);
  RUN_TEST(test_home_clock_rect_is_byte_aligned);
  RUN_TEST(test_home_screen_fits_the_panel_in_every_month);
  RUN_TEST(test_home_clock_update_touches_only_the_clock_rect);
  RUN_TEST(test_page_image_screen_turns_the_page_upright_onto_the_canvas);
  RUN_TEST(test_page_image_screen_refuses_a_missing_or_oversized_page);
  RUN_TEST(test_page_image_screen_draws_the_page_number_and_jump_box_upright);
  RUN_TEST(test_library_books_shelf_fits_and_frames_the_selection);
  RUN_TEST(test_library_books_shelf_grey_mode_leaves_covers_to_the_grey_layer);
  RUN_TEST(test_bookmarks_screen_rows_places_and_fit);
  RUN_TEST(test_ascii_fold_keeps_translation_text_readable);
  RUN_TEST(test_page_image_screen_ribbon_marks_a_bookmarked_page);
  RUN_TEST(test_menu_every_screen_offers_a_way_out);
  RUN_TEST(test_menu_carries_the_actions_that_used_to_be_hidden_holds);
  RUN_TEST(test_menu_library_rows_name_the_level_they_are_on);
  RUN_TEST(test_menu_transfer_row_says_which_way_it_goes);
  RUN_TEST(test_menu_hides_bookmark_rows_until_there_are_bookmarks);
  RUN_TEST(test_menu_omits_sleep_on_a_device_that_never_sleeps);
  RUN_TEST(test_menu_page_jump_rows_follow_the_mode);
  RUN_TEST(test_menu_factory_reset_asks_first_and_defaults_to_no);
  RUN_TEST(test_menu_selection_wraps_both_ways);
  RUN_TEST(test_menu_rowAt_finds_the_row_under_a_finger);
  RUN_TEST(test_menu_renders_inside_the_panel);

  RUN_TEST(test_setup_targets_are_apart_in_both_axes);
  RUN_TEST(test_setup_records_taps_and_needs_both_targets);
  RUN_TEST(test_setup_changing_orientation_forgets_the_old_hits);
  RUN_TEST(test_setup_orientation_cycles_through_all_eight);
  RUN_TEST(test_setup_flip_toggles_between_the_two_landscape_rotations);
  RUN_TEST(test_setup_turning_the_picture_forgets_the_old_hits_too);
  RUN_TEST(test_setup_renders_inside_the_panel);
  RUN_TEST(test_menu_offers_the_setup_screen_where_it_can_be_reached);

  RUN_TEST(test_library_row_at_maps_list_rows_including_the_scroll_offset);
  RUN_TEST(test_library_row_at_rejects_the_header_footer_and_empty_space);
  RUN_TEST(test_library_row_at_hits_shelf_tiles_including_their_titles);
  RUN_TEST(test_library_row_at_hits_the_home_tiles);
  RUN_TEST(test_library_row_at_is_minus_one_when_there_is_nothing_to_hit);

  RUN_TEST(test_bookmarks_row_at_follows_the_scroll_the_render_used);
  RUN_TEST(test_bookmarks_row_at_stops_at_the_last_real_row);
  RUN_TEST(test_surah_picker_row_at_is_zero_based_like_selected);

  return UNITY_END();
}
