#include "net/library_index.h"

#include <string.h>

#include "qpk/byte_order.h"
#include "qpk/crc32.h"
#include "qpk/qpk_reader.h"

namespace net {
namespace {

constexpr char kMagic0 = 'L';
constexpr char kMagic1 = 'I';
constexpr char kMagic2 = 'B';
constexpr char kMagic3 = '1';

constexpr const char* kIndexPath = "/LIBRARY/library_index.bin";
constexpr const char* kIndexTempPath = "/LIBRARY/library_index.tmp";

// Header layout (32 bytes):
//   0  4  magic "LIB1"
//   4  2  format_version
//   6  2  header_size
//   8  2  entry_count
//  10  2  entry_size
//  12  4  entries_crc32
//  16  8  file_size
//  24  4  reserved
//  28  4  header_crc32 over [0, 28)

constexpr uint16_t kTitleAt = 40;
constexpr uint16_t kAuthorAt = kTitleAt + kTitleMaxBytes;
constexpr uint16_t kLanguageAt = kAuthorAt + kAuthorMaxBytes;

static_assert(kLanguageAt + kLanguageMaxBytes <= kLibraryEntrySize,
              "library entry fields do not fit the record");

bool EndsWith(const char* text, const char* suffix) {
  const size_t text_length = strlen(text);
  const size_t suffix_length = strlen(suffix);
  if (suffix_length > text_length) return false;
  return memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0;
}

void EncodeEntry(const LibraryEntry& entry, uint8_t* out) {
  memset(out, 0, kLibraryEntrySize);
  memcpy(out + 0, entry.content_id, 16);
  qpk::Write32(out + 16, entry.content_version);
  qpk::Write64(out + 20, entry.package_size);
  qpk::Write32(out + 28, entry.payload_crc32);
  qpk::Write32(out + 32, entry.installed_at);
  qpk::Write16(out + 36, entry.type);
  qpk::Write16(out + 38, entry.flags);
  memcpy(out + kTitleAt, entry.title, kTitleMaxBytes);
  memcpy(out + kAuthorAt, entry.author, kAuthorMaxBytes);
  memcpy(out + kLanguageAt, entry.language, kLanguageMaxBytes);
}

void DecodeEntry(const uint8_t* in, LibraryEntry* entry) {
  memcpy(entry->content_id, in + 0, 16);
  entry->content_version = qpk::Read32(in + 16);
  entry->package_size = qpk::Read64(in + 20);
  entry->payload_crc32 = qpk::Read32(in + 28);
  entry->installed_at = qpk::Read32(in + 32);
  entry->type = qpk::Read16(in + 36);
  entry->flags = qpk::Read16(in + 38);
  memcpy(entry->title, in + kTitleAt, kTitleMaxBytes);
  memcpy(entry->author, in + kAuthorAt, kAuthorMaxBytes);
  memcpy(entry->language, in + kLanguageAt, kLanguageMaxBytes);
  // Anything read off disk must be NUL-terminated before it is ever printed.
  entry->title[kTitleMaxBytes - 1] = 0;
  entry->author[kAuthorMaxBytes - 1] = 0;
  entry->language[kLanguageMaxBytes - 1] = 0;
}

}  // namespace

// --- path helpers -----------------------------------------------------------

void JoinPath(char* out, uint32_t capacity, const char* a, const char* b,
              const char* c) {
  if (out == nullptr || capacity == 0) return;
  uint32_t at = 0;
  const char* parts[3] = {a, b, c};
  for (const char* part : parts) {
    if (part == nullptr) continue;
    for (const char* p = part; *p != 0; ++p) {
      if (at + 1 >= capacity) {
        out[at] = 0;
        return;
      }
      out[at++] = *p;
    }
  }
  out[at] = 0;
}

bool BuildLocation(const LibraryEntry& entry, char* out, uint32_t capacity) {
  if (out == nullptr || capacity < 56) return false;
  const char* dir = LibraryDirFor(entry.type);
  if (dir == nullptr) return false;
  char hex[33];
  if (!ContentIdToHex(entry.content_id, hex, sizeof(hex))) return false;
  char name[40];
  JoinPath(name, sizeof(name), hex, ".qpk", nullptr);
  JoinPath(out, capacity, dir, "/", name);
  return true;
}

bool BuildPackagePath(const LibraryEntry& entry, char* out, uint32_t capacity) {
  if (out == nullptr || capacity < 64) return false;
  char location[56];
  if (!BuildLocation(entry, location, sizeof(location))) return false;
  JoinPath(out, capacity, "/LIBRARY/", location, nullptr);
  return true;
}

bool HexToContentId(const char* hex, uint8_t out[16]) {
  if (hex == nullptr || out == nullptr) return false;
  for (uint8_t i = 0; i < 32; ++i) {
    const char c = hex[i];
    int value;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
    else return false;
    if ((i & 1) == 0) out[i / 2] = static_cast<uint8_t>(value << 4);
    else out[i / 2] = static_cast<uint8_t>(out[i / 2] | value);
  }
  return hex[32] == 0;
}

bool ContentIdToHex(const uint8_t id[16], char* out, uint32_t capacity) {
  if (id == nullptr || out == nullptr || capacity < 33) return false;
  static const char kDigits[] = "0123456789abcdef";
  for (uint8_t i = 0; i < 16; ++i) {
    out[i * 2] = kDigits[(id[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kDigits[id[i] & 0x0F];
  }
  out[32] = 0;
  return true;
}

void CopyUtf8Truncated(const char* src, uint32_t src_length, char* out,
                       uint32_t capacity) {
  if (out == nullptr || capacity == 0) return;
  out[0] = 0;
  if (src == nullptr || src_length == 0) return;

  uint32_t limit = src_length;
  const bool truncated = limit > capacity - 1;
  if (truncated) limit = capacity - 1;

  // Back off to a codepoint boundary, but only when truncation actually cut
  // the string short. Only then is src[limit] a real byte of `src` that a
  // multi-byte sequence could have been split across -- when the whole
  // string fits (limit == src_length), src[limit] is one past the last byte
  // the caller wrote, and reading it read uninitialized memory: found via a
  // host test that non-deterministically dropped the last character of a
  // plain-ASCII title ("Mini Test Package" -> "Mini Test Packag") whenever
  // that out-of-bounds byte happened to look like a continuation byte
  // (10xxxxxx). A continuation byte is 10xxxxxx, so walk back while the byte
  // at `limit` is one; cutting mid-sequence would put invalid UTF-8 into the
  // index and out through the API.
  if (truncated) {
    while (limit > 0 && (static_cast<unsigned char>(src[limit]) & 0xC0) == 0x80) {
      --limit;
    }
  }
  memcpy(out, src, limit);
  out[limit] = 0;
}

// --- load / save ------------------------------------------------------------

bool LibraryIndex::load(hal::IStorage* storage) {
  count_ = 0;
  if (storage == nullptr) return false;

  hal::IFile* file = storage->open(kIndexPath);
  if (file == nullptr) return false;

  bool ok = false;
  uint8_t header[kLibraryIndexHeaderSize];
  do {
    if (file->size() < kLibraryIndexHeaderSize) break;
    if (!file->readExact(0, header, kLibraryIndexHeaderSize)) break;
    if (header[0] != kMagic0 || header[1] != kMagic1 || header[2] != kMagic2 ||
        header[3] != kMagic3) {
      break;
    }
    if (qpk::Read16(header + 4) != kLibraryIndexVersion) break;
    if (qpk::Read16(header + 6) != kLibraryIndexHeaderSize) break;
    if (qpk::Crc32(header, 28) != qpk::Read32(header + 28)) break;

    const uint16_t entry_count = qpk::Read16(header + 8);
    const uint16_t entry_size = qpk::Read16(header + 10);
    if (entry_size != kLibraryEntrySize) break;
    if (entry_count > kMaxLibraryEntries) break;

    const uint64_t expected_size =
        static_cast<uint64_t>(kLibraryIndexHeaderSize) +
        static_cast<uint64_t>(entry_count) * kLibraryEntrySize;
    if (qpk::Read64(header + 16) != expected_size) break;
    if (file->size() != expected_size) break;

    uint32_t crc = qpk::kCrc32Init;
    uint8_t buffer[kLibraryEntrySize];
    bool read_ok = true;
    for (uint16_t i = 0; i < entry_count; ++i) {
      const uint64_t at = static_cast<uint64_t>(kLibraryIndexHeaderSize) +
                          static_cast<uint64_t>(i) * kLibraryEntrySize;
      if (!file->readExact(at, buffer, kLibraryEntrySize)) {
        read_ok = false;
        break;
      }
      crc = qpk::Crc32Update(crc, buffer, kLibraryEntrySize);
      DecodeEntry(buffer, &entries_[i]);
    }
    if (!read_ok) break;
    if (qpk::Crc32Finish(crc) != qpk::Read32(header + 12)) break;

    count_ = entry_count;
    ok = true;
  } while (false);

  file->close();
  if (!ok) count_ = 0;
  return ok;
}

bool LibraryIndex::save(hal::IStorage* storage) const {
  if (storage == nullptr) return false;

  uint32_t crc = qpk::kCrc32Init;
  uint8_t buffer[kLibraryEntrySize];
  for (uint16_t i = 0; i < count_; ++i) {
    EncodeEntry(entries_[i], buffer);
    crc = qpk::Crc32Update(crc, buffer, kLibraryEntrySize);
  }
  const uint32_t entries_crc = qpk::Crc32Finish(crc);

  uint8_t header[kLibraryIndexHeaderSize];
  memset(header, 0, sizeof(header));
  header[0] = kMagic0;
  header[1] = kMagic1;
  header[2] = kMagic2;
  header[3] = kMagic3;
  qpk::Write16(header + 4, kLibraryIndexVersion);
  qpk::Write16(header + 6, kLibraryIndexHeaderSize);
  qpk::Write16(header + 8, count_);
  qpk::Write16(header + 10, kLibraryEntrySize);
  qpk::Write32(header + 12, entries_crc);
  qpk::Write64(header + 16,
               static_cast<uint64_t>(kLibraryIndexHeaderSize) +
                   static_cast<uint64_t>(count_) * kLibraryEntrySize);
  qpk::Write32(header + 28, qpk::Crc32(header, 28));

  storage->remove(kIndexTempPath);
  if (!storage->writeAll(kIndexTempPath, header, kLibraryIndexHeaderSize)) {
    return false;
  }
  for (uint16_t i = 0; i < count_; ++i) {
    EncodeEntry(entries_[i], buffer);
    if (!storage->append(kIndexTempPath, buffer, kLibraryEntrySize)) {
      storage->remove(kIndexTempPath);
      return false;
    }
  }

  // The rename is the commit point: a reader sees either the old index or the
  // new one, never a half-written file.
  storage->remove(kIndexPath);
  if (!storage->rename(kIndexTempPath, kIndexPath)) {
    storage->remove(kIndexTempPath);
    return false;
  }
  return true;
}

// --- lookup / mutation ------------------------------------------------------

const LibraryEntry* LibraryIndex::at(uint16_t index) const {
  if (index >= count_) return nullptr;
  return &entries_[index];
}

const LibraryEntry* LibraryIndex::find(const uint8_t content_id[16]) const {
  if (content_id == nullptr) return nullptr;
  for (uint16_t i = 0; i < count_; ++i) {
    if (memcmp(entries_[i].content_id, content_id, 16) == 0) return &entries_[i];
  }
  return nullptr;
}

uint16_t LibraryIndex::prune(hal::IStorage* storage) {
  if (storage == nullptr) return 0;

  uint16_t dropped = 0;
  // Backwards, so removing an entry cannot skip the next one.
  for (uint16_t i = count_; i > 0; --i) {
    const uint16_t at = static_cast<uint16_t>(i - 1);
    char path[96];
    bool keep = BuildPackagePath(entries_[at], path, sizeof(path));
    if (keep) keep = storage->exists(path);
    if (keep) {
      // Present but unreadable is just as bad as absent: the library browser
      // would offer it and the reader would fail to open it.
      LibraryEntry parsed;
      keep = describePackage(storage, path, &parsed);
    }
    if (keep) continue;

    for (uint16_t j = static_cast<uint16_t>(at + 1); j < count_; ++j) {
      entries_[j - 1] = entries_[j];
    }
    --count_;
    ++dropped;
  }
  return dropped;
}

bool LibraryIndex::upsert(const LibraryEntry& entry) {
  for (uint16_t i = 0; i < count_; ++i) {
    if (memcmp(entries_[i].content_id, entry.content_id, 16) == 0) {
      entries_[i] = entry;
      return true;
    }
  }
  if (count_ >= kMaxLibraryEntries) return false;
  entries_[count_++] = entry;
  return true;
}

bool LibraryIndex::remove(const uint8_t content_id[16]) {
  for (uint16_t i = 0; i < count_; ++i) {
    if (memcmp(entries_[i].content_id, content_id, 16) != 0) continue;
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < count_; ++j) {
      entries_[j - 1] = entries_[j];
    }
    --count_;
    return true;
  }
  return false;
}

// --- describe / rebuild -----------------------------------------------------

bool LibraryIndex::describePackage(hal::IStorage* storage, const char* path,
                                   LibraryEntry* entry) {
  if (storage == nullptr || path == nullptr || entry == nullptr) return false;

  hal::IFile* file = storage->open(path);
  if (file == nullptr) return false;

  qpk::Reader reader;
  if (reader.open(file) != qpk::Error::kOk) {
    file->close();
    return false;
  }

  *entry = LibraryEntry();
  memcpy(entry->content_id, reader.header().content_id, 16);
  entry->content_version = reader.header().content_version;
  entry->package_size = reader.header().package_size;
  entry->payload_crc32 = reader.header().payload_crc32;
  entry->type = static_cast<uint16_t>(reader.header().package_type);
  entry->flags = static_cast<uint16_t>(reader.header().flags & 0xFFFFu);

  char scratch[kTitleMaxBytes * 2];
  uint32_t length = 0;
  if (reader.metadata(qpk::MetadataKey::kTitle, scratch, sizeof(scratch),
                      &length)) {
    if (length > sizeof(scratch)) length = sizeof(scratch);
    CopyUtf8Truncated(scratch, length, entry->title, kTitleMaxBytes);
  }
  if (reader.metadata(qpk::MetadataKey::kAuthor, scratch, sizeof(scratch),
                      &length)) {
    if (length > sizeof(scratch)) length = sizeof(scratch);
    CopyUtf8Truncated(scratch, length, entry->author, kAuthorMaxBytes);
  }
  if (reader.metadata(qpk::MetadataKey::kLanguage, scratch, sizeof(scratch),
                      &length)) {
    if (length > sizeof(scratch)) length = sizeof(scratch);
    CopyUtf8Truncated(scratch, length, entry->language, kLanguageMaxBytes);
  }

  reader.close();
  file->close();
  return true;
}

bool LibraryIndex::rebuild(hal::IStorage* storage) {
  clear();
  if (storage == nullptr) return false;

  static const char* kDirs[] = {"QURAN", "BOOKS", "TRANSLATIONS", "TAFSIR"};
  char dir_path[48];
  char path[112];
  char name[64];

  for (const char* dir : kDirs) {
    JoinPath(dir_path, sizeof(dir_path), "/LIBRARY/", dir, nullptr);
    hal::IDirectory* handle = storage->openDir(dir_path);
    if (handle == nullptr) continue;

    uint64_t size = 0;
    while (handle->next(name, sizeof(name), &size)) {
      if (!EndsWith(name, ".qpk")) continue;
      JoinPath(path, sizeof(path), dir_path, "/", name);

      LibraryEntry entry;
      if (!describePackage(storage, path, &entry)) continue;
      if (!upsert(entry)) break;  // index is full
    }
    handle->close();
  }
  return true;
}

}  // namespace net
