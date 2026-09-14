import assert from 'node:assert/strict';
import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { AddressInfo } from 'node:net';
import test from 'node:test';

import {
  PHOTO_FILE_BYTES as PROTOCOL_PHOTO_FILE_BYTES,
  PHOTO_HEADER_BYTES as PROTOCOL_PHOTO_HEADER_BYTES,
  PHOTO_HEIGHT as PROTOCOL_PHOTO_HEIGHT,
  PHOTO_NAME_PATTERN as PROTOCOL_PHOTO_NAME_PATTERN,
  PHOTO_WIDTH as PROTOCOL_PHOTO_WIDTH,
} from '@quran-device/protocol';

import { DeviceClient, DeviceError } from '../src/client.js';
import {
  PANEL_GREYS,
  PHOTO_FILE_BYTES,
  PHOTO_HEADER_BYTES,
  PHOTO_HEIGHT,
  PHOTO_NAME_PATTERN,
  PHOTO_WIDTH,
  convertPhoto,
  coverCrop,
  decodePhotoFile,
  encodePhotoFile,
  photoNameFor,
  posixTimeZone,
} from '../src/photo.js';

const COUNT = PHOTO_WIDTH * PHOTO_HEIGHT;

function flatRgba(r: number, g: number, b: number, a = 255): Uint8Array {
  const rgba = new Uint8Array(COUNT * 4);
  for (let i = 0; i < COUNT; i++) rgba.set([r, g, b, a], i * 4);
  return rgba;
}

// --- the format ----------------------------------------------------------------

test('photo.ts constants match the protocol package', () => {
  assert.equal(PHOTO_WIDTH, PROTOCOL_PHOTO_WIDTH);
  assert.equal(PHOTO_HEIGHT, PROTOCOL_PHOTO_HEIGHT);
  assert.equal(PHOTO_HEADER_BYTES, PROTOCOL_PHOTO_HEADER_BYTES);
  assert.equal(PHOTO_FILE_BYTES, PROTOCOL_PHOTO_FILE_BYTES);
  assert.equal(PHOTO_NAME_PATTERN.source, PROTOCOL_PHOTO_NAME_PATTERN.source);
  assert.equal(PHOTO_FILE_BYTES, 48016);
});

test('encode then decode gives back every level', () => {
  const levels = new Uint8Array(COUNT);
  let seed = 7;
  for (let i = 0; i < COUNT; i++) {
    seed = (seed * 1103515245 + 12345) >>> 0;
    levels[i] = seed % 4;
  }
  const file = encodePhotoFile(levels);
  assert.equal(file.length, PHOTO_FILE_BYTES);
  assert.deepEqual([...file.subarray(0, 9)], [0x51, 0x50, 0x48, 0x31, 0x90, 0x01, 0xe0, 0x01, 2]);
  assert.deepEqual(decodePhotoFile(file), levels);
});

test('decode refuses files the device would refuse', () => {
  const good = encodePhotoFile(new Uint8Array(COUNT));
  assert.throws(() => decodePhotoFile(good.subarray(0, 100)));
  const badMagic = good.slice();
  badMagic[0] = 0;
  assert.throws(() => decodePhotoFile(badMagic));
  const badReserved = good.slice();
  badReserved[15] = 1;
  assert.throws(() => decodePhotoFile(badReserved));
});

// --- conversion ------------------------------------------------------------------

test('flat colours land on the matching panel level', () => {
  const cases: Array<[number, number]> = [[0, 0], [85, 1], [170, 2], [255, 3]];
  for (const [value, level] of cases) {
    const { levels } = convertPhoto(flatRgba(value, value, value), { contrast: 0, dither: false });
    assert.ok(levels.every((l) => l === level), `grey ${value} -> level ${level}`);
  }
  const transparent = convertPhoto(flatRgba(0, 0, 0, 0), { contrast: 0, dither: false });
  assert.ok(transparent.levels.every((l) => l === 3), 'transparent shows as white');
});

test('dithering keeps the average brightness of a mid grey', () => {
  const { levels } = convertPhoto(flatRgba(128, 128, 128), { contrast: 0 });
  let sum = 0;
  for (const level of levels) sum += PANEL_GREYS[level as 0 | 1 | 2 | 3];
  const mean = sum / COUNT;
  assert.ok(Math.abs(mean - 128) < 2, `mean ${mean}`);
  assert.ok(new Set(levels).size >= 2, 'mixes neighbouring levels');
});

test('convertPhoto checks the pixel count', () => {
  assert.throws(() => convertPhoto(new Uint8Array(16)), /RGBA bytes/u);
});

