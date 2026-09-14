#include "net/json.h"

#include <string.h>

namespace net {
namespace {

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

uint32_t SkipSpace(const char* text, uint32_t at, uint32_t length) {
  while (at < length && IsSpace(text[at])) ++at;
  return at;
}

// Advances past a complete JSON string starting at the opening quote.
// Returns the index just after the closing quote, or `length` on failure.
uint32_t SkipString(const char* text, uint32_t at, uint32_t length) {
  if (at >= length || text[at] != '"') return length;
  ++at;
  while (at < length) {
    const char c = text[at];
    if (c == '\\') {
      at += 2;
      continue;
    }
    if (c == '"') return at + 1;
    ++at;
  }
  return length;
}

// Advances past any value. Nested objects and arrays are skipped as opaque
// spans so an unexpected nested field cannot desynchronise the scan.
uint32_t SkipValue(const char* text, uint32_t at, uint32_t length) {
  at = SkipSpace(text, at, length);
  if (at >= length) return length;
  const char c = text[at];
  if (c == '"') return SkipString(text, at, length);
  if (c == '{' || c == '[') {
    const char open = c;
    const char close = (c == '{') ? '}' : ']';
    int depth = 0;
    while (at < length) {
      const char d = text[at];
      if (d == '"') {
        at = SkipString(text, at, length);
        continue;
      }
      if (d == open) ++depth;
      else if (d == close) {
        --depth;
        if (depth == 0) return at + 1;
      }
      ++at;
    }
    return length;
  }
  while (at < length && text[at] != ',' && text[at] != '}' && !IsSpace(text[at])) {
    ++at;
  }
  return at;
}

bool KeyMatches(const char* text, uint32_t start, uint32_t end, const char* key) {
  // `start`..`end` spans the quoted key including both quotes.
  if (end < start + 2) return false;
  const uint32_t inner = end - start - 2;
  if (strlen(key) != inner) return false;
  return memcmp(text + start + 1, key, inner) == 0;
}

bool AppendUtf8(char* out, uint32_t capacity, uint32_t* at, uint32_t code) {
  if (code < 0x80) {
    if (*at + 1 >= capacity) return false;
    out[(*at)++] = static_cast<char>(code);
    return true;
  }
  if (code < 0x800) {
    if (*at + 2 >= capacity) return false;
    out[(*at)++] = static_cast<char>(0xC0 | (code >> 6));
    out[(*at)++] = static_cast<char>(0x80 | (code & 0x3F));
    return true;
  }
  if (*at + 3 >= capacity) return false;
  out[(*at)++] = static_cast<char>(0xE0 | (code >> 12));
  out[(*at)++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
  out[(*at)++] = static_cast<char>(0x80 | (code & 0x3F));
  return true;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

JsonReader::JsonReader(const char* text)
    : text_(text), length_(text == nullptr ? 0 : static_cast<uint32_t>(strlen(text))) {}

bool JsonReader::valid() const {
  if (text_ == nullptr) return false;
  const uint32_t at = SkipSpace(text_, 0, length_);
  return at < length_ && text_[at] == '{';
}

bool JsonReader::findValue(const char* key, uint32_t* start, uint32_t* end) const {
  if (!valid() || key == nullptr) return false;

  uint32_t at = SkipSpace(text_, 0, length_) + 1;  // past '{'
  while (true) {
    at = SkipSpace(text_, at, length_);
    if (at >= length_ || text_[at] == '}') return false;
    if (text_[at] != '"') return false;

    const uint32_t key_start = at;
    const uint32_t key_end = SkipString(text_, at, length_);
    if (key_end >= length_ + 1) return false;
    at = SkipSpace(text_, key_end, length_);
    if (at >= length_ || text_[at] != ':') return false;
    ++at;
    at = SkipSpace(text_, at, length_);

    const uint32_t value_start = at;
    const uint32_t value_end = SkipValue(text_, at, length_);
    if (value_end <= value_start) return false;

    if (KeyMatches(text_, key_start, key_end, key)) {
      *start = value_start;
      *end = value_end;
      return true;
    }

    at = SkipSpace(text_, value_end, length_);
    if (at < length_ && text_[at] == ',') {
      ++at;
      continue;
    }
    return false;
  }
}

bool JsonReader::has(const char* key) const {
  uint32_t start = 0;
  uint32_t end = 0;
  return findValue(key, &start, &end);
}

bool JsonReader::getString(const char* key, char* out, uint32_t capacity) const {
  uint32_t start = 0;
  uint32_t end = 0;
  if (out == nullptr || capacity == 0) return false;
  if (!findValue(key, &start, &end)) return false;
  if (text_[start] != '"' || end < start + 2) return false;

  uint32_t written = 0;
  uint32_t at = start + 1;
  const uint32_t stop = end - 1;  // index of the closing quote
  while (at < stop) {
    char c = text_[at];
    if (c != '\\') {
      if (written + 1 >= capacity) return false;
      out[written++] = c;
      ++at;
      continue;
    }
    if (at + 1 >= stop) return false;
    const char escape = text_[at + 1];
    at += 2;
    switch (escape) {
      case '"':  c = '"';  break;
      case '\\': c = '\\'; break;
      case '/':  c = '/';  break;
      case 'b':  c = '\b'; break;
      case 'f':  c = '\f'; break;
      case 'n':  c = '\n'; break;
      case 'r':  c = '\r'; break;
      case 't':  c = '\t'; break;
      case 'u': {
        if (at + 4 > stop) return false;
        uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
          const int digit = HexDigit(text_[at + i]);
          if (digit < 0) return false;
          code = (code << 4) | static_cast<uint32_t>(digit);
        }
        at += 4;
        // Surrogate halves are not reassembled: nothing in this protocol
        // carries astral characters, and guessing would be worse than failing.
        if (code >= 0xD800 && code <= 0xDFFF) return false;
        if (!AppendUtf8(out, capacity, &written, code)) return false;
        continue;
      }
      default:
        return false;
    }
    if (written + 1 >= capacity) return false;
    out[written++] = c;
  }
  out[written] = '\0';
  return true;
}

bool JsonReader::getUint64(const char* key, uint64_t* out) const {
  uint32_t start = 0;
  uint32_t end = 0;
  if (out == nullptr) return false;
  if (!findValue(key, &start, &end)) return false;
  if (start >= end) return false;
  if (text_[start] == '-') return false;

  uint64_t value = 0;
  bool any = false;
  for (uint32_t at = start; at < end; ++at) {
    const char c = text_[at];
    if (c < '0' || c > '9') return false;  // no floats, no exponents
    if (value > (0xFFFFFFFFFFFFFFFFull - static_cast<uint64_t>(c - '0')) / 10ull) {
      return false;  // overflow
    }
    value = value * 10ull + static_cast<uint64_t>(c - '0');
    any = true;
  }
  if (!any) return false;
  *out = value;
  return true;
}

bool JsonReader::getUint32(const char* key, uint32_t* out) const {
  uint64_t wide = 0;
  if (!getUint64(key, &wide)) return false;
  if (wide > 0xFFFFFFFFull) return false;
  *out = static_cast<uint32_t>(wide);
  return true;
}

bool JsonReader::getBool(const char* key, bool* out) const {
  uint32_t start = 0;
  uint32_t end = 0;
  if (out == nullptr) return false;
  if (!findValue(key, &start, &end)) return false;
  const uint32_t span = end - start;
  if (span == 4 && memcmp(text_ + start, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (span == 5 && memcmp(text_ + start, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

// --- writer -----------------------------------------------------------------

JsonWriter::JsonWriter(char* buffer, uint32_t capacity)
    : buffer_(buffer), capacity_(capacity) {
  if (buffer_ == nullptr || capacity_ == 0) {
    ok_ = false;
    return;
  }
  buffer_[0] = '\0';
}

void JsonWriter::rawChar(char c) {
  if (!ok_) return;
  if (length_ + 1 >= capacity_) {
    ok_ = false;
    return;
  }
  buffer_[length_++] = c;
  buffer_[length_] = '\0';
}

void JsonWriter::raw(const char* text) {
  if (!ok_ || text == nullptr) return;
  for (const char* p = text; *p != '\0'; ++p) rawChar(*p);
}

void JsonWriter::escaped(const char* text) {
  rawChar('"');
  if (text != nullptr) {
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
         *p != '\0'; ++p) {
      const unsigned char c = *p;
      switch (c) {
        case '"':  raw("\\\"");  break;
        case '\\': raw("\\\\");  break;
        case '\n': raw("\\n");   break;
        case '\r': raw("\\r");   break;
        case '\t': raw("\\t");   break;
        case '\b': raw("\\b");   break;
        case '\f': raw("\\f");   break;
        default:
          if (c < 0x20) {
            // Control characters must be escaped; \u00XX is the only legal form.
            static const char kHex[] = "0123456789abcdef";
            raw("\\u00");
            rawChar(kHex[(c >> 4) & 0x0F]);
            rawChar(kHex[c & 0x0F]);
          } else {
            // UTF-8 bytes pass through unchanged.
            rawChar(static_cast<char>(c));
          }
          break;
      }
    }
  }
  rawChar('"');
}

void JsonWriter::separator() {
  if (need_comma_) rawChar(',');
  need_comma_ = true;
}

void JsonWriter::beginObject() {
  if (need_comma_) rawChar(',');
  rawChar('{');
  need_comma_ = false;
}

void JsonWriter::beginObject(const char* key) {
  separator();
  escaped(key);
  rawChar(':');
  rawChar('{');
  need_comma_ = false;
}

void JsonWriter::endObject() {
  rawChar('}');
  need_comma_ = true;
}

void JsonWriter::beginArray(const char* key) {
  separator();
  escaped(key);
  rawChar(':');
  rawChar('[');
  need_comma_ = false;
}

void JsonWriter::endArray() {
  rawChar(']');
  need_comma_ = true;
}

void JsonWriter::beginArrayElement() {
  if (need_comma_) rawChar(',');
  need_comma_ = false;
}

void JsonWriter::keyString(const char* key, const char* value) {
  separator();
  escaped(key);
  rawChar(':');
  escaped(value);
}

void JsonWriter::keyUint(const char* key, uint64_t value) {
  separator();
  escaped(key);
  rawChar(':');
  char digits[21];
  uint8_t at = sizeof(digits);
  digits[--at] = '\0';
  if (value == 0) {
    digits[--at] = '0';
  } else {
    while (value > 0 && at > 0) {
      digits[--at] = static_cast<char>('0' + (value % 10));
      value /= 10;
    }
  }
  raw(&digits[at]);
}

void JsonWriter::keyInt(const char* key, int64_t value) {
  if (value >= 0) {
    keyUint(key, static_cast<uint64_t>(value));
    return;
  }
  separator();
  escaped(key);
  rawChar(':');
  rawChar('-');
  char digits[21];
  uint8_t at = sizeof(digits);
  digits[--at] = '\0';
  uint64_t magnitude = static_cast<uint64_t>(-(value + 1)) + 1ull;
  while (magnitude > 0 && at > 0) {
    digits[--at] = static_cast<char>('0' + (magnitude % 10));
    magnitude /= 10;
  }
  raw(&digits[at]);
}

void JsonWriter::keyBool(const char* key, bool value) {
  separator();
  escaped(key);
  rawChar(':');
  raw(value ? "true" : "false");
}

}  // namespace net
