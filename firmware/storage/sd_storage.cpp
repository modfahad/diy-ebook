#include "drivers/sd_storage.h"

#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <string.h>

// The layer underneath SD.h. Needed only to tell two very different mount
// failures apart -- see ClassifyMountFailure below.
#include <ff.h>
#include <sd_diskio.h>

#include "app/app_config.h"
#include "board/board_crowpanel_579.h"
#include "drivers/serial_log.h"

namespace drivers {
namespace {

// SD.begin() collapses two unrelated failures into one false: no card on the
// bus at all, and a card that answers perfectly but carries no volume FatFs
// can mount (exFAT, NTFS, or unformatted). Reporting the first when it is
// really the second sends you looking for a wiring fault instead of running a
// five-minute reformat, so re-run the mount one layer down, where the two are
// distinguishable: FatFs only reaches FR_NO_FILESYSTEM after disk_initialize
// has completed the card's CMD0/ACMD41 handshake, which proves the card is
// present and talking. An absent or miswired card fails earlier, at
// FR_NOT_READY.
//
// Diagnostic-only, and only on the failure path. It unwinds everything it
// allocates -- including the FatFs drive slot SD.begin() has already freed --
// so a later begin() retry starts from the same state it otherwise would.
const char* ClassifyMountFailure(SPIClass& spi) {
  const uint8_t pdrv = sdcard_init(board::kSdCs, &spi, board::kSdSpiHz);
  if (pdrv == 0xFF) return "no card / wiring";

  // f_mount needs a work area, and FATFS is too big to want on the stack.
  FATFS* work = static_cast<FATFS*>(malloc(sizeof(FATFS)));
  if (work == nullptr) {
    sdcard_uninit(pdrv);
    return "no card / wiring";
  }

  const char drive[3] = {static_cast<char>('0' + pdrv), ':', '\0'};
  const FRESULT fr = f_mount(work, drive, 1);  // 1 = mount now, not lazily
  sdcard_unmount(pdrv);
  free(work);
  sdcard_uninit(pdrv);

  // Kept to 19 characters or fewer: the self-test screen prints these in the
  // "Card:" row, which starts at x=560 on a 792 px panel and advances 12 px
  // per character, so anything longer is silently clipped at the edge.
  switch (fr) {
    case FR_NO_FILESYSTEM: return "present, no FAT";
    case FR_NOT_READY:     return "no card / wiring";
    case FR_DISK_ERR:      return "card read error";
    default:               return "mount failed";
  }
}

}  // namespace

// --- SdFile -----------------------------------------------------------------

bool SdFile::openPath(const char* path) {
  file_ = SD.open(path, FILE_READ);
  if (!file_ || file_.isDirectory()) {
    if (file_) file_.close();
    return false;
  }
  size_ = file_.size();
  position_ = 0;
  in_use_ = true;
  return true;
}

int32_t SdFile::read(uint64_t offset, void* dst, uint32_t length) {
  if (!valid() || dst == nullptr) return -1;
  if (offset >= size_) return 0;
  // Arduino's seek() takes a uint32_t, so a >4 GiB package cannot be
  // addressed. That is far beyond anything this device will carry, but fail
  // loudly rather than silently wrapping.
  if (offset > 0xFFFFFFFFULL) return -1;

  const uint64_t remaining = size_ - offset;
  if (length > remaining) length = static_cast<uint32_t>(remaining);
  if (length == 0) return 0;

  if (offset != position_) {
    if (!file_.seek(static_cast<uint32_t>(offset))) return -1;
    position_ = offset;
  }
  const int n = file_.read(static_cast<uint8_t*>(dst), length);
  if (n < 0) return -1;
  position_ += static_cast<uint32_t>(n);
  return n;
}

void SdFile::close() {
  if (file_) file_.close();
  in_use_ = false;
  size_ = 0;
  position_ = 0;
}

// --- SdDirectory ------------------------------------------------------------

bool SdDirectory::openPath(const char* path) {
  dir_ = SD.open(path, FILE_READ);
  if (!dir_ || !dir_.isDirectory()) {
    if (dir_) dir_.close();
    return false;
  }
  in_use_ = true;
  return true;
}

bool SdDirectory::next(char* name, uint32_t capacity, uint64_t* size) {
  if (!in_use_ || name == nullptr || capacity == 0) return false;
  while (true) {
    fs::File entry = dir_.openNextFile();
    if (!entry) return false;
    if (entry.isDirectory()) {
      entry.close();
      continue;  // the library layout does not nest
    }
    const char* full = entry.name();
    // Some core versions hand back a full path; take the basename either way.
    const char* base = full;
    for (const char* p = full; *p != '\0'; ++p) {
      if (*p == '/' || *p == '\\') base = p + 1;
    }
    uint32_t i = 0;
    while (base[i] != '\0' && i + 1 < capacity) {
      name[i] = base[i];
      ++i;
    }
    name[i] = '\0';
    if (size != nullptr) *size = entry.size();
    entry.close();
    return true;
  }
}

void SdDirectory::close() {
  if (dir_) dir_.close();
  in_use_ = false;
}

// --- SdStorage --------------------------------------------------------------

bool SdStorage::begin() {
  if (mounted_) return true;

  pinMode(board::kSdPowerEn, OUTPUT);
  digitalWrite(board::kSdPowerEn, HIGH);
  delay(10);

  spi_.begin(board::kSdSck, board::kSdMiso, board::kSdMosi, board::kSdCs);
  if (!SD.begin(board::kSdCs, spi_, board::kSdSpiHz)) {
    last_error_ = ClassifyMountFailure(spi_);
    spi_.end();
    digitalWrite(board::kSdPowerEn, LOW);
    return false;
  }
  if (SD.cardType() == CARD_NONE) {
    last_error_ = "no card";
    SD.end();
    spi_.end();
    digitalWrite(board::kSdPowerEn, LOW);
    return false;
  }

  last_error_ = "";
  mounted_ = true;
  return true;
}

void SdStorage::end() {
  // Handles must not outlive the mount.
  for (SdFile& f : files_) {
    if (f.inUse()) f.close();
  }
  for (SdDirectory& d : dirs_) {
    if (d.inUse()) d.close();
  }
  if (mounted_) {
    SD.end();
    spi_.end();
    mounted_ = false;
  }
  digitalWrite(board::kSdPowerEn, LOW);
}

hal::StorageInfo SdStorage::info() const {
  hal::StorageInfo out;
  out.mounted = mounted_;
  if (!mounted_) return out;

  out.capacity_bytes = SD.cardSize();
  out.used_bytes = SD.usedBytes();
  switch (SD.cardType()) {
    case CARD_MMC:  out.type = "MMC";   break;
    case CARD_SD:   out.type = "SDSC";  break;
    case CARD_SDHC: out.type = "SDHC";  break;
    default:        out.type = "none";  break;
  }
  return out;
}

bool SdStorage::exists(const char* path) const {
  if (!mounted_ || path == nullptr) return false;
  return SD.exists(path);
}

bool SdStorage::mkdirs(const char* path) {
  if (!mounted_ || path == nullptr) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

bool SdStorage::remove(const char* path) {
  if (!mounted_ || path == nullptr) return false;
  return SD.remove(path);
}

bool SdStorage::rename(const char* from, const char* to) {
  if (!mounted_ || from == nullptr || to == nullptr) return false;
  if (SD.rename(from, to)) return true;
  drivers::Logf("[sd] rename %s -> %s failed", from, to);
  return false;
}

hal::IFile* SdStorage::open(const char* path) {
  if (!mounted_ || path == nullptr) return nullptr;
  for (SdFile& f : files_) {
    if (f.inUse()) continue;
    if (!f.openPath(path)) return nullptr;
    return &f;
  }
  last_error_ = "no free file handle";
  return nullptr;
}

uint8_t SdStorage::openHandleCount() const {
  uint8_t n = 0;
  for (const SdFile& f : files_) {
    if (f.inUse()) ++n;
  }
  return n;
}

int64_t SdStorage::size(const char* path) const {
  if (!mounted_ || path == nullptr) return -1;
  File f = SD.open(path, FILE_READ);
  if (!f) return -1;
  const int64_t n = static_cast<int64_t>(f.size());
  f.close();
  return n;
}

int32_t SdStorage::read(const char* path, uint64_t offset, void* dst,
                        uint32_t length) {
  if (!mounted_ || path == nullptr || dst == nullptr) return -1;
  File f = SD.open(path, FILE_READ);
  if (!f) return -1;
  if (offset != 0 && !f.seek(static_cast<uint32_t>(offset))) {
    f.close();
    return -1;
  }
  const int32_t n = f.read(static_cast<uint8_t*>(dst), length);
  f.close();
  return n;
}

bool SdStorage::writeAll(const char* path, const void* src, uint32_t length) {
  if (!mounted_ || path == nullptr) return false;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  const size_t n = f.write(static_cast<const uint8_t*>(src), length);
  f.close();
  return n == length;
}

bool SdStorage::append(const char* path, const void* src, uint32_t length) {
  if (!mounted_ || path == nullptr) return false;
  if (length == 0) return true;
  // One retry when nothing was written: a package upload failed with 507
  // part-way through megabytes of good chunks (2026-09-14), which an SD open
  // that fails once in a few hundred would explain. A partial write is not
  // retried -- that would duplicate bytes; the upload resumes from the file's
  // real size instead.
  for (int attempt = 1; attempt <= 2; ++attempt) {
    File f = SD.open(path, FILE_APPEND);
    if (!f) {
      drivers::Logf("[sd] append %s: open failed (attempt %d)", path, attempt);
      delay(20);
      continue;
    }
    const size_t before = f.size();
    const size_t n = f.write(static_cast<const uint8_t*>(src), length);
    // Close per chunk on purpose: it costs a little throughput and buys the
    // guarantee that a power loss during an upload costs at most one chunk.
    f.close();
    if (n == length) return true;
    drivers::Logf("[sd] append %s: wrote %u of %u bytes at offset %u (attempt %d)", path,
                  static_cast<unsigned>(n), static_cast<unsigned>(length),
                  static_cast<unsigned>(before), attempt);
    if (n != 0) return false;
    delay(20);
  }
  return false;
}

hal::IDirectory* SdStorage::openDir(const char* path) {
  if (!mounted_ || path == nullptr) return nullptr;
  for (SdDirectory& d : dirs_) {
    if (d.inUse()) continue;
    if (!d.openPath(path)) return nullptr;
    return &d;
  }
  last_error_ = "no free directory handle";
  return nullptr;
}

uint64_t SdStorage::freeBytes() const {
  if (!mounted_) return 0;
  const uint64_t total = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  return used >= total ? 0 : total - used;
}

bool SdStorage::appendLine(const char* path, const char* line) {
  if (!mounted_ || path == nullptr || line == nullptr) return false;
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  f.println(line);
  f.close();
  return true;
}

bool SdStorage::ensureLayout() {
  if (!mounted_) return false;
  static const char* kDirs[] = {
      app::kDirDevice,       app::kDirUser,   app::kDirLibrary,
      app::kDirQuran,        app::kDirBooks,  app::kDirTranslations,
      app::kDirTafsir,       app::kDirPhotos,
  };
  bool ok = true;
  for (const char* dir : kDirs) {
    if (!mkdirs(dir)) ok = false;
  }
  if (!ok) last_error_ = "mkdir failed";
  return ok;
}

}  // namespace drivers