test('coverCrop takes the centred 5:6 region', () => {
  assert.deepEqual(coverCrop(4000, 3000), { sx: 750, sy: 0, sw: 2500, sh: 3000 });
  assert.deepEqual(coverCrop(1000, 2000), { sx: 0, sy: 400, sw: 1000, sh: 1200 });
  assert.deepEqual(coverCrop(400, 480), { sx: 0, sy: 0, sw: 400, sh: 480 });
  assert.throws(() => coverCrop(0, 10));
});

test('photoNameFor makes names the device accepts', () => {
  assert.equal(photoNameFor('Beach Day (2).JPG'), 'beach-day-2');
  assert.equal(photoNameFor('/Users/x/Pictures/IMG_0042.heic'), 'img_0042');
  assert.equal(photoNameFor('....png'), 'photo');
  assert.equal(photoNameFor('Ünïcødé.png'), 'n-c-d');
  const long = photoNameFor(`${'a'.repeat(40)}.png`);
  assert.equal(long.length, 32);
  for (const name of ['beach-day-2', long, photoNameFor('-_-.png')]) {
    assert.ok(PHOTO_NAME_PATTERN.test(name), name);
  }
});

// --- time zone -------------------------------------------------------------------

test('posixTimeZone describes the computer clock as a POSIX rule', () => {
  const original = process.env.TZ;
  const cases: Array<[string, string]> = [
    ['UTC', 'LOC0'],
    ['Asia/Kolkata', 'LOC-5:30'],
    ['Europe/Berlin', 'LOC-1LDT-2,M3.5.0/2,M10.5.0/3'],
    ['America/New_York', 'LOC5LDT4,M3.2.0/2,M11.1.0/2'],
    ['Australia/Sydney', 'LOC-10LDT-11,M10.1.0/2,M4.1.0/3'],
  ];
  try {
    for (const [zone, rule] of cases) {
      process.env.TZ = zone;
      assert.equal(posixTimeZone(2026), rule, zone);
      assert.ok(rule.length <= 63);
    }
  } finally {
    if (original === undefined) delete process.env.TZ;
    else process.env.TZ = original;
  }
});

// --- client against a scripted device ----------------------------------------------

interface PhotoDevice {
  port: number;
  requests: Array<{ method: string; path: string; offset?: string; body?: string }>;
  stored: Map<string, Buffer>;
  close(): Promise<void>;
}

async function startPhotoDevice(options: { conflictOnce?: boolean } = {}): Promise<PhotoDevice> {
  const parts = new Map<string, Buffer>();
  const stored = new Map<string, Buffer>();
  const requests: PhotoDevice['requests'] = [];
  let conflicted = false;

  const send = (response: ServerResponse, status: number, body: unknown) => {
    response.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8' });
    response.end(JSON.stringify(body));
  };

  const server = createServer(async (request: IncomingMessage, response: ServerResponse) => {
    const chunks: Buffer[] = [];
    for await (const chunk of request) chunks.push(chunk as Buffer);
    const body = Buffer.concat(chunks);
    const url = new URL(request.url ?? '/', 'http://device');
    const offsetHeader = request.headers['x-qr-offset'];
    const offset = Array.isArray(offsetHeader) ? offsetHeader[0] : offsetHeader;
    requests.push({
      method: request.method ?? '',
      path: url.pathname,
      ...(offset !== undefined ? { offset } : {}),
      ...(request.method === 'POST' ? { body: body.toString('utf8') } : {}),
    });

    const chunkMatch = /^\/api\/photos\/([^/]+)\/chunk$/u.exec(url.pathname);
    const finishMatch = /^\/api\/photos\/([^/]+)\/finish$/u.exec(url.pathname);
    const itemMatch = /^\/api\/photos\/([^/]+)$/u.exec(url.pathname);

    if (url.pathname === '/api/photos' && request.method === 'GET') {
      send(response, 200, {
        width: 400,
        height: 480,
        items: [...stored.keys()].sort().map((name) => ({ name, bytes: 48016 })),
      });
    } else if (chunkMatch && request.method === 'PUT') {
      const name = chunkMatch[1] ?? '';
      const at = Number(offset);
      const have = at === 0 ? Buffer.alloc(0) : (parts.get(name) ?? Buffer.alloc(0));
      if (options.conflictOnce && !conflicted && at > 0) {
        // Pretend the last chunk never landed.
        conflicted = true;
        const rewound = have.subarray(0, have.length - 1000);
        parts.set(name, rewound);
        send(response, 409, { error: 'OFFSET_MISMATCH', expectedOffset: rewound.length });
        return;
      }
      if (at !== have.length) {
        send(response, 409, { error: 'OFFSET_MISMATCH', expectedOffset: have.length });
        return;
      }
      const next = Buffer.concat([have, body]);
      parts.set(name, next);
      send(response, 200, { receivedBytes: next.length });
    } else if (finishMatch && request.method === 'POST') {
      const name = finishMatch[1] ?? '';
      const part = parts.get(name);
      if (!part || part.length !== 48016) {
        send(response, 422, { error: 'SIZE_MISMATCH' });
        return;
      }
      stored.set(name, part);
      parts.delete(name);
      send(response, 200, { name, installed: true });
    } else if (itemMatch && request.method === 'DELETE') {
      const name = itemMatch[1] ?? '';
      if (!stored.delete(name)) {
        send(response, 404, { error: 'NOT_FOUND' });
        return;
      }
      send(response, 200, {});
    } else if (url.pathname === '/api/device/time' && request.method === 'POST') {
      const { tz } = JSON.parse(body.toString('utf8')) as { tz: string };
      send(response, 200, { tz, synced: true, nowUnix: 1789400000 });
    } else {
      send(response, 404, { error: 'NOT_FOUND' });
    }
  });
  await new Promise<void>((resolve) => server.listen(0, '127.0.0.1', resolve));
  const port = (server.address() as AddressInfo).port;
  return {
    port,
    requests,
    stored,
    close: () => new Promise<void>((resolve) => server.close(() => resolve())),
  };
}

