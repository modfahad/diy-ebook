// render_ui_preview.cpp -- dumps a ui:: screen's canvas to a raw 1bpp file
// for visual inspection, now that a host compiler makes this possible.
//
// Host-only dev tool, not part of the product build or the test suites (it
// has its own main() and would collide with theirs) -- build and run it
// directly:
//
//   g++ -std=c++11 -Iinclude -Itest tools/render_ui_preview.cpp
//       drivers/gfx/canvas.cpp drivers/gfx/font5x7.cpp storage/library_index.cpp
//       wifi/protocol.cpp qpk/crc32.cpp qpk/qpk_format.cpp qpk/qpk_reader.cpp
//       qpk/glyph_blitter.cpp ui/library_screen.cpp ui/quran_screen.cpp
//       -o preview.exe
//   ./preview.exe library-categories out.raw
//   ./preview.exe quran-package out.raw PACKAGE.qpk 78
//
// Then scripts/raw_canvas_to_png.py turns out.raw into a real PNG. This
// exists because icon/layout work is otherwise designed blind: the only
// previous way to see a screen was to flash real hardware.
//
// Deliberately plain -- no rotation, no seam, no controller-RAM padding
// (rotation=0, seam_x=0, width_memory=width): those exist to match the
// physical panel's memory layout, which only matters once bytes are headed
// to the SSD1683s. For a human-readable preview the identity mapping
// (mapPixel with rotation 0 -- see canvas.cpp) is what makes the raw bytes
// directly convertible to an image without first undoing a 180-degree flip.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "board/board_crowpanel_579.h"
#include "gfx/canvas.h"
#include "net/library_index.h"
#include "net/photo_store.h"
#include "qpk/memory_file.h"
#include "qpk/qpk_format.h"
#include "qpk/qpk_reader.h"
#include "test_qpk/qpk_test_package.h"
#include "ui/home_screen.h"
#include "ui/library_screen.h"
#include "ui/page_image_screen.h"
#include "app/surah_names.h"
#include "qpk/translation_text.h"
#include "ui/quran_screen.h"
#include "ui/reader_screen.h"
#include "ui/selftest_screen.h"
#include "ui/surah_picker_screen.h"
#include "util/text_pager.h"

