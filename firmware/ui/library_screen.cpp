#include "ui/library_screen.h"

#include <stdio.h>
#include <string.h>

#include "app/app_config.h"
#include "app/library_icons.h"
#include "gfx/font5x7.h"
#include "net/protocol.h"
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
constexpr int kFooterTextY = 454;     // 14 px tall at kBodyScale -> 467

// Column widths are sized off the font, not guessed: "TRANSLATION" is this
// build's longest PackageTypeName() result, at
// (kGlyphWidth + 1) * kBodyScale = 12px/char -- 11 chars needs 132px, which
// the previous 110px type column didn't have, so the size column started
// overlapping it on real hardware. kTypeColumnW gives that word 12px of
// headroom rather than an exact fit.
constexpr int kCharW = (gfx::kGlyphWidth + 1) * kBodyScale;
constexpr int kTypeColumnW = 12 * kCharW;
constexpr int kColumnGap = 10;

constexpr int kRowLeftX = 30;
constexpr int kRowTypeX = 480;
constexpr int kRowSizeX = kRowTypeX + kTypeColumnW + kColumnGap;  // 634
constexpr int kRowTop = 84;
constexpr int kRowStep = 28;
// 84 + 12 * 28 = 420; the scroll indicator below it ends at 436, clear of
// the footer rule at 446.
constexpr int kListBottom = kRowTop + kLibraryMaxVisibleRows * kRowStep;

// Inside a category every row is the same type, so the type column earns
// nothing and its width goes to the title instead -- which is what actually
// gets truncated ("Al-Fatihah - Saheeh Internation..").
constexpr int kItemTitleMaxWidthPx = kRowSizeX - kRowLeftX - kColumnGap;

// Icons only on the categories view (the "home screen" -- five rows, each a
// different destination, worth telling apart at a glance) -- not the items
// view, where every row in a category is already the same type and an icon
// would repeat itself five times for nothing.
constexpr int kIconGap = 6;
constexpr int kRowIconTextX = kRowLeftX + app::kLibraryIconSize + kIconGap;

void DrawIcon(gfx::Canvas& canvas, int x, int y,
             const uint8_t icon[][2], gfx::Color color) {
  for (uint8_t row = 0; row < app::kLibraryIconSize; ++row) {
    for (uint8_t col = 0; col < app::kLibraryIconSize; ++col) {
      if (icon[row][col >> 3] & (0x80 >> (col & 7))) {
        canvas.setPixel(x + col, y + row, color);
      }
    }
  }
}

const uint8_t (*CategoryIcon(uint8_t i))[2] {
  switch (i) {
    case 0: return app::kIconQuran;
    case 1: return app::kIconBooks;
    case 2: return app::kIconTranslations;
    default: return app::kIconTafsir;
  }
}

void CenterText(gfx::Canvas& canvas, int y, const char* text, uint8_t scale) {
  const int w = gfx::Canvas::textWidth(text, scale);
  canvas.drawText((static_cast<int>(canvas.width()) - w) / 2, y, text, scale,
                  gfx::kBlack);
}

// "1.2MB" / "340KB" / "0B" -- one decimal place only above 1MB, matching how
// much precision actually helps distinguish two similarly-sized books.
void FormatSize(uint64_t bytes, char* out, size_t capacity) {
  if (bytes >= 1024ull * 1024ull) {
    const uint32_t tenths = static_cast<uint32_t>(
        (bytes * 10) / (1024ull * 1024ull));
    snprintf(out, capacity, "%lu.%luMB", static_cast<unsigned long>(tenths / 10),
             static_cast<unsigned long>(tenths % 10));
  } else if (bytes >= 1024ull) {
    snprintf(out, capacity, "%luKB",
             static_cast<unsigned long>(bytes / 1024ull));
  } else {
    snprintf(out, capacity, "%luB", static_cast<unsigned long>(bytes));
  }
}

