// base64.h -- decoding for chunks sent as base64 text.
//
// Every desktop client streams a chunk as a chunked request body. React
// Native's fetch (the Android app, docs/android.md) cannot stream a request
// body, so it sends the chunk as base64 text with X-Qr-Body: base64 instead,
// which WebServer reads into memory whole -- safe for text, where a binary
// body would be cut at its first zero byte.

#pragma once

#include <stdint.h>

namespace net {

// Standard alphabet, '=' padding, line breaks ignored. False on any other
// character, data after padding, or more output than `capacity`; *out_length
// is only meaningful when true.
bool DecodeBase64(const char* in, uint32_t in_length, uint8_t* out, uint32_t capacity,
                  uint32_t* out_length);

}  // namespace net
