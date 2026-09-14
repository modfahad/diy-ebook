// crc32.h -- CRC-32/ISO-HDLC (poly 0xEDB88320, init/final 0xFFFFFFFF).
//
// The same CRC-32 as zlib, PNG and Ethernet. The canonical check value is
// CRC32("123456789") == 0xCBF43926, which both this implementation and the
// TypeScript writer in packages/qpk-format are tested against.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace qpk {

constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;

uint32_t Crc32Update(uint32_t crc, const void* data, size_t length);

inline uint32_t Crc32Finish(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

inline uint32_t Crc32(const void* data, size_t length) {
  return Crc32Finish(Crc32Update(kCrc32Init, data, length));
}

}  // namespace qpk
