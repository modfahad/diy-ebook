# Device CLI

**Status: implemented — Phase 1, Milestone 4.**

Talks to a device over the [protocol](../../docs/protocol.md): identity,
status, library listing, resumable upload, delete, abort.

```bash
npm install --prefix desktop/device-client
npm run build --prefix desktop/device-client

node tools/device-cli/device.mjs info --host 192.168.1.42
node tools/device-cli/device.mjs upload quran.qpk --host 192.168.1.42 --token "$QR_DEVICE_TOKEN"
```

| Option | Effect |
|---|---|
| `--host`, `--port` | device address; also `QR_DEVICE_HOST` / `QR_DEVICE_PORT` |
| `--token` | pairing token from provisioning; also `QR_DEVICE_TOKEN` |
| `--json` | machine-readable output |

The device only serves this API **while it is in transfer mode**. Wi-Fi is
on-demand by design (see [architecture.md §4.3](../../docs/architecture.md)),
so put the device into transfer mode before running anything but `info`.

`abort <contentId>` takes a **content id**, not an opaque session id: on this
device the session id *is* the content id, which is what makes a session
survive a reset. `node tools/package-inspector/inspect.mjs <file.qpk>` prints
the content id of a local package.

`upload` resumes automatically: it asks the device how many bytes it already
holds and continues from there, and it rewinds to whatever offset the device
names if the two disagree. An upload interrupted by a device reset, a dropped
association, or a closed laptop lid is resumed by re-running the same command.

Exit codes: `0` success, `1` the device refused or the transfer failed, `2`
usage or the client is not built.

**Not verified against hardware.** No board has run the firmware side of this
protocol; see [development.md §10](../../docs/development.md).
