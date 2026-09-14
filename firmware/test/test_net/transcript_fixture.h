// GENERATED FILE -- DO NOT EDIT.
//
// Produced by packages/protocol/scripts/build-transcript.ts from the
// constants and ERROR_STATUS mapping in packages/protocol/src/index.ts.
// Regenerate with:
//
//     npm run fixture --prefix packages/protocol
//
// firmware/test/test_net/test_main.cpp checks firmware/include/net/protocol.h
// against these values. A diff here after touching protocol.h or index.ts
// means the two mirrors have drifted -- see docs/protocol.md.

#pragma once

#include <stdint.h>

namespace transcript {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 8080;
constexpr const char* kMdnsService = "_quranreader";
constexpr const char* kMdnsProtocol = "tcp";

// HTTP header names are case-insensitive on the wire; protocol.h capitalizes
// them differently than these lowercase TS constants on purpose. Compare
// these case-insensitively, unlike everything else in this file.
constexpr const char* kHeaderAuthorization = "authorization";
constexpr const char* kHeaderContentType = "content-type";
constexpr const char* kHeaderProtocolVersion = "x-qr-protocol";
constexpr const char* kHeaderOffset = "x-qr-offset";
constexpr const char* kHeaderBody = "x-qr-body";
constexpr const char* kBodyEncodingBase64 = "base64";

constexpr const char* kContentTypeJson = "application/json; charset=utf-8";
constexpr const char* kContentTypeOctet = "application/octet-stream";

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
constexpr const char* kPathPhotos = "/api/photos";
constexpr const char* kPathPhotoPrefix = "/api/photos/";
constexpr const char* kPhotoChunkSuffix = "/chunk";
constexpr const char* kPhotoFinishSuffix = "/finish";
constexpr const char* kPathDeviceTime = "/api/device/time";
constexpr const char* kPathFirmwareBegin = "/api/firmware/begin";
constexpr const char* kPathFirmwareChunk = "/api/firmware/chunk";
constexpr const char* kPathFirmwareFinish = "/api/firmware/finish";
constexpr const char* kPathFirmwareAbort = "/api/firmware/abort";

constexpr uint16_t kStatusOk = 200;
constexpr uint16_t kStatusCreated = 201;
constexpr uint16_t kStatusBadRequest = 400;
constexpr uint16_t kStatusUnauthorized = 401;
constexpr uint16_t kStatusNotFound = 404;
constexpr uint16_t kStatusConflict = 409;
constexpr uint16_t kStatusPayloadTooLarge = 413;
constexpr uint16_t kStatusUnprocessable = 422;
constexpr uint16_t kStatusInsufficientStorage = 507;

struct ErrorEntry {
  const char* code;
  uint16_t status;
};

constexpr ErrorEntry kErrors[] = {
    {"BAD_REQUEST", 400},
    {"UNAUTHORIZED", 401},
    {"NOT_FOUND", 404},
    {"OFFSET_MISMATCH", 409},
    {"CHUNK_TOO_LARGE", 413},
    {"SIZE_MISMATCH", 422},
    {"VERIFY_FAILED", 422},
    {"PACKAGE_REJECTED", 422},
    {"NO_SPACE", 507},
    {"NO_SESSION", 404},
    {"STORAGE_ERROR", 507},
    {"NOT_IN_TRANSFER_MODE", 409},
};
constexpr uint32_t kErrorCount = sizeof(kErrors) / sizeof(kErrors[0]);

}  // namespace transcript