// Truncates by actual pixel width (the font is fixed-width, so this is
// exact, not a byte-count guess), appending ".." when it doesn't fit, so a
// long title never runs into the next column.
void TruncateTitle(const char* title, int max_width_px, char* out,
                   size_t capacity) {
  if (gfx::Canvas::textWidth(title, kBodyScale) <= max_width_px) {
    snprintf(out, capacity, "%s", title);
    return;
  }
  int keep = (max_width_px - 2 * kCharW) / kCharW;
  if (keep < 0) keep = 0;
  const int max_keep = capacity > 3 ? static_cast<int>(capacity) - 3 : 0;
  if (keep > max_keep) keep = max_keep;
  snprintf(out, capacity, "%.*s..", keep, title);
}

constexpr uint8_t kTileIconScale = 4;  // the 16px icons, drawn 64px

void DrawIconScaled(gfx::Canvas& canvas, int x, int y, const uint8_t icon[][2],
                    uint8_t scale, gfx::Color color) {
  for (uint8_t row = 0; row < app::kLibraryIconSize; ++row) {
    for (uint8_t col = 0; col < app::kLibraryIconSize; ++col) {
      if (icon[row][col >> 3] & (0x80 >> (col & 7))) {
        canvas.fillRect(x + col * scale, y + row * scale, scale, scale, color);
      }
    }
  }
}

void CenterTextIn(gfx::Canvas& canvas, int x, int w, int y, const char* text,
                  uint8_t scale, gfx::Color color) {
  const int tw = gfx::Canvas::textWidth(text, scale);
  canvas.drawText(x + (w - tw) / 2, y, text, scale, color);
}

const char* TransferTileDetail(const LibraryState& state) {
  if (state.transfer_mode) return "on";
  if (state.transfer_connecting) return "connecting";
  if (state.transfer_ready) return "off";
  return "not set up";
}

const char* TransferLabel(const LibraryState& state) {
  if (state.transfer_mode) return "Transfer Mode (on)";
  if (state.transfer_connecting) return "Transfer Mode (connecting...)";
  if (state.transfer_ready) return "Transfer Mode";
  return "Transfer Mode (no saved Wi-Fi)";
}

}  // namespace

uint16_t LibraryScreen::categoryType(uint8_t i) {
  switch (i) {
    case 0: return static_cast<uint16_t>(qpk::PackageType::kQuran);
    case 1: return static_cast<uint16_t>(qpk::PackageType::kBook);
    case 2: return static_cast<uint16_t>(qpk::PackageType::kTranslation);
    default: return static_cast<uint16_t>(qpk::PackageType::kTafsir);
  }
}

const char* LibraryScreen::categoryLabel(uint8_t i) {
  switch (i) {
    case 0: return "Quran";
    case 1: return "Books";
    case 2: return "Translations";
    default: return "Tafsir";
  }
}

uint16_t LibraryScreen::countOfType(const LibraryState& state, uint16_t type) {
  if (state.index == nullptr) return 0;
  uint16_t n = 0;
  for (uint16_t i = 0; i < state.index->count(); ++i) {
    const net::LibraryEntry* entry = state.index->at(i);
    if (entry != nullptr && entry->type == type) ++n;
  }
  return n;
}

const net::LibraryEntry* LibraryScreen::entryOfType(const LibraryState& state,
                                                    uint16_t type,
                                                    uint16_t nth) {
  if (state.index == nullptr) return nullptr;
  uint16_t seen = 0;
  for (uint16_t i = 0; i < state.index->count(); ++i) {
    const net::LibraryEntry* entry = state.index->at(i);
    if (entry == nullptr || entry->type != type) continue;
    if (seen == nth) return entry;
    ++seen;
  }
  return nullptr;
}

uint16_t LibraryScreen::rowCount(const LibraryState& state) {
  if (state.view == LibraryView::kCategories) return kLibraryHomeTileCount;
  return static_cast<uint16_t>(countOfType(state, state.category) + 1);  // +1 Back
}

