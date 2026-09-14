#include "ui/home_screen.h"

#include <stdio.h>
#include <string.h>

#include "gfx/font5x7.h"

namespace ui {

namespace {

// Layout for the left half of the 800x480 panel (x 0..399). Text is the 5x7
// face: (5 + 1) * scale px per character, 7 * scale px tall, `y` at the top.
constexpr int kLeftW = kHomePhotoX;       // 400
constexpr int kMarginX = 16;
constexpr int kContentW = kLeftW - 2 * kMarginX;  // 368

constexpr int kWeekdayY = 16;             // scale 3
constexpr int kDateY = 48;                // scale 2
constexpr uint8_t kClockScale = 12;       // "23:59" = 348 x 84
constexpr int kRuleY = 168;
constexpr int kCalendarTitleY = 180;      // scale 2
constexpr int kCalendarHeaderY = 206;     // scale 2
constexpr int kCalendarTop = 230;
constexpr int kCellW = 52;
constexpr int kCellH = 34;                // 6 rows -> 230..434
constexpr int kCalendarX = kMarginX + (kContentW - 7 * kCellW) / 2;  // 18
constexpr int kFooterRuleY = 446;
constexpr int kStatusY = 456;             // scale 1

const char* const kWeekdays[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                  "Thursday", "Friday", "Saturday"};
const char* const kWeekdayShort[7] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
const char* const kMonths[12] = {"January", "February", "March",     "April",
                                 "May",     "June",     "July",      "August",
                                 "September", "October", "November", "December"};

int CharW(uint8_t scale) { return (gfx::kGlyphWidth + 1) * scale; }

// Centred on the horizontal span [x, x + w).
void CenterIn(gfx::Canvas& canvas, int x, int w, int y, const char* text,
              uint8_t scale, gfx::Color color = gfx::kBlack) {
  const int tw = gfx::Canvas::textWidth(text, scale);
  canvas.drawText(x + (w - tw) / 2, y, text, scale, color);
}

void DrawCalendar(gfx::Canvas& canvas, const HomeState& s) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%s %u", kMonths[s.month - 1],
           static_cast<unsigned>(s.year));
  CenterIn(canvas, 0, kLeftW, kCalendarTitleY, buf, 2);

  for (int col = 0; col < 7; ++col) {
    const int weekday = s.week_starts_monday ? (col + 1) % 7 : col;
    CenterIn(canvas, kCalendarX + col * kCellW, kCellW, kCalendarHeaderY,
             kWeekdayShort[weekday], 2);
  }

  const uint8_t first = DayOfWeek(s.year, s.month, 1);
  const int offset = s.week_starts_monday ? (first + 6) % 7 : first;
  const uint8_t days = DaysInMonth(s.year, s.month);
  for (uint8_t day = 1; day <= days; ++day) {
    const int index = offset + day - 1;
    const int cell_x = kCalendarX + (index % 7) * kCellW;
    const int cell_y = kCalendarTop + (index / 7) * kCellH;
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(day));
    const bool today = (day == s.day);
    if (today) {
      canvas.fillRect(cell_x + 3, cell_y + 3, kCellW - 6, kCellH - 6, gfx::kBlack);
    }
    CenterIn(canvas, cell_x, kCellW, cell_y + (kCellH - gfx::Canvas::textHeight(2)) / 2,
             buf, 2, today ? gfx::kWhite : gfx::kBlack);
  }
}

}  // namespace

uint8_t DaysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return leap ? 29 : 28;
  }
  return kDays[month - 1];
}

uint8_t DayOfWeek(uint16_t year, uint8_t month, uint8_t day) {
  // Sakamoto's method.
  static const int kOffsets[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 1 || month > 12) return 0;
  int y = year;
  if (month < 3) --y;
  return static_cast<uint8_t>((y + y / 4 - y / 100 + y / 400 + kOffsets[month - 1] + day) % 7);
}

void HomeScreen::renderClock(gfx::Canvas& canvas, const HomeState& s) {
  canvas.fillRect(kHomeClockX, kHomeClockY, kHomeClockW, kHomeClockH, gfx::kWhite);
  char buf[8];
  if (s.time_valid) {
    snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(s.hour % 24),
             static_cast<unsigned>(s.minute % 60));
  } else {
    snprintf(buf, sizeof(buf), "--:--");
  }
  CenterIn(canvas, kHomeClockX, kHomeClockW,
           kHomeClockY + (kHomeClockH - gfx::Canvas::textHeight(kClockScale)) / 2, buf,
           kClockScale);
}

void HomeScreen::render(gfx::Canvas& canvas, const HomeState& s) {
  canvas.clear(gfx::kWhite);
  canvas.drawVLine(kHomePhotoX - 1, 0, kHomePhotoH, gfx::kBlack);

  char buf[40];
  const bool valid = s.time_valid && s.month >= 1 && s.month <= 12 && s.day >= 1;
  if (valid) {
    CenterIn(canvas, 0, kLeftW, kWeekdayY, kWeekdays[DayOfWeek(s.year, s.month, s.day)], 3);
    snprintf(buf, sizeof(buf), "%u %s %u", static_cast<unsigned>(s.day),
             kMonths[s.month - 1], static_cast<unsigned>(s.year));
    CenterIn(canvas, 0, kLeftW, kDateY, buf, 2);
  } else {
    CenterIn(canvas, 0, kLeftW, kWeekdayY, "Clock not set", 3);
    CenterIn(canvas, 0, kLeftW, kDateY, "Waiting for time sync", 2);
  }

  renderClock(canvas, s);
  canvas.drawHLine(kMarginX, kRuleY, kContentW, gfx::kBlack);

  if (valid) {
    DrawCalendar(canvas, s);
  } else {
    CenterIn(canvas, 0, kLeftW, 280, "The calendar appears once", 2);
    CenterIn(canvas, 0, kLeftW, 304, "the time is synced over Wi-Fi", 2);
  }

  canvas.drawHLine(kMarginX, kFooterRuleY, kContentW, gfx::kBlack);
  // Fitted to the column: kContentW / 6 px per character at scale 1.
  const int max_chars = (kContentW + 1) / CharW(1);
  char status[80];
  snprintf(status, sizeof(status), "%s", s.status != nullptr ? s.status : "");
  if (static_cast<int>(strlen(status)) > max_chars && max_chars >= 2) {
    status[max_chars - 2] = '.';
    status[max_chars - 1] = '.';
    status[max_chars] = '\0';
  }
  canvas.drawText(kMarginX, kStatusY, status, 1, gfx::kBlack);

  if (!s.has_photo) {
    CenterIn(canvas, kHomePhotoX, kHomePhotoW, 216, "No photos yet", 3);
    CenterIn(canvas, kHomePhotoX, kHomePhotoW, 256, "Add photos in the desktop", 2);
    CenterIn(canvas, kHomePhotoX, kHomePhotoW, 280, "app's Photos tab", 2);
  }
}

}  // namespace ui
