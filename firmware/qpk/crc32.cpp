#include "qpk/crc32.h"

namespace qpk {
namespace {

// Nibble-wise table: 64 bytes of flash instead of 1 KiB for the byte-wise
// table, at two lookups per byte. The index sections this runs over are tens
// of kilobytes, so the difference is not worth the flash on an 8 MB part.
const uint32_t kTable[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

}  // namespace

uint32_t Crc32Update(uint32_t crc, const void* data, size_t length) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    crc ^= p[i];
    crc = kTable[crc & 0x0F] ^ (crc >> 4);
    crc = kTable[crc & 0x0F] ^ (crc >> 4);
  }
  return crc;
}

}  // namespace qpk
