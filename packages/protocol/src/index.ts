// The desktop <-> device wire protocol, version 1.
//
// Single source of truth for paths, status codes, header names and JSON
// shapes. The firmware mirrors this file in C++ (firmware/include/net/), and
// the transcript fixture in `fixtures/` is what checks the two mirrors have
// not drifted -- the same trick the QPK golden fixture plays for the package
// format.
//
// Design constraints this encodes, from the product spec:
//   * §17 uploads must be resumable; a Quran package will not always transfer
//     in one request.
//   * §17 installation must be atomic; a partial upload must never become an
//     active library item.
//   * §19 no unauthenticated destructive endpoints, and the Wi-Fi password is
//     never readable through this API.
//   * §6/§7 Wi-Fi is on-demand. The device only serves this API while the
//     user has put it in transfer mode.

export const PROTOCOL_VERSION = 1;

/** Default TCP port. Announced in the BLE provisioning payload too. */
export const DEFAULT_PORT = 8080;

/** mDNS service the device advertises while in transfer mode. */
export const MDNS_SERVICE = '_quranreader';
export const MDNS_PROTOCOL = 'tcp';

// --- headers -----------------------------------------------------------------

export const HEADER_AUTHORIZATION = 'authorization';
export const HEADER_CONTENT_TYPE = 'content-type';
export const HEADER_PROTOCOL_VERSION = 'x-qr-protocol';
/** Byte offset of a chunk PUT's body within the upload, decimal, no sign. */
export const HEADER_OFFSET = 'x-qr-offset';
/**
 * Set to BODY_ENCODING_BASE64 when a chunk PUT's body is base64 text rather
 * than a streamed chunked body -- for clients that cannot stream a request
 * body, like React Native's fetch (docs/android.md).
 */
export const HEADER_BODY = 'x-qr-body';
export const BODY_ENCODING_BASE64 = 'base64';
export const CONTENT_TYPE_JSON = 'application/json; charset=utf-8';
export const CONTENT_TYPE_OCTET = 'application/octet-stream';

export function bearer(token: string): string {
  return `Bearer ${token}`;
}

// --- paths -------------------------------------------------------------------

// Named separately from PATHS below (rather than inlined in the template
// literals) so a generator can put the literal prefix/suffix strings
// themselves into the constants fixture -- see scripts/build-transcript.ts --
// matching how firmware/include/net/protocol.h names the same pieces
// (kPathLibraryItemPrefix, kPathUploadPrefix, kUploadChunkSuffix,
// kUploadFinishSuffix).
export const LIBRARY_ITEM_PREFIX = '/api/library/';
export const UPLOAD_PATH_PREFIX = '/api/library/upload/';
export const UPLOAD_CHUNK_SUFFIX = '/chunk';
export const UPLOAD_FINISH_SUFFIX = '/finish';
export const PHOTO_ITEM_PREFIX = '/api/photos/';
export const PHOTO_CHUNK_SUFFIX = '/chunk';
export const PHOTO_FINISH_SUFFIX = '/finish';

export const PATHS = {
  deviceInfo: '/api/device/info',
  deviceStatus: '/api/device/status',
  library: '/api/library',
  libraryItem: (contentId: string) => `${LIBRARY_ITEM_PREFIX}${contentId}`,
  uploadBegin: '/api/library/upload',
  uploadChunk: (sessionId: string) =>
    `${UPLOAD_PATH_PREFIX}${sessionId}${UPLOAD_CHUNK_SUFFIX}`,
  uploadFinish: (sessionId: string) =>
    `${UPLOAD_PATH_PREFIX}${sessionId}${UPLOAD_FINISH_SUFFIX}`,
  uploadAbort: (sessionId: string) => `${UPLOAD_PATH_PREFIX}${sessionId}`,
  backup: '/api/device/backup',
  restore: '/api/device/restore',
  /** Home-screen photos: GET lists them. */
  photos: '/api/photos',
  /** DELETE removes one photo. */
  photoItem: (name: string) => `${PHOTO_ITEM_PREFIX}${name}`,
  /** PUT appends a chunk (X-Qr-Offset, chunked body, like uploadChunk). */
  photoChunk: (name: string) => `${PHOTO_ITEM_PREFIX}${name}${PHOTO_CHUNK_SUFFIX}`,
  /** POST verifies the uploaded file and makes it a photo. */
  photoFinish: (name: string) => `${PHOTO_ITEM_PREFIX}${name}${PHOTO_FINISH_SUFFIX}`,
  /** POST sets the time zone the clock shows. */
  deviceTime: '/api/device/time',
  /** POST {size, md5} starts a firmware update. */
  firmwareBegin: '/api/firmware/begin',
  /** PUT ?offset=N with a chunked body, strictly in order. */
  firmwareChunk: '/api/firmware/chunk',
  /** POST checks the image, makes it the boot image and restarts the device. */
  firmwareFinish: '/api/firmware/finish',
  /** POST drops an update in progress. */
  firmwareAbort: '/api/firmware/abort',
} as const;

