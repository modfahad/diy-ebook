// verified_packages.h -- /DEVICE/verified.bin: packages whose index checksums
// already passed on this device.
//
// qpk::Reader::open checks every fixed-size index section's CRC (rule 13).
// For a 913-page picture book that sweep was 4.4 s of card reads on every
// open, measured on the board 2026-09-14. A package's bytes do not change
// between opens, so the device checks them once and remembers the package by
// fingerprint: content_id plus the header's own CRC, payload CRC and size.
// A re-uploaded or rebuilt package differs in at least one of those and is
// checked again. What this gives up: damage to an index section on the card
// after the first check goes unnoticed by later opens (the reads themselves
// still bounds-check everything).
//
// One file, rewritten whole via .tmp-then-rename with a CRC, like
// bookmarks.bin; an absent or damaged file just means "check again".

#pragma once

#include <stdint.h>

#include "hal/storage.h"

namespace net {

constexpr uint16_t kMaxVerifiedPackages = 64;

struct PackageFingerprint {
  uint8_t content_id[16] = {0};
  uint32_t header_crc32 = 0;
  uint32_t payload_crc32 = 0;
  uint64_t package_size = 0;
};

class VerifiedPackages {
 public:
  /** Loads the file; an absent or damaged file is an empty list. */
  void load(hal::IStorage* storage);
  bool save(hal::IStorage* storage) const;

  uint16_t count() const { return count_; }
  bool contains(const PackageFingerprint& fingerprint) const;

  /** Adds at the front, or moves an existing one there; drops the oldest when full. */
  void add(const PackageFingerprint& fingerprint);

 private:
  PackageFingerprint items_[kMaxVerifiedPackages];
  uint16_t count_ = 0;
};

}  // namespace net
