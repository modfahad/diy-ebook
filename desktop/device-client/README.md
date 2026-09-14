# @quran-device/device-client

**Status: implemented — Phase 1, Milestone 4.**

The desktop side of the [device protocol](../../docs/protocol.md): discovery,
device info and status, library listing and deletion, and resumable package
upload. A plain TypeScript library with no Tauri or React dependency — the CLI
in [`tools/device-cli`](../../tools/device-cli/README.md) drives it today and
the UI will drive the same code in Milestone 5.

```bash
npm install --prefix desktop/device-client
npm test    --prefix desktop/device-client
```

```ts
import { DeviceClient } from '@quran-device/device-client';

const client = new DeviceClient({ host: '192.168.1.42', token });
await client.uploadPackage(bytes, {
  onProgress: ({ sentBytes, totalBytes, resyncs }) => report(sentBytes, totalBytes, resyncs),
});
```

## The rule that shapes `uploadPackage`

**The device's disk is the authority on how much it has, not this client.**

The client never assumes it knows the resume point. It takes the byte count the
device reports when the session opens, and on a `409 OFFSET_MISMATCH` it takes
the offset the device names and continues from there. That is what makes a
mid-transfer device reset, a dropped association, or a lost response
recoverable rather than fatal — and it is the client-side half of the contract
the firmware's `UploadManager` implements.

Two more deliberate behaviours:

- **The package is validated locally first.** Uploading something the device is
  certain to reject wastes minutes of radio time, so `readPackage()` runs
  before the first request goes out.
- **A refusal is not retried.** `NO_SPACE`, `CHUNK_TOO_LARGE`, `VERIFY_FAILED`
  are answers, not transport hiccups. Only genuine transport failures are
  retried, and only a bounded number of times.

## Discovery

`ManualDiscovery` (type an address) is the only implementation that ships.
mDNS is the right mechanism and the device advertises `_quranreader._tcp`,
but nothing about it can be verified without hardware on a real network, so it
sits behind the `Discovery` interface for a real browser to slot into. A manual
path is needed regardless: guest networks and VLANs routinely block multicast.

`TrustStore` holds the devices the desktop has paired with (spec §19).
Persistence is the app's job; the merge rule lives here, and it is that a
device seen at a new address **keeps its existing token** — moving between
networks must not silently unpair it.

## Testing

The tests run against a scripted in-process HTTP server. That mock is
deliberately *not* a second implementation of the upload state machine — the
firmware owns that, and re-implementing it here would only let the two drift
while both suites passed. It replays whatever each test stages: a resume, a
409, a dropped response, a refusal.

The real cross-language check is
[`packages/protocol`](../../packages/protocol/README.md)'s constants fixture,
which the firmware test suite compiles and asserts against.

**Not verified against hardware.** No board has served this protocol.
