// memory_file.h -- hal::IFile over a byte buffer already in RAM.
//
// Used by the host tests to run the real parser against synthetic packages,
// and available to firmware for parsing a package staged in PSRAM (a Phase 3
// AI response, say) without going back to the card.

#pragma once

#include <stdint.h>
#include <string.h>

#include "hal/storage.h"

namespace qpk {

class MemoryFile : public hal::IFile {
 public:
  MemoryFile() = default;
  MemoryFile(const void* data, uint64_t size)
      : data_(static_cast<const uint8_t*>(data)), size_(size) {}

  void reset(const void* data, uint64_t size) {
    data_ = static_cast<const uint8_t*>(data);
    size_ = size;
  }

  bool valid() const override { return data_ != nullptr; }
  uint64_t size() const override { return size_; }

  int32_t read(uint64_t offset, void* dst, uint32_t length) override {
    if (data_ == nullptr || dst == nullptr) return -1;
    if (offset >= size_) return 0;
    const uint64_t remaining = size_ - offset;
    if (length > remaining) length = static_cast<uint32_t>(remaining);
    if (length != 0) memcpy(dst, data_ + offset, length);
    return static_cast<int32_t>(length);
  }

  void close() override {
    data_ = nullptr;
    size_ = 0;
  }

 private:
  const uint8_t* data_ = nullptr;
  uint64_t size_ = 0;
};

}  // namespace qpk