// --- status codes ------------------------------------------------------------

export interface FirmwareBeginRequest {
  /** The app image's exact size in bytes. */
  size: number;
  /** Lower-case hex MD5 of the image; the device refuses to install on a mismatch. */
  md5: string;
}

export interface FirmwareBeginResponse {
  /** Bytes the device already has for this update (0: start from the beginning). */
  receivedBytes: number;
  maxChunkBytes: number;
  /** The OTA slot's size: the largest image the device can take. */
  slotBytes: number;
}

export interface FirmwareFinishResponse {
  updated: boolean;
  /** The device restarts into the new image about a second after replying. */
  restarting: boolean;
}

export const STATUS = {
  ok: 200,
  created: 201,
  badRequest: 400,
  unauthorized: 401,
  notFound: 404,
  conflict: 409,
  payloadTooLarge: 413,
  unprocessable: 422,
  insufficientStorage: 507,
} as const;

/**
 * Machine-readable error codes. The device sends these; the desktop switches
 * on them. Human text goes in `detail` and is never parsed.
 */
export type ErrorCode =
  | 'BAD_REQUEST'
  | 'UNAUTHORIZED'
  | 'NOT_FOUND'
  | 'OFFSET_MISMATCH'
  | 'CHUNK_TOO_LARGE'
  | 'SIZE_MISMATCH'
  | 'VERIFY_FAILED'
  | 'PACKAGE_REJECTED'
  | 'NO_SPACE'
  | 'NO_SESSION'
  | 'STORAGE_ERROR'
  | 'NOT_IN_TRANSFER_MODE';

export interface ErrorBody {
  error: ErrorCode;
  detail?: string;
  /** Present on OFFSET_MISMATCH: where the client should resume from. */
  expectedOffset?: number;
}

/** Mirrors net::ErrorStatus in firmware/include/net/protocol.h exactly. */
export const ERROR_STATUS: Record<ErrorCode, number> = {
  BAD_REQUEST: STATUS.badRequest,
  UNAUTHORIZED: STATUS.unauthorized,
  NOT_FOUND: STATUS.notFound,
  OFFSET_MISMATCH: STATUS.conflict,
  CHUNK_TOO_LARGE: STATUS.payloadTooLarge,
  SIZE_MISMATCH: STATUS.unprocessable,
  VERIFY_FAILED: STATUS.unprocessable,
  PACKAGE_REJECTED: STATUS.unprocessable,
  NO_SPACE: STATUS.insufficientStorage,
  NO_SESSION: STATUS.notFound,
  STORAGE_ERROR: STATUS.insufficientStorage,
  NOT_IN_TRANSFER_MODE: STATUS.conflict,
};

// --- device info / status ----------------------------------------------------

export interface DeviceInfo {
  protocolVersion: number;
  /** Stable per device; the desktop keys trusted devices on this. */
  deviceId: string;
  name: string;
  model: string;
  firmwareVersion: string;
  /** Largest body the device will accept in one chunk PUT. */
  maxChunkBytes: number;
  /** False before BLE provisioning has run. */
  paired: boolean;
  /**
   * Why the device last restarted: "power-on", "brownout", "panic", a
   * watchdog, ... Absent from firmware older than 2026-09-14.
   */
  resetReason?: string;
  /** Restarts counted by the device. Absent from older firmware. */
  bootCount?: number;
}

export interface DeviceStatus {
  storage: {
    mounted: boolean;
    capacityBytes: number;
    usedBytes: number;
    freeBytes: number;
  };
  battery: {
    /** This board revision publishes no battery-sense GPIO. */
    available: boolean;
    millivolts?: number;
  };
  uptimeSeconds: number;
  transferMode: boolean;
  /** Sessions with a .part file on disk, resumable right now. */
  openSessions: number;
}

// --- library -----------------------------------------------------------------

export type PackageTypeName = 'QURAN' | 'BOOK' | 'TRANSLATION' | 'TAFSIR';

export interface LibraryItem {
  contentId: string;
  title: string;
  author: string;
  type: PackageTypeName;
  language: string;
  contentVersion: number;
  packageSize: number;
  /** Path under /LIBRARY, e.g. "QURAN/abc...qpk". */
  location: string;
  installedAt: number;
}

