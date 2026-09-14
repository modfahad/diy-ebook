// The desktop side of the device protocol.
//
// The interesting part is uploadPackage(). The device's contract is that the
// resume point is whatever is on its disk -- not what this client thinks it
// sent. So the client never assumes: it takes the byte count the device
// reports at "begin", and on any 409 it takes the offset the device names and
// continues from there. That is what makes a mid-transfer device reset, a
// dropped Wi-Fi association, or a lost response recoverable rather than fatal.

import {
  CONTENT_TYPE_JSON,
  CONTENT_TYPE_OCTET,
  DEFAULT_PORT,
  BODY_ENCODING_BASE64,
  HEADER_BODY,
  HEADER_OFFSET,
  PATHS,
  STATUS,
  bearer,
  type DeviceInfo,
  type DeviceStatus,
  type ErrorBody,
  type ErrorCode,
  type LibraryListing,
  type PackageTypeName,
  type PhotoFinishResponse,
  type PhotoListing,
  type TimeZoneResponse,
  type UploadBeginResponse,
  type UploadChunkResponse,
  type UploadFinishResponse,
  type FirmwareBeginResponse,
  type FirmwareFinishResponse,
} from '@quran-device/protocol';
// Pure-JS MD5 rather than node:crypto, so this client also runs in the
// Android app (docs/android.md).
import { md5 } from '@noble/hashes/legacy.js';
import { bytesToHex } from '@noble/hashes/utils.js';
import { PackageType, readPackage } from '@quran-device/qpk-format';

import { decodePhotoFile, PHOTO_NAME_PATTERN } from './photo.js';

/** The device's chunk limit (net::kMaxChunkBytes); getInfo() reports it too. */
const PHOTO_CHUNK_BYTES = 16384;

/**
 * RequestInit plus the statuses this call treats as success, and `duplex`,
 * which the fetch standard requires when the body is a stream but which
 * TypeScript's DOM lib does not yet declare.
 */
type DeviceRequestInit = RequestInit & { expect?: number[]; duplex?: 'half' };

/**
 * Wraps a chunk so `fetch` sends it with `Transfer-Encoding: chunked` and
 * **no `Content-Length`**.
 *
 * This is not a style choice, it is the device's wire contract
 * (docs/protocol.md, "The chunk-upload body is binary-unsafe as a plain PUT").
 * Arduino's `WebServer` only auto-buffers a request body when
 * `Content-Length` is present, and it buffers it through
 * `String(plainBuf)` -- which stops at the first `0x00`. A QPK package
 * contains zero bytes as a matter of course, so a fixed-length PUT is
 * silently truncated. With the header absent the body is left on the socket
 * and `HttpServer::ReadChunkedBody` decodes the chunked transfer-coding
 * itself, straight into a fixed buffer.
 *
 * Passing a `Uint8Array` directly gives a fixed-length body and the wrong
 * framing, which is exactly the bug this replaced: every upload to real
 * hardware failed on its first chunk, unnoticed because uploads had never
 * run against a device. A fresh stream is built per attempt because a
 * consumed stream cannot be retried.
 */
// Not text/plain's usual charset parameter: the body is pure ASCII, and the
// device's WebServer keeps any non-form body as arg("plain") either way.
const CONTENT_TYPE_TEXT = 'text/plain';

const BASE64_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

/** Standard base64 with padding. No btoa/Buffer: neither is dependable in React Native. */
export function encodeBase64(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const n = (bytes[i]! << 16) | ((bytes[i + 1] ?? 0) << 8) | (bytes[i + 2] ?? 0);
    out += BASE64_ALPHABET[(n >> 18) & 63]! + BASE64_ALPHABET[(n >> 12) & 63]!;
    out += i + 1 < bytes.length ? BASE64_ALPHABET[(n >> 6) & 63]! : '=';
    out += i + 2 < bytes.length ? BASE64_ALPHABET[n & 63]! : '=';
  }
  return out;
}

function chunkBody(slice: Uint8Array): ReadableStream<Uint8Array> {
  return new ReadableStream<Uint8Array>({
    start(controller) {
      controller.enqueue(slice);
      controller.close();
    },
  });
}

