#include "net/upload_manager.h"

#include <string.h>

#include "qpk/byte_order.h"
#include "qpk/crc32.h"
#include "qpk/qpk_reader.h"

namespace net {
namespace {

// .meta layout (160 bytes):
//    0   4  magic "UPL1"
//    4   2  format_version
//    6   2  package type
//    8   4  content_version
//   12   8  declared size
//   20   4  declared payload_crc32
//   24   4  expires_at (unix seconds; 0 = no deadline recorded)
//   28  96  title
//  124  32  content id, hex
//  156   4  crc32 over [0, 156)
// Offset 24 was zero padding between payload_crc32 (20..23) and the title.
// expires_at lives there, so adding it moved no field and changed no size --
// and a .meta written before this field existed reads back as 0, which is
// already the documented "no deadline" value. No format version bump needed.
constexpr uint16_t kMetaExpiresAt = 24;
constexpr uint16_t kMetaTitleAt = 28;
constexpr uint16_t kMetaIdAt = 124;
constexpr uint16_t kMetaCrcAt = 156;
constexpr uint16_t kMetaVersion = 1;

constexpr uint32_t kCrcChunkBytes = 512;

bool ValidHexId(const char* hex) {
  if (hex == nullptr) return false;
  uint8_t scratch[16];
  return HexToContentId(hex, scratch);
}

bool EndsWith(const char* text, const char* suffix) {
  const size_t text_length = strlen(text);
  const size_t suffix_length = strlen(suffix);
  if (suffix_length > text_length) return false;
  return memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0;
}

}  // namespace

void UploadManager::begin(hal::IStorage* storage, LibraryIndex* index) {
  storage_ = storage;
  index_ = index;
  if (storage_ != nullptr) storage_->mkdirs(kUploadDir);
}

namespace {

bool BuildUploadPath(const char* content_id_hex, const char* suffix, char* out,
                     uint32_t capacity) {
  if (!ValidHexId(content_id_hex)) return false;
  char name[kContentIdHexChars + 8];
  JoinPath(name, sizeof(name), content_id_hex, suffix, nullptr);
  JoinPath(out, capacity, kUploadDir, "/", name);
  return true;
}

}  // namespace

bool UploadManager::metaPath(const char* content_id_hex, char* out,
                             uint32_t capacity) const {
  return BuildUploadPath(content_id_hex, ".meta", out, capacity);
}

bool UploadManager::partPath(const char* content_id_hex, char* out,
                             uint32_t capacity) const {
  return BuildUploadPath(content_id_hex, ".part", out, capacity);
}

uint64_t UploadManager::partSize(const char* content_id_hex) const {
  char path[96];
  if (!partPath(content_id_hex, path, sizeof(path))) return 0;
  const int64_t size = storage_->size(path);
  return size < 0 ? 0 : static_cast<uint64_t>(size);
}

bool UploadManager::writeMeta(const UploadBegin& request) {
  uint8_t meta[kUploadMetaSize];
  memset(meta, 0, sizeof(meta));
  meta[0] = 'U';
  meta[1] = 'P';
  meta[2] = 'L';
  meta[3] = '1';
  qpk::Write16(meta + 4, kMetaVersion);
  qpk::Write16(meta + 6, request.type);
  qpk::Write32(meta + 8, request.content_version);
  qpk::Write64(meta + 12, request.size);
  qpk::Write32(meta + 20, request.payload_crc32);
  qpk::Write32(meta + kMetaExpiresAt, request.expires_at);
  CopyUtf8Truncated(request.title, static_cast<uint32_t>(strlen(request.title)),
                    reinterpret_cast<char*>(meta + kMetaTitleAt), kTitleMaxBytes);
  memcpy(meta + kMetaIdAt, request.content_id_hex, kContentIdHexChars);
  qpk::Write32(meta + kMetaCrcAt, qpk::Crc32(meta, kMetaCrcAt));

  char path[96];
  if (!metaPath(request.content_id_hex, path, sizeof(path))) return false;
  return storage_->writeAll(path, meta, kUploadMetaSize);
}

bool UploadManager::readMeta(const char* content_id_hex, UploadBegin* out) const {
  char path[96];
  if (out == nullptr) return false;
  if (!metaPath(content_id_hex, path, sizeof(path))) return false;

  uint8_t meta[kUploadMetaSize];
  if (storage_->read(path, 0, meta, kUploadMetaSize) !=
      static_cast<int32_t>(kUploadMetaSize)) {
    return false;
  }
  if (meta[0] != 'U' || meta[1] != 'P' || meta[2] != 'L' || meta[3] != '1') {
    return false;
  }
  if (qpk::Read16(meta + 4) != kMetaVersion) return false;
  if (qpk::Crc32(meta, kMetaCrcAt) != qpk::Read32(meta + kMetaCrcAt)) return false;

  *out = UploadBegin();
  out->type = qpk::Read16(meta + 6);
  out->content_version = qpk::Read32(meta + 8);
  out->size = qpk::Read64(meta + 12);
  out->payload_crc32 = qpk::Read32(meta + 20);
  out->expires_at = qpk::Read32(meta + kMetaExpiresAt);
  memcpy(out->title, meta + kMetaTitleAt, kTitleMaxBytes);
  out->title[kTitleMaxBytes - 1] = 0;
  memcpy(out->content_id_hex, meta + kMetaIdAt, kContentIdHexChars);
  out->content_id_hex[kContentIdHexChars] = 0;
  return true;
}

bool UploadManager::expired(const UploadBegin& meta, uint32_t now_unix) {
  if (meta.expires_at == 0) return false;  // no deadline recorded
  if (now_unix == 0) return false;         // no clock to judge it with
  return now_unix > meta.expires_at;
}

void UploadManager::discard(const char* content_id_hex) {
  char path[96];
  if (partPath(content_id_hex, path, sizeof(path))) storage_->remove(path);
  if (metaPath(content_id_hex, path, sizeof(path))) storage_->remove(path);
}

// --- begin ------------------------------------------------------------------

Error UploadManager::beginUpload(const UploadBegin& request,
                                 uint64_t* received_out, uint32_t now_unix,
                                 uint32_t* expires_at_out) {
  if (storage_ == nullptr || received_out == nullptr) return Error::kStorageError;
  if (expires_at_out != nullptr) *expires_at_out = 0;
  if (!ValidHexId(request.content_id_hex)) return Error::kBadRequest;
  if (request.size == 0) return Error::kBadRequest;
  if (LibraryDirFor(request.type) == nullptr) return Error::kBadRequest;
  if (request.size > 0xFFFFFFFFull) return Error::kBadRequest;  // SD seek limit

  storage_->mkdirs(kUploadDir);

  UploadBegin existing;
  if (readMeta(request.content_id_hex, &existing) &&
      !expired(existing, now_unix)) {
    const bool same = existing.content_version == request.content_version &&
                      existing.size == request.size &&
                      existing.payload_crc32 == request.payload_crc32 &&
                      existing.type == request.type;
    if (same) {
      // Resume. The byte count comes off the filesystem, not from memory.
      *received_out = partSize(request.content_id_hex);
      if (expires_at_out != nullptr) *expires_at_out = existing.expires_at;
      if (*received_out > request.size) {
        // A .part longer than declared cannot be a prefix of this package.
        discard(request.content_id_hex);
        *received_out = 0;
      } else {
        return Error::kOk;
      }
    } else {
      discard(request.content_id_hex);
    }
  } else {
    // A .part with no readable .meta can never be finished. Drop it.
    discard(request.content_id_hex);
  }

  if (storage_->freeBytes() < request.size) return Error::kNoSpace;
  UploadBegin fresh = request;
  fresh.expires_at =
      now_unix == 0 ? 0 : now_unix + kSessionExpirySeconds;  // 0 = no clock
  if (!writeMeta(fresh)) return Error::kStorageError;
  if (expires_at_out != nullptr) *expires_at_out = fresh.expires_at;
  *received_out = 0;
  return Error::kOk;
}

// --- chunk ------------------------------------------------------------------

Error UploadManager::writeChunk(const char* content_id_hex, uint64_t offset,
                                const void* data, uint32_t length,
                                uint64_t* received_out, uint64_t* expected_out,
                                uint32_t now_unix) {
  if (storage_ == nullptr || received_out == nullptr) return Error::kStorageError;
  if (data == nullptr && length > 0) return Error::kBadRequest;
  if (length > kMaxChunkBytes) return Error::kChunkTooLarge;

  UploadBegin meta;
  if (!readMeta(content_id_hex, &meta)) return Error::kNoSession;
  if (expired(meta, now_unix)) {
    // Reclaim it here rather than waiting for a boot sweep: the client is
    // talking to us right now, and kNoSession tells it to re-issue begin --
    // which is exactly the recovery it already implements for a lost session.
    discard(content_id_hex);
    return Error::kNoSession;
  }

  const uint64_t current = partSize(content_id_hex);
  if (expected_out != nullptr) *expected_out = current;

  if (offset > meta.size || length > meta.size - offset) {
    return Error::kSizeMismatch;
  }

  // Already applied in full: a retried chunk after a lost response.
  if (offset + length <= current) {
    *received_out = current;
    return Error::kOk;
  }
  // Any other disagreement is a real desync; tell the client where to resume.
  if (offset != current) return Error::kOffsetMismatch;

  char path[96];
  if (!partPath(content_id_hex, path, sizeof(path))) return Error::kBadRequest;
  if (!storage_->append(path, data, length)) return Error::kStorageError;

  *received_out = current + length;
  if (expected_out != nullptr) *expected_out = *received_out;
  return Error::kOk;
}

// --- finish -----------------------------------------------------------------

Error UploadManager::finish(const char* content_id_hex, uint32_t now_unix,
                            LibraryEntry* installed_out) {
  if (storage_ == nullptr || index_ == nullptr) return Error::kStorageError;

  UploadBegin meta;
  if (!readMeta(content_id_hex, &meta)) return Error::kNoSession;
  if (expired(meta, now_unix)) {
    // Refuse to install from a session whose deadline passed. The bytes may
    // be complete and may even verify, but the session was reclaimable and
    // the desktop has no way to know we did not reclaim it -- installing
    // anyway would make the deadline a fiction.
    discard(content_id_hex);
    return Error::kNoSession;
  }

  char part[96];
  if (!partPath(content_id_hex, part, sizeof(part))) return Error::kBadRequest;

  // 1. exactly the declared size
  if (partSize(content_id_hex) != meta.size) return Error::kSizeMismatch;

  // 2. the package's own header, and its payload checksum
  hal::IFile* file = storage_->open(part);
  if (file == nullptr) return Error::kStorageError;

  uint8_t header[qpk::kHeaderSize];
  bool verified = false;
  uint32_t header_size = 0;
  uint32_t declared_payload_crc = 0;
  do {
    if (!file->readExact(0, header, qpk::kHeaderSize)) break;
    if (header[0] != qpk::kMagic0 || header[1] != qpk::kMagic1 ||
        header[2] != qpk::kMagic2 || header[3] != qpk::kMagic3) {
      break;
    }
    if (qpk::Read16(header + 4) != qpk::kFormatVersion) break;
    header_size = qpk::Read16(header + 6);
    if (header_size < qpk::kHeaderSize || header_size > meta.size) break;
    if (qpk::Crc32(header, 60) != qpk::Read32(header + 60)) break;
    if (qpk::Read64(header + 16) != meta.size) break;

    declared_payload_crc = qpk::Read32(header + 52);
    // The desktop declared a checksum too. If it disagrees with the file, the
    // wrong package was uploaded under this id -- catch it here, not later.
    if (declared_payload_crc != meta.payload_crc32) break;

    uint32_t crc = qpk::kCrc32Init;
    uint8_t buffer[kCrcChunkBytes];
    uint64_t done = header_size;
    bool read_ok = true;
    while (done < meta.size) {
      const uint64_t remaining = meta.size - done;
      const uint32_t n = remaining < kCrcChunkBytes
                             ? static_cast<uint32_t>(remaining)
                             : kCrcChunkBytes;
      if (!file->readExact(done, buffer, n)) {
        read_ok = false;
        break;
      }
      crc = qpk::Crc32Update(crc, buffer, n);
      done += n;
    }
    if (!read_ok) break;
    if (qpk::Crc32Finish(crc) != declared_payload_crc) break;
    verified = true;
  } while (false);

  if (!verified) {
    file->close();
    discard(content_id_hex);
    return Error::kVerifyFailed;
  }

  // 3. full structural validation, the same the device does at open
  LibraryEntry entry;
  qpk::Reader reader;
  const qpk::Error parse = reader.open(file);
  if (parse != qpk::Error::kOk) {
    file->close();
    discard(content_id_hex);
    return Error::kPackageRejected;
  }
  reader.close();
  file->close();

  if (!LibraryIndex::describePackage(storage_, part, &entry)) {
    discard(content_id_hex);
    return Error::kPackageRejected;
  }
  if (entry.type != meta.type) {
    // The package says it is something else than was declared.
    discard(content_id_hex);
    return Error::kPackageRejected;
  }
  entry.installed_at = now_unix;

  // 4. the commit point
  char final_path[96];
  if (!BuildPackagePath(entry, final_path, sizeof(final_path))) {
    return Error::kStorageError;
  }
  char dir_path[64];
  JoinPath(dir_path, sizeof(dir_path), "/LIBRARY/", LibraryDirFor(entry.type),
           nullptr);
  storage_->mkdirs(dir_path);

  // Replacing an existing version: remove first, since rename-over is not
  // reliable on FAT. If power is lost between the two, the .part is still
  // there and resumable, and the index entry for the missing package is
  // dropped by the next rebuild.
  if (storage_->exists(final_path)) storage_->remove(final_path);
  if (!storage_->rename(part, final_path)) return Error::kStorageError;

  // 5. index, then 6. drop the .meta. Order matters: if the index write fails
  // the session is still finishable, and a rebuild would find the package
  // anyway.
  if (!index_->upsert(entry)) return Error::kStorageError;
  if (!index_->save(storage_)) return Error::kStorageError;

  char meta_path[96];
  if (metaPath(content_id_hex, meta_path, sizeof(meta_path))) {
    storage_->remove(meta_path);
  }

  if (installed_out != nullptr) *installed_out = entry;
  return Error::kOk;
}

Error UploadManager::abort(const char* content_id_hex) {
  if (storage_ == nullptr) return Error::kStorageError;
  if (!ValidHexId(content_id_hex)) return Error::kBadRequest;
  UploadBegin meta;
  const bool existed = readMeta(content_id_hex, &meta);
  discard(content_id_hex);
  return existed ? Error::kOk : Error::kNoSession;
}

// --- housekeeping -----------------------------------------------------------

uint16_t UploadManager::openSessionCount() const {
  if (storage_ == nullptr) return 0;
  hal::IDirectory* dir = storage_->openDir(kUploadDir);
  if (dir == nullptr) return 0;

  uint16_t count = 0;
  char name[64];
  uint64_t size = 0;
  while (dir->next(name, sizeof(name), &size)) {
    if (EndsWith(name, ".meta")) ++count;
  }
  dir->close();
  return count;
}

uint16_t UploadManager::sweepOrphans() {
  if (storage_ == nullptr) return 0;
  hal::IDirectory* dir = storage_->openDir(kUploadDir);
  if (dir == nullptr) return 0;

  // Collect first: removing files while iterating a directory is not something
  // every FAT driver copes with.
  char orphans[8][kContentIdHexChars + 1];
  uint8_t found = 0;
  char name[64];
  uint64_t size = 0;
  while (dir->next(name, sizeof(name), &size) && found < 8) {
    if (!EndsWith(name, ".part")) continue;
    const size_t length = strlen(name);
    if (length != kContentIdHexChars + 5) continue;
    char hex[kContentIdHexChars + 1];
    memcpy(hex, name, kContentIdHexChars);
    hex[kContentIdHexChars] = 0;
    if (!ValidHexId(hex)) continue;

    UploadBegin meta;
    if (readMeta(hex, &meta)) continue;  // a live session: keep it
    memcpy(orphans[found], hex, sizeof(hex));
    ++found;
  }
  dir->close();

  for (uint8_t i = 0; i < found; ++i) discard(orphans[i]);
  return found;
}

}  // namespace net
