// sd_storage.h -- hal::IStorage backed by the on-board microSD slot.
//
// The card has its own SPI bus (HSPI) on this board, so no arbitration with
// the panel is needed. The rail on kSdPowerEn must be raised before begin().

#pragma once

#include <FS.h>
#include <SPI.h>
#include <stdint.h>

#include "hal/storage.h"

namespace drivers {

// One open file handle. Handles live in a fixed-size pool inside SdStorage,
// so opening a package never touches the heap.
class SdFile : public hal::IFile {
 public:
  bool valid() const override { return in_use_ && static_cast<bool>(file_); }
  uint64_t size() const override { return size_; }
  int32_t read(uint64_t offset, void* dst, uint32_t length) override;
  void close() override;

  // --- used by SdStorage only -------------------------------------------
  bool inUse() const { return in_use_; }
  bool openPath(const char* path);

 private:
  fs::File file_;
  bool in_use_ = false;
  uint64_t size_ = 0;
  uint64_t position_ = 0;  // tracked so sequential reads skip the seek
};

// One open directory handle, pooled the same way SdFile is.
class SdDirectory : public hal::IDirectory {
 public:
  bool next(char* name, uint32_t capacity, uint64_t* size) override;
  void close() override;

  bool inUse() const { return in_use_; }
  bool openPath(const char* path);

 private:
  fs::File dir_;
  bool in_use_ = false;
};

class SdStorage : public hal::IStorage {
 public:
  bool begin() override;
  void end() override;

  bool mounted() const override { return mounted_; }
  hal::StorageInfo info() const override;

  bool exists(const char* path) const override;
  bool mkdirs(const char* path) override;
  bool remove(const char* path) override;
  bool rename(const char* from, const char* to) override;
  int64_t size(const char* path) const override;

  hal::IFile* open(const char* path) override;

  int32_t read(const char* path, uint64_t offset, void* dst,
               uint32_t length) override;
  bool writeAll(const char* path, const void* src, uint32_t length) override;
  bool append(const char* path, const void* src, uint32_t length) override;
  hal::IDirectory* openDir(const char* path) override;
  uint64_t freeBytes() const override;

  // Appends one line to a text file, creating it if needed. Used by the
  // self-test to prove the card is writable, not just readable.
  bool appendLine(const char* path, const char* line);

  // Creates the standard directory layout from app_config.h. Safe to repeat.
  bool ensureLayout();

  const char* lastError() const { return last_error_; }

  // How many handle slots are currently taken. Exposed for diagnostics; a
  // leak here shows up as "cannot open package" much later otherwise.
  uint8_t openHandleCount() const;

  static constexpr uint8_t kMaxOpenFiles = 4;
  static constexpr uint8_t kMaxOpenDirs = 2;

 private:
  SPIClass spi_{HSPI};
  SdFile files_[kMaxOpenFiles];
  SdDirectory dirs_[kMaxOpenDirs];
  bool mounted_ = false;
  const char* last_error_ = "";
};

}  // namespace drivers
