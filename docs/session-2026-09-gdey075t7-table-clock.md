# Session record: 7.5" panel port and table clock (2026-09-13 to 09-14)

What was done, in order, why, and how to pick it up again. The design detail
lives in [architecture.md](architecture.md) (§3.1–3.2 panel, §4.2 wake
restore, §4.4 table clock) and [protocol.md](protocol.md) (photo and time
endpoints); this page is the narrative and the checklist.

---

## 1. Starting point

- Board: Elecrow CrowPanel ESP32-S3 E-Paper HMI (ESP32-S3-WROOM-1, 8 MB flash,
  8 MB octal PSRAM, CH340 USB-serial on UART0 at `/dev/cu.usbserial-10`).
- Its stock 5.79" 792x272 dual-SSD1683 panel was replaced with a Good Display
  **GDEY075T7-T01**: 7.5", 800x480, one UC8179 controller, 24-pin FPC, in the
  CrowPanel's own socket.
- Firmware: this repository's PlatformIO project (`firmware/`), which drove the
  old panel with a bit-banged port of Elecrow's `EPD_Init`/`spi` code.
- Board facts that did not change: SCK 12, MOSI 11, CS 45, DC 46, RST 47,
  BUSY 48; GPIO 7 = panel power enable, must be HIGH first.

## 2. Toolchain on this Mac

| Tool | Where | Notes |
|---|---|---|
| PlatformIO 6.x | `~/Library/Python/3.9/bin/pio` (pip `--user`) | the build system for `firmware/`; pins `espressif32@6.5.0` = Arduino core 2.0.14 |
| arduino-cli 1.5.1 | `~/.local/bin` | **not used for this firmware**: its esp32 core 3.3.11 breaks `http_server` (`WiFiClient` typedef) and BLE (NimBLE callbacks), and on core 3.x GxEPD2's default `SPI.begin()` would steal GPIO 13 from the SD card |
| Node 20.20.2 / npm 10.8.2 | `~/.local/node` (official tarball, SHA-256 checked) | desktop packages, bridge, app |
| Rust 1.98.1 | `~/.cargo` (rustup, minimal profile, shell profile untouched) | Tauri shell |

None of these are on the default PATH. Export what you need:

```bash
export PATH="$HOME/Library/Python/3.9/bin:$HOME/.local/node/bin:$HOME/.cargo/bin:$PATH"
```

## 3. Panel port: SSD1683 → GDEY075T7 (GxEPD2, then GxEPD2_4G)

- **Removed** `drivers/epd/epd579_panel.*`, `epd579_display.*` and the
  15-strategy `REFRESH_EXPERIMENT` build (all SSD1683-specific).
- **Added** `drivers::Epd750Display` (`include/drivers/epd750_display.h`,
  `drivers/epd/epd750_display.cpp`). UI code still draws on the host-testable
  `gfx::Canvas`; the driver copies it into the GxEPD library with
  `drawInvertedBitmap()` and refreshes.
- **Startup order**, as specified: GPIO 7 HIGH → 100 ms →
  `init(115200, true, 2, false)` → `SPI.end()` →
  `SPI.begin(12, -1, 11, 45)`. Checked in core 2.0.14's SPI code that this does
  not disturb the SD card's MISO (GPIO 13). A saved frame on a real wake uses
  `initial=false` so the first repaint is partial (architecture.md §4.2).
- **Board header** (`include/board/board_crowpanel_579.h`): 800x480, 100-byte
  stride, 48,000-byte framebuffer, `kDisplayRotation = 0` (TODO(hw): set 2 if
  the image is upside down), seam/cascade constants removed.
- **All five screens re-laid out** for 800x480 (library, self-test, surah
  picker, Quran reader, book reader): more list rows (12), seven mushaf lines
  per screen instead of four. Long values are truncated with `..` instead of
  running into the border.
- **Fit is tested, not eyeballed:** `Canvas::clippedPixels()` counts ink drawn
  off the canvas; host tests render every screen with worst-case content and
  assert 0 clipped pixels and no ink outside the frame. The repaint log prints
  `clipped=`.