namespace {

constexpr uint16_t kWidth = board::kWidth;
constexpr uint16_t kHeight = board::kHeight;
constexpr uint16_t kStride = (kWidth + 7) / 8;
uint8_t g_buffer[static_cast<uint32_t>(kStride) * kHeight];

gfx::Canvas MakeCanvas() {
  return gfx::Canvas(g_buffer, kWidth, kHeight, kWidth, kHeight, kStride, 0, 0);
}

void RenderLibraryCategories() {
  net::LibraryIndex index;
  net::LibraryEntry entry;
  entry.content_id[0] = 1;
  entry.type = static_cast<uint16_t>(qpk::PackageType::kQuran);
  snprintf(entry.title, sizeof(entry.title), "The Holy Quran (readable)");
  index.upsert(entry);
  net::LibraryEntry book;
  book.content_id[0] = 2;
  book.type = static_cast<uint16_t>(qpk::PackageType::kBook);
  snprintf(book.title, sizeof(book.title), "For Bushra");
  index.upsert(book);

  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kCategories;

  gfx::Canvas canvas = MakeCanvas();
  ui::LibraryScreen::render(canvas, state);
}

void RenderLibraryTransferMode() {
  net::LibraryIndex index;
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kCategories;
  state.transfer_mode = true;
  state.transfer_address = "10.102.150.184:8080";

  gfx::Canvas canvas = MakeCanvas();
  ui::LibraryScreen::render(canvas, state);
}

// Kept alive for the whole program, not just this function: qpk::Reader
// holds a pointer to the backing qpk::MemoryFile, and QuranScreen::render()
// re-reads through it during the draw (see quran_screen.h's own comment on
// why it is not a pure renderer).
qpktest::MiniShapedQuran g_shaped_quran;
qpk::MemoryFile g_shaped_quran_file;
qpk::Reader g_shaped_quran_reader;

void RenderQuranShaped() {
  g_shaped_quran = qpktest::BuildMiniShapedQuran();
  g_shaped_quran_file.reset(g_shaped_quran.file.data(), g_shaped_quran.file.size());
  g_shaped_quran_reader.open(&g_shaped_quran_file);

  ui::QuranState state;
  state.reader = &g_shaped_quran_reader;
  ui::QuranScreen::loadSurah(&state, 1);

  gfx::Canvas canvas = MakeCanvas();
  ui::QuranScreen::render(canvas, state);
}

// A REAL package off disk, rendered by the same ui::QuranScreen the device
// runs. Slurped whole into RAM and handed to qpk::MemoryFile -- 5MB is
// nothing on a host, and it keeps this tool free of a second IFile
// implementation whose bugs would be indistinguishable from the renderer's.
//
// This is the only way to see real Arabic and real Uthmani ayah markers
// without the SD card, which is why it exists: the card lives in the device.
std::vector<uint8_t> g_package_bytes;
qpk::MemoryFile g_package_file;
qpk::Reader g_package_reader;

bool RenderQuranPackage(const char* path, uint16_t surah_id) {
  FILE* in = fopen(path, "rb");
  if (in == nullptr) {
    fprintf(stderr, "could not open %s\n", path);
    return false;
  }
  fseek(in, 0, SEEK_END);
  const long size = ftell(in);
  fseek(in, 0, SEEK_SET);
  if (size <= 0) {
    fprintf(stderr, "%s is empty\n", path);
    fclose(in);
    return false;
  }
  g_package_bytes.resize(static_cast<size_t>(size));
  const size_t read = fread(g_package_bytes.data(), 1, g_package_bytes.size(), in);
  fclose(in);
  if (read != g_package_bytes.size()) {
    fprintf(stderr, "short read on %s\n", path);
    return false;
  }

  g_package_file.reset(g_package_bytes.data(), g_package_bytes.size());
  if (g_package_reader.open(&g_package_file) != qpk::Error::kOk) {
    fprintf(stderr, "%s does not open as a QPK package\n", path);
    return false;
  }

  uint16_t marker_first = 0;
  uint16_t marker_count = 0;
  if (g_package_reader.ayahMarkerGlyphs(&marker_first, &marker_count)) {
    printf("ayah markers: glyph ids %u..%u (%u numbers)\n",
           static_cast<unsigned>(marker_first),
           static_cast<unsigned>(marker_first + marker_count - 1),
           static_cast<unsigned>(marker_count));
  } else {
    printf("ayah markers: none -- the Latin \"(n)\" fallback will be drawn\n");
  }

  ui::QuranState state;
  state.reader = &g_package_reader;
  if (!ui::QuranScreen::loadSurah(&state, surah_id)) {
    fprintf(stderr, "surah %u is not in this package\n",
            static_cast<unsigned>(surah_id));
    return false;
  }

  // Same bridge main.cpp uses: an ayah-addressed position becomes a line
  // cursor. Without it the layout path would draw line 0 for every surah.
  ui::QuranScreen::seekToAyah(&state, state.first_ayah_index);

  gfx::Canvas canvas = MakeCanvas();
  const ui::QuranRenderResult result = ui::QuranScreen::render(canvas, state);
  printf("  layout %s, first_line_index %u, lines drawn %u, next line %u\n",
         result.used_layout ? "yes" : "no (fill-and-wrap)",
         static_cast<unsigned>(state.first_line_index),
         static_cast<unsigned>(result.lines_drawn),
         static_cast<unsigned>(result.next_line_index));
  printf("surah %u: %u ayah(s), %u glyph(s) blitted, next ayah index %u\n",
         static_cast<unsigned>(surah_id),
         static_cast<unsigned>(result.ayahs_drawn),
         static_cast<unsigned>(result.glyphs_drawn),
         static_cast<unsigned>(result.next_ayah_index));
  return !result.unsupported;
}

// Worst-case-ish content for eyeballing the 800x480 layouts: long titles,
// scrolled lists, and the longest footers each screen can show.
void RenderLibraryItems() {
  static net::LibraryIndex index;
  for (uint8_t i = 0; i < 20; ++i) {
    net::LibraryEntry entry;
    entry.content_id[0] = static_cast<uint8_t>(i + 1);
    entry.type = static_cast<uint16_t>(qpk::PackageType::kBook);
    snprintf(entry.title, sizeof(entry.title),
             "Book %u - a title long enough to need truncating in the list",
             static_cast<unsigned>(i + 1));
    entry.package_size = (i + 1) * 123456ull;
    index.upsert(entry);
  }
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  state.selected = 14;
  state.scroll_top = 3;

  gfx::Canvas canvas = MakeCanvas();
  ui::LibraryScreen::render(canvas, state);
}

void RenderSelfTest() {
  ui::SelfTestState s;
  s.rotary = ui::TestResult::kPass;
  s.menu = ui::TestResult::kPass;
  s.sd = ui::TestResult::kPass;
  s.eink = ui::TestResult::kPass;
  s.psram = ui::TestResult::kPass;
  s.sd_capacity_mb = 59640;
  s.wake_reason_name = "button/rotary";
  s.real_wake_count = 42;
  s.ui_counter = -17;
  s.refresh_count = 311;
  s.library_count = 5;
  s.library_title = "The Holy Quran (shaped, with Saheeh International)";
  s.firmware_version = "0.1.0-m1";
  s.transfer_mode = true;
  s.transfer_address = "192.168.100.200:8080";

  gfx::Canvas canvas = MakeCanvas();
  ui::SelfTestScreen::render(canvas, s);
}

qpktest::MiniQuran g_mini_quran;
qpk::MemoryFile g_mini_quran_file;
qpk::Reader g_mini_quran_reader;

void RenderSurahPicker() {
  g_mini_quran = qpktest::BuildMiniQuran();
  g_mini_quran_file.reset(g_mini_quran.file.data(), g_mini_quran.file.size());
  g_mini_quran_reader.open(&g_mini_quran_file);

  ui::SurahPickerState state;
  state.reader = &g_mini_quran_reader;
  state.selected = 1;

  gfx::Canvas canvas = MakeCanvas();
  ui::SurahPickerScreen::render(canvas, state);
}

void RenderReader() {
  static const char kBody[] =
      "In the name of God, the Most Gracious, the Most Merciful. This is "
      "placeholder text for checking the reader's layout on the 800x480 "
      "panel: how many characters fit on a line, how many lines fit on a "
      "page, and whether the footer still clears the last line.\n\n"
      "A second paragraph, so the pager has a break to keep.";
  constexpr uint8_t kScale = 4;
  const uint16_t chars = ui::ReaderCharsPerLine(kWidth, kScale);
  const uint8_t lines_per_page = ui::ReaderLinesPerPage(kScale);
  util::TextPager pager;
  pager.reset(kBody, sizeof(kBody) - 1, chars, lines_per_page);
  util::LineSlice lines[ui::kReaderMaxLines];

  ui::ReaderState state;
  state.title = "Reader preview";
  state.body = kBody;
  state.lines = lines;
  state.line_count = pager.linesForPage(1, lines, ui::kReaderMaxLines);
  state.page = 1;
  state.page_count = pager.pageCount();
  state.scale = kScale;
  state.chars_per_line = chars;
  state.lines_per_page = lines_per_page;

  gfx::Canvas canvas = MakeCanvas();
  ui::ReaderScreen::render(canvas, state);
}

// The table clock. The canvas is 1bpp and leaves the photo rectangle empty for
// the driver to fill; for a preview the synthetic photo below is dithered
// down to black and white into it, so the layout reads as it will on glass.
void RenderHome(bool with_photo) {
  ui::HomeState state;
  state.time_valid = true;
  state.year = 2026;
  state.month = 9;
  state.day = 14;
  state.hour = 18;
  state.minute = 42;
  state.has_photo = with_photo;
  state.status = "Wi-Fi 192.168.1.23:8080  synced 18:00  photo 3/12";

  gfx::Canvas canvas = MakeCanvas();
  ui::HomeScreen::render(canvas, state);
  if (!with_photo) return;

  static const uint8_t kBayer[4][4] = {
      {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int y = 0; y < ui::kHomePhotoH; ++y) {
    for (int x = 0; x < ui::kHomePhotoW; ++x) {
      // A soft diagonal gradient with a dark disc: something photo-like.
      const int dx = x - 200;
      const int dy = y - 260;
      int v = (x + y) * 255 / (ui::kHomePhotoW + ui::kHomePhotoH);
      if (dx * dx + dy * dy < 110 * 110) v = v / 3;
      const bool black = v * 16 / 256 <= kBayer[y % 4][x % 4];
      if (black) canvas.setPixel(ui::kHomePhotoX + x, ui::kHomePhotoY + y, gfx::kBlack);
    }
  }
}

bool SlurpFile(const char* path, std::vector<uint8_t>* out) {
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    fprintf(stderr, "could not open %s\n", path);
    return false;
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  out->resize(size > 0 ? static_cast<size_t>(size) : 0);
  const size_t got = size > 0 ? fread(out->data(), 1, out->size(), file) : 0;
  fclose(file);
  return got == out->size();
}

// A real translation package, one surah, through the text reader: what the
// device's Translation reader shows, with surah bounds from the aligned Quran.
bool RenderTranslationPackage(const char* translation_path, const char* quran_path,
                              uint16_t surah_id) {
  static std::vector<uint8_t> translation_bytes;
  static std::vector<uint8_t> quran_bytes;
  if (!SlurpFile(translation_path, &translation_bytes) || !SlurpFile(quran_path, &quran_bytes)) {
    return false;
  }
  static qpk::MemoryFile translation_file;
  static qpk::MemoryFile quran_file;
  translation_file.reset(translation_bytes.data(), translation_bytes.size());
  quran_file.reset(quran_bytes.data(), quran_bytes.size());
  static qpk::Reader translation;
  static qpk::Reader quran;
  if (translation.open(&translation_file) != qpk::Error::kOk ||
      quran.open(&quran_file) != qpk::Error::kOk) {
    fprintf(stderr, "a package did not open\n");
    return false;
  }
  qpk::SurahRecord surah;
  if (surah_id < 1 || surah_id > 114 || quran.getSurah(surah_id, &surah) != qpk::Error::kOk) {
    fprintf(stderr, "no surah %u\n", static_cast<unsigned>(surah_id));
    return false;
  }

  static char text[256 * 1024];
  int length = snprintf(text, sizeof(text), "Surah %u: %s\n\n", static_cast<unsigned>(surah_id),
                        app::kSurahNames[surah_id - 1]);
  length += static_cast<int>(qpk::AppendTranslationVerses(
      translation, surah.first_ayah_index, surah.ayah_count, text + length,
      static_cast<uint32_t>(sizeof(text) - 1 - length)));
  text[length] = 0;

  const uint8_t scale = 4;
  const uint16_t chars = ui::ReaderCharsPerLine(kWidth, scale);
  const uint8_t lines_per_page = ui::ReaderLinesPerPage(scale);
  util::TextPager pager;
  pager.reset(text, static_cast<uint32_t>(length), chars, lines_per_page);
  static util::LineSlice lines[ui::kReaderMaxLines];

  char name[64] = {0};
  uint32_t name_length = 0;
  translation.metadata(qpk::MetadataKey::kTitle, name, sizeof(name) - 1, &name_length);
  static char title[96];
  snprintf(title, sizeof(title), "%.28s - %u %.20s", name, static_cast<unsigned>(surah_id),
           app::kSurahNames[surah_id - 1]);

  ui::ReaderState state;
  state.title = title;
  state.body = text;
  state.scale = scale;
  state.chars_per_line = chars;
  state.lines_per_page = lines_per_page;
  state.page = 1;
  state.page_count = pager.pageCount();
  state.line_count = pager.linesForPage(1, lines, ui::kReaderMaxLines);
  state.lines = lines;
  gfx::Canvas canvas = MakeCanvas();
  ui::ReaderScreen::render(canvas, state);
  printf("surah %u: %u ayahs, %d bytes, %u pages\n", static_cast<unsigned>(surah_id),
         static_cast<unsigned>(surah.ayah_count), length, static_cast<unsigned>(pager.pageCount()));
  return true;
}

// The Books shelf: 13 books, every other one with a made-up gradient cover.
void RenderLibraryBooks() {
  static net::LibraryIndex index;
  const char* titles[] = {"The International Jew", "Riyad as-Salihin", "Tafsir Notes",
                          "A Short History", "Sealed Nectar"};
  for (uint8_t i = 0; i < 13; ++i) {
    net::LibraryEntry entry;
    entry.content_id[0] = static_cast<uint8_t>(i + 1);
    entry.type = static_cast<uint16_t>(qpk::PackageType::kBook);
    snprintf(entry.title, sizeof(entry.title), "%s", titles[i % 5]);
    index.upsert(entry);
  }
  static uint8_t cover[qpk::kCoverPixelBytes];
  for (int row = 0; row < qpk::kCoverHeight; ++row) {
    for (int col = 0; col < qpk::kCoverWidth; ++col) {
      const int level = (row * 4 / qpk::kCoverHeight + (col > 30 && col < 78 && row > 40 && row < 90 ? 2 : 0)) % 4;
      cover[row * (qpk::kCoverWidth / 4) + (col >> 2)] |=
          static_cast<uint8_t>(level << (6 - 2 * (col & 3)));
    }
  }
  ui::LibraryState state;
  state.index = &index;
  state.view = ui::LibraryView::kItems;
  state.category = static_cast<uint16_t>(qpk::PackageType::kBook);
  state.selected = 3;
  for (uint8_t slot = 0; slot < ui::kLibraryShelfPageTiles; ++slot) {
    state.shelf_covers[slot] = (slot % 2) != 0 ? cover : nullptr;
  }
  gfx::Canvas canvas = MakeCanvas();
  ui::LibraryScreen::render(canvas, state);
}

// A made-up page picture (docs/qpk-format.md 9d) through ui::PageImageScreen,
// with its page number and, for "pages-jump", the "Go to page" box.
void RenderPages(bool jump) {
  constexpr int kPageW = 480;
  constexpr int kPageH = 800;
  constexpr int kPageStride = (kPageW + 7) / 8;
  std::vector<uint8_t> paper(static_cast<size_t>(kPageStride) * kPageH);
  gfx::Canvas page(paper.data(), kPageW, kPageH, kPageW, kPageH, kPageStride, 0, 0);
  page.clear(gfx::kWhite);
  page.drawText(24, 30, "CHAPTER VII", 3, gfx::kBlack);
  for (int line = 0; line < 30; ++line) {
    if (line >= 12 && line < 20) continue;  // room for a figure
    page.drawText(24, 80 + line * 22,
                  line % 5 == 4 ? "the end of a paragraph." : "Text kept as the PDF printed it",
                  2, gfx::kBlack);
  }
  page.drawRect(60, 80 + 12 * 22, 360, 8 * 22 - 8, gfx::kBlack);
  page.drawText(192, 80 + 15 * 22, "Figure 3", 2, gfx::kBlack);
  // Canvas memory is 1 = white; page pictures are 1 = black.
  for (uint8_t& byte : paper) byte = static_cast<uint8_t>(~byte);

  gfx::Canvas canvas = MakeCanvas();
  ui::PageImageState state;
  state.bits = paper.data();
  state.width = kPageW;
  state.height = kPageH;
  state.turn = ui::PortraitTurn::kClockwise;
  state.page = 212;
  state.page_count = 913;
  state.jump_mode = jump;
  state.jump_target = 250;
  state.jump_chapter = "Chapter VII: The Scope of Jewish Dictatorship";
  ui::PageImageScreen::render(canvas, state);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <scene> <output.raw>\n"
                    "       %s quran-package <output.raw> <file.qpk> [surah]\n"
                    "  scenes: library-categories, library-transfer, "
                    "library-items, selftest, surah-picker, reader, "
                    "home, home-empty, quran-shaped, quran-package\n",
           argv[0], argv[0]);
    return 2;
  }