export class DeviceError extends Error {
  constructor(
    readonly status: number,
    readonly code: ErrorCode | 'HTTP_ERROR',
    message: string,
    readonly expectedOffset?: number,
  ) {
    super(message);
    this.name = 'DeviceError';
  }
}

export interface DeviceClientOptions {
  /** Host name or IP. mDNS discovery hands this over; the user can also type it. */
  host: string;
  port?: number;
  /** The pairing token established over BLE. Omitted only for getInfo(). */
  token?: string;
  /** Per-request timeout. Uploads use it per chunk, not for the whole file. */
  timeoutMs?: number;
  /** Injected in tests; defaults to global fetch. */
  fetchImpl?: typeof fetch;
  /**
   * How chunk bodies go out. 'stream' (the default) sends each chunk as a
   * streamed chunked body; 'base64' sends it as base64 text marked
   * X-Qr-Body: base64, for runtimes whose fetch cannot stream a request body
   * -- React Native's, in the Android app (docs/android.md).
   */
  chunkEncoding?: 'stream' | 'base64';
}

export interface UploadProgress {
  sentBytes: number;
  totalBytes: number;
  /** Set when the device made us rewind: worth surfacing, not hiding. */
  resyncs: number;
}

export interface UploadOptions {
  onProgress?: (progress: UploadProgress) => void;
  /** Retries per chunk for transport-level failures. */
  maxRetriesPerChunk?: number;
  signal?: AbortSignal;
}

const PACKAGE_TYPE_NAMES: Record<number, PackageTypeName> = {
  [PackageType.Quran]: 'QURAN',
  [PackageType.Book]: 'BOOK',
  [PackageType.Translation]: 'TRANSLATION',
  [PackageType.Tafsir]: 'TAFSIR',
};

export class DeviceClient {
  private readonly base: string;
  private readonly token: string | undefined;
  private readonly timeoutMs: number;
  private readonly doFetch: typeof fetch;
  private readonly chunkEncoding: 'stream' | 'base64';

  constructor(options: DeviceClientOptions) {
    this.base = `http://${options.host}:${options.port ?? DEFAULT_PORT}`;
    this.token = options.token;
    this.timeoutMs = options.timeoutMs ?? 15000;
    this.doFetch = options.fetchImpl ?? fetch;
    this.chunkEncoding = options.chunkEncoding ?? 'stream';
  }

  // --- plumbing -------------------------------------------------------------

  private headers(extra: Record<string, string> = {}): Record<string, string> {
    const out: Record<string, string> = { ...extra };
    if (this.token) out['Authorization'] = bearer(this.token);
    return out;
  }

  private async request(path: string, init: DeviceRequestInit = {}): Promise<Response> {
    const expect = init.expect ?? [STATUS.ok, STATUS.created];
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    let response: Response;
    try {
      response = await this.doFetch(`${this.base}${path}`, {
        ...init,
        // Authorisation is attached here, not at each call site: an endpoint
        // added later cannot forget it, and forgetting it looks exactly like
        // an unpaired device.
        headers: this.headers((init.headers as Record<string, string>) ?? {}),
        signal: init.signal ?? controller.signal,
      });
    } finally {
      clearTimeout(timer);
    }
    if (expect.includes(response.status)) return response;
    throw await toDeviceError(response);
  }

  private async json<T>(path: string, init: DeviceRequestInit = {}): Promise<T> {
    const response = await this.request(path, init);
    return (await response.json()) as T;
  }

  // --- device ---------------------------------------------------------------

  /** The only endpoint that works before pairing. */
  getInfo(): Promise<DeviceInfo> {
    return this.json<DeviceInfo>(PATHS.deviceInfo);
  }

  getStatus(): Promise<DeviceStatus> {
    return this.json<DeviceStatus>(PATHS.deviceStatus);
  }

  // --- library --------------------------------------------------------------

  async listLibrary(): Promise<LibraryListing> {
    return this.json<LibraryListing>(PATHS.library);
  }

  async deleteItem(contentId: string): Promise<void> {
    await this.request(PATHS.libraryItem(contentId), { method: 'DELETE' });
  }

