#include "net/verified_packages.h"

#include <string.h>

#include "qpk/byte_order.h"
#include "qpk/crc32.h"

namespace net {
namespace {

// File: 12-byte header ("VPK1", version u16, count u16, crc32 over the
// records), then `count` 32-byte records:
//   0  16  content_id
//  16   4  header_crc32
//  20   4  payload_crc32
//  24   8  package_size (low, high)
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 12;
constexpr uint32_t kRecordBytes = 32;
constexpr const char* kPath = "/DEVICE/verified.bin";
constexpr const char* kTempPath = "/DEVICE/verified.tmp";

// Static: 64 records is 2 KB, more than the main task's stack should carry.
uint8_t g_file[kHeaderBytes + kMaxVerifiedPackages * kRecordBytes];

bool Same(const PackageFingerprint& x, const PackageFingerprint& y) {
  return memcmp(x.content_id, y.content_id, sizeof(x.content_id)) == 0 &&
         x.header_crc32 == y.header_crc32 && x.payload_crc32 == y.payload_crc32 &&
         x.package_size == y.package_size;
}

}  // namespace

void VerifiedPackages::load(hal::IStorage* storage) {
  count_ = 0;
  if (storage == nullptr) return;
  const int64_t size = storage->size(kPath);
  if (size < static_cast<int64_t>(kHeaderBytes) ||
      size > static_cast<int64_t>(sizeof(g_file))) {
    return;
  }
  const uint32_t length = static_cast<uint32_t>(size);
  if (storage->read(kPath, 0, g_file, length) != static_cast<int32_t>(length)) return;
  if (g_file[0] != 'V' || g_file[1] != 'P' || g_file[2] != 'K' || g_file[3] != '1') return;
  if (qpk::Read16(g_file + 4) != kVersion) return;
  const uint16_t count = qpk::Read16(g_file + 6);
  if (count > kMaxVerifiedPackages || length != kHeaderBytes + count * kRecordBytes) return;
  if (qpk::Crc32(g_file + kHeaderBytes, count * kRecordBytes) != qpk::Read32(g_file + 8)) return;

  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* r = g_file + kHeaderBytes + i * kRecordBytes;
    PackageFingerprint& item = items_[i];
    memcpy(item.content_id, r, 16);
    item.header_crc32 = qpk::Read32(r + 16);
    item.payload_crc32 = qpk::Read32(r + 20);
    item.package_size =
        static_cast<uint64_t>(qpk::Read32(r + 24)) | (static_cast<uint64_t>(qpk::Read32(r + 28)) << 32);
  }
  count_ = count;
}

bool VerifiedPackages::save(hal::IStorage* storage) const {
  if (storage == nullptr) return false;
  const uint32_t length = kHeaderBytes + count_ * kRecordBytes;
  memset(g_file, 0, length);
  for (uint16_t i = 0; i < count_; ++i) {
    uint8_t* r = g_file + kHeaderBytes + i * kRecordBytes;
    const PackageFingerprint& item = items_[i];
    memcpy(r, item.content_id, 16);
    qpk::Write32(r + 16, item.header_crc32);
    qpk::Write32(r + 20, item.payload_crc32);
    qpk::Write32(r + 24, static_cast<uint32_t>(item.package_size & 0xFFFFFFFFu));
    qpk::Write32(r + 28, static_cast<uint32_t>(item.package_size >> 32));
  }
  g_file[0] = 'V';
  g_file[1] = 'P';
  g_file[2] = 'K';
  g_file[3] = '1';
  qpk::Write16(g_file + 4, kVersion);
  qpk::Write16(g_file + 6, count_);
  qpk::Write32(g_file + 8, qpk::Crc32(g_file + kHeaderBytes, count_ * kRecordBytes));

  storage->remove(kTempPath);
  if (!storage->writeAll(kTempPath, g_file, length)) {
    storage->remove(kTempPath);
    return false;
  }
  storage->remove(kPath);
  if (!storage->rename(kTempPath, kPath)) {
    storage->remove(kTempPath);
    return false;
  }
  return true;
}

bool VerifiedPackages::contains(const PackageFingerprint& fingerprint) const {
  for (uint16_t i = 0; i < count_; ++i) {
    if (Same(items_[i], fingerprint)) return true;
  }
  return false;
}

void VerifiedPackages::add(const PackageFingerprint& fingerprint) {
  uint16_t existing = count_;
  for (uint16_t i = 0; i < count_; ++i) {
    if (Same(items_[i], fingerprint)) {
      existing = i;
      break;
    }
  }
  const uint16_t last =
      existing < count_ ? existing : (count_ < kMaxVerifiedPackages ? count_ : count_ - 1);
  for (uint16_t i = last; i > 0; --i) items_[i] = items_[i - 1];
  items_[0] = fingerprint;
  if (existing == count_ && count_ < kMaxVerifiedPackages) ++count_;
}

}  // namespace net
