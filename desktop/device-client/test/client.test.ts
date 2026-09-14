import assert from 'node:assert/strict';
import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http';
import { AddressInfo } from 'node:net';
import test from 'node:test';

import { ERROR_STATUS, PATHS, STATUS, type ErrorCode } from '@quran-device/protocol';
import { buildMiniQuran } from '@quran-device/qpk-format';

import { DeviceClient, DeviceError, encodeBase64 } from '../src/client.js';
import { ManualDiscovery, TrustStore } from '../src/discovery.js';

// ---------------------------------------------------------------------------
// A scripted stand-in for the device.
//
// Deliberately NOT a second implementation of the upload state machine -- the
// firmware owns that, and re-implementing it here would just let the two
// drift while both tests passed. This records what it was sent and replays
// whatever the test tells it to, so each test can stage one specific
// behaviour: a resume, a 409, a lost response.
// ---------------------------------------------------------------------------

interface MockOptions {
  token?: string;
  maxChunkBytes?: number;
  /** Bytes the device claims to already hold when the session opens. */
  initialReceived?: number;
  /** Chunk index (0-based) at which to answer 409 once. */
  conflictAtChunk?: number;
  /** Chunk index at which to apply the write but drop the response once. */
  dropResponseAtChunk?: number;
  /** Fail every chunk with this error code. */
  failChunksWith?: ErrorCode;
  library?: unknown[];
}

interface MockDevice {
  server: Server;
  port: number;
  received: Uint8Array;
  requests: Array<{ method: string; path: string; auth?: string }>;
  chunkCount: number;
  finished: boolean;
  deleted: string[];
  /** How each chunk PUT was framed on the wire. See the framing test below. */
  chunkFraming: Array<{ contentLength?: string; transferEncoding?: string; bodyEncoding?: string }>;
  /** The JSON body of the last firmware begin. */
  firmwareBegin: unknown;
  close(): Promise<void>;
}