  const char* scene = argv[1];
  if (strcmp(scene, "library-categories") == 0) {
    RenderLibraryCategories();
  } else if (strcmp(scene, "library-transfer") == 0) {
    RenderLibraryTransferMode();
  } else if (strcmp(scene, "library-items") == 0) {
    RenderLibraryItems();
  } else if (strcmp(scene, "selftest") == 0) {
    RenderSelfTest();
  } else if (strcmp(scene, "surah-picker") == 0) {
    RenderSurahPicker();
  } else if (strcmp(scene, "reader") == 0) {
    RenderReader();
  } else if (strcmp(scene, "home") == 0) {
    RenderHome(true);
  } else if (strcmp(scene, "home-empty") == 0) {
    RenderHome(false);
  } else if (strcmp(scene, "library-books") == 0) {
    RenderLibraryBooks();
  } else if (strcmp(scene, "pages") == 0) {
    RenderPages(false);
  } else if (strcmp(scene, "pages-jump") == 0) {
    RenderPages(true);
  } else if (strcmp(scene, "quran-shaped") == 0) {
    RenderQuranShaped();
  } else if (strcmp(scene, "translation-package") == 0) {
    if (argc < 5) {
      fprintf(stderr, "translation-package needs <translation.qpk> <quran.qpk> [surah]\n");
      return 2;
    }
    const uint16_t surah_id = argc >= 6 ? static_cast<uint16_t>(atoi(argv[5])) : 1;
    if (!RenderTranslationPackage(argv[3], argv[4], surah_id)) return 1;
  } else if (strcmp(scene, "quran-package") == 0) {
    if (argc < 4) {
      fprintf(stderr, "quran-package needs a .qpk path\n");
      return 2;
    }
    const uint16_t surah_id =
        argc >= 5 ? static_cast<uint16_t>(atoi(argv[4])) : 1;
    if (!RenderQuranPackage(argv[3], surah_id)) return 1;
  } else {
    fprintf(stderr, "unknown scene: %s\n", scene);
    return 2;
  }

  FILE* out = fopen(argv[2], "wb");
  if (out == nullptr) {
    fprintf(stderr, "could not open %s for writing\n", argv[2]);
    return 1;
  }
  fwrite(g_buffer, 1, sizeof(g_buffer), out);
  fclose(out);
  printf("wrote %s: %ux%u, stride %u, %zu bytes\n", argv[2],
        static_cast<unsigned>(kWidth), static_cast<unsigned>(kHeight),
        static_cast<unsigned>(kStride), sizeof(g_buffer));
  return 0;
}
