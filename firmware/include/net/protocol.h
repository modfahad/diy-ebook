// protocol.h -- C++ mirror of packages/protocol/src/index.ts.
//
// Paths, limits, error codes. If you change one side, change the other and
// regenerate the transcript fixture: firmware/test/test_net/transcript_fixture.h
// is what catches the two mirrors drifting apart.

#pragma once

#include <stdint.h>

namespace net {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 8080;

constexpr const char* kMdnsService = "_quranreader";

// --- paths -------------------------------------------------------------------
//
// The upload sub-paths take a session id, so they are matched by prefix rather
// than compared whole: strip kPathUploadPrefix, then look for kUploadChunkSuffix
// / kUploadFinishSuffix on the remainder. No suffix + DELETE is abort. Mirrors
// PATHS.uploadChunk/uploadFinish/uploadAbort in packages/protocol/src/index.ts.
constexpr const char* kPathDeviceInfo = "/api/device/info";
constexpr const char* kPathDeviceStatus = "/api/device/status";
constexpr const char* kPathLibrary = "/api/library";
constexpr const char* kPathLibraryItemPrefix = "/api/library/";
constexpr const char* kPathUploadBegin = "/api/library/upload";
constexpr const char* kPathUploadPrefix = "/api/library/upload/";
constexpr const char* kUploadChunkSuffix = "/chunk";
constexpr const char* kUploadFinishSuffix = "/finish";
constexpr const char* kPathBackup = "/api/device/backup";
constexpr const char* kPathRestore = "/api/device/restore";

// Home-screen photos (net::PhotoStore) and the time zone. A photo is
// addressed by name: GET kPathPhotos lists them; PUT <prefix><name>/chunk
// appends (X-Qr-Offset, chunked body, same framing as the package upload);
// POST <prefix><name>/finish commits; DELETE <prefix><name> removes.
constexpr const char* kPathPhotos = "/api/photos";
constexpr const char* kPathPhotoPrefix = "/api/photos/";
constexpr const char* kPhotoChunkSuffix = "/chunk";
constexpr const char* kPhotoFinishSuffix = "/finish";
constexpr const char* kPathDeviceTime = "/api/device/time";

// Firmware over Wi-Fi (protocol.md): POST begin {"size", "md5"}, PUT chunk
// ?offset=N bodies strictly in order, POST finish -- which checks the MD5,
// makes the new image the boot image and restarts -- or POST abort.
constexpr const char* kPathFirmwareBegin = "/api/firmware/begin";
constexpr const char* kPathFirmwareChunk = "/api/firmware/chunk";
constexpr const char* kPathFirmwareFinish = "/api/firmware/finish";
constexpr const char* kPathFirmwareAbort = "/api/firmware/abort";

// --- headers -------------------------------------------------------------------

constexpr const char* kHeaderAuthorization = "Authorization";
constexpr const char* kHeaderContentType = "Content-Type";
constexpr const char* kHeaderProtocolVersion = "X-Qr-Protocol";
// Byte offset of a chunk PUT's body within the upload, decimal, no sign.
// Not in the TS mirror's PATHS/STATUS tables because it is a header, not a
// path -- kept here and in index.ts's HEADER_OFFSET together, same as the
// others.
constexpr const char* kHeaderOffset = "X-Qr-Offset";

// A chunk sent as base64 text instead of a streamed chunked body -- for
// clients that cannot stream a request body, like the Android app's fetch.
constexpr const char* kHeaderBody = "X-Qr-Body";
constexpr const char* kBodyEncodingBase64 = "base64";

constexpr const char* kContentTypeJson = "application/json; charset=utf-8";
constexpr const char* kContentTypeOctet = "application/octet-stream";

// --- limits ------------------------------------------------------------------

// The Arduino WebServer buffers a non-form request body into a String on the
// internal heap, so the chunk size is bounded by RAM, not by the protocol.
// The device advertises this in /api/device/info and refuses anything larger
// with 413.
constexpr uint32_t kMaxChunkBytes = 16384;

// A session is just a .part plus a .meta on disk, so "expiry" is advisory:
// it tells the desktop how long to assume a resume is still worthwhile.
constexpr uint32_t kSessionExpirySeconds = 900;

constexpr uint8_t kContentIdHexChars = 32;
constexpr uint8_t kTokenMaxChars = 64;
constexpr uint16_t kTitleMaxBytes = 96;
constexpr uint16_t kAuthorMaxBytes = 64;
constexpr uint8_t kLanguageMaxBytes = 16;
constexpr uint8_t kLocationMaxBytes = 32;

// --- status codes ------------------------------------------------------------

constexpr uint16_t kStatusOk = 200;
constexpr uint16_t kStatusCreated = 201;
constexpr uint16_t kStatusBadRequest = 400;
constexpr uint16_t kStatusUnauthorized = 401;
constexpr uint16_t kStatusNotFound = 404;
constexpr uint16_t kStatusConflict = 409;
constexpr uint16_t kStatusPayloadTooLarge = 413;
constexpr uint16_t kStatusUnprocessable = 422;
constexpr uint16_t kStatusInsufficientStorage = 507;

// --- errors ------------------------------------------------------------------

enum class Error : uint8_t {
  kOk = 0,
  kBadRequest,
  kUnauthorized,
  kNotFound,
  kOffsetMismatch,
  kChunkTooLarge,
  kSizeMismatch,
  kVerifyFailed,
  kPackageRejected,
  kNoSpace,
  kNoSession,
  kStorageError,
  kNotInTransferMode,
};

/** The wire code, e.g. "OFFSET_MISMATCH". Matches ErrorCode in the TS mirror. */
const char* ErrorCodeName(Error error);

/** The HTTP status this error maps to. */
uint16_t ErrorStatus(Error error);

// --- package types -----------------------------------------------------------

/** "QURAN" / "BOOK" / "TRANSLATION" / "TAFSIR" -> qpk::PackageType value. */
uint16_t PackageTypeFromName(const char* name);
const char* PackageTypeName(uint16_t type);

/** Sub-directory of /LIBRARY for a package type, or nullptr if unknown. */
const char* LibraryDirFor(uint16_t type);

}  // namespace net