- **Previews:** `tools/render_ui_preview.cpp` renders every screen, home
  included, to PNG (`scripts/preview_ui.py`, or build it directly with c++).
- **Warnings fixed:** three `-Wformat-truncation` in `library_screen.cpp` and
  `main.cpp` (behaviour-preserving `%.32s`), and `test_build_src = yes` in the
  `native` env so `pio test -e native` links at all.

Verified on the board: boot, full refresh ~1.6 s, partial ~0.43 s, no BUSY
timeouts, `clipped=0` on every screen visited.

## 4. Quran page turns: 6 s → well under 1 s of layout

A page turn took ~7 s; the refresh was only 0.78 s. Measured on the host with
the real shaped Quran package: one surah-3 screen did **236,143 SD reads**,
because `qpk::Reader::getGlyph()` scanned the 1,793-entry FONT_METADATA table
on the card for every lookup.

- `Reader::buildGlyphIndex()` copies that table into caller-owned storage
  (PSRAM, allocated in `main.cpp`'s `OpenQuran`) in 29 reads, sorted by id;
  `getGlyph()` then binary-searches it.
- Same screen afterwards: **784 reads**, pixels byte-identical, all 65,536 ids
  return the same records as the scan. Four host tests cover it.
- The `[quran] ... glyphs in Nms` and `[quran] glyph index: ...` log lines show
  the on-board timing. (Not yet read back from the board.)

## 5. Timestamped logs

Every firmware log line goes through `drivers::Logf` / `drivers::LogLine`
(`include/drivers/serial_log.h`): `[+12.345]` seconds since boot until NTP
syncs, then `[14:32:05.123]` local time. ~80 `Serial.printf/println` calls were
moved over. GxEPD's own unstamped timing prints are compiled out with
`-DDISABLE_DIAGNOSTIC_OUTPUT`; the driver logs panel timings itself.

Also: boot banner (reset reason, chip, flash, PSRAM, heap, build time), a
`[hb]` heartbeat every 10 s, and Wi-Fi waits that print progress instead of
blocking silently.

## 6. Four greys and the grey refresh experiment

The GDEY075T7 shows four greys via **GxEPD2_4G** (github.com/ZinggJM/GxEPD2_4G,
pinned to commit `de006169`; not in the PlatformIO registry, and it replaces
GxEPD2 because the two define the same classes).

The problem, found in the library source: its first black/white write after a
grey refresh clears both controller RAM planes and forces a full black/white
refresh (`_writeImage` → `_Force_Init_Full`), so a minute clock update would
wipe the photo.

`src/grey_test.cpp` (`pio run -e grey_test -t upload`) drew a grey test photo
and updated only a clock box four ways. It ran on the board (after fixing a
hang: GxEPD2_4G's 16-bit buffer index needs pages of ≤ 65,535 bytes):

| Variant | Method | Clock update |
|---|---|---|
| A1 | grey window, partial-in, gates scan all | 2.8 s |
| A2 | grey window, gates inside window | 2.8 s |
| **B1** | **b/w fast OTP partial, partial-in, gates scan all** | **0.6 s** |
| B2 | b/w, gates inside window | 0.6 s |

All completed with no BUSY timeouts; the full grey frame took 3.7 s and showed
**four clearly distinct shades**. B1 was chosen (fastest). **Not confirmed by
eye:** whether B1 leaves the photo's greys untouched. If the photo fades or
flashes on minute changes, switch `Epd750Display::flushWindow` to the A1
sequence from `grey_test.cpp`.

## 7. Table clock

Enabled by `app::kTableClockMode` (`include/app/app_config.h`). Details in
architecture.md §4.4.

- **Boots to the home screen** (`ui/home_screen.*`): weekday, date, big
  clock, month calendar with today highlighted, status line (Wi-Fi address,
  sync state, photo n/m) on the left; 400x480 photo on the right.
- **Refreshes** (`PollHomeScreen` in `src/main.cpp`): full 4-grey frame on
  entry, date change, first sync, photo/time-zone change and every
  `kPhotoIntervalMinutes` (10) with the next photo, ≥ 15 s apart; every other
  minute only the clock rectangle (`flushWindow`, B1).
- **Always on:** Wi-Fi and the HTTP API come up at boot and reconnect every
  30 s if lost; no deep sleep; no transfer-session timeout.
- **Buttons:** MENU cycles home → library → self-test → home; EXIT from the top
  of a menu goes home (on home, a manual redraw); long-press EXIT factory
  reset still works; the wheel and OK do nothing on home (no pointless grey
  refreshes).
- **Time:** SNTP (`pool.ntp.org`) with the POSIX rule in
  `/DEVICE/timezone.txt` (`SystemClock::setTimeZone`, `configTzTime`). Until
  the desktop app sends one, the clock shows UTC.
- **Photos:** `net::PhotoStore` (`include/net/photo_store.h`,
  `wifi/photo_store.cpp`) keeps `/PHOTOS/<name>.g4` — `QPH1` header + 400x480
  at 2 bpp (48,016 bytes) — with resumable chunk uploads verified before
  commit. Shown in name order.
- **API** (protocol.md): `GET /api/photos`, `PUT /api/photos/:name/chunk`,
  `POST /api/photos/:name/finish`, `DELETE /api/photos/:name`,
  `POST /api/device/time`. Mirrored in `packages/protocol` and checked by the
  regenerated transcript fixture.

## 8. Desktop app: Photos tab

- `desktop/device-client/src/photo.ts` (dependency-free, runs in the webview
  and under Node): centre 5:6 crop, luma with a contrast control,
  Floyd–Steinberg dithering to the panel's four greys, `.g4` encode/decode,
  safe names, and `posixTimeZone()` — the computer's zone as a POSIX rule with
  DST transitions (e.g. `LOC-5:30`, `LOC-1LDT-2,M3.5.0/2,M10.5.0/3`).
- `DeviceClient`: `listPhotos`, `uploadPhoto` (shares the package upload's
  resumable chunk loop), `deletePhoto`, `setTimeZone`.
- Bridge commands `device.photos`, `device.photoUpload`, `device.photoDelete`,
  `device.setTimeZone`.
- `desktop/src/Photos.tsx`: choose pictures, contrast slider, 4-grey previews
  from exactly the uploaded pixels, upload with progress, list/delete on the
  device, send time zone. Uses the address and pairing token from the Device
  tab.

Run it:

```bash
PATH="$HOME/.local/node/bin:$HOME/.cargo/bin:$PATH" npm run tauri dev --prefix ~/Documents/epaper/desktop
```

### 8.1 Photo upload failed on the board — fixed

The first real upload from the Photos tab failed; the device log said
`ReadChunkedBody first line: ""`. Cause: the Arduino core's `WebServer`
calls `client.flush()` at the end of `_parseRequest`, which in core 2.0.14
empties the socket's receive buffer — so the chunk body bytes that arrived with
the headers were thrown away. Hand-framed PUTs proved it (body 0.3 s after the
headers: read; same request in one packet: nothing; 16 KB body: first segment
lost).

- `firmware/lib/WebServer/` is a copy of the core library with that one call
  removed (`PATCHED.md` explains it; PlatformIO prefers `lib/` over the
  framework copy). Re-apply if the core is upgraded.
- `http_server.cpp` also reads the chunk through `WebServer`'s own
  `_currentClient` (`ApiWebServer::currentClient()`) and logs how many bytes
  were already buffered. That was a first guess that did not fix it on its
  own; it is harmless and the log line is useful.
- Verified: all four framings pass, and a real photo uploaded in three chunks,
  installed and appeared on the panel. This also fixes package uploads
  (same body reader) — not yet run on the board.

## 9. Build, flash, monitor, test

```bash
export PATH="$HOME/Library/Python/3.9/bin:$HOME/.local/node/bin:$PATH"
cd ~/Documents/epaper/firmware
pio run -e crowpanel_579                                                  # build
pio run -e crowpanel_579 -t upload --upload-port /dev/cu.usbserial-10     # flash (460800 baud)
pio device monitor -p /dev/cu.usbserial-10 -b 115200 --dtr 0 --rts 0      # serial
pio test -e native                                                        # firmware host tests
```

- **Close the serial monitor before flashing.** A monitor holding the port
  made one upload fail with "serial noise or corruption".
- `--dtr 0 --rts 0` stops the monitor holding the board in reset.
- Desktop tests: `npm test` in `packages/protocol`, `packages/qpk-format`,
  `desktop/converter`, `desktop/device-client`, `desktop/app-bridge`;
  `npm run build` in `desktop`; `cargo check` in `desktop/src-tauri`.
- Protocol constants changed? `npm run fixture --prefix packages/protocol`.

Results at the end of the session: firmware build 0 warnings (RAM 47.8%,
flash 44.3%); firmware host tests 187/190; device-client 27/27; protocol 6/6;
app-bridge 25/26; desktop build and `cargo check` clean.

## 10. Open items

1. **B1 clock update not confirmed by eye** to keep the photo intact (§6).
2. **Booster resistor**: fitted for the SSD1683 panel. Greys looked right; if
   the image ever looks faint or half-refreshed, suspect it first.
3. **Rotation** unconfirmed against the enclosure (`kDisplayRotation`).
4. **Wi-Fi provisioning and pairing token** are needed for the clock and the
   Photos tab: BLE pairing, or `/DEVICE/wifi.json` on the card with
   `ssid`/`passphrase`/`token` (one-shot, removed after reading).
5. ~~**Three `test_qpk` failures** and `scripts/run_host_tests.py` passing
   `-std=c++11` to Unity's C file~~ — both fixed the same day, see §11.2.
6. **app-bridge** "progress lines ... parseable JSON" test fails at 8,192 bytes
   (pipe chunking) — pre-existing.
7. **An-Naba demo pages** are 792x272 and shown centred; regenerate with
   `tools/arabic-pager/render_pages.py` at 800x480 to fill the panel.
8. **Quran on-board page-turn time** after the glyph index not yet read back.
9. **Time-zone rules** are derived for the current year; the app sends the
   rule again whenever you press "Send to device".
10. A pre-change copy of the firmware sources and docs was kept in a temporary
    scratch folder during the session; it is not part of the repository.

## 11. Later on 2026-09-14: covers, PDF page pictures, launcher

Built and host-tested; **none of it has run on the board yet** — the board was
unplugged when this was written.

- **Desktop path boxes** get a native **Browse...** (tauri-plugin-dialog):
  Library folder, Converter input/output, Device package.
- **Book covers.** New optional `COVER` section (id 19, 108×144, 4 greys,
  qpk-format.md 9c) in the TS writer, C++ reader (`Reader::readCover`), Rust
  scan and validator. Converter tab: cover taken from an EPUB automatically
  (`extractEpubCover`, bridge `coverSource`) or "Choose picture...", with a
  4-grey preview; Library tab shows cover thumbnails. The device's Books
  cover grid is **not built yet**.
- **PDF page pictures** ("keep the PDF's page layout"). New optional
  `PAGE_IMAGE_INDEX`/`PAGE_IMAGE_DATA` sections (ids 20/21, 480×800 1bpp,
  PackBits, qpk-format.md 9d). The webview renders each PDF page with pdf.js
  (`desktop/src/pdfPages.ts`), trims margins, and turns it black and white
  with only mid-tones dithered (`ditherToBits`). Text sections are still
  written. The Converter shows a page exactly as the device will.
- **Device page reader** (`ui::PageImageScreen`, `ScreenMode::kPages`):
  portrait, page turned onto the landscape panel (`app::kPortraitClockwise`,
  TODO(hw): flip if upside down); wheel turns pages with partial refreshes;
  EXIT/MENU back to the library; reopening the same book resumes its page.
- **Pre-loading.** The page on screen and both neighbours stay decoded in
  PSRAM (`app::kPageCacheSlots` = 3, 48 KB each); `PollPagePrefetch` decodes
  the next, then previous page while idle, so a turn does not read the card.
- **Launcher.** The library's top level is now a phone-style icon grid:
  Quran, Books, Translations, Tafsir / Wi-Fi, Clock, Device, the selected tile
  drawn inverted. Clock and Device icons added to `gen_library_icons.py`
  (Pillow installed for it).
- **Upload diagnostics.** A 5 MB package upload failed once with
  `STORAGE_ERROR` (507) at an unlogged step. Every storage failure on the
  upload path now logs (`[sd] append/rename`, `[transfer] chunk PUT ... failed`,
  `[transfer] finish ... failed`), and a chunk append that wrote nothing is
  retried once. The retry resumed cleanly from 1.8 MB and passed 6.5 MB before
  the board disconnected from USB; the cause of the original 507 is still
  unknown.

Tests: qpk-format 30/30, device-client 30/30, converter 44/44, app-bridge
25/26 (the known 8,192-byte pipe test), firmware test_ui passing, test_qpk with
the 3 known failures plus 3 new passing page-picture tests; firmware build
clean, flash 44.5%.

### 11.1 On the board, afternoon of 2026-09-14

- **Flashed** (verified) with everything in §11. The icon home screen came up.
- **A PDF read through the bridge came back truncated** ("the bridge exited
  without producing a result"). `run.mjs` called `process.exit()` straight
  after writing its result; pipe writes are asynchronous, so anything past
  the first 8 KB was lost. It now exits from the write's callback
  (`finish()`), and a malformed request no longer falls through into the
  command. This was also the long-standing failing app-bridge test: 26/26 now.
- **The 913-page book** (page pictures, 25.8 MB) uploaded in about 4 minutes
  and installed: `[transfer] installed ... (The International Jew)`. The 507
  from the morning did not come back.
- **Reading it:** page 1 read from the card in 38 ms (26 KB) and drawn in
  191 ms; with the neighbours pre-decoded, page turns were drawn from memory
  in 8–50 ms, so a turn is just the partial refresh. A multi-detent jump
  skips the prepared page and reads from the card (~60–70 ms).
- **The board dropped off USB and Wi-Fi together, twice** -- mid-upload in the
  morning and mid-reading here -- and did not come back until replugged.
  That points at power (cable, port, or current draw during Wi-Fi + refresh),
  not firmware. Not yet diagnosed.

### 11.2 Fixed afterwards, built and host-tested, not yet flashed

- **Opening a book took ~9 s.** `Reader::open` verifies every fixed-size
  index section's CRC in 256-byte reads, and OpenBook opened the package twice
  (once to look, once to read). The CRC now streams through a static 8 KB
  buffer, and the package is opened once (`OpenBook` opens `g_page_reader`
  and hands it to `OpenPageBook`). The open time is logged:
  `[library] opened <path> in Nms`.
- **Picture books leave out word coordinates** (the Converter sends
  `includeWordLayout: false` with page pictures): the page reader never uses
  them and they were most of what the open verifies.
- **Page log trimmed:** a line per card read only if it took 150 ms or more,
  and a page line only for card reads and every tenth page.
- **The reading page survives a restart:** `/USER/pages.bin` (`PPG1`,
  32 bytes, CRC, .tmp-then-rename; `net::Load/SavePageProgress`), separate
  from the Quran's `progress.bin`. Written 3 s after the last page turn and
  on every way out (EXIT, MENU, sleep); read when the book is opened.

- **The three old test_qpk failures** (§10 item 5) were the tests, not the
  reader: `OpenBytes(builder.build(), ...)` pointed a MemoryFile at a
  temporary that was destroyed as soon as the call returned. open() ran while
  it was alive; any later read hit freed memory, which decoded as "no such
  glyph" or a zero line count. `OpenBytes` now keeps its own copy.
- **`scripts/run_host_tests.py` on macOS** compiled Unity's C file in the same
  clang++ command as `-std=c++11`, which clang rejects. Unity is now compiled
  on its own with the matching C compiler, then linked.

- **Demo builds removed.** `NABA_DEMO`, `READER_DEMO` and `GLYPH_ATLAS_DEMO`
  are gone from `main.cpp` (2,698 → about 2,300 lines; the product branches
  kept exactly), with `app/naba_pages.h` (506 KB), `app/embedded_note.h` and
  `scripts/gen_embedded_note.py`. Real packages on the card now cover what
  they showed. `EPD_SMOKE_TEST` and `DEV_AUTOPROVISION` stay. The product
  image is unchanged in size (the demos were never compiled into it); the
  pre-removal `main.cpp` was kept in the session scratchpad, not the repo.

- **Page number while reading.** `ui::PageImageScreen` draws "212 / 913"
  in a small white box in the page's own bottom-right corner, turned with
  the page (drawing helpers work in page coordinates).
- **Go to page.** OK while reading opens a centred "Go to page" box; the
  wheel moves the target `ui::kPageJumpStep` (10) pages a detent, OK jumps
  there with a full refresh, EXIT closes the box and stays put.
- **Books shelf on the device.** Books is a grid of covers now: two rows of
  five, "< Back" first, the selected cover framed, "page 1 of 2" in the
  header. Covers are the package's COVER section drawn in black and white
  (the two greys as 2x2 patterns), so moving the selection is still a fast
  partial refresh; packages without one get a framed book icon.
  `qpk::ReadCoverFromFile` reads just the header, section table and COVER --
  no checksum sweep -- and `main.cpp` caches a page of covers by content_id.
- Preview scenes `pages`, `pages-jump` and `library-books` added to
  `tools/render_ui_preview.cpp` (and `ui/page_image_screen.cpp` to
  `scripts/preview_ui.py`).

### 11.3 Built while the board was away (host-tested, not yet flashed)

- **Reset history on the card.** Every boot appends
  `boot=N reset=<reason> last_alive=<unix>` to `/DEVICE/resets.log`
  (`net::AppendResetRecord`, capped at 8 KB, trimmed to whole lines); the
  device saves `/DEVICE/alive.bin` once a minute when its clock is synced.
  `GET /api/device/info` now carries `resetReason` and `bootCount`, and the
  desktop Device tab shows "Last restart", with a hint for `brownout` and
  `power-on`. This is what should explain the USB/Wi-Fi drops without a
  serial console attached.
- **Faster PDF conversion.** Pages render four at a time
  (`renderPdfPages(..., { concurrency })`), progress shows an estimate of the
  time left, and a **Cancel** button stops rendering between pages.
- **Text size in the text reader.** OK cycles x2 / x3 / x4, re-paginating
  to about the same place with a full refresh; the footer hint now says
  `OK=size EXIT=back` (it said `MENU=size`, left over from the reader demo).
- **Chapters in "Go to page".** The box shows the chapter holding the target
  page (a binary search of CHAPTER_INDEX), and MENU moves the target to the
  next chapter's first page, wrapping around.
- **The desktop app as an installed .app.** It finds the checkout it was
  built from (`CARGO_MANIFEST_DIR`) when launched from /Applications, and
  finds Node in `~/.local/node/bin`, `/opt/homebrew/bin` or `/usr/local/bin`
  when Finder's PATH does not include it.

- **Bookmarks and "Continue".** Hold OK while reading (a picture book, a
  text book or the Quran) to keep the place; `net::Bookmarks` stores up to 64,
  newest first, in `/USER/bookmarks.bin` (`BMK1`, CRC, .tmp-then-rename;
  the same place is moved to the front rather than duplicated). An eighth
  home tile, **Bookmarks**, opens `ui::BookmarksScreen`: "Continue" (the last
  place read, from pages.bin or progress.bin) and then every bookmark with its
  book and place; OK opens it through that reader's own resume path, holding
  OK deletes it, EXIT goes back. A bookmarked picture page shows a ribbon in
  its top-right corner; the Quran reader says "Bookmarked ..." in its status
  line. Holding OK elsewhere still toggles transfer mode.

- **Translation and Tafsir reader.** Opening a TRANSLATION or TAFSIR package
  (it used to say "No reader for TRANSLATION yet") shows one surah at a time
  as numbered verses through the text reader: the wheel turns pages and runs
  on into the next or previous surah, OK changes the text size, EXIT closes
  it. Surah boundaries and names come from the Quran package the translation
  is aligned to (metadata key 9), read once into RAM; without it on the card
  the translation reads as one numbered run. The text is built into a 256 KB
  PSRAM buffer by `qpk::AppendTranslationVerses`, which folds UTF-8 to the
  font's ASCII (`util::FoldToAscii`: curly quotes, dashes, ellipses, and the
  marked letters of transliteration like a-macron and h-underdot). Bookmarks
  in a translation keep the surah as well as the page.

- **Firmware over Wi-Fi.** The partition table always had two OTA app
  slots; now they are used. `POST /api/firmware/begin` (`size`, `md5`)
  starts `Update` on the slot the device is not running from, chunks go to
  `PUT /api/firmware/chunk?offset=N` strictly in order (the same chunked
  framing and offset resync as packages), and `POST /api/firmware/finish`
  checks the MD5, switches the boot slot, replies, and restarts a second
  later. A failed or abandoned update leaves the running firmware as it was;
  USB flashing remains the fallback. The desktop Device tab has an **Update
  firmware** card (pick `firmware/.pio/build/crowpanel_579/firmware.bin`,
  progress bar), through `DeviceClient.uploadFirmware` and the bridge command
  `device.firmwareUpdate`, which refuse anything that is not an ESP32 app
  image. There is no automatic rollback if the new image fails to boot.

**The installed app, built 2026-09-14.** `npm run tauri build` in `desktop/`
produced `src-tauri/target/release/bundle/macos/Quran Device.app` (11 MB).
Tauri's own DMG step failed (`bundle_dmg.sh` scripts Finder to lay out the
window, which a non-interactive build cannot do), so the installer was made
directly:

```bash
cd desktop/src-tauri/target/release/bundle
hdiutil create -volname "Quran Device" -srcfolder "macos/Quran Device.app" -ov -format UDZO "dmg/Quran Device_0.1.0_aarch64.dmg"
```

(3.9 MB).

**Standalone app, and Windows through CI.** The app now carries its own
bridge. `desktop/scripts/prepare-standalone.mjs` bundles the bridge with the
converter and device client into `src-tauri/bridge/bridge.mjs` (esbuild,
3.4 MB; pdf.js's worker runs on the main thread) and copies the Node running
it to `src-tauri/binaries/node-<target triple>`, which Tauri ships as a
sidecar (`bundle.externalBin`, `bundle.resources`). The Rust side
(`locate_bridge`) runs that pair in a release build and falls back to a
checkout; a development build prefers the checkout. The bridge's main code
moved to `app-bridge/src/main.mjs`, started by `run.mjs` (checkout) or
`standalone.mjs` (bundle).

Build order, same on every platform:

```bash
npm run build --prefix packages/qpk-format && npm run build --prefix packages/protocol
npm run build --prefix desktop/converter && npm run build --prefix desktop/device-client
node desktop/scripts/prepare-standalone.mjs
npm run tauri build --prefix desktop
```

`.github/workflows/desktop.yml` runs exactly that on `macos-latest` and
`windows-latest` (tests included) and attaches the `.dmg`, `.msi` and NSIS
`.exe` as artifacts -- it needs the project in a GitHub repository. A root
`.gitignore` keeps `firmware/include/app/dev_secrets.h` (real Wi-Fi
credentials), build output, the bundled Node and the fetched Quran data out of
any commit.

**To verify on the board, all at once:** book open time, resuming a book
after a restart, page number and Go to page (with MENU for chapters), the
Books shelf, text size in a text book, the reset line in the Device tab after
a replug, plus the earlier page direction, sharpness, ghosting, and the
clock/photo check.

Tests: firmware **205/205** via `pio test -e native` (page number and jump
box, books shelf, `ReadCoverFromFile`, and the reset log added), Rust 13/13,
and all four suites
plus the dev tools via `scripts/run_host_tests.py`; test_net gained 2
page-progress tests. app-bridge 26/26. Firmware flash 44.5%.
