// byte_order.h -- explicit little-endian field access.
//
// Every multi-byte field in a QPK file goes through these. Nothing casts a
// byte buffer to a struct, so unaligned fields, struct padding and host
// endianness never come into it.

#pragma once

#include <stdint.h>

namespace qpk {

inline uint8_t Read8(const uint8_t* p) { return p[0]; }

inline uint16_t Read16(const uint8_t* p) {
  return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                               (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t Read32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t Read64(const uint8_t* p) {
  return static_cast<uint64_t>(Read32(p)) |
         (static_cast<uint64_t>(Read32(p + 4)) << 32);
}

inline void Write8(uint8_t* p, uint8_t v) { p[0] = v; }

inline void Write16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

inline void Write32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

inline void Write64(uint8_t* p, uint64_t v) {
  Write32(p, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  Write32(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

}  // namespace qpk
