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

## Installers

Every push to `main` builds Windows (`.msi`, NSIS `.exe`) and macOS (`.dmg`)
installers in GitHub Actions (`.github/workflows/desktop.yml`) and attaches
them to the run as artifacts. The same run tests the libraries, the bridge and
the Rust side (`cargo test --lib`). An installed app carries its own bridge
bundle and Node (`scripts/prepare-standalone.mjs`), so it needs neither this
checkout nor a Node install.

## The tabs

- **Dashboard** -- whether Node and both libraries are actually available, a
  summary of the chosen library folder and device, and the honest status of
  each half of the app.
- **Library** -- scans a folder laid out like the device's card
  (`LIBRARY/{QURAN,BOOKS,TRANSLATIONS,TAFSIR}/*.qpk`), reads each package's
  header and metadata, validates any row on demand, and hands one to the
  Device tab's uploader. Many books go in and out at once:
  - **Add books.** "Choose books..." takes any number of files. Each one's
    title, author and language are read from its own metadata (a PDF's Info
    dictionary or XMP and catalog `/Lang`, an EPUB's OPF -- never its pages)
    and filled in, the title falling back to the file name. Every field stays
    editable, a cover can be chosen per book, and "For every book" sets an
    author and language for all of them. "Convert & add" then converts PDF,
    EPUB and TXT one after another and copies `.qpk` files, each into the
    library subfolder for its type; a failed book can be fixed and run again.
  - **Send many.** Ticked rows are sent to the device one after another, never
    in parallel (the device keys an upload session by content id). A failure
    that would hit every package -- unpaired, not in transfer mode, card full,
    unreachable -- stops the batch once.
- **Device** -- identity, status, installed content, resumable upload with
  progress, delete, abandoning an interrupted upload, and firmware over Wi-Fi.
- **Photos** -- home-screen photos and the device's time zone.
- **Converter** -- the full pipeline with every option the CLI exposes, for
  one book at a time: validate and preview first, write second.

## Known limitations

- **Run from a checkout, the app needs Node on PATH.** Only the installers
  carry their own (above). Set `QURAN_DEVICE_REPO` if a development build
  cannot find the checkout on its own.
- **The installers are not code-signed.** Windows SmartScreen asks for
  confirmation, and a PC with Smart App Control on blocks them -- and also
  blocks building the app locally, since the Rust compiler, Tauri's CLI and
  Vite's native modules are unsigned too. Build in CI instead. On macOS,
  open the app the first time with right-click, Open.
- **Adding the same book twice writes a second file.** A `.qpk` whose content
  id and version are already in the folder is skipped, but converting a
  document again with the same title writes `name-2.qpk` beside the first.
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
