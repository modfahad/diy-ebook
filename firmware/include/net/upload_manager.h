// upload_manager.h -- resumable upload and atomic install.
//
// THE DESIGN RULE: every byte of session state lives on disk, never in RAM.
//
// The device can reset in the middle of a transfer. If the received-byte count
// lived in a session struct, a reset would silently restart from zero -- or
// worse, resume at a stale offset and corrupt the .part file. So:
//
//   * the session id IS the content id, so a reset does not invalidate it;
//   * the resume point is the .part file's size on disk, read fresh each time;
//   * the declared size/version/checksum live in a .meta sidecar next to it.
//
// A reset mid-transfer is therefore completely transparent: the desktop
// re-issues "begin", gets the same session id back with the true byte count,
// and carries on.
//
// Files, all under /DEVICE/uploads until the moment of install:
//   /DEVICE/uploads/<contentIdHex>.part   the bytes received so far
//   /DEVICE/uploads/<contentIdHex>.meta   what was declared
//   /LIBRARY/<DIR>/<contentIdHex>.qpk     only after verify + rename
//
// In-progress transfers are deliberately kept OUT of /LIBRARY, so nothing
// scanning the library tree can ever see a partial package.

#pragma once

#include <stdint.h>

#include "hal/storage.h"
#include "net/library_index.h"
#include "net/protocol.h"

namespace net {

constexpr const char* kUploadDir = "/DEVICE/uploads";
constexpr uint16_t kUploadMetaSize = 160;

/** What the desktop declares when it opens (or resumes) a session. */
struct UploadBegin {
  char content_id_hex[kContentIdHexChars + 1] = {0};
  uint32_t content_version = 0;
  uint16_t type = 0;
  uint64_t size = 0;
  uint32_t payload_crc32 = 0;
  char title[kTitleMaxBytes] = {0};
  /**
   * Server-computed, not declared by the desktop: the unix second after which
   * this session may be reclaimed. 0 means "no deadline recorded" -- either
   * the device had no synced clock when the session opened, or the .meta
   * predates this field. Sessions with 0 never expire, on purpose: refusing
   * an upload because we cannot prove it is *not* stale would be worse than
   * keeping a few dead bytes.
   */
  uint32_t expires_at = 0;
};

class UploadManager {
 public:
  void begin(hal::IStorage* storage, LibraryIndex* index);

  /**
   * Opens a new session, or resumes an existing one.
   *
   * `received_out` gets the number of bytes already on disk: 0 for a fresh
   * transfer, N for a resume. A .part whose .meta disagrees with this request
   * (different version, size or checksum) is discarded -- it is a different
   * package wearing the same id. So is one whose deadline has passed.
   *
   * `now_unix` is the device's clock, or 0 when it has none (the same
   * convention as LibraryEntry::installed_at). It defaults to 0 so that
   * callers with no clock -- including the host test suite -- keep the
   * pre-expiry behaviour exactly: no deadline is recorded and none is
   * enforced.
   */
  Error beginUpload(const UploadBegin& request, uint64_t* received_out,
                    uint32_t now_unix = 0, uint32_t* expires_at_out = nullptr);

  /**
   * Appends one chunk.
   *
   * Retries are idempotent: a chunk that is already entirely on disk returns
   * kOk with the current size rather than appending twice, because a lost
   * response is the common case, not the rare one. A genuine offset
   * disagreement returns kOffsetMismatch and sets `expected_out`, so the
   * desktop re-syncs in one round trip instead of guessing.
   */
  Error writeChunk(const char* content_id_hex, uint64_t offset, const void* data,
                   uint32_t length, uint64_t* received_out,
                   uint64_t* expected_out, uint32_t now_unix = 0);

  /**
   * Verifies and installs, in this order:
   *   1. the .part is exactly the declared size
   *   2. its QPK header is intact and its payload checksum matches -- both the
   *      value inside the file and the one the desktop declared
   *   3. the whole package passes qpk::Reader validation
   *   4. rename .part -> /LIBRARY/...  (the commit point)
   *   5. update library_index.bin
   *   6. delete the .meta
   *
   * Anything failing at 1-3 deletes the .part and .meta: a package that does
   * not verify is not worth resuming.
   */
  Error finish(const char* content_id_hex, uint32_t now_unix,
               LibraryEntry* installed_out);

  /** Abandons a session and unlinks its files. */
  Error abort(const char* content_id_hex);

  /** Sessions with a .meta on disk, i.e. resumable right now. */
  uint16_t openSessionCount() const;

  /**
   * Boot-time tidy-up. Removes .part files with no matching .meta -- those can
   * never be resumed or finished, so they are pure leaked space. Sessions WITH
   * a .meta are deliberately kept: surviving a reset is the point.
   */
  uint16_t sweepOrphans();

 private:
  bool metaPath(const char* content_id_hex, char* out, uint32_t capacity) const;
  bool partPath(const char* content_id_hex, char* out, uint32_t capacity) const;
  bool readMeta(const char* content_id_hex, UploadBegin* out) const;
  bool writeMeta(const UploadBegin& request);
  void discard(const char* content_id_hex);
  uint64_t partSize(const char* content_id_hex) const;
  /**
   * True only when a deadline is recorded AND we have a clock to judge it
   * with. Both halves matter: an unknown clock must not expire anything.
   */
  static bool expired(const UploadBegin& meta, uint32_t now_unix);

  hal::IStorage* storage_ = nullptr;
  LibraryIndex* index_ = nullptr;
};

}  // namespace net