  // --- upload ---------------------------------------------------------------

  /**
   * Installs a package, resuming an interrupted transfer if the device still
   * has one. The package is validated locally first: uploading something the
   * device is guaranteed to reject wastes minutes of radio time.
   */
  async uploadPackage(
    bytes: Uint8Array,
    options: UploadOptions = {},
  ): Promise<UploadFinishResponse> {
    const pkg = readPackage(bytes);
    const typeName = PACKAGE_TYPE_NAMES[pkg.header.packageType];
    if (!typeName) {
      throw new DeviceError(0, 'HTTP_ERROR', `unsupported package type ${pkg.header.packageType}`);
    }
    const contentId = [...pkg.header.contentId]
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');

    const begin = await this.json<UploadBeginResponse>(PATHS.uploadBegin, {
      method: 'POST',
      headers: { 'Content-Type': CONTENT_TYPE_JSON },
      body: JSON.stringify({
        contentId,
        contentVersion: pkg.header.contentVersion,
        type: typeName,
        size: bytes.length,
        payloadCrc32: pkg.header.payloadCrc32,
      }),
      expect: [STATUS.ok, STATUS.created],
    });

    await this.sendChunks(
      (offset) => `${PATHS.uploadChunk(begin.sessionId)}?offset=${offset}`,
      bytes,
      begin.receivedBytes,
      Math.max(1, begin.maxChunkBytes),
      options,
    );

    return this.json<UploadFinishResponse>(PATHS.uploadFinish(begin.sessionId), {
      method: 'POST',
      expect: [STATUS.ok],
    });
  }

  /**
   * The chunk loop shared by packages and photos. Resumes from `offset`, and
   * on any OFFSET_MISMATCH continues from wherever the device says it is.
   */
  private async sendChunks(
    pathFor: (offset: number) => string,
    bytes: Uint8Array,
    startOffset: number,
    chunkSize: number,
    options: UploadOptions,
  ): Promise<void> {
    const maxRetries = options.maxRetriesPerChunk ?? 3;
    let offset = startOffset;
    let resyncs = 0;
    let attempts = 0;

    options.onProgress?.({ sentBytes: offset, totalBytes: bytes.length, resyncs });

    while (offset < bytes.length) {
      const end = Math.min(offset + chunkSize, bytes.length);
      const slice = bytes.subarray(offset, end);
      try {
        const framing: DeviceRequestInit =
          this.chunkEncoding === 'base64'
            ? {
                headers: {
                  'Content-Type': CONTENT_TYPE_TEXT,
                  [HEADER_OFFSET]: String(offset),
                  [HEADER_BODY]: BODY_ENCODING_BASE64,
                },
                body: encodeBase64(slice),
              }
            : {
                headers: {
                  'Content-Type': CONTENT_TYPE_OCTET,
                  [HEADER_OFFSET]: String(offset),
                },
                body: chunkBody(slice),
                duplex: 'half',
              };
        const response = await this.request(pathFor(offset), {
          method: 'PUT',
          ...framing,
          expect: [STATUS.ok],
          ...(options.signal ? { signal: options.signal } : {}),
        });
        const body = (await response.json()) as UploadChunkResponse;
        // Trust the device's count, not our own arithmetic.
        offset = body.receivedBytes;
        attempts = 0;
      } catch (error) {
        if (error instanceof DeviceError && error.code === 'OFFSET_MISMATCH') {
          // The device tells us where it actually is; one round trip, no guessing.
          if (error.expectedOffset === undefined) throw error;
          offset = error.expectedOffset;
          resyncs++;
          attempts = 0;
        } else if (error instanceof DeviceError && error.status !== 0) {
          throw error; // a real refusal: too large, no space, no session
        } else if (++attempts > maxRetries) {
          throw error; // transport failure that will not settle
        }
      }
      options.onProgress?.({ sentBytes: offset, totalBytes: bytes.length, resyncs });
    }
  }

  // --- home-screen photos and time zone ------------------------------------

  listPhotos(): Promise<PhotoListing> {
    return this.json<PhotoListing>(PATHS.photos);
  }

