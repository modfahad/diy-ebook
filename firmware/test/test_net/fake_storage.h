// fake_storage.h -- an in-memory hal::IStorage for host tests.
//
// The interesting part is power-loss injection. `cutPowerAfter(n)` lets the
// nth mutating operation be the last one that happens: the fake throws
// PowerLoss, which models the device stopping mid-call far better than
// returning false would. The test catches it, then builds fresh objects over
// the same "disk" -- that is the reboot.
//
// Host tests only. Uses std::map/std::vector/exceptions, none of which the
// firmware may.

#pragma once

#include <stdint.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "hal/storage.h"

namespace faketest {

struct PowerLoss {};

class FakeStorage;

class FakeFile : public hal::IFile {
 public:
  FakeFile(std::vector<uint8_t>* data) : data_(data) {}

  bool valid() const override { return data_ != nullptr; }
  uint64_t size() const override { return data_ == nullptr ? 0 : data_->size(); }

  int32_t read(uint64_t offset, void* dst, uint32_t length) override {
    if (data_ == nullptr || dst == nullptr) return -1;
    if (offset >= data_->size()) return 0;
    const uint64_t remaining = data_->size() - offset;
    if (length > remaining) length = static_cast<uint32_t>(remaining);
    if (length != 0) memcpy(dst, data_->data() + offset, length);
    return static_cast<int32_t>(length);
  }

  void close() override {
    data_ = nullptr;
    in_use_ = false;
  }

  bool inUse() const { return in_use_; }
  void take(std::vector<uint8_t>* data) {
    data_ = data;
    in_use_ = true;
  }

 private:
  std::vector<uint8_t>* data_ = nullptr;
  bool in_use_ = false;
};

class FakeDirectory : public hal::IDirectory {
 public:
  void take(std::vector<std::pair<std::string, uint64_t>> entries) {
    entries_ = std::move(entries);
    at_ = 0;
    in_use_ = true;
  }

  bool next(char* name, uint32_t capacity, uint64_t* size) override {
    if (!in_use_ || at_ >= entries_.size()) return false;
    const auto& entry = entries_[at_++];
    uint32_t i = 0;
    while (i + 1 < capacity && i < entry.first.size()) {
      name[i] = entry.first[i];
      ++i;
    }
    name[i] = 0;
    if (size != nullptr) *size = entry.second;
    return true;
  }

  void close() override {
    entries_.clear();
    in_use_ = false;
  }

  bool inUse() const { return in_use_; }

 private:
  std::vector<std::pair<std::string, uint64_t>> entries_;
  size_t at_ = 0;
  bool in_use_ = false;
};

class FakeStorage : public hal::IStorage {
 public:
  // --- power-loss injection ------------------------------------------------

  /** The nth mutating call from now throws PowerLoss instead of happening. */
  void cutPowerAfter(int32_t operations) { budget_ = operations; }
  void restorePower() { budget_ = -1; }
  /** Mutating operations performed since the last reset of the counter. */
  uint32_t writeCount() const { return writes_; }
  void resetWriteCount() { writes_ = 0; }

  // --- test helpers ---------------------------------------------------------

  bool hasFile(const std::string& path) const { return files_.count(path) != 0; }
  const std::vector<uint8_t>* peek(const std::string& path) const {
    auto it = files_.find(path);
    return it == files_.end() ? nullptr : &it->second;
  }
  void put(const std::string& path, const std::vector<uint8_t>& data) {
    files_[path] = data;
  }
  std::vector<std::string> paths() const {
    std::vector<std::string> out;
    for (const auto& entry : files_) out.push_back(entry.first);
    return out;
  }
  void setFreeBytes(uint64_t bytes) { free_bytes_ = bytes; }

  // --- hal::IStorage --------------------------------------------------------

  bool begin() override { return true; }
  void end() override {}
  bool mounted() const override { return true; }