export interface LibraryListing {
  items: LibraryItem[];
}

// --- resumable upload --------------------------------------------------------

export interface UploadBeginRequest {
  contentId: string;
  contentVersion: number;
  type: PackageTypeName;
  /** Total package size. The device refuses a finish that disagrees. */
  size: number;
  /** QPK header field, verified over the .part before the rename. */
  payloadCrc32: number;
  title?: string;
}

export interface UploadBeginResponse {
  sessionId: string;
  /**
   * Bytes already on disk. THIS IS THE RESUME POINT, and it is derived from
   * the .part file's size, never from a counter held in RAM -- the device can
   * reset mid-transfer.
   */
  receivedBytes: number;
  maxChunkBytes: number;
  expiresInSeconds: number;
}

export interface UploadChunkResponse {
  receivedBytes: number;
}

export interface UploadFinishResponse {
  contentId: string;
  installed: boolean;
  location: string;
}

// --- backup / restore --------------------------------------------------------

export interface BackupResponse {
  /** Base64 of the packed /USER and /DEVICE state. */
  data: string;
  bytes: number;
}

export interface RestoreRequest {
  data: string;
}

// --- home-screen photos and time zone ---------------------------------------
//
// Mirrors firmware/include/net/photo_store.h. The desktop converts a picture to
// finished pixels; the device stores and shows them. One photo file:
//
//   "QPH1" | width u16 LE | height u16 LE | bpp u8 = 2 | 7 zero bytes | pixels
//
// pixels are row-major, 4 per byte, MSB first: 0 black, 1 dark grey,
// 2 light grey, 3 white.

export const PHOTO_WIDTH = 400;
export const PHOTO_HEIGHT = 480;
export const PHOTO_HEADER_BYTES = 16;
export const PHOTO_PIXEL_BYTES = (PHOTO_WIDTH / 4) * PHOTO_HEIGHT;
export const PHOTO_FILE_BYTES = PHOTO_HEADER_BYTES + PHOTO_PIXEL_BYTES;
export const PHOTO_NAME_MAX_CHARS = 32;
/** Safe as a FAT file name and in a URL path without escaping. */
export const PHOTO_NAME_PATTERN = /^[a-z0-9_-]{1,32}$/u;
export const TIME_ZONE_MAX_CHARS = 63;

export interface PhotoItem {
  name: string;
  bytes: number;
}

export interface PhotoListing {
  width: number;
  height: number;
  items: PhotoItem[];
}

export interface PhotoChunkResponse {
  receivedBytes: number;
}

export interface PhotoFinishResponse {
  name: string;
  installed: boolean;
}

export interface TimeZoneRequest {
  /** POSIX TZ rule, e.g. "IST-5:30" or "CET-1CEST,M3.5.0,M10.5.0/3". */
  tz: string;
}

export interface TimeZoneResponse {
  tz: string;
  synced: boolean;
  /** Unix seconds, 0 while unsynced. */
  nowUnix: number;
}

// --- transcript fixture ------------------------------------------------------
//
// scripts/build-transcript.ts generates fixtures/protocol-constants.json and
// firmware/test/test_net/transcript_fixture.h from the constants and error
// mapping above (paths, headers, status codes, ERROR_STATUS) -- not from the
// TranscriptStep/TranscriptScenario/Transcript shapes below. Those describe a
// fuller request/response-body fixture (the JSON shapes: DeviceInfo,
// DeviceStatus, LibraryItem, ...), which would need the firmware's response
// building pulled out of http_server.cpp into pure, host-testable functions
// first -- deliberately not done yet. These interfaces are declared for that
// future fixture and are not currently produced or consumed by anything.

/**
 * One request/response pair. The committed transcript in `fixtures/` is the
 * cross-language contract: the TypeScript client is driven against it, and a
 * generated C++ header drives the firmware's session state machine against the
 * same steps.
 */
export interface TranscriptStep {
  note: string;
  request: {
    method: 'GET' | 'POST' | 'PUT' | 'DELETE';
    path: string;
    /** Chunk PUTs only. */
    offset?: number;
    /** Chunk PUTs only: body length in bytes. */
    bodyBytes?: number;
    json?: unknown;
  };
  response: {
    status: number;
    json?: unknown;
  };
}

export interface TranscriptScenario {
  name: string;
  description: string;
  steps: TranscriptStep[];
}

export interface Transcript {
  protocolVersion: number;
  scenarios: TranscriptScenario[];
}
