// json.h -- just enough JSON for this protocol, and no more.
//
// Every request body in the protocol is a FLAT object of strings and numbers.
// A full JSON parser (and its allocations) would be a poor trade on a device
// that never sees nesting. This reads values out of a flat object without
// allocating, and writes one out into a caller-supplied buffer.
//
// Deliberately strict: anything it does not understand is a parse failure, not
// a best-effort guess. A malformed body must be a 400, never a silently
// defaulted field.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace net {

class JsonReader {
 public:
  JsonReader(const char* text, uint32_t length) : text_(text), length_(length) {}
  explicit JsonReader(const char* text);

  /** True if the document is a syntactically plausible flat object. */
  bool valid() const;

  /**
   * Copies the string value of `key` into `out`, NUL-terminated. Returns false
   * if the key is absent, is not a string, or does not fit.
   * Understands \" \\ \/ \b \f \n \r \t and \uXXXX (BMP, encoded as UTF-8).
   */
  bool getString(const char* key, char* out, uint32_t capacity) const;

  /** Unsigned integer value. Returns false if absent, negative or not a number. */
  bool getUint64(const char* key, uint64_t* out) const;
  bool getUint32(const char* key, uint32_t* out) const;

  bool getBool(const char* key, bool* out) const;

  bool has(const char* key) const;

 private:
  // Locates the raw value span for `key`. Returns false if absent.
  bool findValue(const char* key, uint32_t* start, uint32_t* end) const;

  const char* text_;
  uint32_t length_;
};

/**
 * Builds a flat JSON object into a fixed buffer. Every append is checked; if
 * anything overflows, `ok()` goes false and stays false, so a truncated
 * response can never be sent as if it were complete.
 */
class JsonWriter {
 public:
  JsonWriter(char* buffer, uint32_t capacity);

  void beginObject();
  /** A nested object under `key`, e.g. `"storage": { ... }`. */
  void beginObject(const char* key);
  void endObject();
  void beginArray(const char* key);
  void endArray();
  void beginArrayElement();

  void keyString(const char* key, const char* value);
  void keyUint(const char* key, uint64_t value);
  void keyInt(const char* key, int64_t value);
  void keyBool(const char* key, bool value);

  bool ok() const { return ok_; }
  uint32_t length() const { return length_; }
  const char* c_str() const { return buffer_; }

 private:
  void raw(const char* text);
  void rawChar(char c);
  void escaped(const char* text);
  void separator();

  char* buffer_;
  uint32_t capacity_;
  uint32_t length_ = 0;
  bool ok_ = true;
  bool need_comma_ = false;
};

}  // namespace net
