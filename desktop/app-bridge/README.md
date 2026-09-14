# app-bridge

The desktop app's link to the two TypeScript libraries.

```
Tauri (Rust)  --(request JSON on stdin)-->  run.mjs  -->  @quran-device/converter
              <--(JSON lines on stdout)---                @quran-device/device-client
```

One short-lived `node` process per operation. No sidecar to supervise, no port
to allocate, no state carried between calls.

## Why a Node process at all

The UI is a webview and could in principle run TypeScript itself. The converter
cannot: `yauzl` (EPUB) is Node-only, `pdfjs-dist` needs its legacy build, and
content ids come from a synchronous `node:crypto` hash whose exact bytes the
package format, the firmware and the golden fixture all depend on. Porting that
is rework, and Milestone 5's desktop UI is meant to be assembly.

Since Node is in the process tree anyway, `device-client` rides the same
mechanism rather than earning a second one (a Rust HTTP proxy) purely to get
around the webview's CORS rules.

## Protocol

Request: one JSON object, on stdin (or as `argv[2]`).

Response: JSON lines on stdout, nothing else -- `console.log` is redirected to
stderr so a chatty library cannot corrupt the stream.

```
{"type":"progress", "stage":"parsing", ...}     zero or more
{"type":"result","value":X}                     exactly one, then exit 0
{"type":"error","code":C,"message":M}           on failure, then exit 1
```

`code` is the bridge's own vocabulary (`BAD_REQUEST`, `LIBRARY_NOT_BUILT`,
`VALIDATION_FAILED`, `NO_PAGES`, `DEVICE_UNREACHABLE`) or, for a device call
that reached the device, the protocol's own error code passed straight through
(`UNAUTHORIZED`, `NOT_IN_TRANSFER_MODE`, `NO_SPACE`, ...).

## Commands

| Command | What it does |
|---|---|
| `ping` | Node version, and whether each library is built |
| `convert` | parse, structure, validate, preview, then write (`dryRun` skips the write) |
| `inspect` | validate a `.qpk` already on disk |
| `preview` | read one page back out of a package |
| `device.info` | identity and capabilities -- the only endpoint that works unpaired |
| `device.status` | storage, battery, transfer mode, open sessions |
| `device.list` | installed content |
| `device.upload` | resumable install, streaming progress |
| `device.delete` | remove installed content |
| `device.abort` | abandon an interrupted upload |

Full request fields are in [`src/commands.mjs`](src/commands.mjs); the typed
wrappers the UI actually calls are in [`../src/bridge.ts`](../src/bridge.ts).

## Running it by hand

```bash
echo '{"command":"ping"}' | node desktop/app-bridge/run.mjs
```

```bash
node desktop/app-bridge/run.mjs '{"command":"convert","input":"book.pdf","dryRun":true}'
```

## Tests

```bash
npm test --prefix desktop/app-bridge
```

22 tests. The dispatch table runs against the *real* converter and
device-client builds -- only the device transport is faked, because there is no
device -- and `run.mjs` is additionally exercised as a spawned process, which is
the only way to check the part the Rust side depends on: that stdout carries
JSON lines and nothing else.

Requires both libraries built:

```bash
npm install --prefix desktop/converter && npm run build --prefix desktop/converter
npm install --prefix desktop/device-client && npm run build --prefix desktop/device-client
```