async function startMockDevice(total: number, options: MockOptions = {}): Promise<MockDevice> {
  const maxChunkBytes = options.maxChunkBytes ?? 4096;
  const state = {
    received: new Uint8Array(0),
    chunkCount: 0,
    finished: false,
    conflictFired: false,
    dropFired: false,
    deleted: [] as string[],
    requests: [] as Array<{ method: string; path: string; auth?: string }>,
    chunkFraming: [] as Array<{ contentLength?: string; transferEncoding?: string; bodyEncoding?: string }>,
    firmwareBegin: undefined as unknown,
  };
  state.received = new Uint8Array(options.initialReceived ?? 0);

  const sendJson = (response: ServerResponse, status: number, body: unknown) => {
    const text = JSON.stringify(body);
    response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
    response.end(text);
  };
  const sendError = (response: ServerResponse, code: ErrorCode, extra: object = {}) => {
    sendJson(response, ERROR_STATUS[code], { error: code, detail: code, ...extra });
  };

  const readBody = (request: IncomingMessage): Promise<Buffer> =>
    new Promise((resolve, reject) => {
      const chunks: Buffer[] = [];
      request.on('data', (chunk: Buffer) => chunks.push(chunk));
      request.on('end', () => resolve(Buffer.concat(chunks)));
      request.on('error', reject);
    });

  const server = createServer(async (request, response) => {
    const url = new URL(request.url ?? '/', 'http://device');
    const path = url.pathname;
    const auth = request.headers['authorization'];
    state.requests.push({
      method: request.method ?? 'GET',
      path,
      ...(typeof auth === 'string' ? { auth } : {}),
    });

    // Everything but device info requires the pairing token.
    if (path !== PATHS.deviceInfo && options.token) {
      if (auth !== `Bearer ${options.token}`) {
        sendError(response, 'UNAUTHORIZED');
        return;
      }
    }

    if (path === PATHS.deviceInfo) {
      sendJson(response, STATUS.ok, {
        protocolVersion: 1,
        deviceId: 'device-1',
        name: 'Test Reader',
        model: 'crowpanel-579',
        firmwareVersion: '0.1.0-m4',
        maxChunkBytes,
        paired: Boolean(options.token),
      });
      return;
    }

    if (path === PATHS.deviceStatus) {
      sendJson(response, STATUS.ok, {
        storage: { mounted: true, capacityBytes: 1000, usedBytes: 10, freeBytes: 990 },
        battery: { available: false },
        uptimeSeconds: 42,
        transferMode: true,
        openSessions: state.finished ? 0 : 1,
      });
      return;
    }

    if (path === PATHS.library && request.method === 'GET') {
      sendJson(response, STATUS.ok, { items: options.library ?? [] });
      return;
    }

    if (request.method === 'DELETE' && path.startsWith(PATHS.library + '/') &&
        !path.startsWith(PATHS.uploadBegin)) {
      state.deleted.push(path.slice((PATHS.library + '/').length));
      sendJson(response, STATUS.ok, {});
      return;
    }

    if (path === PATHS.firmwareBegin && request.method === 'POST') {
      state.firmwareBegin = JSON.parse((await readBody(request)).toString('utf8'));
      state.received = new Uint8Array(0);
      sendJson(response, STATUS.ok, { receivedBytes: 0, maxChunkBytes, slotBytes: 3342336 });
      return;
    }

    if (path === PATHS.uploadBegin && request.method === 'POST') {
      await readBody(request);
      sendJson(response, STATUS.created, {
        sessionId: 'session-1',
        receivedBytes: state.received.length,
        maxChunkBytes,
        expiresInSeconds: 900,
      });
      return;
    }

    if (path.endsWith('/chunk') && request.method === 'PUT') {
      state.chunkFraming.push({
        ...(request.headers['content-length'] !== undefined
          ? { contentLength: request.headers['content-length'] }
          : {}),
        ...(request.headers['transfer-encoding'] !== undefined
          ? { transferEncoding: request.headers['transfer-encoding'] }
          : {}),
        ...(typeof request.headers['x-qr-body'] === 'string'
          ? { bodyEncoding: request.headers['x-qr-body'] }
          : {}),
      });
      const raw = await readBody(request);
      // Decoded the way the firmware's ReadChunkBody does, so a base64 client
      // is checked on the bytes it actually delivers.
      const body =
        request.headers['x-qr-body'] === 'base64'
          ? Buffer.from(raw.toString('ascii'), 'base64')
          : raw;
      const offset = Number(url.searchParams.get('offset'));
      const index = state.chunkCount++;

      if (options.failChunksWith) {
        sendError(response, options.failChunksWith);
        return;
      }
      if (options.conflictAtChunk === index && !state.conflictFired) {
        state.conflictFired = true;
        sendError(response, 'OFFSET_MISMATCH', { expectedOffset: state.received.length });
        return;
      }
      if (offset !== state.received.length) {
        sendError(response, 'OFFSET_MISMATCH', { expectedOffset: state.received.length });
        return;
      }

      const merged = new Uint8Array(state.received.length + body.length);
      merged.set(state.received, 0);
      merged.set(body, state.received.length);
      state.received = merged;

      if (options.dropResponseAtChunk === index && !state.dropFired) {
        // The write landed but the answer never arrived: the case the device's
        // idempotent retry handling exists for.
        state.dropFired = true;
        request.socket.destroy();
        return;
      }
      sendJson(response, STATUS.ok, { receivedBytes: state.received.length });
      return;
    }

    if (path.endsWith('/finish') && request.method === 'POST') {
      if (state.received.length !== total) {
        sendError(response, 'SIZE_MISMATCH');
        return;
      }
      state.finished = true;
      sendJson(response, STATUS.ok, {
        contentId: 'abc',
        installed: true,
        location: 'QURAN/abc.qpk',
      });
      return;
    }

    if (request.method === 'DELETE') {
      sendJson(response, STATUS.ok, {});
      return;
    }

    sendError(response, 'NOT_FOUND');
  });

  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = (server.address() as AddressInfo).port;

  return {
    server,
    port,
    get received() {
      return state.received;
    },
    get requests() {
      return state.requests;
    },
    get chunkCount() {
      return state.chunkCount;
    },
    get chunkFraming() {
      return state.chunkFraming;
    },
    get finished() {
      return state.finished;
    },
    get deleted() {
      return state.deleted;
    },
    get firmwareBegin() {
      return state.firmwareBegin;
    },
    close: () => new Promise<void>((resolve) => server.close(() => resolve())),
  } as MockDevice;
}

