#include "net/photo_store.h"

#include <stdio.h>
#include <string.h>

#include "app/app_config.h"

namespace net {
namespace {

constexpr const char* kPhotoExtension = ".g4";
constexpr const char* kPartExtension = ".part";

bool BuildPath(const char* name, const char* extension, char* out,
               size_t capacity) {
  const int n = snprintf(out, capacity, "%s/%s%s", app::kDirPhotos, name, extension);
  return n > 0 && static_cast<size_t>(n) < capacity;
}

bool EndsWith(const char* text, const char* suffix) {
  const size_t text_length = strlen(text);
  const size_t suffix_length = strlen(suffix);
  return text_length >= suffix_length &&
         strcmp(text + text_length - suffix_length, suffix) == 0;
}

constexpr uint8_t kMagic[4] = {'Q', 'P', 'H', '1'};

}  // namespace

bool ValidPhotoName(const char* name) {
  if (name == nullptr) return false;
  size_t length = 0;
  for (const char* p = name; *p != '\0'; ++p) {
    const char c = *p;
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                         c == '-' || c == '_';
    if (!allowed || length >= kPhotoNameMaxChars) return false;
    ++length;
  }
  return length > 0;
}

bool ParsePhotoHeader(const uint8_t* header) {
  if (header == nullptr || memcmp(header, kMagic, sizeof(kMagic)) != 0) return false;
  const uint16_t width = static_cast<uint16_t>(header[4] | (header[5] << 8));
  const uint16_t height = static_cast<uint16_t>(header[6] | (header[7] << 8));
  if (width != kPhotoWidth || height != kPhotoHeight || header[8] != 2) return false;
  for (uint32_t i = 9; i < kPhotoHeaderBytes; ++i) {
    if (header[i] != 0) return false;
  }
  return true;
}

void WritePhotoHeader(uint8_t* header) {
  memset(header, 0, kPhotoHeaderBytes);
  memcpy(header, kMagic, sizeof(kMagic));
  header[4] = static_cast<uint8_t>(kPhotoWidth & 0xFF);
  header[5] = static_cast<uint8_t>(kPhotoWidth >> 8);
  header[6] = static_cast<uint8_t>(kPhotoHeight & 0xFF);
  header[7] = static_cast<uint8_t>(kPhotoHeight >> 8);
  header[8] = 2;
}

Error PhotoStore::writeChunk(const char* name, uint64_t offset,
                             const uint8_t* data, uint32_t length,
                             uint64_t* received, uint64_t* expected) {
  if (storage_ == nullptr) return Error::kStorageError;
  if (!ValidPhotoName(name) || (data == nullptr && length != 0)) {
    return Error::kBadRequest;
  }
  char part[64];
  if (!BuildPath(name, kPartExtension, part, sizeof(part))) return Error::kBadRequest;
  if (!storage_->mkdirs(app::kDirPhotos)) return Error::kStorageError;

  int64_t have = storage_->exists(part) ? storage_->size(part) : 0;
  if (have < 0) return Error::kStorageError;
  if (offset == 0 && have > 0) {
    // A fresh upload of a name whose previous attempt was abandoned.
    if (!storage_->remove(part)) return Error::kStorageError;
    have = 0;
  }
  if (offset != static_cast<uint64_t>(have)) {
    if (expected != nullptr) *expected = static_cast<uint64_t>(have);
    return Error::kOffsetMismatch;
  }
  if (offset + length > kPhotoFileBytes) return Error::kSizeMismatch;
  if (storage_->freeBytes() < length) return Error::kNoSpace;
  if (!storage_->append(part, data, length)) return Error::kStorageError;
  if (received != nullptr) *received = offset + length;
  return Error::kOk;
}

Error PhotoStore::finish(const char* name) {
  if (storage_ == nullptr) return Error::kStorageError;
  if (!ValidPhotoName(name)) return Error::kBadRequest;
  char part[64];
  char photo[64];
  if (!BuildPath(name, kPartExtension, part, sizeof(part)) ||
      !BuildPath(name, kPhotoExtension, photo, sizeof(photo))) {
    return Error::kBadRequest;
  }
  if (!storage_->exists(part)) return Error::kNoSession;
  if (storage_->size(part) != static_cast<int64_t>(kPhotoFileBytes)) {
    return Error::kSizeMismatch;
  }
  uint8_t header[kPhotoHeaderBytes];
  if (storage_->read(part, 0, header, kPhotoHeaderBytes) !=
          static_cast<int32_t>(kPhotoHeaderBytes) ||
      !ParsePhotoHeader(header)) {
    storage_->remove(part);
    return Error::kVerifyFailed;
  }
  if (storage_->exists(photo) && !storage_->remove(photo)) return Error::kStorageError;
  if (!storage_->rename(part, photo)) return Error::kStorageError;
  return Error::kOk;
}

Error PhotoStore::remove(const char* name) {
  if (storage_ == nullptr) return Error::kStorageError;
  if (!ValidPhotoName(name)) return Error::kBadRequest;
  char part[64];
  char photo[64];
  if (!BuildPath(name, kPartExtension, part, sizeof(part)) ||
      !BuildPath(name, kPhotoExtension, photo, sizeof(photo))) {
    return Error::kBadRequest;
  }
  const bool had_photo = storage_->exists(photo);
  const bool had_part = storage_->exists(part);
  if (!had_photo && !had_part) return Error::kNotFound;
  if (had_photo && !storage_->remove(photo)) return Error::kStorageError;
  if (had_part && !storage_->remove(part)) return Error::kStorageError;
  return Error::kOk;
}

uint16_t PhotoStore::list(PhotoName* out, uint16_t capacity) const {
  if (storage_ == nullptr || out == nullptr || capacity == 0) return 0;
  hal::IDirectory* dir = storage_->openDir(app::kDirPhotos);
  if (dir == nullptr) return 0;

  uint16_t count = 0;
  char entry[64];
  uint64_t size = 0;
  while (dir->next(entry, sizeof(entry), &size)) {
    if (size != kPhotoFileBytes || !EndsWith(entry, kPhotoExtension)) continue;
    entry[strlen(entry) - strlen(kPhotoExtension)] = '\0';
    if (!ValidPhotoName(entry)) continue;

    // Insertion into the sorted list; past capacity, the largest names drop off.
    uint16_t position = count;
    while (position > 0 && strcmp(out[position - 1].name, entry) > 0) --position;
    if (position >= capacity) continue;
    const uint16_t last = count < capacity ? count : static_cast<uint16_t>(capacity - 1);
    for (uint16_t i = last; i > position; --i) out[i] = out[i - 1];
    // ValidPhotoName() above already bounds it to kPhotoNameMaxChars (32).
    snprintf(out[position].name, sizeof(out[position].name), "%.32s", entry);
    if (count < capacity) ++count;
  }
  dir->close();
  return count;
}

bool PhotoStore::load(const char* name, uint8_t* pixels, uint32_t capacity) const {
  if (storage_ == nullptr || pixels == nullptr || capacity < kPhotoPixelBytes ||
      !ValidPhotoName(name)) {
    return false;
  }
  char photo[64];
  if (!BuildPath(name, kPhotoExtension, photo, sizeof(photo))) return false;
  uint8_t header[kPhotoHeaderBytes];
  if (storage_->read(photo, 0, header, kPhotoHeaderBytes) !=
          static_cast<int32_t>(kPhotoHeaderBytes) ||
      !ParsePhotoHeader(header)) {
    return false;
  }
  return storage_->read(photo, kPhotoHeaderBytes, pixels, kPhotoPixelBytes) ==
         static_cast<int32_t>(kPhotoPixelBytes);
}

}  // namespace net
