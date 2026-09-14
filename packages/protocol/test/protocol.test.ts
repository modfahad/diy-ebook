import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  ERROR_STATUS,
  LIBRARY_ITEM_PREFIX,
  PATHS,
  PROTOCOL_VERSION,
  STATUS,
  UPLOAD_CHUNK_SUFFIX,
  UPLOAD_FINISH_SUFFIX,
  UPLOAD_PATH_PREFIX,
  bearer,
  type ErrorCode,
} from '../src/index.js';

/** dist/test/ -> package root. */
function packageRoot(): string {
  return join(dirname(fileURLToPath(import.meta.url)), '..', '..');
}

test('paths compose from the prefixes the firmware matches on', () => {
  // The device has no router: it matches these prefixes by hand in
  // http_server.cpp. If a path stops being "prefix + id + suffix", that
  // matching breaks silently, so the shape is asserted rather than assumed.
  const session = 'abcdef0123456789abcdef0123456789';
  assert.equal(PATHS.uploadChunk(session), `${UPLOAD_PATH_PREFIX}${session}${UPLOAD_CHUNK_SUFFIX}`);
  assert.equal(PATHS.uploadFinish(session), `${UPLOAD_PATH_PREFIX}${session}${UPLOAD_FINISH_SUFFIX}`);
  assert.equal(PATHS.uploadAbort(session), `${UPLOAD_PATH_PREFIX}${session}`);
  assert.equal(PATHS.libraryItem(session), `${LIBRARY_ITEM_PREFIX}${session}`);

  // The upload prefix must sit under the library prefix without colliding
  // with a content id: "/api/library/upload" has to be routed before
  // "/api/library/:id" or an upload looks like an item lookup.
  assert.ok(PATHS.uploadBegin.startsWith(PATHS.library));
  assert.ok(UPLOAD_PATH_PREFIX.startsWith(LIBRARY_ITEM_PREFIX));
});

test('every error code maps to a status', () => {
  const codes: ErrorCode[] = [
    'BAD_REQUEST',
    'UNAUTHORIZED',
    'NOT_FOUND',
    'OFFSET_MISMATCH',
    'CHUNK_TOO_LARGE',
    'SIZE_MISMATCH',
    'VERIFY_FAILED',
    'PACKAGE_REJECTED',
    'NO_SPACE',
    'NO_SESSION',
    'STORAGE_ERROR',
    'NOT_IN_TRANSFER_MODE',
  ];
  for (const code of codes) {
    assert.equal(typeof ERROR_STATUS[code], 'number', `${code} has no status`);
  }
  assert.equal(Object.keys(ERROR_STATUS).length, codes.length);
});

test('the statuses that drive client behaviour are the documented ones', () => {
  // These four are not cosmetic: the client branches on each of them.
  assert.equal(ERROR_STATUS.OFFSET_MISMATCH, STATUS.conflict);
  assert.equal(ERROR_STATUS.CHUNK_TOO_LARGE, STATUS.payloadTooLarge);
  assert.equal(ERROR_STATUS.VERIFY_FAILED, STATUS.unprocessable);
  assert.equal(ERROR_STATUS.NO_SPACE, STATUS.insufficientStorage);
});

test('bearer builds the header value the firmware parses', () => {
  // net::CheckBearer looks for exactly "Bearer " with one space.
  assert.equal(bearer('abc'), 'Bearer abc');
});

test('the committed constants fixture matches the current protocol', () => {
  // The same trick the QPK golden fixture plays: this file is generated from
  // these constants and also compiled into the firmware test suite, so a
  // stale fixture means the two mirrors have drifted.
  const path = join(packageRoot(), 'fixtures', 'protocol-constants.json');
  const fixture = JSON.parse(readFileSync(path, 'utf8')) as {
    protocolVersion: number;
    paths: Record<string, string>;
    status: Record<string, number>;
    errors: Array<{ code: string; status: number }>;
  };

  assert.equal(fixture.protocolVersion, PROTOCOL_VERSION);
  assert.equal(fixture.paths['deviceInfo'], PATHS.deviceInfo);
  assert.equal(fixture.paths['uploadBegin'], PATHS.uploadBegin);
  assert.equal(fixture.paths['uploadPrefix'], UPLOAD_PATH_PREFIX);
  assert.equal(fixture.paths['uploadChunkSuffix'], UPLOAD_CHUNK_SUFFIX);
  assert.equal(fixture.status['conflict'], STATUS.conflict);

  const fromFixture = Object.fromEntries(fixture.errors.map((e) => [e.code, e.status]));
  assert.deepEqual(
    fromFixture,
    ERROR_STATUS,
    'fixtures/protocol-constants.json is stale -- run `npm run fixture` and commit both artifacts',
  );
});

test('the generated C++ header agrees with the constants', () => {
  // Cheap textual check rather than a parse: if a constant is edited on the
  // TypeScript side and the header is not regenerated, the value below is
  // simply absent from the header.
  const header = readFileSync(
    join(packageRoot(), '..', '..', 'firmware', 'test', 'test_net', 'transcript_fixture.h'),
    'utf8',
  );
  assert.ok(header.includes(`kProtocolVersion = ${PROTOCOL_VERSION}`));
  assert.ok(header.includes(`"${PATHS.deviceInfo}"`));
  assert.ok(header.includes(`"${UPLOAD_PATH_PREFIX}"`));
  assert.ok(header.includes(`"OFFSET_MISMATCH"`));
});