const PACKAGE = buildMiniQuran();

function clientFor(device: MockDevice, token?: string): DeviceClient {
  return new DeviceClient({
    host: '127.0.0.1',
    port: device.port,
    ...(token ? { token } : {}),
    timeoutMs: 5000,
  });
}

// ---------------------------------------------------------------------------

test('device info is readable before pairing', async () => {
  const device = await startMockDevice(PACKAGE.length, { token: 'tok' });
  try {
    const info = await new DeviceClient({ host: '127.0.0.1', port: device.port }).getInfo();
    assert.equal(info.deviceId, 'device-1');
    assert.equal(info.paired, true);
  } finally {
    await device.close();
  }
});

test('every other endpoint requires the pairing token', async () => {
  const device = await startMockDevice(PACKAGE.length, { token: 'tok' });
  try {
    const anonymous = clientFor(device);
    await assert.rejects(
      () => anonymous.getStatus(),
      (error: unknown) =>
        error instanceof DeviceError && error.code === 'UNAUTHORIZED' && error.status === 401,
    );

    const authorised = clientFor(device, 'tok');
    const status = await authorised.getStatus();
    assert.equal(status.transferMode, true);
    assert.equal(
      device.requests.at(-1)?.auth,
      'Bearer tok',
      'the client must send the bearer header',
    );
  } finally {
    await device.close();
  }
});

test('a package uploads and installs', async () => {
  const device = await startMockDevice(PACKAGE.length, { maxChunkBytes: 256 });
  try {
    const progress: number[] = [];
    const result = await clientFor(device).uploadPackage(PACKAGE, {
      onProgress: (p) => progress.push(p.sentBytes),
    });
    assert.equal(result.installed, true);
    assert.deepEqual(Buffer.from(device.received), Buffer.from(PACKAGE));
    assert.ok(device.chunkCount >= Math.ceil(PACKAGE.length / 256));
    assert.equal(progress.at(-1), PACKAGE.length);
    assert.equal(progress.at(0), 0);
  } finally {
    await device.close();
  }
});

test('chunks never exceed the size the device advertises', async () => {
  // The device buffers a chunk body in RAM, so this is a hard limit, not a hint.
  const device = await startMockDevice(PACKAGE.length, { maxChunkBytes: 64 });
  try {
    await clientFor(device).uploadPackage(PACKAGE);
    assert.equal(device.chunkCount, Math.ceil(PACKAGE.length / 64));
  } finally {
    await device.close();
  }
});

test('an interrupted transfer resumes from the device count, not ours', async () => {
  const already = 300;
  const device = await startMockDevice(PACKAGE.length, {
    maxChunkBytes: 256,
    initialReceived: already,
  });
  try {
    const first: number[] = [];
    await clientFor(device).uploadPackage(PACKAGE, {
      onProgress: (p) => first.push(p.sentBytes),
    });
    // It started where the device said it was, not at zero.
    assert.equal(first.at(0), already);
    assert.equal(device.received.length, PACKAGE.length);
    assert.equal(device.chunkCount, Math.ceil((PACKAGE.length - already) / 256));
  } finally {
    await device.close();
  }
});

test('a 409 rewinds the client to the offset the device names', async () => {
  const device = await startMockDevice(PACKAGE.length, {
    maxChunkBytes: 128,
    conflictAtChunk: 2,
  });
  try {
    let resyncs = 0;
    const result = await clientFor(device).uploadPackage(PACKAGE, {
      onProgress: (p) => {
        resyncs = p.resyncs;
      },
    });
    assert.equal(result.installed, true);
    assert.equal(resyncs, 1, 'the resync should be surfaced, not hidden');
    assert.deepEqual(Buffer.from(device.received), Buffer.from(PACKAGE));
  } finally {
    await device.close();
  }
});

