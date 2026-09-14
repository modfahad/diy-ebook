// library_index.h -- /LIBRARY/library_index.bin (spec section 16).
//
// Device state, not content, so this is a small dedicated format rather than a
// QPK package: the section-table machinery would be overkill for a flat list.
// It reuses the *discipline* though -- magic, version, count, a CRC over the
// payload, and a .tmp-then-rename rewrite so a power loss never leaves a
// half-written index.
//
// The constraint the spec does not state but that matters most: the index MUST
// be rebuildable. A corrupt index would otherwise brick the whole library.
// rebuild() re-derives it from a bounded scan of the four content directories,
// reading each package's 64-byte header and metadata. That does not violate
// section 15's "never scan the SD card to find the reading position" -- that
// rule is about net::ReadingProgress (/USER/progress.bin, reading_progress.h),
// and this path only runs when the index is unreadable.

#pragma once

#include <stdint.h>

#include "hal/storage.h"
#include "net/protocol.h"

namespace net {

constexpr uint16_t kLibraryIndexHeaderSize = 32;
constexpr uint16_t kLibraryEntrySize = 256;
constexpr uint16_t kMaxLibraryEntries = 96;
constexpr uint16_t kLibraryIndexVersion = 1;

struct LibraryEntry {
  uint8_t content_id[16] = {0};
  uint32_t content_version = 0;
  uint64_t package_size = 0;
  uint32_t payload_crc32 = 0;
  uint32_t installed_at = 0;  // unix seconds, 0 if the clock was unset
  uint16_t type = 0;
  uint16_t flags = 0;
  char title[kTitleMaxBytes] = {0};
  char author[kAuthorMaxBytes] = {0};
  char language[kLanguageMaxBytes] = {0};
};

/**
 * The package's path under /LIBRARY, e.g. "QURAN/abcd....qpk".
 *
 * Derived from `type` and `content_id` rather than stored: a stored copy could
 * disagree with where the file actually is, and there is exactly one correct
 * answer. `capacity` must be at least 56.
 */
bool BuildLocation(const LibraryEntry& entry, char* out, uint32_t capacity);

/** Absolute path, i.e. "/LIBRARY/" + BuildLocation(). `capacity` >= 64. */
bool BuildPackagePath(const LibraryEntry& entry, char* out, uint32_t capacity);

/** Concatenates up to three parts into a fixed buffer, always terminating. */
void JoinPath(char* out, uint32_t capacity, const char* a, const char* b,
              const char* c);

/** 32 hex chars -> 16 bytes. Returns false on any non-hex input. */
bool HexToContentId(const char* hex, uint8_t out[16]);

/** 16 bytes -> 32 lower-case hex chars plus a NUL. `capacity` must be >= 33. */
bool ContentIdToHex(const uint8_t id[16], char* out, uint32_t capacity);

/**
 * Copies at most `capacity - 1` bytes of UTF-8, never splitting a multi-byte
 * sequence, and NUL-terminates. Truncating mid-sequence would put invalid
 * UTF-8 into the index and out through the API.
 */
void CopyUtf8Truncated(const char* src, uint32_t src_length, char* out,
                       uint32_t capacity);

class LibraryIndex {
 public:
  /** Reads and validates the index. False means "absent or corrupt". */
  bool load(hal::IStorage* storage);

  /** Writes to a .tmp then renames. */
  bool save(hal::IStorage* storage) const;

  /**
   * Re-derives the index from the content directories. Used when load() fails
   * and after any operation that could have left the index behind reality.
   */
  bool rebuild(hal::IStorage* storage);

  uint16_t count() const { return count_; }
  const LibraryEntry* at(uint16_t index) const;
  const LibraryEntry* find(const uint8_t content_id[16]) const;

  /**
   * Drops entries whose package is missing or unreadable, returning how many
   * went.
   *
   * This is the other half of the atomic install. Replacing an installed
   * package means remove-then-rename, and FAT gives no way to make those one
   * operation: lose power between them and the index names a file that is no
   * longer there. The invariant the system can actually offer is therefore
   * not "never" but "not after boot recovery" -- and this is the call that
   * makes that true. Run it at boot, after load(), beside
   * UploadManager::sweepOrphans().
   *
   * The caller decides whether to save(); pruning nothing should not cost a
   * write.
   */
  uint16_t prune(hal::IStorage* storage);

  /** Adds, or replaces an entry with the same content id. */
  bool upsert(const LibraryEntry& entry);
  bool remove(const uint8_t content_id[16]);
  void clear() { count_ = 0; }

  /** Fills `entry` from a package on disk. False if it is not a valid package. */
  static bool describePackage(hal::IStorage* storage, const char* path,
                              LibraryEntry* entry);

 private:
  LibraryEntry entries_[kMaxLibraryEntries];
  uint16_t count_ = 0;
};

}  // namespace net
