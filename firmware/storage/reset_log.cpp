#include "net/reset_log.h"

#include <stdio.h>
#include <string.h>

#include "qpk/byte_order.h"

namespace net {
namespace {

constexpr const char* kLogPath = "/DEVICE/resets.log";
constexpr const char* kLogTempPath = "/DEVICE/resets.tmp";
constexpr const char* kAlivePath = "/DEVICE/alive.bin";

// Static rather than on the stack: the main task's stack is a few kilobytes.
char g_tail[kResetLogKeepBytes];

// Keeps the newest kResetLogKeepBytes, starting at a whole line, via
// .tmp-then-rename like every other small file on the card.
bool TrimLog(hal::IStorage* storage, int64_t size) {
  const uint64_t offset = static_cast<uint64_t>(size) - kResetLogKeepBytes;
  if (storage->read(kLogPath, offset, g_tail, kResetLogKeepBytes) !=
      static_cast<int32_t>(kResetLogKeepBytes)) {
    return false;
  }
  const char* newline = static_cast<const char*>(memchr(g_tail, '\n', kResetLogKeepBytes));
  const uint32_t start = newline == nullptr ? 0 : static_cast<uint32_t>(newline - g_tail + 1);
  storage->remove(kLogTempPath);
  if (!storage->writeAll(kLogTempPath, g_tail + start, kResetLogKeepBytes - start)) {
    storage->remove(kLogTempPath);
    return false;
  }
  storage->remove(kLogPath);
  return storage->rename(kLogTempPath, kLogPath);
}

}  // namespace

bool AppendResetRecord(hal::IStorage* storage, uint32_t boot_count, const char* reason,
                       uint32_t last_alive_unix) {
  if (storage == nullptr || reason == nullptr) return false;

  const int64_t size = storage->size(kLogPath);
  if (size > static_cast<int64_t>(kResetLogMaxBytes)) TrimLog(storage, size);

  char line[96];
  const int n = snprintf(line, sizeof(line), "boot=%lu reset=%.24s last_alive=%lu\n",
                         static_cast<unsigned long>(boot_count), reason,
                         static_cast<unsigned long>(last_alive_unix));
  if (n <= 0 || n >= static_cast<int>(sizeof(line))) return false;
  return storage->append(kLogPath, line, static_cast<uint32_t>(n));
}

bool SaveAliveTime(hal::IStorage* storage, uint32_t unix_seconds) {
  if (storage == nullptr) return false;
  uint8_t record[8] = {'A', 'L', 'V', '1', 0, 0, 0, 0};
  qpk::Write32(record + 4, unix_seconds);
  return storage->writeAll(kAlivePath, record, sizeof(record));
}

uint32_t LoadAliveTime(hal::IStorage* storage) {
  if (storage == nullptr) return 0;
  uint8_t record[8];
  if (storage->read(kAlivePath, 0, record, sizeof(record)) != static_cast<int32_t>(sizeof(record))) {
    return 0;
  }
  if (record[0] != 'A' || record[1] != 'L' || record[2] != 'V' || record[3] != '1') return 0;
  return qpk::Read32(record + 4);
}

}  // namespace net