function samplePhoto(): Uint8Array {
  const levels = new Uint8Array(COUNT);
  for (let i = 0; i < COUNT; i++) levels[i] = i % 4;
  return encodePhotoFile(levels);
}

test('uploadPhoto sends the file in chunks at the offsets the device reports, then finishes', async () => {
  const device = await startPhotoDevice();
  try {
    const client = new DeviceClient({ host: '127.0.0.1', port: device.port, token: 't' });
    const file = samplePhoto();
    const progress: number[] = [];
    const result = await client.uploadPhoto('sunset', file, {
      onProgress: (p) => progress.push(p.sentBytes),
    });
    assert.deepEqual(result, { name: 'sunset', installed: true });
    assert.deepEqual(device.stored.get('sunset'), Buffer.from(file));
    const puts = device.requests.filter((r) => r.method === 'PUT');
    assert.deepEqual(puts.map((r) => r.offset), ['0', '16384', '32768']);
    assert.ok(puts.every((r) => r.path === '/api/photos/sunset/chunk'));
    assert.deepEqual(progress, [0, 16384, 32768, 48016]);
  } finally {
    await device.close();
  }
});

test('uploadPhoto resumes from the offset a 409 names', async () => {
  const device = await startPhotoDevice({ conflictOnce: true });
  try {
    const client = new DeviceClient({ host: '127.0.0.1', port: device.port, token: 't' });
    const file = samplePhoto();
    await client.uploadPhoto('resumed', file);
    assert.deepEqual(device.stored.get('resumed'), Buffer.from(file));
    assert.ok(device.requests.some((r) => r.offset === String(16384 - 1000)));
  } finally {
    await device.close();
  }
});

test('uploadPhoto refuses a bad name or a bad file before sending anything', async () => {
  const device = await startPhotoDevice();
  try {
    const client = new DeviceClient({ host: '127.0.0.1', port: device.port, token: 't' });
    await assert.rejects(() => client.uploadPhoto('Bad Name', samplePhoto()), DeviceError);
    await assert.rejects(() => client.uploadPhoto('ok', new Uint8Array(10)), /photo file/u);
    assert.equal(device.requests.length, 0);
  } finally {
    await device.close();
  }
});

test('listPhotos, deletePhoto and setTimeZone use their endpoints', async () => {
  const device = await startPhotoDevice();
  try {
    const client = new DeviceClient({ host: '127.0.0.1', port: device.port, token: 't' });
    await client.uploadPhoto('b', samplePhoto());
    await client.uploadPhoto('a', samplePhoto());
    const listing = await client.listPhotos();
    assert.deepEqual(listing.items.map((item) => item.name), ['a', 'b']);

    await client.deletePhoto('a');
    assert.deepEqual([...device.stored.keys()], ['b']);
    await assert.rejects(() => client.deletePhoto('a'), (error: unknown) =>
      error instanceof DeviceError && error.code === 'NOT_FOUND');

    const time = await client.setTimeZone('LOC-5:30');
    assert.deepEqual(time, { tz: 'LOC-5:30', synced: true, nowUnix: 1789400000 });
    const post = device.requests.find((r) => r.path === '/api/device/time');
    assert.deepEqual(JSON.parse(post?.body ?? '{}'), { tz: 'LOC-5:30' });
  } finally {
    await device.close();
  }
});
