// Emits the protocol-constants fixture in two forms:
//
//   packages/protocol/fixtures/protocol-constants.json   the same data,
//                                                        for a future desktop
//                                                        test to check itself
//                                                        against
//   firmware/test/test_net/transcript_fixture.h          a generated C++
//                                                        header the firmware
//                                                        test suite checks
//                                                        protocol.h against
//
// This is deliberately narrower than a full request/response transcript (see
// the comment on TranscriptStep in src/index.ts): it covers paths, headers,
// status codes, and the error-code -> status mapping -- the constants that
// silently drift when one side of the mirror is edited and the other is not.
// It does not cover JSON response *shapes* (DeviceInfo, DeviceStatus, ...);
// that needs the firmware's response building pulled out of http_server.cpp
// into pure functions first, which has not been done.
//
// Regenerate with:
//
//     npm run fixture --prefix packages/protocol
//
// and commit the result; a diff in transcript_fixture.h against what
// protocol.h currently declares is exactly the drift this exists to catch.

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  CONTENT_TYPE_JSON,
  CONTENT_TYPE_OCTET,
  DEFAULT_PORT,
  ERROR_STATUS,
  HEADER_AUTHORIZATION,
  HEADER_CONTENT_TYPE,
  HEADER_OFFSET,
  HEADER_BODY,
  BODY_ENCODING_BASE64,
  HEADER_PROTOCOL_VERSION,
  LIBRARY_ITEM_PREFIX,
  MDNS_PROTOCOL,
  MDNS_SERVICE,
  PATHS,
  PHOTO_CHUNK_SUFFIX,
  PHOTO_FINISH_SUFFIX,
  PHOTO_ITEM_PREFIX,
  PROTOCOL_VERSION,
  STATUS,
  UPLOAD_CHUNK_SUFFIX,
  UPLOAD_FINISH_SUFFIX,
  UPLOAD_PATH_PREFIX,
} from '../src/index.js';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = join(here, '..', '..');  // .. from scripts/, one more from packages/protocol
const repoRoot = join(packageRoot, '..', '..');

const fixture = {
  protocolVersion: PROTOCOL_VERSION,
  defaultPort: DEFAULT_PORT,
  mdnsService: MDNS_SERVICE,
  mdnsProtocol: MDNS_PROTOCOL,
  headers: {
    authorization: HEADER_AUTHORIZATION,
    contentType: HEADER_CONTENT_TYPE,
    protocolVersion: HEADER_PROTOCOL_VERSION,
    offset: HEADER_OFFSET,
    body: HEADER_BODY,
    bodyEncodingBase64: BODY_ENCODING_BASE64,
  },
  contentTypes: {
    json: CONTENT_TYPE_JSON,
    octet: CONTENT_TYPE_OCTET,
  },
  paths: {
    deviceInfo: PATHS.deviceInfo,
    deviceStatus: PATHS.deviceStatus,
    library: PATHS.library,
    libraryItemPrefix: LIBRARY_ITEM_PREFIX,
    uploadBegin: PATHS.uploadBegin,
    uploadPrefix: UPLOAD_PATH_PREFIX,
    uploadChunkSuffix: UPLOAD_CHUNK_SUFFIX,
    uploadFinishSuffix: UPLOAD_FINISH_SUFFIX,
    backup: PATHS.backup,
    restore: PATHS.restore,
    photos: PATHS.photos,
    photoPrefix: PHOTO_ITEM_PREFIX,
    photoChunkSuffix: PHOTO_CHUNK_SUFFIX,
    photoFinishSuffix: PHOTO_FINISH_SUFFIX,
    deviceTime: PATHS.deviceTime,
    firmwareBegin: PATHS.firmwareBegin,
    firmwareChunk: PATHS.firmwareChunk,
    firmwareFinish: PATHS.firmwareFinish,
    firmwareAbort: PATHS.firmwareAbort,
  },
  status: STATUS,
  errors: Object.entries(ERROR_STATUS).map(([code, status]) => ({ code, status })),
};

const jsonPath = join(packageRoot, 'fixtures', 'protocol-constants.json');
mkdirSync(dirname(jsonPath), { recursive: true });
writeFileSync(jsonPath, JSON.stringify(fixture, null, 2) + '\n');

function cEscape(value: string): string {
  return value.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
}

const errorEntries = fixture.errors
  .map((e) => `    {"${cEscape(e.code)}", ${e.status}},`)
  .join('\n');