LibraryRow LibraryScreen::rowKind(const LibraryState& state, uint16_t row,
                                  uint16_t* ordinal) {
  if (ordinal != nullptr) *ordinal = 0;
  if (row >= rowCount(state)) return LibraryRow::kNone;

  if (state.view == LibraryView::kCategories) {
    if (row < kLibraryCategoryCount) {
      if (ordinal != nullptr) *ordinal = row;
      return LibraryRow::kCategory;
    }
    if (row == kLibraryCategoryCount) return LibraryRow::kTransferMode;
    if (row == kLibraryCategoryCount + 1) return LibraryRow::kClock;
    if (row == kLibraryCategoryCount + 2) return LibraryRow::kDevice;
    return LibraryRow::kBookmarks;
  }

  if (row == 0) return LibraryRow::kBack;
  if (ordinal != nullptr) *ordinal = static_cast<uint16_t>(row - 1);
  return LibraryRow::kItem;
}

void LibraryScreen::tileRect(uint16_t row, int* x, int* y, int* w, int* h) {
  const int column = row % kLibraryTileColumns;
  const int line = row / kLibraryTileColumns;
  *x = kLibraryTileLeft + column * (kLibraryTileW + kLibraryTileGap);
  *y = kLibraryTileTop + line * (kLibraryTileH + kLibraryTileGap);
  *w = kLibraryTileW;
  *h = kLibraryTileH;
}