  /**
   * Uploads a finished .g4 photo file (see photo.ts's convertPhoto) and makes
   * it a photo on the device, replacing any photo of the same name. Starts
   * from offset 0, which the device takes as "start this photo over".
   */
  async uploadPhoto(
    name: string,
    file: Uint8Array,
    options: UploadOptions = {},
  ): Promise<PhotoFinishResponse> {
    if (!PHOTO_NAME_PATTERN.test(name)) {
      throw new DeviceError(0, 'HTTP_ERROR', `invalid photo name "${name}": use a-z, 0-9, - and _`);
    }
    decodePhotoFile(file); // throws on anything the device would refuse
    await this.sendChunks((offset) => `${PATHS.photoChunk(name)}?offset=${offset}`, file, 0,
      PHOTO_CHUNK_BYTES, options);
    return this.json<PhotoFinishResponse>(PATHS.photoFinish(name), {
      method: 'POST',
      expect: [STATUS.ok],
    });
  }

  /**
   * Firmware over Wi-Fi. `bytes` is the app image PlatformIO builds
   * (`.pio/build/<env>/firmware.bin`); the device writes it into its spare OTA
   * slot, checks the MD5 sent here, makes it the boot image and restarts
   * about a second after replying. Chunks are strictly in order -- a device
   * that stored a different amount resyncs the client, as for packages.
   */
  async uploadFirmware(bytes: Uint8Array, options: UploadOptions = {}): Promise<FirmwareFinishResponse> {
    // Every ESP32 app image starts with this magic byte; a merged image
    // (bootloader + partitions + app) does not, and would never boot from a slot.
    if (bytes.length < 24 || bytes[0] !== 0xe9) {
      throw new DeviceError(
        0,
        'HTTP_ERROR',
        'not an ESP32 app image: send firmware.bin from .pio/build, not a merged image',
      );
    }
    const imageMd5 = bytesToHex(md5(bytes));
    const begin = await this.json<FirmwareBeginResponse>(PATHS.firmwareBegin, {
      method: 'POST',
      headers: { 'Content-Type': CONTENT_TYPE_JSON },
      body: JSON.stringify({ size: bytes.length, md5: imageMd5 }),
      expect: [STATUS.ok],
    });
    if (begin.slotBytes > 0 && bytes.length > begin.slotBytes) {
      throw new DeviceError(0, 'NO_SPACE', `firmware is ${bytes.length} bytes, the slot holds ${begin.slotBytes}`);
    }
    await this.sendChunks(
      (offset) => `${PATHS.firmwareChunk}?offset=${offset}`,
      bytes,
      begin.receivedBytes,
      Math.max(1, begin.maxChunkBytes),
      options,
    );
    return this.json<FirmwareFinishResponse>(PATHS.firmwareFinish, {
      method: 'POST',
      expect: [STATUS.ok],
    });
  }

  async deletePhoto(name: string): Promise<void> {
    await this.request(PATHS.photoItem(name), { method: 'DELETE' });
  }

  /** `tz` is a POSIX TZ rule; photo.ts's posixTimeZone() gives this computer's. */
  setTimeZone(tz: string): Promise<TimeZoneResponse> {
    return this.json<TimeZoneResponse>(PATHS.deviceTime, {
      method: 'POST',
      headers: { 'Content-Type': CONTENT_TYPE_JSON },
      body: JSON.stringify({ tz }),
      expect: [STATUS.ok],
    });
  }

  async abortUpload(sessionId: string): Promise<void> {
    await this.request(PATHS.uploadAbort(sessionId), {
      method: 'DELETE',
      expect: [STATUS.ok, STATUS.notFound],
    });
  }
}

async function toDeviceError(response: Response): Promise<DeviceError> {
  let body: Partial<ErrorBody> = {};
  try {
    body = (await response.json()) as Partial<ErrorBody>;
  } catch {
    // A device that fails before it can build JSON is still a failure.
  }
  const code = (body.error ?? 'HTTP_ERROR') as ErrorCode | 'HTTP_ERROR';
  const detail = body.detail ?? `HTTP ${response.status}`;
  return new DeviceError(response.status, code, `${code}: ${detail}`, body.expectedOffset);
}
