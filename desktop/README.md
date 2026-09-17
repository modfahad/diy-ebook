# Desktop application

| Path | Status |
|---|---|
| [`converter/`](converter/README.md) | **implemented** -- Milestone 3: the conversion pipeline |
| [`device-client/`](device-client/README.md) | **implemented** -- Milestone 4: discovery, library management, resumable upload |
| [`app-bridge/`](app-bridge/README.md) | **implemented** -- Milestone 5: the app's link to both libraries |
| `src/` | **implemented** -- Milestone 5: React UI (Dashboard, Library, Device, Converter) |
| `src-tauri/` | **implemented** -- Milestone 5: Rust backend, package scanning and bridge plumbing |

`converter` and `device-client` are plain TypeScript with **no Tauri or React
dependency**, unchanged by this milestone: the CLIs in [`../tools/`](../tools/)
and the app now drive the same code. Coupling them to Tauri would have made
them untestable outside the app.

## Running it

Both libraries must be built first -- the app drives them as a Node
subprocess, so a stale `dist/` is a real failure mode. The Dashboard tab checks
this on open and says what to run if something is missing.

```bash
npm install --prefix packages/qpk-format
npm install --prefix desktop/converter     && npm run build --prefix desktop/converter
npm install --prefix desktop/device-client && npm run build --prefix desktop/device-client
```

```bash
npm install --prefix desktop
npm run tauri dev --prefix desktop
```

## How it fits together

```
  React UI (src/)
      |
      |  invoke("scan_library")            invoke("bridge_call")
      v                                        v
  Rust (src-tauri/)                        Rust (src-tauri/src/bridge.rs)
    library.rs: an independent                 |  spawns one node process
    QPK1 header reader                         v
                                          app-bridge/run.mjs
                                               |
                                               +-- converter      (convert, validate, preview)
                                               +-- device-client  (info, status, list, upload, delete)
```

Two paths, chosen per job rather than for symmetry. Browsing a folder is
filesystem work and stays in Rust. Everything that means *running the
conversion pipeline or the device protocol* goes to the libraries that already
implement and test it, over the bridge -- see
[`app-bridge/README.md`](app-bridge/README.md) for why that is a Node process
and not a port to the webview.

## The four tabs

- **Dashboard** -- whether Node and both libraries are actually available, a
  summary of the chosen library folder and device, and the honest status of
  each half of the app.
- **Library** -- scans a folder laid out like the device's card
  (`LIBRARY/{QURAN,BOOKS,TRANSLATIONS,TAFSIR}/*.qpk`), reads each package's
  header and metadata, validates any row on demand, and hands one to the
  Device tab's uploader. "Choose books..." adds many at once: PDF, EPUB and
  TXT are converted with the Converter's defaults and `.qpk` files are copied,
  each into the subfolder for its type. Ticked rows are sent to the device in
  one go, one after another.
- **Device** -- identity, status, installed content, resumable upload with
  progress, delete, and abandoning an interrupted upload.
- **Converter** -- the full pipeline with every option the CLI exposes:
  validate and preview first, write second.

## Known limitations

- **The app needs Node on PATH and this checkout on disk.** The bridge is a
  script in the repository, not a bundled resource, and the two libraries are
  loaded from their built `dist/`. Shipping an installer means bundling Node
  and both builds as Tauri resources; that is separate work and is recorded in
  [docs/pending.md](../docs/pending.md). Set `QURAN_DEVICE_REPO` if the app
  cannot find the checkout on its own.
- **Paths are typed, not picked.** No native file dialog: that means
  `tauri-plugin-dialog`, and the choice to add it was left rather than made
  silently. The Library tab's "Send to device" avoids the retyping that
  matters most.
- **Everything on the Device tab except Identify is unproven against
  hardware.** Not because it is unfinished -- `device-client` is host-tested
  and the firmware side is host-tested including power loss at every write --
  but because every endpoint but `GET /api/device/info` requires a pairing
  token, and BLE provisioning has never delivered one. See
  [docs/pending.md](../docs/pending.md) section 3.
- **The pairing token is stored in the webview's `localStorage`.** A real
  trust store (`device-client`'s `TrustStore` shape, keyed by device id, with
  OS keychain backing) belongs on the Rust side and is not this milestone.

## Documentation

- Conversion pipeline design: [docs/conversion.md](../docs/conversion.md)
- Device protocol: [docs/protocol.md](../docs/protocol.md)
- Provisioning: [docs/provisioning.md](../docs/provisioning.md)