test('a lost response is retried and does not double-write', async () => {
  const device = await startMockDevice(PACKAGE.length, {
    maxChunkBytes: 128,
    dropResponseAtChunk: 1,
  });
  try {
    const result = await clientFor(device).uploadPackage(PACKAGE);
    assert.equal(result.installed, true);
    // The bytes are correct: the retry re-sent a chunk the device already had,
    // and the device's idempotence (and the client's trust in its count) meant
    // it landed once.
    assert.deepEqual(Buffer.from(device.received), Buffer.from(PACKAGE));
  } finally {
    await device.close();
  }
});

test('a refusal is reported, not retried forever', async () => {
  const device = await startMockDevice(PACKAGE.length, { failChunksWith: 'NO_SPACE' });
  try {
    await assert.rejects(
      () => clientFor(device).uploadPackage(PACKAGE),
      (error: unknown) =>
        error instanceof DeviceError && error.code === 'NO_SPACE' && error.status === 507,
    );
    // One attempt, not a retry storm.
    assert.equal(device.chunkCount, 1);
  } finally {
    await device.close();
  }
});

test('an invalid package is rejected locally before anything is sent', async () => {
  const device = await startMockDevice(PACKAGE.length);
  try {
    const damaged = Uint8Array.from(PACKAGE);
    const last = damaged.length - 1;
    damaged.set([(damaged.at(last) ?? 0) ^ 0xff], last);
    await assert.rejects(() => clientFor(device).uploadPackage(damaged));
    // Uploading something the device is certain to reject wastes minutes of
    // radio time, so nothing should have left the desktop.
    assert.equal(device.requests.length, 0);
  } finally {
    await device.close();
  }
});

test('a firmware image uploads in order, with its size and MD5 up front', async () => {
  const image = Uint8Array.from({ length: 3000 }, (_, i) => (i * 31) & 0xff);
  image[0] = 0xe9; // the ESP32 app image magic byte
  const device = await startMockDevice(image.length, { maxChunkBytes: 512 });
  try {
    const progress: number[] = [];
    await clientFor(device).uploadFirmware(image, { onProgress: (p) => progress.push(p.sentBytes) });
    assert.deepEqual(Buffer.from(device.received), Buffer.from(image));
    assert.equal(device.finished, true);
    assert.equal(progress.at(-1), image.length);
    const { createHash } = await import('node:crypto');
    assert.deepEqual(device.firmwareBegin, {
      size: image.length,
      md5: createHash('md5').update(image).digest('hex'),
    });
    assert.ok(device.requests.some((r) => r.path === PATHS.firmwareChunk));
    assert.equal(device.requests.at(-1)?.path, PATHS.firmwareFinish);
  } finally {
    await device.close();
  }
});

test('anything that is not an ESP32 app image is refused before it is sent', async () => {
  const device = await startMockDevice(10);
  try {
    const merged = new Uint8Array(4096); // a merged image starts with the bootloader, not 0xE9
    await assert.rejects(() => clientFor(device).uploadFirmware(merged), DeviceError);
    assert.equal(device.requests.length, 0);
  } finally {
    await device.close();
  }
});

test('library listing and deletion', async () => {
  const device = await startMockDevice(0, {
    library: [{ contentId: 'abc', title: 'A Book', type: 'BOOK' }],
  });
  try {
    const client = clientFor(device);
    const listing = await client.listLibrary();
    assert.equal(listing.items.length, 1);
    await client.deleteItem('abc');
    assert.deepEqual(device.deleted, ['abc']);
  } finally {
    await device.close();
  }
});

// ---------------------------------------------------------------------------
// Discovery / trust store
// ---------------------------------------------------------------------------

test('manual discovery parses host and port', async () => {
  const found = await new ManualDiscovery(['192.168.1.5', '10.0.0.2:9000']).find(0);
  assert.deepEqual(found, [
    { host: '192.168.1.5', port: 8080, source: 'manual' },
    { host: '10.0.0.2', port: 9000, source: 'manual' },
  ]);
});

