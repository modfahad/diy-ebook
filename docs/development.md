# Development

Covers Phase 1, Milestones 1-4: the firmware bring-up layer, the QPK1 content
package (device parser, desktop reader/writer, inspector), the conversion
pipeline, and the desktop-to-device transport. The Tauri UI, the on-device
library browser and the AI server are not implemented yet — see
[architecture.md](architecture.md) for what is deferred.

---

## 1. Prerequisites

| Tool | Why | Notes |
|---|---|---|
| Python 3.8+ | PlatformIO runs on it | |
| PlatformIO Core | firmware build/flash/test | `pip install platformio` |
| A host C++ compiler | the `native` unit-test environment | g++, clang++ or MSVC on `PATH` |
| Node.js 20+ | the TypeScript packages, the CLIs, and the desktop app at runtime | `node --test` is used directly; no test runner dependency |
| Rust (stable) + Tauri prerequisites | the desktop application | only for `desktop/`; see [tauri.app/start/prerequisites](https://tauri.app/start/prerequisites/) for the platform WebView package |
| CH34x USB-serial driver | the board's USB port is a UART bridge | Windows/macOS need the vendor driver; most Linux kernels have it |

The first `pio run` downloads the Xtensa toolchain and the Arduino-ESP32 core
(several hundred MB). Later builds are fast.

## 2. Build

```bash
pio run -d firmware -e crowpanel_579
```

## 3. Flash

Connect the board over USB-C, then:

```bash
pio run -d firmware -e crowpanel_579 -t upload
```

If the port is not detected, add it explicitly:

```bash
pio run -d firmware -e crowpanel_579 -t upload --upload-port COM5
```

If upload fails to start, force download mode: **hold BOOT, tap RESET, release
BOOT**, then upload again.

## 4. Serial monitor

```bash
pio device monitor -d firmware -b 115200
```

Expected on a healthy cold boot:

```
[boot] Quran Reader fw=0.1.0-m1 boot=1 wake=cold boot gpio=-1
[epd] init=1 psram_fb=1 psram_total=8388608
[sd] SDHC 15193 MB, layout=1 write=1
```

## 5. Unit tests

```bash
pio test -d firmware -e native
```

PlatformIO's `native` platform hardcodes gcc/g++. On Windows with only the
Visual Studio Build Tools installed, use the standalone runner instead — it
builds and runs exactly the same tests with `cl.exe` (open a Visual Studio
developer shell, or call `vcvars64.bat` first):

```bash
python firmware/scripts/run_host_tests.py
```

It also works with g++/clang++ and is the fallback if PlatformIO is not set up.

These cover the hardware-independent logic: switch debouncing, click vs
long-press, quadrature decoding (including detent jitter), the pulse-decoder
fallback, rotary coalescing, the idle/sleep policy including `millis()`
wraparound, the EXT1 wake-mask constraints, and the canvas pixel mapping
(rotation, clipping, and the generic two-controller seam offset), plus
worst-case layout-fit checks for every screen on the 800x480 panel.

Since Milestone 2 the suite also covers the QPK1 parser: one case per
validation rule in [qpk-format.md](qpk-format.md) §11 (bad magic, wrong
version, bad header size, header and section checksums, size mismatch,
misaligned / out-of-range / overlapping sections, non-ascending ids, wrong
record stride, `count * record_size` overflow, missing required sections,
inconsistent indexes), plus the direct-access paths and the cross-language
golden fixture.

Since Milestone 4 it also covers the transport layer: the JSON codec, the
constant-time pairing-token check, `library_index.bin`, the resumable upload
and atomic-install state machine, and the BLE provisioning state machine.
Development rule 8 — power loss during package installation — is exercised by
cutting power at **every** write of a full upload-and-install and asserting,
each time, that no partially transferred package is ever visible in the library
index and that the transfer still recovers on the next boot.

**Not covered, on purpose:** the HTTP socket handling, the BLE GATT wiring and
the Wi-Fi radio. Those need hardware; see §10. SD removal mid-write is also
still untested — it needs a card to pull out.

## 5a. The desktop QPK package

`packages/qpk-format` is the TypeScript reader/writer. It needs Node 20+.

```bash
npm install --prefix packages/qpk-format
npm test    --prefix packages/qpk-format
```

Inspect a package:

```bash
node tools/package-inspector/inspect.mjs packages/qpk-format/fixtures/mini-quran.qpk
```

After **any** change to the format, regenerate the golden fixture and commit
both artifacts it writes:

```bash
npm run fixture --prefix packages/qpk-format
```

Then re-run the firmware host tests. `test_golden_package_is_byte_identical_to_the_cpp_builder`
is what catches the two writers drifting apart; if it fails, one side was
changed and the other was not.

## 5b. The conversion pipeline

`desktop/converter` turns documents into packages. Node 20+.

```bash
npm install --prefix desktop/converter
npm test    --prefix desktop/converter
```

Convert something:

```bash
npm run build --prefix desktop/converter
node tools/pdf-converter/convert.mjs book.pdf --title "A Book" --preview 1
node tools/quran-validator/validate.mjs desktop/converter/examples/quran-source.example.json
```

The converter writes nothing unless the package validates, under both the
desktop profile (every checksum, full index sweep) and the device's. Design
notes and the Quran import schema: [conversion.md](conversion.md).

Its tests generate their own PDF and EPUB fixtures rather than committing
binaries, so a failing case can be read and reasoned about.

## 5c. The transport packages

`packages/protocol` holds the wire types and constants both sides mirror;
`desktop/device-client` is the desktop half of the protocol.

```bash
npm install --prefix packages/protocol
npm test    --prefix packages/protocol

npm install --prefix desktop/device-client
npm test    --prefix desktop/device-client
```

Talk to a device (which must be in transfer mode — see
[architecture.md §4.3](architecture.md)):

```bash
npm run build --prefix desktop/device-client
node tools/device-cli/device.mjs info --host 192.168.1.42
node tools/device-cli/device.mjs upload quran.qpk --host 192.168.1.42 --token "$QR_DEVICE_TOKEN"
```

After changing anything in `packages/protocol/src/index.ts`, regenerate the
constants fixture and commit both artifacts it writes:

```bash
npm run fixture --prefix packages/protocol
```

Then re-run the firmware host tests. `test_net` compiles the generated
`transcript_fixture.h` and asserts `firmware/include/net/protocol.h` against
it; that check is what catches the C++ and TypeScript mirrors drifting apart.

The device-client tests run against a scripted in-process HTTP server. It is
deliberately not a second implementation of the upload state machine — the
firmware owns that — so a passing client suite says the client behaves, not
that the device does.

## 5d. Bring-up builds

The demo builds (`NABA_DEMO`, `READER_DEMO`, `GLYPH_ATLAS_DEMO`) and their
flash-embedded content (`app/naba_pages.h`, `app/embedded_note.h`,
`scripts/gen_embedded_note.py`) were **removed on 2026-09-14**: real packages
on the card -- the Quran, text books and PDF page-picture books -- now cover
everything they demonstrated. `bring-up-log.md` still describes them as they
were.

One panel check remains, opt-in through `PLATFORMIO_BUILD_FLAGS`:

```bash
# Panel smoke test: solid black, then solid white, held 3 s each
PLATFORMIO_BUILD_FLAGS="-DEPD_SMOKE_TEST=1" pio run -d firmware -e crowpanel_579 -t upload
```

`EPD_SMOKE_TEST` exists because "the screen is blank" is an unreliable
observation: a text layout seen through the shipping film, or glanced at
before the ~2 s refresh finishes, looks exactly like a panel that never drew.
A full black fill does not.

### The DEV_AUTOPROVISION build

A fourth opt-in flag, and the only one that touches credentials. It provisions
Wi-Fi and the pairing token from a gitignored header at boot, so a test device
comes up online and already paired without an SD-card shuffle or a BLE client.

```bash
cp firmware/include/app/dev_secrets.example.h firmware/include/app/dev_secrets.h
# fill in SSID, passphrase and a freshly generated token, then:
PLATFORMIO_BUILD_FLAGS="-DDEV_AUTOPROVISION=1" pio run -d firmware -e crowpanel_579 -t upload
```

`firmware/include/app/dev_secrets.h` is in `.gitignore` and must stay there.
**Never ship a build with this flag set** -- the pairing token is the bearer
credential for every destructive endpoint, and this puts it in the firmware
image. The product build was verified to contain neither the passphrase nor
the token.

It deliberately refuses to overwrite a device that is already provisioned and
holds a token, logging `already provisioned and has a token; leaving NVS
alone`. To make the compiled-in token win instead, erase NVS first:

```bash
pio run -d firmware -e crowpanel_579 -t erase
```

## 5e. The desktop application

The Tauri app is a UI over `desktop/converter` and `desktop/device-client`.
It does not reimplement either: it runs them as a one-shot `node` subprocess
per operation through `desktop/app-bridge`, so **both libraries must be built
before the app is useful**, and a stale `dist/` is a real failure mode rather
than a theoretical one. The app's Dashboard tab checks exactly this on open
and prints the commands to fix it.

```bash
npm install --prefix packages/qpk-format
npm install --prefix desktop/converter     && npm run build --prefix desktop/converter
npm install --prefix desktop/device-client && npm run build --prefix desktop/device-client
```

Then:

```bash
npm install --prefix desktop
npm run tauri dev --prefix desktop
```

Tests, all three layers:

```bash
npm test --prefix desktop/app-bridge                              # 22, Node
cargo test --lib --manifest-path desktop/src-tauri/Cargo.toml     # 13, Rust
npm run build --prefix desktop                                    # tsc + vite
```

`--lib` rather than plain `cargo test`: the `main.rs` test harness binary
contains no tests, and on this machine Windows Application Control
intermittently refuses to execute a freshly built test binary
(`os error 4551`) until it has finished evaluating it — re-running the command
succeeds. Skipping the empty harness avoids the noise; if the `--lib` binary
itself is blocked on the first run, run it again.

The bridge is also usable directly, which is the fastest way to check a
conversion or a device call without opening the app:

```bash
echo '{"command":"ping"}' | node desktop/app-bridge/run.mjs
node desktop/app-bridge/run.mjs '{"command":"convert","input":"book.pdf","dryRun":true}'
```

See [desktop/app-bridge/README.md](../desktop/app-bridge/README.md) for the
full command list and the protocol, and
[desktop/README.md](../desktop/README.md) for the app's known limitations —
chiefly that it expects Node on PATH and this checkout on disk, since nothing
is bundled yet.

## 5f. Building the Quran content

The full 114-surah corpus, from nothing to a staged card. Design decisions and
verification are in [quran-content.md](quran-content.md); this is the command
sequence.

```bash
# 1. fetch (228 API calls; resumable, safe to re-run)
python tools/arabic-pager/fetch_all_surahs.py

# 2. assemble one QuranSource covering surahs 1..114, plus a cleaned translation
python tools/arabic-pager/build_full_quran_source.py

# 3. text QURAN package + aligned TRANSLATION package
node --max-old-space-size=4096 desktop/converter/scripts/build-full-quran.mjs

# 4. shape every word through HarfBuzz into a glyph atlas
python tools/arabic-pager/shape_glyph_atlas.py   "fonts/KFGQPC Uthmanic Script HAFS Regular.otf"   tools/arabic-pager/data/all/all-verses.json   tools/arabic-pager/data/all/glyph-atlas.json

# 5. shaped QURAN package (separate: FLAG_SHAPED_TEXT_DATA redefines WORD_INDEX)
node --max-old-space-size=6144 desktop/converter/scripts/build-full-quran-shaped.mjs

# 6. readback check, by real surah number
node --max-old-space-size=4096 desktop/converter/scripts/verify-full-quran.mjs
```

`--max-old-space-size` is not superstition: the assembled source is 3.4 MB of
JSON expanding to 6236 ayahs and 82,011 words in memory, and the default heap
is not enough.

Page previews in An-Naba's layout, for any surah:

```bash
python tools/arabic-pager/make_render_translations.py   # once, after step 2
python tools/arabic-pager/render_surah.py 1
```

**Nothing here is committed.** The fetched text, the built packages and the
previews are all gitignored — translations are copyrighted, and the packages
are reproducible from these commands. Expect ~30 MB under
`tools/arabic-pager/data/all/` and ~10 MB under `sdcard-staging/` after a full
run.

### The invariants these scripts assert

They fail loudly rather than producing a subtly wrong package:

- **6236 ayahs**, the canonical count, checked in two separate processes.
- **Per-surah verse counts** against the independent `/chapters` endpoint.
- **82,011 words with no empty entries** — the API puts a leading space on the
  first ayah of 110 of the 114 surahs, and a naive split mints a phantom word
  for each, silently desynchronising the text and shaped packages.
- **Surah ids 1..114 in order**, which is what makes one package legal.
- **Word text sampled at seven positions** between the source and the atlas —
  equal counts are not enough, two different splits can agree on the total.

## 6. Physically testing the hardware

Flash, then work through this list. The self-test screen is the instrument.

**Results of the first real run are in [bring-up-log.md](bring-up-log.md)** — what held, what broke, and what is still untested.

```
+---------------------------------------------------------------+
|                       Quran Reader                            |
|                       Hardware Test                           |
| ------------------------------------------------------------- |
|   Rotary:  OK          Wake:     cold boot                    |
|   MENU:    OK          Boots:    1                            |
|   EXIT:    --          Card:     15193 MB                     |
|   OK btn:  OK          Enc pins: A=1 B=1                      |
|   SD:      OK          Quad:     3                            |
|   E-Ink:   OK          Pulse:    0                            |
|   PSRAM:   OK          Counter:  3  (r4)                      |
| ------------------------------------------------------------- |
|      MENU=refresh EXIT=sleep turn=count fw 0.1.0-m1           |
+---------------------------------------------------------------+
```

A row shows `--` until that input has been exercised, then `OK`. `SD`, `E-Ink`
and `PSRAM` are decided at boot and show `FAIL` if they failed.

| # | Test | Action | Pass criterion |
|---|---|---|---|
| 1 | E-Ink | power on | The screen draws. `E-Ink: OK`. |
| 2 | PSRAM | power on | `PSRAM: OK` and `psram_total=8388608` on serial. If it fails, switch `board_build.arduino.memory_type` to `qio_qspi`. |
| 3 | SD present | insert a FAT32 card, reset | `SD: OK` and a plausible `Card: NNNN MB`. |
| 4 | SD writable | after test 3, remove the card and read it on a PC | `/DEVICE/selftest.log` exists with one line per boot. |
| 5 | SD absent | remove the card, reset | `SD: FAIL`, `Card: no card / wiring`, and **the device still boots and renders**. |
| 6 | MENU | press MENU | `MENU: OK`, screen does a clean full refresh. |
| 7 | Encoder switch | press the wheel | `OK btn: OK`. |
| 8 | Long press | hold the wheel ~1 s | `OK btn: OK` (a long press is registered; see serial). |
| 9 | **Rotary direction** | turn clockwise one detent | `Counter` +1 and `Pulse` +1. If `Counter` moves by 2 or 4 per click, or not at all, see test 10. |
| 10 | **Rotary decoding mode** | ✅ **RESOLVED 2026-08-30** | It is **not** quadrature. One detent toggles exactly one of PRV/NEXT while the other holds: `A=0` steps back, `B=0` steps forward. The pulse decoder is now the only one built; see [bring-up-log.md §2.1](bring-up-log.md). |
| 11 | Debounce | turn quickly back and forth, then stop | The counter must land on a stable value with no phantom steps. |
| 12 | Coalescing | spin the wheel fast through ~15 detents | The panel must refresh a small number of times, not once per detent. `(rN)` in the Counter row is the refresh count. |
| 13 | Idle deep sleep | leave it alone for 20 s | Serial prints `[power] idle, sleeping`, then goes quiet. **The screen keeps showing the page.** |
| 14 | Wake from a button | press MENU while asleep | Device wakes, `Boots:` increments, `Wake: button/rotary`, and the checklist ticks are still there (they live in RTC memory). |
| 15 | Wake from the wheel | turn the wheel while asleep | Same as 14. Note the first detent is consumed by the wake — this is by design, see architecture.md 3.4. |
| 16 | Manual sleep | press EXIT | Screen repaints with `EXIT: OK`, then sleeps immediately. |
| 17 | No wake loop | hold MENU down while it is trying to sleep | It must **not** enter sleep while the pin is held, and must not thrash. |
| 18 | Cold boot vs wake | tap RESET | `Wake: cold boot`, `Boots:` resets to 1, checklist clears. |

### Measuring sleep current (optional but worth doing once)

Power the board from the BAT connector through a multimeter in series. Deep
sleep with both rails low should be in the tens-of-microamps range. A reading in
the milliamps means a rail is not actually off — most likely the `GPIO42`
float described in architecture.md 3.5.

## 7. Known limitations of Milestone 1

1. **The panel flashes white on every wake.** The vendor init sequence includes
   a clear + full refresh, and the previous frame cannot be kept across deep
   sleep (27 KB does not fit in ~8 KB of RTC memory). Deferred to Milestone 4.
   *Since the GDEY075T7 port, `Epd750Display` restores the saved 48 KB frame
   from SD on a real wake instead -- architecture.md 4.2.*
2. **The first encoder detent after a wake is consumed** by the wake itself. A
   deliberate trade — see architecture.md 3.4.
3. **No battery reporting.** No documented sense GPIO on this board revision.
4. **`GPIO42` (SD power) floats during deep sleep** — it is not an RTC pad and
   cannot be held.
5. **Panel SPI is bit-banged**, so a full frame transfer is not fast. Fine at
   one refresh per page turn; revisit if it dominates wake time.
   *Resolved by the GDEY075T7 port: GxEPD2 uses hardware SPI.*
6. **The built-in font is Latin-only 5x7.** Arabic rendering needs a real
   shaping-aware font pipeline and arrives with the Quran renderer.
7. **`pio test -e native` needs gcc or clang.** The tests themselves have been
   run and pass (117/117, via `run_host_tests.py` with MSVC), but the PlatformIO
   `native` environment has not been exercised because the machine used had no
   gcc.
8. **Nothing has been run on physical hardware yet.** Every hardware value is
   sourced from Elecrow's published examples for this exact board, and every
   unresolved one is marked `TODO(hw)`.

## 8. Known limitations of Milestone 2

1. **No package is ever read from a real card yet.** The parser is exercised
   against memory buffers on the host and compiles for the device, but nothing
   has opened a `.qpk` from SD. That happens in Milestone 4.
2. **`LAYOUT_DATA` is now implemented and parsed** (`Reader::getLayoutHeader`/
   `getLine`), produced by `desktop/converter`. What still waits for
   Milestone 5 is the on-device renderer that would actually consume it.
   *(Resolved 2026-09-02: `ui::QuranScreen` consumes it, and the records now
   carry the Madinah mushaf's own line breaks.)*
3. **`FONT_METADATA` and `ASSETS` are reserved.** The container carries them;
   their internal layout is not specified until Arabic rendering exists.
   *(Resolved 2026-09-01: specified in [qpk-format.md §9a](qpk-format.md) and
   exercised end to end by the real 1,793-glyph atlas.)*
4. **No real content.** Every fixture is synthetic placeholder text. Producing
   a real mushaf package requires a verified source and the conversion
   pipeline, which is the next milestone.
5. **`SdStorage` exposes 4 file handles.** Enough for a package plus state, but
   a leak shows up much later as "cannot open package"; `openHandleCount()`
   exists to make that visible.

## 9. Known limitations of Milestone 3

1. **No OCR.** A scanned PDF is detected and reported, not processed.
2. **Word boxes from multi-word PDF text runs are apportioned, not measured.**
   The fraction is reported; see [conversion.md §2.2](conversion.md).
3. **Multi-column PDFs merge columns into single lines.** Column detection
   needs real documents to tune against, not synthetic fixtures.
4. **No right-to-left reordering for generic books.** The flag is recorded in
   the package; the converter does not reorder runs.
5. **`LAYOUT_DATA` is now written**, derived from the same word boxes that
   populate `WORD_INDEX` (`quran-package.ts`, `book-package.ts`). Nothing on
   the device reads it yet -- that is still the renderer's job.
6. **No package has been converted from a real document.** Every fixture is
   generated. The pipeline has not met a real mushaf PDF or a real EPUB.

## 10. Known limitations of Milestone 4

The HTTP API ([protocol.md](protocol.md)) and BLE provisioning
([provisioning.md](provisioning.md)) are implemented and wired end to end,
including the resumable chunked upload state machine, the pairing-token
auth, and encrypted BLE characteristics. What's left:

1. **Transfer mode now has a trigger, but it is a stopgap, not a menu.** A
   long-press on the encoder switch (`main.cpp`'s `ToggleTransferMode`)
   reconnects Wi-Fi with the saved credentials and brings `HttpServer` up;
   the self-test screen's footer shows `host:port` once connected, and a
   second long-press tears it back down. This exists so the desktop
   `device-cli`/`device-client` (built independently, see §5c) has something
   real to talk to before Milestone 5's library browser exists — it is
   deliberately not that browser. A real menu action should replace this
   gesture once one exists, not keep it alongside a second way to do the
   same thing.
2. **Backup/restore are 501 stubs.** No packing format for `/USER` +
   `/DEVICE` state exists yet. Not simplified, unbuilt: the routes exist so a
   client gets a clean "not implemented" instead of a 404, nothing more.
3. **The protocol fixture covers constants only.** `transcript_fixture.h`
   (generated by `packages/protocol/scripts/build-transcript.ts`) checks
   paths, header names, status codes, and the error-code mapping between
   `protocol.h` and `index.ts`. It does not check JSON response *shapes*
   (`DeviceInfo`, `DeviceStatus`, ...); that would need
   `http_server.cpp`'s response-building pulled out into pure,
   host-testable functions first, which has not been done.
4. **No MITM protection on BLE pairing.** Pairing mode is `ESP_IO_CAP_NONE`
   ("Just Works"), because that is what the hardware's I/O capability
   actually is — no display to show a passkey on, no keypad to enter one.
   This is a hardware-shaped limit, not something deferred for later.
5. **Factory-reset exists now: a long-press on EXIT.** `main.cpp`'s
   `FactoryReset` clears the `netcred` NVS namespace
   (`NetCredentials::clear`), resets the provisioning state machine, and
   re-enters BLE pairing — same as an unprovisioned first boot. Guarded
   against firing during an active BLE session or transfer mode, so it
   can't orphan either. The self-test screen's footer now shows "Pairing
   mode" whenever BLE is advertising, whether that's from this or a real
   first boot — a gap that existed even before this, since first-boot
   pairing had no screen feedback either.
6. **Internal heap during a transfer is unmeasured, but the chunk path is not
   the risk it looks like at first read.** `HandleUploadChunk` does not use
   `WebServer`'s `arg("plain")` for the chunk body — it can't: that path
   heap-allocates a `String` and stops at the first `0x00` byte, corrupting
   any binary chunk that contains one (see protocol.md's "chunk-upload body
   is binary-unsafe as a plain PUT"). `ReadChunkedBody` reads the
   `Transfer-Encoding: chunked` body straight off the socket into
   `HandleUploadChunk`'s `static uint8_t chunk_buf[kMaxChunkBytes]` — `.bss`,
   not heap, sized once at link time, already inside the RAM percentage
   `pio run` reports. There is no "2x the chunk size, transient, per chunk"
   allocation to measure on that path.

   What *is* still unmeasured: the small heap-`String` bodies on
   `HandleUploadBegin`/`HandleRestore` (a few hundred bytes of JSON, not
   `kMaxChunkBytes` — low risk but real), and simply what headroom is left
   once the Wi-Fi/LWIP stack, `WebServer`'s own per-connection state, and
   `chunk_buf`'s static 16 KB are all resident together. That number needs a
   board, not more reasoning — `HttpServer::begin()` and every chunk now log
   `ESP.getFreeHeap()`, so the first bring-up session answers this by
   reading the monitor during a real upload, not by writing new
   instrumentation under time pressure.
7. **`expiresInSeconds` is advisory, and enforcing it is blocked on something
   real, not just unbuilt.** There is no RTC and no NTP client anywhere in
   this firmware — `UploadManager::finish()` always receives `now_unix = 0`
   (`LibraryEntry::installed_at`'s documented "clock unset" value) because
   there is no trustworthy wall-clock time to pass it. A time-based expiry
   needs a time source before it needs code. A session with a valid `.meta`
   is kept indefinitely so it can survive a reset — that is the point of the
   design — but it also means an abandoned transfer holds disk space until
   someone calls `DELETE` on it, and `beginUpload`'s free-space check is
   what that space comes out of. `.meta` has a reserved field for a
   timestamp for whenever a clock source exists to fill it.
8. **Nothing has run on physical hardware.** Same caveat as Milestones 1-3.
   The HTTP socket handling, GATT wiring, and Wi-Fi radio are untested
   beyond "compiles, links, and matches the documented library API" — see
   `test_net/test_main.cpp`'s own header comment on what it does not cover.

## 11. Known limitations of the desktop application

1. **Node on PATH, and this checkout on disk.** `app-bridge/run.mjs` is a
   script in the repository, not a bundled resource, and it imports both
   libraries from their built `dist/`. Packaging an installer means shipping
   Node and both builds as Tauri resources — not started. `QURAN_DEVICE_REPO`
   overrides the search for the checkout.
2. **File paths are typed, not picked.** A native dialog means
   `tauri-plugin-dialog`; left as an open decision rather than added
   silently. The Library tab's "Send to device" covers the case that
   mattered.
3. **The pairing token lives in the webview's `localStorage`.** A real trust
   store keyed by device id, with OS keychain backing, belongs in Rust.
4. **Only Identify has ever reached hardware.** Status, listing, upload,
   delete and abort are implemented, host-tested and wired to the UI, but
   every one of them needs a pairing token that BLE provisioning has never
   delivered — [pending.md §3](pending.md). The Device tab says so on screen.
5. **The `invoke` seam is not covered by an automated test.** Rust↔Node has a
   real spawned-process integration test and the UI typechecks and builds,
   but nothing drives a click through the native window.

## 12. Next milestone

The rest of Milestone 5: the Quran renderer (see
[qpk-format.md §9a](qpk-format.md) for the glyph-atlas format — now built and
confirmed on hardware for Al-Fatihah, not yet driven from `LAYOUT_DATA`) and
reading-position persistence. The device-side library browser landed with a
real Transfer Mode menu action, replacing the `ToggleTransferMode` long-press
gesture (§10) as the primary trigger; the gesture still works.
