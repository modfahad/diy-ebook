// ascii_fold.h -- UTF-8 text folded to the ASCII the 5x7 font can draw.
//
// Translations arrive as UTF-8 with typographic quotes, dashes and the marked
// letters of Arabic transliteration (a-macron, h-underdot, ...). The reader's
// font covers ASCII only and draws anything else as a hollow box, which in a
// translation is several boxes a line. Folding keeps the words readable.
// Header-only and Arduino-free, so the host tests cover it.

#pragma once

#include <stdint.h>

namespace util {

namespace detail {

inline char FoldLetter(uint32_t cp) {
  if (cp >= 0xC0 && cp <= 0xC5) return 'A';
  if (cp == 0xC7) return 'C';
  if (cp >= 0xC8 && cp <= 0xCB) return 'E';
  if (cp >= 0xCC && cp <= 0xCF) return 'I';
  if (cp == 0xD1) return 'N';
  if (cp >= 0xD2 && cp <= 0xD6) return 'O';
  if (cp >= 0xD9 && cp <= 0xDC) return 'U';
  if (cp >= 0xE0 && cp <= 0xE5) return 'a';
  if (cp == 0xE7) return 'c';
  if (cp >= 0xE8 && cp <= 0xEB) return 'e';
  if (cp >= 0xEC && cp <= 0xEF) return 'i';
  if (cp == 0xF1) return 'n';
  if (cp >= 0xF2 && cp <= 0xF6) return 'o';
  if (cp >= 0xF9 && cp <= 0xFC) return 'u';
  switch (cp) {
    case 0x100: return 'A';
    case 0x101: return 'a';
    case 0x112: return 'E';
    case 0x113: return 'e';
    case 0x12A: return 'I';
    case 0x12B: return 'i';
    case 0x16A: return 'U';
    case 0x16B: return 'u';
    case 0x1E0C: return 'D';
    case 0x1E0D: return 'd';
    case 0x1E24: return 'H';
    case 0x1E25: return 'h';
    case 0x1E62: return 'S';
    case 0x1E63: return 's';
    case 0x1E6C: return 'T';
    case 0x1E6D: return 't';
    case 0x1E92: return 'Z';
    case 0x1E93: return 'z';
    default: return 0;
  }
}

}  // namespace detail

// Copies `in` into `out` as ASCII: curly quotes and ayin/hamza marks become
// ' or ", dashes -- or -, an ellipsis ..., odd spaces a space, marked Latin
// letters their plain letter, anything else (and malformed UTF-8) '?'.
// Returns the bytes written; `out` is not NUL-terminated.
inline uint32_t FoldToAscii(const char* in, uint32_t in_length, char* out, uint32_t capacity) {
  uint32_t i = 0;
  uint32_t o = 0;
  const auto put = [&](const char* text) {
    while (*text != 0 && o < capacity) out[o++] = *text++;
  };
  while (i < in_length && o < capacity) {
    const uint8_t lead = static_cast<uint8_t>(in[i]);
    if (lead < 0x80) {
      out[o++] = static_cast<char>(lead);
      ++i;
      continue;
    }
    uint32_t cp = 0;
    uint32_t extra = 0;
    bool malformed = false;
    if ((lead & 0xE0) == 0xC0) {
      cp = lead & 0x1F;
      extra = 1;
    } else if ((lead & 0xF0) == 0xE0) {
      cp = lead & 0x0F;
      extra = 2;
    } else if ((lead & 0xF8) == 0xF0) {
      cp = lead & 0x07;
      extra = 3;
    } else {
      malformed = true;  // a stray continuation byte, or not a UTF-8 lead at all
    }
    if (!malformed && i + extra >= in_length) malformed = true;  // cut off
    for (uint32_t k = 1; !malformed && k <= extra; ++k) {
      const uint8_t next = static_cast<uint8_t>(in[i + k]);
      if ((next & 0xC0) != 0x80) malformed = true;
      cp = (cp << 6) | (next & 0x3F);
    }
    if (malformed) {
      // One '?' for the whole broken character, not one per byte of it.
      out[o++] = '?';
      ++i;
      while (i < in_length && (static_cast<uint8_t>(in[i]) & 0xC0) == 0x80) ++i;
      continue;
    }
    i += extra + 1;

    switch (cp) {
      case 0x2018: case 0x2019: case 0x201B: case 0x02BB: case 0x02BC: case 0x02BE: case 0x02BF:
        put("'");
        break;
      case 0x201C: case 0x201D: case 0x201F:
        put("\"");
        break;
      case 0x2010: case 0x2011: case 0x2012: case 0x2013:
        put("-");
        break;
      case 0x2014: case 0x2015:
        put("--");
        break;
      case 0x2026:
        put("...");
        break;
      case 0x00A0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F:
        put(" ");
        break;
      case 0x200B: case 0x200C: case 0x200D: case 0xFEFF:
        break;  // zero-width: nothing to draw
      default: {
        const char letter = detail::FoldLetter(cp);
        out[o++] = letter != 0 ? letter : '?';
      }
    }
  }
  return o;
}

}  // namespace util
