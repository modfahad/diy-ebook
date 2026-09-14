#include "net/base64.h"

namespace net {

bool DecodeBase64(const char* in, uint32_t in_length, uint8_t* out, uint32_t capacity,
                  uint32_t* out_length) {
  if (out_length == nullptr) return false;
  *out_length = 0;
  if (in == nullptr && in_length > 0) return false;

  uint32_t accumulator = 0;
  int bits = 0;
  uint32_t written = 0;
  uint32_t padding = 0;
  for (uint32_t i = 0; i < in_length; ++i) {
    const char c = in[i];
    uint32_t value = 0;
    if (c >= 'A' && c <= 'Z') {
      value = static_cast<uint32_t>(c - 'A');
    } else if (c >= 'a' && c <= 'z') {
      value = static_cast<uint32_t>(c - 'a') + 26;
    } else if (c >= '0' && c <= '9') {
      value = static_cast<uint32_t>(c - '0') + 52;
    } else if (c == '+') {
      value = 62;
    } else if (c == '/') {
      value = 63;
    } else if (c == '=') {
      ++padding;
      continue;
    } else if (c == '\r' || c == '\n') {
      continue;
    } else {
      return false;
    }
    if (padding > 0) return false;  // data after '='

    accumulator = ((accumulator << 6) | value) & 0xFFFFFFu;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (written >= capacity || out == nullptr) return false;
      out[written++] = static_cast<uint8_t>((accumulator >> bits) & 0xFF);
    }
  }
  if (padding > 2) return false;
  *out_length = written;
  return true;
}

}  // namespace net