namespace {

// One launcher tile: a big icon, its name, and one line of detail. The
// selected tile is drawn inverted -- on e-paper a solid block reads at a
// glance from across a room, where a thin frame does not.
void DrawTile(gfx::Canvas& canvas, const LibraryState& state, uint16_t row) {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  LibraryScreen::tileRect(row, &x, &y, &w, &h);
  const bool selected = row == state.selected;
  const gfx::Color ink = selected ? gfx::kWhite : gfx::kBlack;
  if (selected) {
    canvas.fillRect(x, y, w, h, gfx::kBlack);
    // Clipped corners, so the block reads as a tile rather than a hole.
    for (int i = 0; i < 3; ++i) {
      canvas.drawHLine(x, y + i, 3 - i, gfx::kWhite);
      canvas.drawHLine(x + w - 3 + i, y + i, 3 - i, gfx::kWhite);
      canvas.drawHLine(x, y + h - 1 - i, 3 - i, gfx::kWhite);
      canvas.drawHLine(x + w - 3 + i, y + h - 1 - i, 3 - i, gfx::kWhite);
    }
  }

  char detail[24];
  const uint8_t (*icon)[2] = app::kIconDevice;
  const char* label = "Device";
  uint16_t ordinal = 0;
  switch (LibraryScreen::rowKind(state, row, &ordinal)) {
    case LibraryRow::kCategory: {
      icon = CategoryIcon(static_cast<uint8_t>(ordinal));
      label = LibraryScreen::categoryLabel(static_cast<uint8_t>(ordinal));
      const uint16_t n = LibraryScreen::countOfType(
          state, LibraryScreen::categoryType(static_cast<uint8_t>(ordinal)));
      if (n == 0) {
        snprintf(detail, sizeof(detail), "empty");
      } else {
        snprintf(detail, sizeof(detail), "%u item%s", static_cast<unsigned>(n), n == 1 ? "" : "s");
      }
      break;
    }
    case LibraryRow::kTransferMode:
      icon = app::kIconTransfer;
      label = "Wi-Fi";
      snprintf(detail, sizeof(detail), "%s", TransferTileDetail(state));
      break;
    case LibraryRow::kClock:
      icon = app::kIconClock;
      label = "Clock";
      snprintf(detail, sizeof(detail), "home screen");
      break;
    case LibraryRow::kBookmarks:
      icon = app::kIconBookmark;
      label = "Bookmarks";
      if (state.bookmark_count == 0) {
        snprintf(detail, sizeof(detail), "continue");
      } else {
        snprintf(detail, sizeof(detail), "%u saved", static_cast<unsigned>(state.bookmark_count));
      }
      break;
    case LibraryRow::kDevice:
    default:
      snprintf(detail, sizeof(detail), "diagnostics");
      break;
  }

  const int icon_px = app::kLibraryIconSize * kTileIconScale;
  DrawIconScaled(canvas, x + (w - icon_px) / 2, y + 20, icon, kTileIconScale, ink);
  CenterTextIn(canvas, x, w, y + 100, label, kBodyScale, ink);
  CenterTextIn(canvas, x, w, y + 126, detail, kBodyScale, ink);
}

// --- Books shelf -------------------------------------------------------------

constexpr int kShelfLeft = 20;
constexpr int kShelfPitchX = 152;
constexpr int kShelfTop = 84;
constexpr int kShelfPitchY = 178;
constexpr int kShelfCoverW = 108;  // qpk::kCoverWidth
constexpr int kShelfCoverH = 144;  // qpk::kCoverHeight
// The selected title's white-on-black box in grey mode: from 4 px above the
// title's top to 4 px below its 14 px glyphs.
constexpr int kShelfTitleBoxH = 22;

// A 4-grey cover on the black/white library screen: black and white as they
// are, the two greys as 3-in-4 and 1-in-4 ink in a 2x2 pattern, so selection
// moves stay fast partial refreshes instead of grey ones.
void DrawCoverBW(gfx::Canvas& canvas, int x, int y, const uint8_t* pixels) {
  const int stride = kShelfCoverW / 4;
  for (int row = 0; row < kShelfCoverH; ++row) {
    for (int col = 0; col < kShelfCoverW; ++col) {
      const int level = (pixels[row * stride + (col >> 2)] >> (6 - 2 * (col & 3))) & 3;
      const int cell = ((row & 1) << 1) | (col & 1);
      const bool ink = level == 0 || (level == 1 && cell != 3) || (level == 2 && cell == 0);
      if (ink) canvas.setPixel(x + col, y + row, gfx::kBlack);
    }
  }
}

void DrawShelf(gfx::Canvas& canvas, const LibraryState& state) {
  const uint16_t start = LibraryScreen::shelfPageStart(state);
  const uint16_t total = LibraryScreen::rowCount(state);
  for (uint8_t slot = 0; slot < kLibraryShelfPageTiles; ++slot) {
    const uint16_t row = static_cast<uint16_t>(start + slot);
    if (row >= total) break;
    int x = 0;
    int y = 0;
    LibraryScreen::shelfCoverRect(slot, &x, &y);
    const int tile_x = x - (kShelfPitchX - kShelfCoverW) / 2;
    const int title_y = y + kShelfCoverH + 9;

    // Grey mode marks the selection on its title (white on black), a strip
    // flushWindow() can update without touching a cover's grey pixels.
    const bool title_inverted = state.grey_covers && row == state.selected;
    const gfx::Color title_ink = title_inverted ? gfx::kWhite : gfx::kBlack;
    if (title_inverted) {
      canvas.fillRect(tile_x + 6, title_y - 4, kShelfPitchX - 12, kShelfTitleBoxH, gfx::kBlack);
    }

    uint16_t ordinal = 0;
    const LibraryRow kind = LibraryScreen::rowKind(state, row, &ordinal);
    if (kind == LibraryRow::kBack) {
      canvas.drawRect(x, y, kShelfCoverW, kShelfCoverH, gfx::kBlack);
      CenterTextIn(canvas, x, kShelfCoverW, y + 54, "<", 5, gfx::kBlack);
      CenterTextIn(canvas, tile_x, kShelfPitchX, title_y, "Back", kBodyScale, title_ink);
    } else if (kind == LibraryRow::kItem) {
      const net::LibraryEntry* entry =
          LibraryScreen::entryOfType(state, state.category, ordinal);
      if (entry == nullptr) continue;  // index changed under us
      if (state.shelf_covers[slot] != nullptr) {
        // In grey mode the inside stays white: composeShelfGrey() supplies it.
        if (!state.grey_covers) DrawCoverBW(canvas, x, y, state.shelf_covers[slot]);
        canvas.drawRect(x, y, kShelfCoverW, kShelfCoverH, gfx::kBlack);
      } else {
        // No cover in the package: a plain one with the book icon, the same
        // fallback the desktop app draws.
        canvas.drawRect(x, y, kShelfCoverW, kShelfCoverH, gfx::kBlack);
        canvas.drawRect(x + 1, y + 1, kShelfCoverW - 2, kShelfCoverH - 2, gfx::kBlack);
        DrawIconScaled(canvas, x + (kShelfCoverW - 48) / 2, y + 48, app::kIconBooks, 3,
                       gfx::kBlack);
      }
      char title[net::kTitleMaxBytes];
      // 24 px narrower than the tile, so neighbouring titles never touch.
      TruncateTitle(entry->title, kShelfPitchX - 24, title, sizeof(title));
      CenterTextIn(canvas, tile_x, kShelfPitchX, title_y, title, kBodyScale, title_ink);
    }

    if (row == state.selected && !state.grey_covers) {
      for (int t = 0; t < 3; ++t) {
        canvas.drawRect(x - 4 - t, y - 4 - t, kShelfCoverW + 8 + 2 * t, kShelfCoverH + 8 + 2 * t,
                        gfx::kBlack);
      }
    }
  }
}

}  // namespace

