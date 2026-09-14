# E-Ink Quran & Islamic Library Device

An offline-first E-Ink Quran and Islamic library reader built on the Elecrow
CrowPanel ESP32-S3 E-Paper HMI board, with a desktop library manager and an
optional, on-demand AI recitation service.

> **Panel swap (2026-09-13).** The board's stock 5.79" 792x272 panel has been
> replaced with a Good Display **GDEY075T7** (7.5", 800x480, UC8179), driven
> through GxEPD2. The firmware is ported and every screen re-laid out for
> 800x480, but the new panel has **not yet been run on the device** -- the
> "verified on physical hardware" rows below describe the 5.79" build. See
> [architecture.md §3.1](docs/architecture.md).
>
> **Table clock (2026-09-14).** The device now boots to a home screen: a clock
> and calendar synced over Wi-Fi, beside a photo in the panel's four greys that
> changes every 10 minutes. Photos and the time zone come from the desktop
> app's new **Photos** tab. See [architecture.md §4.4](docs/architecture.md).
>
> Everything done in that work, how to build/flash/test it, and what is still
> open: [docs/session-2026-09-gdey075t7-table-clock.md](docs/session-2026-09-gdey075t7-table-clock.md).

The device is fully usable with no Internet connection. AI is optional and
never required for reading.

---

## Where the project is

**Phase 1, Milestones 1-4 — complete. Milestone 5 — the desktop application,
the device's library browser and the whole Quran as validated content are
built; the on-device renderer that would draw it is not.**

- **M1** firmware hardware bring-up: E-Ink driver, SD card, rotary encoder,
  MENU/EXIT, deep sleep and wake, plus a hardware self-test screen.
- **M2** the QPK1 content package: a validating, allocation-free parser on the
  device and a TypeScript reader/writer for the desktop, kept byte-compatible
  by a golden fixture.
- **M3** the conversion pipeline: PDF/EPUB/TXT and structured Quran sources in,
  validated QPK1 packages out, with preview before install.
- **M4** getting packages onto the device: a resumable HTTP upload with atomic
  installation, BLE first-boot provisioning, and the desktop client that drives
  both. The device-side logic is host-tested including power loss at every
  write; the radios themselves are written and compiled, not run.
- **M5, desktop half** the Tauri application: Dashboard, Library, Device and
  Converter over the M3 and M4 libraries, which are unchanged. It drives them
  as a Node subprocess rather than reimplementing either.
- **M5, content** the complete Quran: 114 surahs, 6236 ayahs in Uthmani script
  with an aligned translation and a shaped glyph atlas, in three packages
  costing 5 of the device's 96 library index slots.

| | Status |
|---|---|
| Firmware builds (`pio run`) | ✅ clean, no warnings — product and all four opt-in flags |
| Firmware host unit tests | ✅ 198/198 passing across `test_logic`, `test_net`, `test_qpk` and `test_ui` — run 2026-09-14 with both `pio test -e native` and `firmware/scripts/run_host_tests.py`, see [the session record](docs/session-2026-09-gdey075t7-table-clock.md) §11.2 |
| Desktop package tests | ✅ 23 (qpk-format) + 6 (protocol) |
| Converter tests | ✅ 44 tests, all passing |
| Device client tests | ✅ 14 tests, all passing |
| Desktop app tests | ✅ 22 (app-bridge) + 13 (Rust) — frontend `tsc` + `vite build` clean |
| Quran content | ✅ **all 114 surahs, 6236 ayahs** packaged and validated — [quran-content.md](docs/quran-content.md) |
| Cross-language byte identity | ✅ asserted by the golden fixture |
| Power loss during install | ✅ simulated at every write, invariant holds |
| Verified on physical hardware | ✅ **panel, PSRAM, all inputs, deep sleep/wake** — see [bring-up-log.md](docs/bring-up-log.md) |
| SD card | ✅ **mounts, reads and writes** — reformatted FAT32, confirmed on hardware 2026-09-01 (`[sd] SDHC 59640 MB, layout=1 write=1`) |

Hardware values were sourced from Elecrow's published examples for this exact
board, and the one genuinely undocumented value — the rotary encoder's decode
mode — was carried behind a build flag with **both interpretations
implemented** until real hardware could settle it. It did:
the wheel is two independent pulse lines, not a quadrature encoder.
[bring-up-log.md](docs/bring-up-log.md) records that and everything else the
board actually did.

## Quick start

```bash
pip install platformio
pio run -d firmware -e crowpanel_579          # build
pio run -d firmware -e crowpanel_579 -t upload # flash
pio device monitor -d firmware -b 115200       # watch
```

Host unit tests:

```bash
pio test -d firmware -e native
```

On Windows without MinGW, use the Visual Studio Build Tools instead — open a
developer shell and run:

```bash
python firmware/scripts/run_host_tests.py
```

Desktop package and inspector:

```bash
npm install --prefix packages/qpk-format
npm test --prefix packages/qpk-format
```

```bash
node tools/package-inspector/inspect.mjs packages/qpk-format/fixtures/mini-quran.qpk
```

Convert a document into an installable package:

```bash
npm install --prefix desktop/converter
npm run build --prefix desktop/converter
node tools/pdf-converter/convert.mjs book.pdf --title "A Book" --preview 1
```

Install a package on a device (which must be in transfer mode):

```bash
npm install --prefix desktop/device-client
npm run build --prefix desktop/device-client
node tools/device-cli/device.mjs upload book.qpk --host 192.168.1.42 --token "$QR_DEVICE_TOKEN"
```

Or do all of that in the desktop application, which drives the same two
libraries — so build them (the two blocks above) first:

```bash
npm install --prefix desktop
npm run tauri dev --prefix desktop
```

## Repository layout

```
firmware/     ESP32-S3 firmware, QPK parser, transport   M1, M2, M4: done
packages/     qpk-format (TS reader/writer)              M2: done
              protocol (wire types + constants)          M4: done
desktop/      converter (conversion pipeline)            M3: done
              device-client (discovery, upload)          M4: done
              app-bridge (app -> both libraries)         M5: done
              src, src-tauri (Tauri UI)                  M5: done
tools/        package-inspector                          M2: done
              pdf-converter, quran-validator             M3: done
              device-cli                                 M4: done
              arabic-pager (fetch, shape, render)        M5: done
fonts/        KFGQPC Uthmanic HAFS + Naskh               supplied
sdcard-staging/  what belongs on the card                staged, not copied
ai-server/    optional speech / alignment service        Phase 3
hardware/     enclosure (OpenSCAD source + STLs)         unmeasured
docs/         architecture, formats, protocols
```

The Quran itself is **not** in this repository — no Quranic text or translation
is committed. `tools/arabic-pager/fetch_all_surahs.py` fetches it from the
quran.com API into a gitignored data directory, and the packages are built from
that. See [quran-content.md](docs/quran-content.md).

## Documentation

| Document | What it covers |
|---|---|
| [docs/architecture.md](docs/architecture.md) | layering, hardware facts, power model, every deferred decision |
| [docs/development.md](docs/development.md) | build, flash, test, demo builds, and the bring-up checklist |
| [docs/bring-up-log.md](docs/bring-up-log.md) | **what the real board did** — verified, broken, still untested |
| [docs/pending.md](docs/pending.md) | **everything outstanding**, grouped by what unblocks it |
| [docs/qpk-format.md](docs/qpk-format.md) | QPK1 binary content package |
| [docs/conversion.md](docs/conversion.md) | conversion pipeline, the Quran import schema, PDF caveats |
| [docs/quran-content.md](docs/quran-content.md) | **the full 114-surah build**: font, fetching, why it is one package, indexing |
| [docs/protocol.md](docs/protocol.md) | desktop ↔ device API: resumable upload, atomic install |
| [docs/provisioning.md](docs/provisioning.md) | BLE first-boot provisioning |

## Roadmap

- **Phase 1** — offline reader + desktop library manager
  - M1 hardware bring-up ✅
  - M2 QPK1 package format — device parser + desktop reader/writer ✅
  - M3 conversion pipeline (PDF/EPUB/TXT → validated QPK1) ✅
  - M4 Wi-Fi transfer + BLE provisioning ✅ — implemented and host-tested, desktop
    client and CLI built, reachable via a long-press stopgap trigger ahead of
    the Milestone 5 menu. See [docs/development.md §10](docs/development.md#10-known-limitations-of-milestone-4)
  - M5 desktop UI ✅ — four tabs over the M3/M4 libraries. Only Identify has
    ever reached hardware; the rest needs a pairing token BLE has never
    delivered. See [docs/pending.md](docs/pending.md)
  - M5 library browser ✅ — two levels on the device: Quran / Books /
    Translations / Tafsir, then that type's items. Confirmed on hardware
  - M5 Quran content ✅ — all 114 surahs and 6236 ayahs packaged with real
    chapter numbers, an aligned Saheeh International translation, and a
    1,793-glyph shaped atlas (1,507 word glyphs plus 286 ayah markers) with
    full font coverage, laid out on the Madinah mushaf's own 604 pages.
    See [docs/quran-content.md](docs/quran-content.md)
  - M5 Quran reader ✅ — `ui::QuranScreen` blits the shaped atlas off the
    card, right-to-left, paging across surah boundaries. Confirmed on
    hardware. Real Madinah-mushaf line breaks and real Uthmani ayah markers
    were both added 2026-09-02 — built and host-tested, and rendered off the
    real package on the host, but **not yet seen on the panel**: the SD card
    is in the device and the package carrying them is still staged.
    See [docs/pending.md](docs/pending.md) §2
  - M5 surah picker ✅ — `ui::SurahPickerScreen`, entered instead of jumping
    straight into surah 1. Confirmed on hardware 2026-09-01, real surah
    names rendering off the full 114-surah package
  - M5 reading-position persistence ✅ — `net::ReadingProgress`
    (`/USER/progress.bin`). The "nothing saved yet" path is confirmed on
    hardware; a full save-then-resume round trip is not yet
- **Phase 2** — Hifz: hide/reveal, testing mode, goals, khatma, mistake data
- **Phase 3** — optional AI: recitation recognition, alignment, mistake
  detection, voice search, behind replaceable provider interfaces

## Credit

The GDEY075T7 panel is driven by Jean-Marc Zingg's
[GxEPD2](https://github.com/ZinggJM/GxEPD2) on Adafruit GFX. Board pin
assignments were sourced from Elecrow's reference examples for this board:
[Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792](https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792).
