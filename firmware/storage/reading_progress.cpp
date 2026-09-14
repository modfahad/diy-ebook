#include "net/reading_progress.h"

#include <string.h>

#include "app/app_config.h"
#include "qpk/byte_order.h"
#include "qpk/crc32.h"

namespace net {
namespace {

constexpr char kMagic0 = 'P';
constexpr char kMagic1 = 'R';
constexpr char kMagic2 = 'G';
constexpr char kMagic3 = '1';
constexpr uint16_t kFormatVersion = 1;

// 36-byte record. hal::IStorage::read()/writeAll() is the "one-shot small
// file" style storage.h documents for exactly this case, so unlike
// LibraryIndex (many records, opened and randomly read) this never needs
// IFile.
//
//   0   4  magic "PRG1"
//   4   2  format_version
//   6   2  surah_id
//   8   4  first_ayah_index
//  12   2  screen_number
//  14   2  reserved
//  16  16  content_id
//  32   4  crc32 over [0, 32)
constexpr uint32_t kRecordSize = 36;

constexpr const char* kTempPath = "/USER/progress.tmp";

}  // namespace

bool LoadReadingProgress(hal::IStorage* storage, ReadingProgress* out) {
  if (out != nullptr) *out = ReadingProgress();
  if (storage == nullptr || out == nullptr) return false;

  uint8_t buf[kRecordSize];
  const int32_t n = storage->read(app::kFileProgress, 0, buf, kRecordSize);
  if (n != static_cast<int32_t>(kRecordSize)) return false;

  if (buf[0] != kMagic0 || buf[1] != kMagic1 || buf[2] != kMagic2 ||
      buf[3] != kMagic3) {
    return false;
  }
  if (qpk::Read16(buf + 4) != kFormatVersion) return false;
  if (qpk::Crc32(buf, 32) != qpk::Read32(buf + 32)) return false;

  out->surah_id = qpk::Read16(buf + 6);
  out->first_ayah_index = qpk::Read32(buf + 8);
  out->screen_number = qpk::Read16(buf + 12);
  memcpy(out->content_id, buf + 16, 16);
  return true;
}

bool SaveReadingProgress(hal::IStorage* storage, const ReadingProgress& progress) {
  if (storage == nullptr) return false;

  uint8_t buf[kRecordSize];
  memset(buf, 0, sizeof(buf));
  buf[0] = kMagic0;
  buf[1] = kMagic1;
  buf[2] = kMagic2;
  buf[3] = kMagic3;
  qpk::Write16(buf + 4, kFormatVersion);
  qpk::Write16(buf + 6, progress.surah_id);
  qpk::Write32(buf + 8, progress.first_ayah_index);
  qpk::Write16(buf + 12, progress.screen_number);
  memcpy(buf + 16, progress.content_id, 16);
  qpk::Write32(buf + 32, qpk::Crc32(buf, 32));

  // Same discipline as LibraryIndex::save(): the rename is the commit point,
  // so a reader sees either the old record or the new one, never a partial
  // write.
  storage->remove(kTempPath);
  if (!storage->writeAll(kTempPath, buf, kRecordSize)) {
    storage->remove(kTempPath);
    return false;
  }
  storage->remove(app::kFileProgress);
  if (!storage->rename(kTempPath, app::kFileProgress)) {
    storage->remove(kTempPath);
    return false;
  }
  return true;
}

namespace {

// 32-byte record:
//   0   4  magic "PPG1"
//   4   2  format_version
//   6   2  reserved
//   8   4  page (0-based)
//  12  16  content_id
//  28   4  crc32 over [0, 28)
constexpr uint32_t kPageRecordSize = 32;
constexpr const char* kPagePath = "/USER/pages.bin";
constexpr const char* kPageTempPath = "/USER/pages.tmp";

}  // namespace

bool LoadPageProgress(hal::IStorage* storage, PageProgress* out) {
  if (out != nullptr) *out = PageProgress();
  if (storage == nullptr || out == nullptr) return false;

  uint8_t buf[kPageRecordSize];
  if (storage->read(kPagePath, 0, buf, kPageRecordSize) !=
      static_cast<int32_t>(kPageRecordSize)) {
    return false;
  }
  if (buf[0] != 'P' || buf[1] != 'P' || buf[2] != 'G' || buf[3] != '1') return false;
  if (qpk::Read16(buf + 4) != kFormatVersion) return false;
  if (qpk::Crc32(buf, 28) != qpk::Read32(buf + 28)) return false;

  out->page = qpk::Read32(buf + 8);
  memcpy(out->content_id, buf + 12, 16);
  return true;
}

bool SavePageProgress(hal::IStorage* storage, const PageProgress& progress) {
  if (storage == nullptr) return false;

  uint8_t buf[kPageRecordSize];
  memset(buf, 0, sizeof(buf));
  buf[0] = 'P';
  buf[1] = 'P';
  buf[2] = 'G';
  buf[3] = '1';
  qpk::Write16(buf + 4, kFormatVersion);
  qpk::Write32(buf + 8, progress.page);
  memcpy(buf + 12, progress.content_id, 16);
  qpk::Write32(buf + 28, qpk::Crc32(buf, 28));

  storage->remove(kPageTempPath);
  if (!storage->writeAll(kPageTempPath, buf, kPageRecordSize)) {
    storage->remove(kPageTempPath);
    return false;
  }
  storage->remove(kPagePath);
  if (!storage->rename(kPageTempPath, kPagePath)) {
    storage->remove(kPageTempPath);
    return false;
  }
  return true;
}

}  // namespace net