test('a remembered device keeps its token when it moves network', async () => {
  const store = new TrustStore([
    { deviceId: 'd1', name: 'Reader', token: 'secret', lastHost: '192.168.1.5' },
  ]);
  // Seen again at a new address, with no token supplied.
  store.remember({ deviceId: 'd1', name: 'Reader', token: '', lastHost: '10.0.0.9' });
  assert.equal(store.get('d1')?.token, 'secret');
  assert.equal(store.get('d1')?.lastHost, '10.0.0.9');

  assert.equal(store.forget('d1'), true);
  assert.equal(store.list().length, 0);
});

test('every chunk PUT is chunked-encoded with no Content-Length', async () => {
  // Not a style preference: it is the device's wire contract
  // (docs/protocol.md, "The chunk-upload body is binary-unsafe as a plain
  // PUT"). Arduino's WebServer only auto-buffers a body when Content-Length
  // is present, and buffers it through String(plainBuf), which stops at the
  // first 0x00 -- and a QPK package contains zero bytes routinely. So a
  // fixed-length PUT is silently truncated on real hardware, while
  // HttpServer::ReadChunkedBody parses a hex chunk-size line and rejects it
  // outright.
  //
  // This asserts the FRAMING, not the bytes. The rest of the suite checks the
  // bytes and passed happily while the client sent Content-Length -- which is
  // exactly why the bug survived to be found against real firmware.
  const device = await startMockDevice(PACKAGE.length, { maxChunkBytes: 256 });
  try {
    await clientFor(device).uploadPackage(PACKAGE);

    assert.ok(device.chunkFraming.length > 1, 'expected several chunks');
    for (const framing of device.chunkFraming) {
      assert.equal(framing.transferEncoding, 'chunked');
      assert.equal(framing.contentLength, undefined);
    }
  } finally {
    await device.close();
  }
});

test('base64 chunk bodies carry every byte, with a length and the X-Qr-Body marker', async () => {
  // The Android app's fetch cannot stream a body, so it sends base64 text with
  // Content-Length instead -- text survives the WebServer's String buffering
  // that binary would not. The resync path is included: it rebuilds the body.
  const device = await startMockDevice(PACKAGE.length, {
    maxChunkBytes: 256,
    conflictAtChunk: 2,
  });
  try {
    await new DeviceClient({
      host: '127.0.0.1',
      port: device.port,
      timeoutMs: 5000,
      chunkEncoding: 'base64',
    }).uploadPackage(PACKAGE);

    assert.deepEqual(new Uint8Array(device.received), PACKAGE);
    assert.ok(device.chunkFraming.length > 2, 'expected several chunks');
    for (const framing of device.chunkFraming) {
      assert.equal(framing.bodyEncoding, 'base64');
      assert.equal(framing.transferEncoding, undefined);
      assert.ok(Number(framing.contentLength) > 0);
    }
  } finally {
    await device.close();
  }
});

test('encodeBase64 matches the standard encoding at every padding length', () => {
  for (const length of [0, 1, 2, 3, 4, 5, 255, 256, 1000]) {
    const bytes = new Uint8Array(length).map((_, i) => (i * 37) & 0xff);
    assert.equal(encodeBase64(bytes), Buffer.from(bytes).toString('base64'));
  }
});

test('a chunk retried after a resync is still framed correctly', async () => {
  // A ReadableStream body cannot be replayed, so every attempt has to build a
  // fresh one. If a retry reused a consumed stream it would send an empty
  // body, or fall back to a fixed-length one.
  const device = await startMockDevice(PACKAGE.length, {
    maxChunkBytes: 256,
    conflictAtChunk: 1,
  });
  try {
    await clientFor(device).uploadPackage(PACKAGE);

    assert.deepEqual(device.received, PACKAGE);
    for (const framing of device.chunkFraming) {
      assert.equal(framing.transferEncoding, 'chunked');
      assert.equal(framing.contentLength, undefined);
    }
  } finally {
    await device.close();
  }
});