  hal::StorageInfo info() const override {
    hal::StorageInfo out;
    out.mounted = true;
    out.capacity_bytes = free_bytes_;
    out.used_bytes = 0;
    out.type = "fake";
    return out;
  }

  bool exists(const char* path) const override {
    if (path == nullptr) return false;
    return files_.count(path) != 0 || dirs_.count(path) != 0;
  }

  bool mkdirs(const char* path) override {
    if (path == nullptr) return false;
    spend();
    dirs_.insert(path);
    return true;
  }

  bool remove(const char* path) override {
    if (path == nullptr) return false;
    spend();
    return files_.erase(path) != 0;
  }

  bool rename(const char* from, const char* to) override {
    if (from == nullptr || to == nullptr) return false;
    spend();
    auto it = files_.find(from);
    if (it == files_.end()) return false;
    files_[to] = it->second;
    files_.erase(it);
    return true;
  }

  int64_t size(const char* path) const override {
    if (path == nullptr) return -1;
    auto it = files_.find(path);
    if (it == files_.end()) return -1;
    return static_cast<int64_t>(it->second.size());
  }

  hal::IFile* open(const char* path) override {
    if (path == nullptr) return nullptr;
    auto it = files_.find(path);
    if (it == files_.end()) return nullptr;
    for (FakeFile& file : handles_) {
      if (file.inUse()) continue;
      file.take(&it->second);
      return &file;
    }
    return nullptr;
  }

  int32_t read(const char* path, uint64_t offset, void* dst,
               uint32_t length) override {
    if (path == nullptr || dst == nullptr) return -1;
    auto it = files_.find(path);
    if (it == files_.end()) return -1;
    if (offset >= it->second.size()) return 0;
    const uint64_t remaining = it->second.size() - offset;
    if (length > remaining) length = static_cast<uint32_t>(remaining);
    if (length != 0) memcpy(dst, it->second.data() + offset, length);
    return static_cast<int32_t>(length);
  }

  bool writeAll(const char* path, const void* src, uint32_t length) override {
    if (path == nullptr) return false;
    spend();
    std::vector<uint8_t>& target = files_[path];
    target.assign(static_cast<const uint8_t*>(src),
                  static_cast<const uint8_t*>(src) + length);
    return true;
  }

  bool append(const char* path, const void* src, uint32_t length) override {
    if (path == nullptr) return false;
    spend();
    std::vector<uint8_t>& target = files_[path];
    const uint8_t* bytes = static_cast<const uint8_t*>(src);
    target.insert(target.end(), bytes, bytes + length);
    return true;
  }

  hal::IDirectory* openDir(const char* path) override {
    if (path == nullptr) return nullptr;
    const std::string prefix = std::string(path) + "/";
    std::vector<std::pair<std::string, uint64_t>> entries;
    for (const auto& entry : files_) {
      if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string rest = entry.first.substr(prefix.size());
      if (rest.find('/') != std::string::npos) continue;  // does not nest
      entries.emplace_back(rest, entry.second.size());
    }
    if (dirs_.count(path) == 0 && entries.empty()) return nullptr;

    for (FakeDirectory& dir : dir_handles_) {
      if (dir.inUse()) continue;
      dir.take(std::move(entries));
      return &dir;
    }
    return nullptr;
  }

  uint64_t freeBytes() const override { return free_bytes_; }

 private:
  void spend() {
    ++writes_;
    if (budget_ < 0) return;
    if (budget_ == 0) throw PowerLoss{};
    --budget_;
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> dirs_;
  FakeFile handles_[4] = {FakeFile(nullptr), FakeFile(nullptr), FakeFile(nullptr),
                          FakeFile(nullptr)};
  FakeDirectory dir_handles_[2];
  int32_t budget_ = -1;
  uint32_t writes_ = 0;
  uint64_t free_bytes_ = 64ull * 1024ull * 1024ull;
};

}  // namespace faketest
