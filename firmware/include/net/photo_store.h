// photo_store.h -- the home screen's photos on the card.
//
// The desktop app does the image work (crop, grey conversion, dithering) and
// uploads finished pixels; the device only stores and shows them. One photo is
// one file, /PHOTOS/<name>.g4:
//
//   offset  size  field
//   0       4     magic "QPH1"
//   4       2     width, little-endian   (always kPhotoWidth)
//   6       2     height, little-endian  (always kPhotoHeight)
//   8       1     bits per pixel         (always 2)
//   9       7     reserved, zero
//   16      ...   pixels: row-major, 4 per byte, MSB first;
//                 0 = black, 1 = dark grey, 2 = light grey, 3 = white
//
// Uploads arrive as chunks appended to <name>.part -- the same resumable shape
// as the package upload (net::UploadManager): the part file's size on disk is
// the resume point, so a dropped connection or a reset costs one chunk.
// finish() checks the size and the header before the rename, so a half-sent
// or foreign file never becomes a photo.
//
// Pure: no Arduino, only hal::IStorage, so it runs against the host tests'
// FakeStorage.

#pragma once

#include <stdint.h>

#include "hal/storage.h"
#include "net/protocol.h"

namespace net {

constexpr uint16_t kPhotoWidth = 400;
constexpr uint16_t kPhotoHeight = 480;
constexpr uint32_t kPhotoHeaderBytes = 16;
constexpr uint32_t kPhotoPixelBytes =
    static_cast<uint32_t>(kPhotoWidth / 4) * kPhotoHeight;  // 48,000
constexpr uint32_t kPhotoFileBytes = kPhotoHeaderBytes + kPhotoPixelBytes;

// Names are [a-z0-9_-], 1..kPhotoNameMaxChars: safe as a FAT file name and in
// a URL path without escaping.
constexpr uint8_t kPhotoNameMaxChars = 32;

struct PhotoName {
  char name[kPhotoNameMaxChars + 1] = {0};
};

bool ValidPhotoName(const char* name);

// True for a header describing exactly the format above.
bool ParsePhotoHeader(const uint8_t* header);
void WritePhotoHeader(uint8_t* header);

class PhotoStore {
 public:
  void begin(hal::IStorage* storage) { storage_ = storage; }

  // Appends one chunk to <name>.part. `offset` must equal the bytes already
  // there (kOffsetMismatch, with *expected set, otherwise); offset 0 restarts
  // an upload from scratch. *received is the part's size afterwards.
  Error writeChunk(const char* name, uint64_t offset, const uint8_t* data,
                   uint32_t length, uint64_t* received, uint64_t* expected);

  // Verifies <name>.part (size, header) and renames it over any existing photo
  // of the same name. A part that fails verification is deleted.
  Error finish(const char* name);

  // Deletes the photo and any unfinished upload of it.
  Error remove(const char* name);

  // Completed photos, sorted by name, at most `capacity`. Returns the count.
  uint16_t list(PhotoName* out, uint16_t capacity) const;

  // Reads a photo's kPhotoPixelBytes pixels into `pixels`.
  bool load(const char* name, uint8_t* pixels, uint32_t capacity) const;

 private:
  hal::IStorage* storage_ = nullptr;
};

}  // namespace net