const header = `// GENERATED FILE -- DO NOT EDIT.
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

constexpr uint16_t kProtocolVersion = ${fixture.protocolVersion};
constexpr uint16_t kDefaultPort = ${fixture.defaultPort};
constexpr const char* kMdnsService = "${cEscape(fixture.mdnsService)}";
constexpr const char* kMdnsProtocol = "${cEscape(fixture.mdnsProtocol)}";

// HTTP header names are case-insensitive on the wire; protocol.h capitalizes
// them differently than these lowercase TS constants on purpose. Compare
// these case-insensitively, unlike everything else in this file.
constexpr const char* kHeaderAuthorization = "${cEscape(fixture.headers.authorization)}";
constexpr const char* kHeaderContentType = "${cEscape(fixture.headers.contentType)}";
constexpr const char* kHeaderProtocolVersion = "${cEscape(fixture.headers.protocolVersion)}";
constexpr const char* kHeaderOffset = "${cEscape(fixture.headers.offset)}";
constexpr const char* kHeaderBody = "${cEscape(fixture.headers.body)}";
constexpr const char* kBodyEncodingBase64 = "${cEscape(fixture.headers.bodyEncodingBase64)}";

constexpr const char* kContentTypeJson = "${cEscape(fixture.contentTypes.json)}";
constexpr const char* kContentTypeOctet = "${cEscape(fixture.contentTypes.octet)}";

constexpr const char* kPathDeviceInfo = "${cEscape(fixture.paths.deviceInfo)}";
constexpr const char* kPathDeviceStatus = "${cEscape(fixture.paths.deviceStatus)}";
constexpr const char* kPathLibrary = "${cEscape(fixture.paths.library)}";
constexpr const char* kPathLibraryItemPrefix = "${cEscape(fixture.paths.libraryItemPrefix)}";
constexpr const char* kPathUploadBegin = "${cEscape(fixture.paths.uploadBegin)}";
constexpr const char* kPathUploadPrefix = "${cEscape(fixture.paths.uploadPrefix)}";
constexpr const char* kUploadChunkSuffix = "${cEscape(fixture.paths.uploadChunkSuffix)}";
constexpr const char* kUploadFinishSuffix = "${cEscape(fixture.paths.uploadFinishSuffix)}";
constexpr const char* kPathBackup = "${cEscape(fixture.paths.backup)}";
constexpr const char* kPathRestore = "${cEscape(fixture.paths.restore)}";
constexpr const char* kPathPhotos = "${cEscape(fixture.paths.photos)}";
constexpr const char* kPathPhotoPrefix = "${cEscape(fixture.paths.photoPrefix)}";
constexpr const char* kPhotoChunkSuffix = "${cEscape(fixture.paths.photoChunkSuffix)}";
constexpr const char* kPhotoFinishSuffix = "${cEscape(fixture.paths.photoFinishSuffix)}";
constexpr const char* kPathDeviceTime = "${cEscape(fixture.paths.deviceTime)}";
constexpr const char* kPathFirmwareBegin = "${cEscape(fixture.paths.firmwareBegin)}";
constexpr const char* kPathFirmwareChunk = "${cEscape(fixture.paths.firmwareChunk)}";
constexpr const char* kPathFirmwareFinish = "${cEscape(fixture.paths.firmwareFinish)}";
constexpr const char* kPathFirmwareAbort = "${cEscape(fixture.paths.firmwareAbort)}";

constexpr uint16_t kStatusOk = ${fixture.status.ok};
constexpr uint16_t kStatusCreated = ${fixture.status.created};
constexpr uint16_t kStatusBadRequest = ${fixture.status.badRequest};
constexpr uint16_t kStatusUnauthorized = ${fixture.status.unauthorized};
constexpr uint16_t kStatusNotFound = ${fixture.status.notFound};
constexpr uint16_t kStatusConflict = ${fixture.status.conflict};
constexpr uint16_t kStatusPayloadTooLarge = ${fixture.status.payloadTooLarge};
constexpr uint16_t kStatusUnprocessable = ${fixture.status.unprocessable};
constexpr uint16_t kStatusInsufficientStorage = ${fixture.status.insufficientStorage};

struct ErrorEntry {
  const char* code;
  uint16_t status;
};

constexpr ErrorEntry kErrors[] = {
${errorEntries}
};
constexpr uint32_t kErrorCount = sizeof(kErrors) / sizeof(kErrors[0]);

}  // namespace transcript
`;

const headerPath = join(repoRoot, 'firmware', 'test', 'test_net', 'transcript_fixture.h');
writeFileSync(headerPath, header);

console.log(`wrote ${jsonPath}`);
console.log(`wrote ${headerPath}`);
