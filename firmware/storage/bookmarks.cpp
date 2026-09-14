#include "net/bookmarks.h"

#include <string.h>

#include "qpk/byte_order.h"
#include "qpk/crc32.h"

namespace net {
namespace {

// File: 12-byte header ("BMK1", version u16, count u16, crc32 over the
// records), then `count` 36-byte records:
//   0  16  content_id
//  16   1  kind
//  17   3  reserved
//  20   4  a
//  24   4  b
//  28   4  c
//  32   4  created_unix
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 12;
constexpr uint32_t kRecordBytes = 36;
constexpr const char* kPath = "/USER/bookmarks.bin";
constexpr const char* kTempPath = "/USER/bookmarks.tmp";

// Static: up to 64 records is 2.3 KB, too much for the main task's stack.
uint8_t g_file[kHeaderBytes + kMaxBookmarks * kRecordBytes];

bool SamePlace(const Bookmark& x, const Bookmark& y) {
  return memcmp(x.content_id, y.content_id, sizeof(x.content_id)) == 0 && x.kind == y.kind &&
         x.a == y.a && x.b == y.b && x.c == y.c;
}

}  // namespace

const Bookmark* Bookmarks::at(uint16_t index) const {
  return index < count_ ? &items_[index] : nullptr;
}

void Bookmarks::load(hal::IStorage* storage) {
  count_ = 0;
  if (storage == nullptr) return;
  const int64_t size = storage->size(kPath);
  if (size < static_cast<int64_t>(kHeaderBytes) ||
      size > static_cast<int64_t>(sizeof(g_file))) {
    return;
  }
  const uint32_t length = static_cast<uint32_t>(size);
  if (storage->read(kPath, 0, g_file, length) != static_cast<int32_t>(length)) return;
  if (g_file[0] != 'B' || g_file[1] != 'M' || g_file[2] != 'K' || g_file[3] != '1') return;
  if (qpk::Read16(g_file + 4) != kVersion) return;
  const uint16_t count = qpk::Read16(g_file + 6);
  if (count > kMaxBookmarks || length != kHeaderBytes + count * kRecordBytes) return;
  if (qpk::Crc32(g_file + kHeaderBytes, count * kRecordBytes) != qpk::Read32(g_file + 8)) return;

  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* r = g_file + kHeaderBytes + i * kRecordBytes;
    Bookmark& item = items_[i];
    memcpy(item.content_id, r, 16);
    item.kind = static_cast<BookmarkKind>(r[16]);
    item.a = qpk::Read32(r + 20);
    item.b = qpk::Read32(r + 24);
    item.c = qpk::Read32(r + 28);
    item.created_unix = qpk::Read32(r + 32);
  }
  count_ = count;
}

bool Bookmarks::save(hal::IStorage* storage) const {
  if (storage == nullptr) return false;
  const uint32_t length = kHeaderBytes + count_ * kRecordBytes;
  memset(g_file, 0, length);
  for (uint16_t i = 0; i < count_; ++i) {
    uint8_t* r = g_file + kHeaderBytes + i * kRecordBytes;
    const Bookmark& item = items_[i];
    memcpy(r, item.content_id, 16);
    r[16] = static_cast<uint8_t>(item.kind);
    qpk::Write32(r + 20, item.a);
    qpk::Write32(r + 24, item.b);
    qpk::Write32(r + 28, item.c);
    qpk::Write32(r + 32, item.created_unix);
  }
  g_file[0] = 'B';
  g_file[1] = 'M';
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

void Bookmarks::add(const Bookmark& bookmark) {
  uint16_t existing = count_;
  for (uint16_t i = 0; i < count_; ++i) {
    if (SamePlace(items_[i], bookmark)) {
      existing = i;
      break;
    }
  }
  // Shift everything before the slot being reused (or, for a new one, the
  // whole list minus the oldest when full) down by one, then put it first.
  uint16_t last = existing < count_ ? existing : (count_ < kMaxBookmarks ? count_ : count_ - 1);
  for (uint16_t i = last; i > 0; --i) items_[i] = items_[i - 1];
  items_[0] = bookmark;
  if (existing == count_ && count_ < kMaxBookmarks) ++count_;
}

bool Bookmarks::removeAt(uint16_t index) {
  if (index >= count_) return false;
  for (uint16_t i = index; i + 1 < count_; ++i) items_[i] = items_[i + 1];
  --count_;
  return true;
}

bool Bookmarks::contains(const Bookmark& bookmark) const {
  for (uint16_t i = 0; i < count_; ++i) {
    if (SamePlace(items_[i], bookmark)) return true;
  }
  return false;
}

}  // namespace net
