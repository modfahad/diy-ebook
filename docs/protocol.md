# Desktop ↔ device protocol

**Implemented — Milestone 4.** The device serves this API from
`firmware/drivers/net/http_server.cpp`; the desktop-side types live in
[packages/protocol/src/index.ts](../packages/protocol/src/index.ts), which
`firmware/include/net/protocol.h` mirrors in C++. Those two files, plus this
document, are the actual contract — treat this page as a guide to them, not a
substitute for reading them.

A constants fixture now exists and is checked:
`packages/protocol/scripts/build-transcript.ts` generates
`packages/protocol/fixtures/protocol-constants.json` and
`firmware/test/test_net/transcript_fixture.h` from `index.ts`'s paths,
headers, status codes, and `ERROR_STATUS` mapping. `test_net`'s "Transcript
fixture" tests check the hand-maintained `protocol.h` against the generated
header — a failure there means one side was edited without the other.
Regenerate with `npm run fixture --prefix packages/protocol` and commit the
result, same discipline as the QPK golden fixture in
[qpk-format.md](qpk-format.md).

This is deliberately narrower than the `TranscriptStep`/`TranscriptScenario`
shapes `index.ts` also declares: it covers the constants, not JSON response
*bodies* (`DeviceInfo`, `DeviceStatus`, ...). Checking response bodies the
same way would need `http_server.cpp`'s response-building pulled out into
pure, host-testable functions first, which has not been done — a real gap,
not a stale comment.

## Transport

- HTTP/1.1 over Wi-Fi, port `net::kDefaultPort` / `DEFAULT_PORT` (8080).
- The device advertises `_quranreader._tcp` over mDNS while serving this API.
- In table-clock mode (`app::kTableClockMode`, the default since 2026-09-14)
  Wi-Fi and this API are up whenever the device is, so the desktop app can
  send photos and the time zone at any time. Otherwise:
- Wi-Fi is on-demand: the radio only comes up in response to a user action.
  Until Milestone 5's library browser/menu exists, that action is a
  long-press on the encoder switch (`main.cpp`'s `ToggleTransferMode`) — a
  stopgap trigger, not the intended UI. See [architecture.md](architecture.md)
  §4 and [development.md §10](development.md).

## Auth

`Authorization: Bearer <pairing token>`, constant-time compared
(`net::CheckBearer`, `net/auth.h`). Required on every endpoint except
`GET /api/device/info` — that one has to work before pairing has happened and
exposes nothing sensitive. An unpaired device (no token persisted yet) never
authenticates a request, including to the exempt endpoint's sibling
authenticated routes.

## Paths

| Method | Path | Auth | Body |
|---|---|---|---|
| GET | `/api/device/info` | no | — → `DeviceInfo` |
| GET | `/api/device/status` | yes | — → `DeviceStatus` |
| GET | `/api/library` | yes | — → `LibraryListing` |
| GET | `/api/library/:contentId` | yes | — → `LibraryItem` |
| DELETE | `/api/library/:contentId` | yes | — → `{}` |
| POST | `/api/library/upload` | yes | `UploadBeginRequest` → `UploadBeginResponse` |
| PUT | `/api/library/upload/:sessionId/chunk` | yes | raw bytes, see below → `UploadChunkResponse` |
| POST | `/api/library/upload/:sessionId/finish` | yes | — → `UploadFinishResponse` |
| DELETE | `/api/library/upload/:sessionId` | yes | — → `{}` |
| POST | `/api/device/backup` | yes | **501 Not Implemented** |
| POST | `/api/device/restore` | yes | **501 Not Implemented** |
| POST | `/api/device/time` | yes | `TimeZoneRequest` (POSIX TZ) → `TimeZoneResponse` |
| GET | `/api/photos` | yes | — → `PhotoListing` |
| PUT | `/api/photos/:name/chunk` | yes | raw bytes, same framing as the package chunk → `PhotoChunkResponse` |
| POST | `/api/photos/:name/finish` | yes | — → `PhotoFinishResponse` |
| DELETE | `/api/photos/:name` | yes | — → `{}` |
| POST | `/api/firmware/begin` | yes | `FirmwareBeginRequest` (`size`, `md5`) → `FirmwareBeginResponse` |
| PUT | `/api/firmware/chunk?offset=N` | yes | raw bytes, same framing as the package chunk, strictly in order → `{ receivedBytes }` |
| POST | `/api/firmware/finish` | yes | — → `FirmwareFinishResponse`; the device checks the MD5, switches its boot slot and restarts |
| POST | `/api/firmware/abort` | yes | — → `{}` |

Photos are the home screen's (architecture.md §4.4). `name` is
`[a-z0-9_-]{1,32}`; the body is a finished 48,016-byte `QPH1` file
(`packages/protocol` `PHOTO_*`, `firmware/include/net/photo_store.h`). Chunks
append to `<name>.part` at `X-Qr-Offset`, which must equal the bytes already
there (`OFFSET_MISMATCH` with `expectedOffset` otherwise) -- except offset 0,
which starts that photo over. `finish` checks the size and header before
renaming the part over any photo of the same name.

`sessionId` is the upload's content id (hex) — see `upload_manager.h`'s
comment on why the session id and the content id are deliberately the same
value. Errors are `{"error": ErrorCode, "expectedOffset"?: number}`, status
mapped by `net::ErrorStatus` / `STATUS`.

Backup/restore are declared in `index.ts` (`BackupResponse`, `RestoreRequest`)
but have no implementation behind them: packing `/USER` and `/DEVICE` into a
transferable blob is its own untested design problem, not a byproduct of
wiring up what already existed. The routes exist and reject cleanly rather
than 404ing, so a client can detect "not built yet" instead of "wrong URL".

## The chunk-upload body is binary-unsafe as a plain PUT — read this before touching it

The obvious design — `PUT` the chunk bytes raw as the body, `Content-Length`
set normally — does not work on this device. The Arduino `WebServer` (whose
source is worth actually reading before changing this endpoint:
`WebServer/src/Parsing.cpp`) buffers a non-multipart body via
`arg.value = String(plainBuf)`, and `String(const char*)` stops at the first
`0x00` byte. A QPK package is arbitrary binary data and will contain zero
bytes as a matter of course, so a normal `Content-Length` PUT silently
truncates.

The fix in `http_server.cpp` (`ReadChunkedBody`): the chunk PUT is sent with
**`Transfer-Encoding: chunked` and no `Content-Length` header.** The Arduino
parser only auto-buffers a body when `Content-Length` is present (see
`Parsing.cpp`'s `_clientContentLength`-gated branch) — with it absent, the
body is left untouched on the socket, and the handler decodes the HTTP
chunked-transfer-coding itself, straight into a fixed buffer, with no
`String` anywhere in the path. This is standard HTTP; any client library that
can stream a request body without knowing its length upfront produces exactly
this framing.

The byte offset within the upload still goes in a header, since the chunked
transfer-coding's own per-fragment sizes are a wire-level detail, not the
application-level resume offset:

```
PUT /api/library/upload/<sessionId>/chunk HTTP/1.1
Transfer-Encoding: chunked
X-Qr-Offset: <decimal byte offset>

<chunked-encoded body>
```

`net::kHeaderOffset` / `HEADER_OFFSET` = `X-Qr-Offset`. `UploadBeginRequest`
and `finish`'s JSON bodies are plain text and have no such issue — only the
chunk endpoint needed this.

The desktop client sends this framing: a `ReadableStream` body with
`duplex: 'half'`, asserted on the wire by `client.test.ts`.

### Base64 chunk bodies, for clients that cannot stream

React Native's `fetch` (the Android app, [android.md](android.md)) cannot
stream a request body, so it cannot produce the framing above. Every chunk
endpoint -- packages, photos and firmware -- also accepts the chunk as base64
text with a normal `Content-Length`:

```
PUT /api/library/upload/<sessionId>/chunk HTTP/1.1
Content-Type: text/plain
Content-Length: <length of the text>
X-Qr-Offset: <decimal byte offset>
X-Qr-Body: base64

<the chunk's bytes, standard base64 with padding>
```

Text survives `String(plainBuf)` where binary does not, so the WebServer's
own buffering is fine here; `HttpServer::ReadChunkBody` decodes
`arg("plain")` into the chunk buffer (`net::DecodeBase64`). The decoded
length is what counts against `maxChunkBytes`: too long answers 413
`CHUNK_TOO_LARGE`, bad characters 400 `BAD_REQUEST`. Costs a third more bytes
on the air and the text is held in RAM, so desktop clients keep streaming.
`net::kHeaderBody` / `HEADER_BODY`; `DeviceClient`'s
`chunkEncoding: 'base64'` selects it.

## Design constraints (from the product spec, unchanged since this was a placeholder)

- §17 uploads must be resumable; a Quran package will not always transfer in
  one request. Chunked upload keyed by content id + version + size +
  checksum, with the `.part` file's size on disk as the sole resume point.
- §17 installation must be atomic — `.part` → verify → rename, specified in
  [qpk-format.md](qpk-format.md) §12.
- §19 no unauthenticated destructive endpoints; the Wi-Fi password is never
  exposed through this API (only through BLE provisioning writes, which are
  one-way — see [provisioning.md](provisioning.md)).