bool LibraryScreen::isShelf(const LibraryState& state) {
  return state.view == LibraryView::kItems &&
         state.category == static_cast<uint16_t>(qpk::PackageType::kBook);
}

uint16_t LibraryScreen::shelfPageStart(const LibraryState& state) {
  return static_cast<uint16_t>(state.selected - state.selected % kLibraryShelfPageTiles);
}

void LibraryScreen::shelfCoverRect(uint8_t slot, int* x, int* y) {
  *x = kShelfLeft + (slot % kLibraryShelfColumns) * kShelfPitchX + (kShelfPitchX - kShelfCoverW) / 2;
  *y = kShelfTop + (slot / kLibraryShelfColumns) * kShelfPitchY;
}

void LibraryScreen::shelfTitleBand(uint8_t shelf_row, int* x, int* y, int* w, int* h) {
  // Covers end at +144 and the next row's start at +178 (kShelfPitchY); the
  // title box spans +149..+171. Horizontally the whole shelf, byte-aligned.
  *x = (kShelfLeft / 8) * 8;
  *w = ((kShelfLeft + kLibraryShelfColumns * kShelfPitchX - *x + 7) / 8) * 8;
  *y = kShelfTop + shelf_row * kShelfPitchY + kShelfCoverH + 2;
  *h = kShelfPitchY - kShelfCoverH - 4;
}

void LibraryScreen::composeShelfGrey(const LibraryState& state, uint8_t* out) {
  memset(out, 0xFF, kShelfGreyBytes);  // level 3, white
  const int stride = kShelfGreyW / 4;
  const uint16_t start = shelfPageStart(state);
  const uint16_t total = rowCount(state);
  for (uint8_t slot = 0; slot < kLibraryShelfPageTiles; ++slot) {
    const uint16_t row = static_cast<uint16_t>(start + slot);
    const uint8_t* cover = state.shelf_covers[slot];
    if (row >= total || cover == nullptr ||
        rowKind(state, row, nullptr) != LibraryRow::kItem) {
      continue;
    }
    int x = 0;
    int y = 0;
    shelfCoverRect(slot, &x, &y);
    const int ox = x - kShelfGreyX;
    const int oy = y - kShelfGreyY;
    const int cover_stride = kShelfCoverW / 4;
    for (int r = 0; r < kShelfCoverH; ++r) {
      const uint8_t* src = cover + r * cover_stride;
      uint8_t* dst = out + (oy + r) * stride;
      for (int c = 0; c < kShelfCoverW; ++c) {
        const uint8_t level = (src[c >> 2] >> (6 - 2 * (c & 3))) & 3;
        const int col = ox + c;
        const int shift = 6 - 2 * (col & 3);
        dst[col >> 2] = static_cast<uint8_t>((dst[col >> 2] & ~(3 << shift)) | (level << shift));
      }
    }
  }
}

