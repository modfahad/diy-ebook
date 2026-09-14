// storage.h -- persistent storage abstraction (SD/TF card).
//
// Two access styles, on purpose:
//
//   * IStorage::read()/writeAll() -- one-shot convenience for small files
//     (state.bin, settings.bin). Each call opens and closes the file.
//
//   * IStorage::open() -> IFile -- a handle held open across many small random
//     reads. This is what the QPK parser uses: resolving an ayah reads a
//     24-byte record, and a rendered page does hundreds of those. Re-opening
//     the file per record would be pathological.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {

struct StorageInfo {
  bool mounted = false;
  uint64_t capacity_bytes = 0;
  uint64_t used_bytes = 0;
  const char* type = "unknown";
};

// An open, randomly-readable file. Handles are owned by the IStorage that
// produced them; release one with close().
class IFile {
 public:
  virtual ~IFile() = default;

  virtual bool valid() const = 0;
  virtual uint64_t size() const = 0;

  // Reads up to `length` bytes at `offset`. Returns the number of bytes read,
  // or -1 on error. A SHORT READ IS NOT AN ERROR here -- callers that need
  // exactly `length` bytes must use readExact().
  virtual int32_t read(uint64_t offset, void* dst, uint32_t length) = 0;

  virtual void close() = 0;

  // Reads exactly `length` bytes or fails. Loops over read() so a backend
  // that returns short reads cannot silently truncate a record.
  bool readExact(uint64_t offset, void* dst, uint32_t length) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint32_t done = 0;
    while (done < length) {
      const int32_t n = read(offset + done, out + done, length - done);
      if (n <= 0) return false;
      done += static_cast<uint32_t>(n);
    }
    return true;
  }
};

// Iterates the entries of one directory. Like IFile, handles are owned by the
// IStorage that produced them.
class IDirectory {
 public:
  virtual ~IDirectory() = default;

  // Copies the next entry's name into `name` and its size into `size`.
  // Returns false when the directory is exhausted. Sub-directories are
  // skipped: nothing in the library layout nests.
  virtual bool next(char* name, uint32_t capacity, uint64_t* size) = 0;

  virtual void close() = 0;
};

class IStorage {
 public:
  virtual ~IStorage() = default;

  // Powers the rail and mounts the filesystem. Idempotent; called on every
  // wake. Returns false if no card / mount failed.
  virtual bool begin() = 0;

  // Unmounts and drops the rail.
  virtual void end() = 0;

  virtual bool mounted() const = 0;
  virtual StorageInfo info() const = 0;

  virtual bool exists(const char* path) const = 0;
  virtual bool mkdirs(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  virtual int64_t size(const char* path) const = 0;

  // Opens a file for repeated random reads. Returns nullptr if the file
  // cannot be opened or every handle slot is in use. The returned handle
  // belongs to this IStorage; release it with IFile::close().
  virtual IFile* open(const char* path) = 0;

  // One-shot read of up to `length` bytes starting at `offset`. Returns bytes
  // read, or -1 on error.
  virtual int32_t read(const char* path, uint64_t offset, void* dst,
                       uint32_t length) = 0;

  // Whole-file write.
  virtual bool writeAll(const char* path, const void* src, uint32_t length) = 0;

  // Appends to a file, creating it if absent. This is the resumable-upload
  // primitive: one chunk is one append followed by a close, so a power loss
  // costs at most one chunk and the file's size on disk is always the true
  // resume point.
  virtual bool append(const char* path, const void* src, uint32_t length) = 0;

  // Opens a directory for iteration. Returns nullptr if it does not exist or
  // no handle slot is free.
  virtual IDirectory* openDir(const char* path) = 0;

  // Free space in bytes. Used to refuse an upload that cannot fit.
  virtual uint64_t freeBytes() const = 0;
};

}  // namespace hal