void LibraryScreen::render(gfx::Canvas& canvas, const LibraryState& state) {
  char buf[80];

  canvas.clear(gfx::kWhite);
  canvas.drawRect(kBorderX, kBorderY, kBorderW, kBorderH, gfx::kBlack);

  const bool categories = (state.view == LibraryView::kCategories);
  const bool shelf = isShelf(state);
  const uint16_t total_items = state.index != nullptr ? state.index->count() : 0;
  const uint16_t total_rows = rowCount(state);

  // --- header -------------------------------------------------------------
  if (categories) {
    CenterText(canvas, 14, "Library", kTitleScale);
    if (total_items > 0) {
      snprintf(buf, sizeof(buf), "%u item%s installed",
               static_cast<unsigned>(total_items), total_items == 1 ? "" : "s");
    } else {
      snprintf(buf, sizeof(buf), "No content yet -- copy .qpk files to /LIBRARY");
    }
    CenterText(canvas, 48, buf, kBodyScale);
  } else {
    uint8_t which = 0;
    for (uint8_t i = 0; i < kLibraryCategoryCount; ++i) {
      if (categoryType(i) == state.category) which = i;
    }
    snprintf(buf, sizeof(buf), "Library / %s", categoryLabel(which));
    CenterText(canvas, 14, buf, kTitleScale);

    const uint16_t n = countOfType(state, state.category);
    if (n == 0) {
      snprintf(buf, sizeof(buf), "Nothing here yet");
    } else if (state.category == static_cast<uint16_t>(qpk::PackageType::kQuran)) {
      // Quran content is one package per surah, so this level *is* the
      // chapter list. Say so rather than calling them "items".
      snprintf(buf, sizeof(buf), "%u chapter%s", static_cast<unsigned>(n),
               n == 1 ? "" : "s");
    } else if (shelf && total_rows > kLibraryShelfPageTiles) {
      snprintf(buf, sizeof(buf), "%u item%s  -  page %u of %u", static_cast<unsigned>(n),
               n == 1 ? "" : "s",
               static_cast<unsigned>(state.selected / kLibraryShelfPageTiles + 1),
               static_cast<unsigned>((total_rows + kLibraryShelfPageTiles - 1) /
                                     kLibraryShelfPageTiles));
    } else {
      snprintf(buf, sizeof(buf), "%u item%s", static_cast<unsigned>(n),
               n == 1 ? "" : "s");
    }
    CenterText(canvas, 48, buf, kBodyScale);
  }
  canvas.drawHLine(kRuleX, kHeaderRuleY, kRuleW, gfx::kBlack);

  // --- home tiles -----------------------------------------------------------
  if (categories) {
    for (uint16_t row = 0; row < total_rows; ++row) DrawTile(canvas, state, row);
  }
  if (shelf) DrawShelf(canvas, state);

  // --- rows ---------------------------------------------------------------
  for (uint8_t row = 0; !categories && !shelf && row < kLibraryMaxVisibleRows; ++row) {
    const uint16_t item = static_cast<uint16_t>(state.scroll_top + row);
    if (item >= total_rows) break;

    const int y = kRowTop + row * kRowStep;
    canvas.drawText(kRowLeftX - 14, y, item == state.selected ? ">" : " ",
                    kBodyScale, gfx::kBlack);

    uint16_t ordinal = 0;
    switch (rowKind(state, item, &ordinal)) {
      case LibraryRow::kCategory: {
        DrawIcon(canvas, kRowLeftX, y, CategoryIcon(static_cast<uint8_t>(ordinal)),
                gfx::kBlack);
        canvas.drawText(kRowIconTextX, y,
                        categoryLabel(static_cast<uint8_t>(ordinal)), kBodyScale,
                        gfx::kBlack);
        const uint16_t n = countOfType(state, categoryType(static_cast<uint8_t>(ordinal)));
        snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(n));
        canvas.drawText(kRowTypeX, y, buf, kBodyScale, gfx::kBlack);
        // A category you can enter is worth marking as such.
        if (n > 0) canvas.drawText(kRowSizeX, y, ">", kBodyScale, gfx::kBlack);
        break;
      }
      case LibraryRow::kTransferMode:
        DrawIcon(canvas, kRowLeftX, y, app::kIconTransfer, gfx::kBlack);
        canvas.drawText(kRowIconTextX, y, TransferLabel(state), kBodyScale,
                        gfx::kBlack);
        break;

      case LibraryRow::kBack:
        canvas.drawText(kRowLeftX, y, "< Back", kBodyScale, gfx::kBlack);
        break;

      case LibraryRow::kItem: {
        const net::LibraryEntry* entry =
            entryOfType(state, state.category, ordinal);
        if (entry == nullptr) break;  // index changed under us; skip, not crash
        char title[net::kTitleMaxBytes];
        TruncateTitle(entry->title, kItemTitleMaxWidthPx, title, sizeof(title));
        canvas.drawText(kRowLeftX, y, title, kBodyScale, gfx::kBlack);
        char size_buf[16];
        FormatSize(entry->package_size, size_buf, sizeof(size_buf));
        canvas.drawText(kRowSizeX, y, size_buf, kBodyScale, gfx::kBlack);
        break;
      }

      case LibraryRow::kClock:
      case LibraryRow::kDevice:
      case LibraryRow::kBookmarks:
      case LibraryRow::kNone:
        break;
    }
  }

  // Scroll position, only when the list doesn't fit -- otherwise it is
  // visual noise for the common case of a handful of rows.
  if (!shelf && total_rows > kLibraryMaxVisibleRows) {
    snprintf(buf, sizeof(buf), "%u/%u",
             static_cast<unsigned>(state.selected + 1),
             static_cast<unsigned>(total_rows));
    canvas.drawText(kRowSizeX + 40, kListBottom + 2, buf, kBodyScale,
                    gfx::kBlack);
  }

  // --- footer -------------------------------------------------------------
  // Transfer mode's address takes priority over the usual hint once it is
  // on or connecting: Transfer Mode is toggled from this screen (the
  // trailing row), and the IP:port a desktop client needs to connect used to
  // be shown only on SelfTestScreen -- reachable by pressing MENU away from
  // here, which is a real usability gap since this screen is the product
  // build's default. status_message still wins over both: it is fresher
  // feedback from whatever the user just pressed OK on.
  canvas.drawHLine(kRuleX, kFooterRuleY, kRuleW, gfx::kBlack);
  if (state.status_message[0] != 0) {
    CenterText(canvas, kFooterTextY, state.status_message, kBodyScale);
  } else if (state.transfer_mode) {
    snprintf(buf, sizeof(buf), "Transfer mode: %s  hold OK to stop",
             state.transfer_address);
    CenterText(canvas, kFooterTextY, buf, kBodyScale);
  } else if (state.transfer_connecting) {
    CenterText(canvas, kFooterTextY, "Connecting to Wi-Fi...  hold OK to cancel",
              kBodyScale);
  } else if (categories) {
    snprintf(buf, sizeof(buf), "WHEEL=move OK=open MENU=diagnostics fw %s",
             app::kFirmwareVersion);
    CenterText(canvas, kFooterTextY, buf, kBodyScale);
  } else {
    CenterText(canvas, kFooterTextY, "WHEEL=move OK=open EXIT=back", kBodyScale);
  }
}

}  // namespace ui
