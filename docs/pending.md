# Pending

Everything outstanding, in one place. Grouped by what unblocks it rather than
by component, because the first group is the one that decides what is possible
next.

Last updated 2026-09-02 (ayah markers, then the mushaf line breaks;
see the dated notes below). Before
that, 2026-09-01, after a session that built the desktop application,
took the device's library browser to two levels, enforced upload-session
expiry, packaged **the complete Quran** — all 114 surahs, 6236 ayahs, with an
aligned translation and a shaped glyph atlas — and then **made it render on
the device**.

**Milestone 5's central question is answered: the Quran is readable on the
hardware.** `ui::QuranScreen` blits pre-shaped glyph runs off the card, and
paging carries across surah boundaries. Confirmed on the real board.

What remains falls into five kinds:

1. **Refinement of a working reader** — a surah picker (the biggest usability
   gap: today you page forward from surah 1), `LAYOUT_DATA`-driven line breaks
   instead of fill-and-wrap, and proper Uthmani ayah markers. Section 2's
   "Still open" table. ***All three have since been built*** — see the dated
   notes below and section 2's "Done" list. None of the three has been seen on
   the panel: the card is in the device.
2. **Real engineering, gated on being able to verify it** — reading-position
   persistence. Now genuinely worth doing, because there is finally a reader
   whose position is worth remembering.
3. **Hardware or instruments** — the four measurements in section 5, the
   upload chunk-PUT failure and the first-refresh-doesn't-commit quirk in
   section 3.
4. **A decision that is the project owner's** — the backup/restore packing
   format, kashida justification, the `kMaxLibraryEntries = 96` ceiling, and
   the 34 translation verses carrying an Arabic honorific inside otherwise
   English prose.
5. **Blocked on something absent** — the battery divider and the canonical
   Madinah line-break data. *(The KFGQPC Uthmani font left this list on
   2026-09-01: it arrived, has full coverage, and the whole Quran shapes with
   zero warnings.)*

**Later the same day (2026-09-01):** item 1's surah picker and item 2's
reading-position persistence are both built — see section 2's "Done, most
recent first" list. `LAYOUT_DATA` line breaks and Uthmani ayah markers, item
1's other two pieces, are unchanged (both need a desktop re-shape, out of
reach while the card is in the device).

**Still later the same day:** the card came out of hiding — the device is
now on the bench, connected over USB (COM6), with the same card still in
it. The surah picker and reading-position persistence's "no saved progress"
path are now **confirmed on real hardware**, not just host-tested — see the
"First real hardware verification" entry at the top of section 2's "Done"
list. Still open from today specifically: the transfer-mode footer's actual
IP:port text hasn't been read off the physical screen (only inferred from
an ink-count change), and the new library icons haven't been looked at on
real e-ink.

**And once more the same day:** reading an actual surah from the picker
was tried, and reproduced the first-refresh-doesn't-commit quirk in the
*real product path*, not just `GLYPH_ATLAS_DEMO` — see the "Confirmed in
the real product path too" update inside section 2's glyph-atlas entry.
This raises that item's priority: it now means every fresh entry into the
reader shows the wrong screen for up to 8 page-turns, not a one-off
bring-up artifact.

**2026-09-02 — the ayah markers are real.** Item 1's "proper Uthmani ayah
markers" is built, host-tested, and rendered off the real 6,236-ayah package
on the host; it has not been seen on the panel, because the card is still in
the device. The item was smaller than this document claimed: the KFGQPC font
composes the whole ornate marker into **one glyph per ayah number**, reached
by shaping the Arabic-Indic digits *alone* — adding `U+06DD`, which this doc
assumed was required, actually shapes to two glyphs and draws the mark twice.
One `METADATA` key records where the 286 marker glyphs start; nothing in the
wire format changed. See section 2's "Done" list. Item 1's `LAYOUT_DATA` line
breaks were left alone at that point, on the reasoning that replacing a
hardware-confirmed layout path with an unverifiable one would trade a working
reader for an unprovable one.

**Later on 2026-09-02 — the line breaks are the mushaf's.** That reasoning
held right up until the blocker turned out not to exist: the "unsourced
Madinah data" is served per-word by quran.com's `/verses/by_page`. It is
built, host-tested, and rendered off the real package on the host, and the
"unprovable" objection is answered by keeping fill-and-wrap as a second path
rather than replacing it — the package on the card renders exactly as it did.
`PAGE_INDEX` is 604 real mushaf pages now, which also closes section 4's
"page-boundary fidelity". **All three of section 2's open items are now
built; what is left is the card swap.**

One structural gap underlies several of these:

### No host C++ compiler on this machine — RESOLVED 2026-09-01

**A compiler is installed and all host tests now run and pass** (141 at
first, 152 after the `ui::` follow-up below).
`winget install BrechtSanders.WinLibs.POSIX.UCRT` put a standalone GCC 16.1.0
/ MinGW-w64 build (g++, gcc, gdb, cmake) on the user PATH — no Visual Studio
Build Tools modification needed, and it coexists with the VS 2022 install
that still lacks the C++ workload. `firmware/scripts/run_host_tests.py` now
runs directly: `test_logic` 38/38, `test_net` 45/45, `test_qpk` 53/53,
`test_ui` 5/5 — 141/141, matching this pass's own `RUN_TEST` count exactly
and resolving the 117-vs-131 discrepancy this doc flagged earlier today (the
codebase was fine; the recorded figure was just stale).

**Running it for the first time found a real, pre-existing bug, not just
confirmed the new code.** `net::CopyUtf8Truncated` (`storage/library_index.cpp`,
used by every `LibraryEntry` title/author/language field) read `src[limit]`
to check for a UTF-8 continuation byte to back off from — but when the whole
string already fit (`limit == src_length`, no truncation needed), that index
is one byte *past* the last byte the caller wrote into `src`, i.e. an
out-of-bounds read of uninitialized stack memory in `describePackage()`'s
scratch buffer. `test_library_index_rebuilds_from_the_packages_on_disk`
caught it immediately on the very first real run: "Mini Test Package" came
back as "Mini Test Packag" — the garbage byte one past the title happened to
look like a continuation byte (`10xxxxxx`), so the function silently chopped
off a real trailing character it never should have touched. Fixed by only
running the backward scan when truncation actually happened
(`limit < src_length`). Worth naming plainly: this bug shipped and was
undetected through every session that built on `library_index.cpp` before
this one, purely because nothing had run the test that exercised it — the
exact risk this whole "no host compiler" gap posed the entire time, not a
hypothetical one. Verified: all 141 tests pass with the fix; `pio run` still
builds the device firmware clean (this file is shared, not test-only code).

**Two of the new suite's own build issues surfaced too**, both fixed the same
pass: `test_ui/test_main.cpp` was missing `setUp()`/`tearDown()` (a Unity
linker requirement every other suite already defines) — a link error, not a
test failure, caught before any test in that suite could even run. And
Windows Application Control blocked a freshly-linked test binary from
launching on the first pass (`WinError 4551`, exactly the known race
`run_host_tests.py`'s own module docstring names) — cleared on retry, no
code change needed.

**The other three `ui::` screens now have host tests too — done the same
day, 11 new tests.** `LibraryScreen`, `ReaderScreen` and `SelfTestScreen`
joined `SurahPickerScreen` in `test/test_ui/`, all compiling and passing on
the first try (`LibraryScreen`'s `rowCount`/`rowKind`/`categoryType`/
`entryOfType` against a real `net::LibraryIndex`; `ReaderScreen` against a
real `util::TextPager`-paginated page, not hand-built line slices;
`SelfTestScreen`'s `resultText()` and both subtitle branches). All four are
Arduino-free, so `platformio.ini`'s native `build_src_filter` now pulls in
`ui/` whole instead of cherry-picking `surah_picker_screen.cpp` alone — one
line instead of four, and nothing to remember to add for a future screen.
141 became **152/152**, then **154/154** after the icon-feature entry
below added two more, then **157/157** once `ui::QuranScreen` -- the one
screen flagged as still missing right below -- got its fixture and three
tests. `pio run` confirms the device build is still clean throughout.

**`ui::QuranScreen` was, for a few hours the same day, the one screen
without a host test — closed later in this same pass.** It was never
actually untestable: `qpk::Reader` already worked against a
`qpk::MemoryFile` the same way every `test_qpk` fixture does, and
`ui/quran_screen.cpp` compiled clean into the native test binary the moment
`ui/` was pulled in wholesale. What was missing was a *fixture* --
`MiniQuran` had no `FONT_METADATA`/`ASSETS`/shaped `WORD_INDEX`. See the
"`ui::QuranScreen` finally has a host test" entry in the "Done" list below
for how that was closed (`qpktest::BuildMiniShapedQuran()`, three new
tests, all 5 `ui::` screens now covered).

**The card is in the device.** A retitled shaped package and a first-refresh
fix are staged in `sdcard-staging/` and cannot be copied until the card is in a
reader. Nothing is broken by the delay.

---

## 0. To do on the Windows PC (added 2026-09-14)

**Where this stands, 2026-09-17:** the repository is on the PC and on GitHub
(`modfahad/diy-ebook`). Windows and macOS installers and an Android APK are
all *built* -- the installers by GitHub Actions, the APK by EAS -- from
`ec8fe92`, which includes adding and converting many books at once and
sending many to the device (desktop/README.md, android.md). **None of the
three has been run yet.** What is left is installing them and the checks
below.

**Touch (2026-09-19, driver 2026-09-20):** the panel is the -T01 version,
with a GT911 capacitive touch layer on its own 6-pin ribbon.

- **Run on the board 2026-09-19** with `env:touch_test`
  (`firmware/src/touch_test.cpp`): the GT911 answers at 0x5D (id "911", fw
  0x1060, 800x480), taps and drags track, and **the point data starts at
  0x814F** -- the one thing the driver could not settle without hardware. The
  wiring is GPIO 15/16/17/18 per the board header, and the ribbon pin order
  off the spec sheet's page 5 drawing is right. Steps:
  board-test-checklist.md section 11.
- The product firmware now *reads* touch: `drivers::TouchGt911` ->
  `drivers::InputManager` -> `hal::InputEvent{kTouch, ...}`, with the tap and
  mapping logic host-tested in `util/touch.h` (architecture.md 5c).
- **Still to settle on the board:** the three orientation flags -- which is
  now what `ui::SetupScreen` is for (architecture.md 5e), so it is eight
  presses on the device rather than eight reflashes -- and a few I2C read
  errors right after the first refresh: the bus has only the ESP32's internal
  pull-ups, so add 4.7k to 3V3 or drop to 100 kHz if they persist.
- **The driver, the menu and the setup screen have not been compiled** (Smart
  App Control blocks every compiler on the Windows PC) and have not run on the
  board. First compile and the new host tests are a Mac job:
  `python firmware/scripts/run_host_tests.py` and `pio run -d firmware`.
- The touch layer now sleeps with the screen (`app::kTouchSleepWithScreen`,
  ~100 uA instead of ~8 mA scanning). **Tap-to-wake is built but off**
  (`app::kWakeOnTouch`): it needs INT confirmed to idle HIGH and pulse LOW
  first -- the setup screen prints the live level -- because EXT1 is ANY_LOW
  and the wrong polarity is a device that never stays asleep. Turning it on
  also gives up that sleep current, since a sleeping chip cannot feel a
  finger. See architecture.md 5c and checklist section 15.

**The options menu (2026-09-20).** Hold OK on any screen opens
`ui::OptionsMenu` -- that screen's actions by name, worked by the wheel and OK
or by a tap (architecture.md 5d). This replaced every hidden hold: hold OK no
longer means bookmark / delete / text size / transfer mode by screen, and
**hold EXIT no longer factory resets** -- that is a row on the hardware test
screen's menu now, and it asks first. 10 new host tests in `test_ui`.
**Not compiled and not run on hardware**, same reason as everything above.
Taps now act: a library row or shelf tile opens (`ui::LibraryScreen::rowAt`),
and the left/right edges of the book, picture-book and Quran readers turn the
page through the wheel's own `HandleRotate()` (architecture.md 5d).
The saved places list and the surah picker take taps as well. Still open: a
hold on the glass deliberately does nothing (a thumb resting while reading
would open the menu), and the reading screens have no on-screen menu corner --
hold OK is how the menu opens.

**Screen and touch setup (2026-09-20).** The touch orientation and the
picture's rotation are found on the device now, not by reflashing:
Options -> "Screen and touch setup" (`ui::SetupScreen`, architecture.md 5e)
turns the picture, steps through the eight touch orientations against two
targets, and saves both to `/DEVICE/screen.txt` (`util::ScreenSetup`).
`board::kTouchSwapXY` and friends are still the defaults; the saved file wins.
11 more host tests. **Not compiled and not run on hardware.** The one thing
worth watching on the bench: `Epd750Display::setRotation()` replaced four
reads of `board::kDisplayRotation`, two of them in the partial-window and
restored-frame paths that were verified on real glass -- so check a rotated
device still does partial refreshes cleanly, not just full ones.

**The Windows PC can compile after all, through WSL -- 2026-09-21.** Smart
App Control blocks Windows binaries; it does not police the Linux ELF
toolchain inside a WSL2 VM. Ubuntu 24.04 needed one package
(`sudo apt install python3.12-venv`; g++ and git were already there), then
PlatformIO in a venv, and both the firmware and the host tests build and run:

```bash
python3 -m venv ~/.pio-venv && ~/.pio-venv/bin/pip install platformio
wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/d/interview/diy-ebook && ~/.pio-venv/bin/pio run -d firmware"
wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/d/interview/diy-ebook && ~/.pio-venv/bin/python firmware/scripts/run_host_tests.py"
```

Watch out for one trap: inside WSL, `pio` and `pip3` resolve to the *Windows*
ones through `/mnt/c`, which is the blocked toolchain again. Call the venv's
binaries by full path. Uploading to the board still needs usbipd-win to pass
the USB serial device through, or a flash from Windows (esptool is a Python
script, not a blocked binary).

**First compile of the touch work, same day: it built after two fixes, and
the host tests found a third.** `util::ScreenSetup` was brace-initialised in
`main.cpp`, which C++11 refuses for a struct with default member initialisers;
`run_host_tests.py` had drifted out of step with `platformio.ini`'s native env
and was missing `storage/bookmarks.cpp`, `reset_log.cpp` and
`verified_packages.cpp`, so *every* suite failed to link `bookmarks_screen.cpp`
(pre-existing, nothing to do with touch); and a new test asserted an empty
library category has nothing to tap when it still has a Back row. **268 host
tests pass (test_logic 57, test_net 66, test_qpk 72, test_ui 73), and
`crowpanel_579`, `touch_test` and `grey_test` all build clean.** Still not run
on the board.

**Touch runs on the board -- 2026-09-21, first real use.** Flashed from
Windows over COM6 (development.md has the recipe) and driven by hand. What it
showed:

- **The GT911 works and the driver reads it.** `raw=(283,375) size=35`, taps
  and drags both tracked, and a tap opened a library tile
  (`[library] OK on category Books`).
- **"Touch is not working" was the clock screen.** Every tap was being seen
  and then ignored, because the home screen was the one screen with no tap
  targets -- 80-odd taps logged `no tap targets yet`. A tap anywhere on it now
  opens the options menu, which is where Library and the rest are named.
- **The chip does not always answer the first probe.** One cold boot logged
  `no GT911 on SDA15/SCL16` and touch was dead until a reflash. `begin()` now
  tries three times, prints an I2C scan when it gives up, and can be retried
  without a reboot (`InputManager::retryTouch`) -- automatically once the
  rails have settled, and again whenever the setup screen is opened. This is
  the weak-pull-up problem the 2026-09-19 run flagged; the 4.7k resistors are
  still the real fix.
- **Every tap now says what it did**: `[tap] library: row 3 of 7, opening it`,
  `[tap] reader: x=700 -> page forward`, `[tap] home: opening the options
  menu`, and the ignored cases say why. The touch log line carries the mapped
  point, the chip's own numbers, the orientation and the screen.

**Touch went silent mid-session, twice -- 2026-09-21.** Taps worked for
minutes and then stopped, with nothing in the log mentioning touch at all:
`poll()` was getting I2C errors, returning false and saying nothing, so from
the outside it looked like the options menu was broken. The driver now counts
consecutive failures, says `[touch] I2C going quiet` at 20, and resets the
chip at 60 (rate-limited to one attempt per 5 s, so an unplugged panel cannot
become a reset loop). The heartbeat carries `touch=ok frames=N i2c_err=N`, so
the same silence is visible next time without a reflash.

**This is software working around a hardware weakness, and it should not be
mistaken for a fix.** The bus has no pull-up resistors of its own: 4.7k from
SDA (GPIO15) and SCL (GPIO16) to 3V3 is the real answer, and `i2c_err` in the
heartbeat is now the number that says whether they are needed.

**The touch bus goes fully dead, and that is hardware -- 2026-09-21,
still open.** Not bit errors: the whole bus stops answering. Caught in the
log as `[touch] no GT911 on SDA15/SCL16 (I2C scan: nothing)` after a run of
failures, and before that as reads of 0xFF everywhere -- `raw=(65405,65535)`,
`fw=0xFFFF res=65535x65535`, which is what an undriven bus looks like. It
comes and goes: touch worked for minutes, died, came back after a reset, died
again, and one cold boot never found the chip at all.

Weak pull-ups explain corrupted bits, not a chip that vanishes. **Suspect the
physical connection first:** the 6-pin touch FPC in its breakout, the solder
joints on the four flying leads, and the 3.3V feed to the touch layer. Reseat
the ribbon, check continuity from each header pin to the FPC pad, and check
3.3V at the panel end while it is failing. Then fit the 4.7k pull-ups on SDA
and SCL, which are needed regardless.

**The driver believed the garbage, which was its own bug.** `identify()` took
all-ones as a valid chip and announced `GT911 at 0x5D id="9#??"`, then
reported "recovered" and fed nonsense coordinates to the UI. It now requires
the product id to read "911", a firmware word that is neither 0 nor 0xFFFF,
and a sane resolution; anything else is logged as nonsense and the bus is
treated as empty. Frames of all zeros or all ones are dropped, as is any
point outside twice the chip's resolution.

**Still not settled: the orientation flags.** Taps land somewhere sensible at
`touch=0`, but nobody has tapped the two boxes on the setup screen to prove
it, and nothing is saved to `/DEVICE/screen.txt` yet.

**Smart App Control is on on this PC**, and it blocks every unsigned native
binary a local build needs: `rustc`, Tauri's CLI, Rollup's native module and
`hermesc.exe`. So neither app builds or runs from source here; build in CI
(desktop) and EAS (Android). Turning it off is a Windows Security setting that
cannot be turned back on without resetting Windows. It may also block the
unsigned installers themselves.

**Security:** the git remote URL on this PC embeds a GitHub personal access
token. Revoke it on GitHub and `git remote set-url origin
https://github.com/modfahad/diy-ebook.git`.

Before the repository goes anywhere more public, decide whether the ~20 MB of
built Quran `.qpk` files (`sdcard-staging/`, `desktop/converter/examples/`)
stay in it (docs/quran-content.md: the text is not redistributed from it).
`firmware/include/app/dev_secrets.h` is not in git: copy
`dev_secrets.example.h` and fill it in only where firmware is built.

### Windows desktop app

Either way below produces an `.msi` and an NSIS `.exe` installer.

- **GitHub Actions (no setup):** every push to `main` runs
  `.github/workflows/desktop.yml`, which tests the libraries, the bridge and
  the Rust side, builds macOS and Windows, and attaches the installers to the
  run as artifacts. Green for `b124d8b`, `fac43d0` and `ec8fe92`.
- **Locally on a PC without Smart App Control:**
  1. Install Node.js 20, Rust (rustup, MSVC toolchain) with the Visual Studio
     C++ Build Tools, and Git. WebView2 is already part of Windows 10/11.
  2. In `packages\qpk-format`, `packages\protocol`, `desktop\converter`,
     `desktop\device-client`, `desktop\app-bridge` and `desktop`, in that
     order: `npm ci`, then `npm run build` where the package has a build
     script, and `npm test` for the libraries.
  3. In `desktop`: `node scripts/prepare-standalone.mjs` (bundles the bridge
     and copies this PC's `node.exe` in as the sidecar), then
     `npm run tauri build`.
  4. Installers land in `desktop\src-tauri\target\release\bundle\msi\` and
     `...\bundle\nsis\`.

To check on Windows:
- the app starts from the installed copy, with no repository checkout present
  (Device tab → "bundled with the app")
- the Browse buttons, a TXT/EPUB/PDF conversion and a photo preview
- finding the device on the LAN and an upload, which needs the board in
  transfer mode (Windows Firewall may prompt)
- Library, many at once: pick several PDFs and EPUBs and see their titles,
  authors and languages fill in from their metadata; edit one, choose a
  cover, set "For every book", then "Convert & add" -- each lands under
  `BOOKS/`, a `.qpk` under its type's folder, and a failed book can be run
  again
- tick several packages and "Send N to device": one after another, with the
  batch stopping once (not per book) if the device is not in transfer mode

### Android app (`mobile/`)

Plan, milestones and details: [android.md](android.md).

**Install the built APK:** open the EAS build page on the phone (or copy the
APK from `mobile/builds/`) and install it; uninstall an older copy first if
Android refuses. New builds: `npx eas build --platform android --profile
preview` in `mobile` -- on this PC the only way, since a local release build
is blocked at `hermesc.exe` (above).

**Set up and run on the phone from source (a PC without Smart App Control):**
1. Install JDK 17 and Android Studio (SDK, platform tools). Set `ANDROID_HOME`
   to `%LOCALAPPDATA%\Android\Sdk` and add `%ANDROID_HOME%\platform-tools` to
   `PATH`.
2. On the phone: Developer options → USB debugging, connect by USB, accept the
   prompt; `adb devices` should list it.
3. Build the shared libraries first (step 2 of the desktop build above, for
   `packages\protocol`, `packages\qpk-format`, `desktop\device-client`,
   `desktop\converter`). Then, in `mobile`:
   ```
   npm install
   npm run worker
   npx expo prebuild --platform android
   npx expo run:android --device
   ```
   `npm run worker` builds the render worker page (pdf.js, the converter and
   picture decoding for a hidden WebView) into
   `src/render/workerHtml.generated.ts`; the app does not compile without it.

**First-run checks:**
- the app opens, and the address and token entered on the Dashboard survive a
  restart
- "Check connection" reaches the device over Wi-Fi (plain HTTP is allowed by
  `usesCleartextTraffic`)
- Device tab: status, library list, delete, installing a picked `.qpk` (the
  first real use of base64 chunk bodies) and a firmware update
- Photos tab: set time zone
- picking a file from Downloads/Drive reads correctly (`File.arrayBuffer()` on
  a `content://` URI)
- Hermes has `TextDecoder` (the converter needs it)

**Also check on the phone** (android.md milestones 5–7, built 2026-09-14 but
not yet run on Android):
- Photos: choose pictures, contrast −/+, upload, delete, send the time zone.
- Library: add `.qpk` files, covers shown, Validate, Send to device, Share,
  Delete.
- Library, many at once (in the EAS APK from `ec8fe92`, kept in
  `mobile/builds/`): "Add books…" with several PDFs and EPUBs -- details
  filled in from metadata, editable, a cover per book -- then "Convert & add";
  adding the same book again reports it already on the phone. Tick several
  and send them; a big batch of PDFs with "keep layout" is the memory test.
- Converter: a TXT, an EPUB (cover picked up automatically) and a PDF with
  "keep layout" (page pictures, progress, preview page turns); Save to
  library, Send to device, Share. A big PDF is the memory test.
- Converter, many at once (added after `ec8fe92`, needs a new EAS build):
  "Choose files…" with several PDFs and EPUBs, set author/language and the
  switches, "Convert N books"; each lands in the Library tab, a failed one can
  be run again, and choosing a single file still shows the preview.
- Title covers and untitled books (same build): a PDF or TXT with no title
  metadata shows an empty Title field asking for one, in the Converter (one
  or many) and in Library "Add books…"; with no cover picture the package's
  cover is the title in type (the Converter shows it before converting).
  Check an Arabic title on the device's library screen.

## 1. Blocked on materials

Two physical things gate more than their size suggests.

### A FAT32 microSD card — RESOLVED 2026-09-01

**The card is formatted and mounts.** Read straight off the board's serial
console:

```
[boot] Quran Reader fw=0.1.0-m1 boot=1 wake=cold boot gpio=-1
[sd] SDHC 59640 MB, layout=1 write=1
```

`layout=1` means `BringUpStorage()` created the directory tree, `write=1`
means it wrote to it. Mount, layout and write are all confirmed on the real
card — the last untested subsystem is no longer untested. The history below
is kept because the diagnosis in it is what made the fix a one-liner.

**Previously (2026-08-31, second session): the card was in the slot, the
device could reach it, and it was exFAT — so it did not mount.** The whole
item reduced to one action: format that card FAT32.

What the session settled, in full in
[bring-up-log.md §6](bring-up-log.md):

- The COM port was never missing. `Win32_SerialPort` under-reports USB-serial
  bridges; `Get-CimInstance Win32_PnPEntity` finds the CH340 on COM6
  immediately. The previous entry's guess about a missing driver was wrong.
- The card is present, powered and talking. Reaching FatFs's
  `FR_NO_FILESYSTEM (13)` requires `disk_initialize` to have completed the
  CMD0/ACMD41 handshake first, so `kSdPowerEn`, CS and the SPI wiring are all
  verified — by a failure.
- The SPI clock was ruled out as a cause: 4 MHz produced a byte-identical
  failure, so the filesystem is the only remaining explanation.
- exFAT cannot be made to work from this framework. `FF_FS_EXFAT` is hardcoded
  to `0` and `libfatfs.a` ships prebuilt, so the code paths are not in the
  binary at all. Rebuilding the IDF or swapping to SdFat would buy nothing —
  the largest file here is a few MB against exFAT's 4 GB threshold.

When formatting: FAT32, default allocation unit. On a card over 32 GB the
Windows dialog will not offer FAT32 — that ceiling is Microsoft's formatter,
not the filesystem — so use guiformat or Rufus, and *not* the SD Association's
formatter, which writes exFAT above 32 GB by spec.

Once it mounts, the first boot proves the rest by itself: `BringUpStorage()`
appends a line to `app::kFileSelfTest` on every success (see `main.cpp`), so a
passing `SD:` row plus a `Card: <n> MB` row means mount *and* write both work.

That reformat used to gate **four** separate things. All four are now
unblocked as *work that can proceed*, though none of them has been done yet —
nothing has been copied to the card, and the device is currently running the
`GLYPH_ATLAS_DEMO` build rather than the product build:

- **Real content.** Packages live on the card. Today the An-Naba pages are
  compiled into the firmware image, which is why flash sits at 55%.
- **Bushra's note on the device.** It is converted and staged at
  `sdcard-staging/LIBRARY/BOOKS/85c9fc7c….qpk`, ready to copy.
- **Six short surahs, staged and validated, ready to copy.** Al-Fatihah (1),
  Al-'Asr (103), Al-Kawthar (108), Al-Ikhlas (112), Al-Falaq (113) and An-Nas
  (114), each as a QURAN + companion TRANSLATION (Saheeh International)
  package pair, at `sdcard-staging/LIBRARY/QURAN/` and
  `sdcard-staging/LIBRARY/TRANSLATIONS/`. This is text/index content only —
  `SURAH_INDEX`, `PAGE_INDEX`, `AYAH_INDEX`, `WORD_INDEX` (word text, no
  coordinates), `TEXT_DATA`, and a `TRANSLATION_INDEX`/`TRANSLATION_DATA`
  package joined by `MetadataKey.AlignedContentId` — not rendered pixel
  pages, and nothing on-device reads a QURAN package yet (the reader,
  Milestone 5, isn't built). Both desktop validation profiles pass
  (`tools/quran-validator`, `tools/package-inspector`); the firmware's own
  C++ parser has not opened these specific bytes (no host compiler was on
  PATH this session — see `firmware/scripts/run_host_tests.py`'s Windows
  note). Two things worth knowing before building on this:
  - **SUPERSEDED 2026-09-01.** These six pairs have been deleted from
    staging and replaced by a single QURAN + TRANSLATION pair covering all
    114 surahs with their real chapter numbers — which also answers the
    "undecided, deferred" question below. See
    [quran-content.md](quran-content.md). The original note:
  - Each package declares surah id `1` internally, not the real chapter
    number — `SURAH_INDEX` is addressed as `[id - 1]` (see
    `quran-package.ts`'s header comment), so one package can only hold
    contiguous surah ids starting at 1. A set like {1, 103, 108, 112, 113,
    114} can't share one package without either renumbering (chosen here:
    real number lives in `metadata.title` and the surah's Arabic name, not
    the id) or extending the format to a sparse/searchable index — undecided,
    deferred.
  - Ayah `page`/`line` were placeholders (page 1, one ayah per line), not real
    Madinah-mushaf breaks. **No longer true as of 2026-09-02** — they are the
    mushaf's own pages and lines now; see "Page-boundary fidelity" below.
  - `fetch_translation.py`'s `clean()` had a latent bug fixed in the same
    pass: academic transliteration diacritics (Allah -> the same word with a
    macron, Muhammad -> the same word with an under-dot) survived into text
    meant for the panel's ASCII-only Latin font. Now stripped via NFKD
    normalization before the ASCII check.
- **Wake without the white flash — RESOLVED 2026-09-01.** `Epd579Display::setRestorePath()`
  persists the last frame and restores it on wake, skipping the
  clear-and-redraw. This item used to read "every log line so far reads
  `restored=0`" -- no longer true. Live serial from the real board, three
  separate wakes in a row (`boot=2`/`3`/`4`, each `wake=button/rotary`):
  every one reads `[epd] init=1 psram_fb=1 psram_total=8386231 restored=1`,
  followed by `[epd] repaint mode=partial ... in 621ms` -- not the ~2s
  `mode=full` flash a cold boot or a picker-entry `EnterSurahPicker()` still
  costs (both confirmed the same session, e.g. `ink=3720/27200 ... in
  2096ms` when opening the Quran package). The premise of this item was
  wrong, not just outdated: the feature already works, on the firmware
  already flashed. Nothing left to do here except notice it was already
  done.
- **The last untested subsystem.** Only SD's *failure* paths have run. Mount,
  read, write, and removal mid-write are all still unexercised.

### KFGQPC Uthmanic Script (Hafs) — RESOLVED 2026-09-01

**The font arrived and is standard Unicode.** `fonts/KFGQPC Uthmanic Script
HAFS Regular.otf`: 271 mapped codepoints, **zero** private-use, and **U+06D6
present** — the exact glyph that occurs 3x in An-Naba and was being dropped
via `--drop-unsupported`. The private-use risk this section warned about does
not apply to this build, checked before anything was built on it. The whole
Quran has since been fetched in Uthmani script and packaged; see
[quran-content.md](quran-content.md).

The old Naskh font stays in `fonts/` — the existing An-Naba assets were built
with it and deleting it would make them unreproducible.

**Original entry, kept for the reasoning:**

The Naskh Regular font in use lacks **U+06D6** (small high ligature sad-lam-
alef-maksura), which occurs 3× in An-Naba and is currently dropped via
`--drop-unsupported`. The Hafs font carries the full Uthmani repertoire and
would close the last gap between what is rendered and the true mushaf text.

Worth knowing before it arrives: some KFGQPC builds map glyphs to private-use
codepoints rather than standard Unicode Arabic. The coverage gate will say
which immediately — it reports the whole text as missing rather than rendering
something wrong.

### The battery (3000mAh) and its ADC pin

**Update 2026-08-31: resolved as "does not exist," not "not yet found."**
Pulled Elecrow's actual schematic for this board
(`Elecrow-RD/ESP32_S3-Ink-Screen` on GitHub, `5.79 Inch_ESP32_S3/Schematic
diagram/CrowPanel ESP32 Display-5.79(E) Inch.pdf`) and read the battery
block directly. It's battery connector → protection diode → `4054A` Li-ion
charger IC → a PMOS load switch into the 3.3V rail — no resistor divider
anywhere on the `BAT` net. Every other functional GPIO on the sheet carries
a descriptive net name (`IO41_LED`, `IO48_BUSY`, `IO7_LCD_3.3_CTL`, ...);
`BAT` only ever appears as a bare power-rail label, never paired with an
`IOxx`. So `board::kBatteryAdcPin = -1` (`board_crowpanel_579.h`) is correct
for this hardware revision, not a placeholder waiting to be filled in.

Turning the feature on for real needs a hardware mod: solder a resistor
divider from the `BAT` net to a spare ADC-capable pin (`IO14`/`IO15`/`IO38`
look unused on the header) and re-measure with a multimeter — physical work
only doable at the bench, not something to keep looking up. The
percent-and-remaining-time logic and UI row are already built and tested
against this unknown (`firmware/include/util/battery.h`, the "Batt:" row on
the self-test screen); until the mod happens, `IPower::batteryAvailable()`
correctly reports false and that row reads "--".

---

## 2. Milestone 5 — the reader

**Where this stands, 2026-09-01: the Quran is readable on the device.** The
content, the shaped atlas, the library browser and the reader screen all work
on real hardware. What is left is refinement and navigation, not "does it work
at all".

### Still open

| Item | Why it is not done |
|---|---|
| **The staged package is not on the card** | Staged as `fe9f9a8b…`, now carrying the distinguishing title, the Uthmani ayah markers *and* the mushaf's line breaks; the card still holds the older `fc7c07fe…aaf`. Harmless — the old one renders fine, by the fill-and-wrap path that is still there for exactly that reason — but the card is in the device, so it waits for the next swap. This is now the only thing between everything built on 2026-09-02 and the panel. |
| **Kashida justification** | Lines are set flush right; a real mushaf stretches them to full width with kashida. Unchanged by the layout work, and still what section 4 says it is: real typography work in the shaping tool, not a layout-data problem. |

### Done, most recent first

- **The refresh-mode log told the truth as of 2026-09-02, and one hypothesis for the first-refresh bug was eliminated from code.** Neither fixes the bug; both narrow it.
  - **`main.cpp` was logging the mode it ASKED for, not the one the panel ran.** `Epd579Display::flush()` silently escalates a `kPartial` to a full refresh every `kPartialsBeforeFullRefresh` (8) flushes, so the serial line read `mode=partial` while a 2,096ms full-panel waveform was running -- the escalation was invisible, and only the duration gave it away. That cost real time on this very investigation. `hal::IDisplay` gained `lastRefreshMode()`, set on every branch of `flush()` including the escalation, and the log now prints what actually ran plus an explicit `(escalated)` marker when the two differ. It also prints `fast`, which it previously could not say at all -- `kFast` was being reported as "partial".
  - **The shared-SPI-bus half of the standing hypothesis is ruled out, by reading the board header rather than by measuring.** This doc's working theory was "a shared-SPI-bus or rail-settling effect specific to doing real SD I/O in the window between the display's own init-refresh and a content flush". The two halves are not equally likely: the panel is **bit-banged on GPIO 12/11/45/46/47/48** and the SD card sits on **its own HSPI bus, GPIO 39/13/40/10** ([board_crowpanel_579.h:76](../firmware/include/board/board_crowpanel_579.h)) -- the header says so in as many words, and no pin is shared. There is no bus contention to have. What survives is the **rail-settling** half: SD I/O at 40MHz drawing current while the panel rail is still settling, which is the scope measurement section 5 already lists.
  - **What is still true, and worth not re-deriving:** the framebuffer is provably correct at flush time (`ink=4580/27200` is counted from it), `flush()` reports success, and the elapsed time is a genuine full-refresh duration -- so the failure is between a correct framebuffer and the glass, not upstream of it. Also ruled out previously: glyph data (byte-verified), word positioning (bbox verified), glyph-specificity (a filled-rectangle control test reproduced it), and a 150ms settle delay (did not fix it).
  - **The timing is split now.** Every repaint line carries `(write=Nms wave=Nms)`: streaming 27KB over bit-banged SPI, and the waveform actually driving the glass. One combined figure hid which half was slow, and they fail for entirely different reasons -- a first content flush anomalous in the *streaming* half is not a rail-settling bug at all.
  - **Five candidate fixes are selectable at runtime, behind `-DREFRESH_EXPERIMENT=1`** (`pio run -e refresh_experiment -t upload`, then a serial monitor; `?` prints the menu). One flash tests all of them against the panel, which is the scarce resource here -- not compute:

    | Key | Strategy | What it would prove |
    |---|---|---|
    | `0` | `baseline` | what ships: `writeFrame` then `updateFull` |
    | `1` | `double-write` | streams the frame **twice** before the waveform. Tests the original observation head-on -- if this fixes it, the first stream is not landing and this was never a waveform problem |
    | `2` | `sync-previous-ram` | also seeds `0x26`/`0xA6`. Tests whether the previous-frame reference is stale |
    | `3` | `reload-fast-lut` | re-forces the 100C LUT before the waveform |
    | `4` | `fast-waveform` | `updateFast` (`0xC7`) instead of `updateFull` (`0xF7`) |

    `f` forces a full repaint; the strategy name is appended to every repaint line so the scrollback says which strategy produced which result.
  - **Why strategy 3 is the one to try first, and what it rests on.** `fastModeInit()` forges the temperature register to 100C (`0x1A = 0x64`) and loads the OTP waveform for it -- the standard fast-mode trick. But `updateFull()` sends `0x22 = 0xF7`, which sets *both* "load temperature" and "load LUT", so a full refresh reloads the waveform for the panel's **real** temperature and discards the forged one. That makes the first full refresh after init structurally different from every later one, which is the exact shape of this bug. **This is a reading of the SSD1683 bit definitions, not a measurement** -- it is a hypothesis with a mechanism, which is why it is behind a flag and not in the product build.
  - **Flashed and measured on real hardware, 2026-09-02 — and the split timing already eliminates two of the three possible failure sites.** The experiment build is on the device (COM6) and answering serial. A forced DTR/RTS reset gives the exact failing case, the first content full refresh after a cold-boot init clear:

    ```
    [boot] Quran Reader fw=0.1.0-m1 boot=1 wake=cold boot gpio=-1
    [sd] SDHC 59640 MB, layout=1 write=1
    [epd] init=1 psram_fb=1 psram_total=8386231 restored=0
    [epd] repaint mode=full ink=3071/27200 flush=1 in 2096ms (write=242ms wave=1854ms) strategy=baseline
    ```

    Against a later, known-good forced full refresh in the same session:

    ```
    [epd] repaint mode=full ink=6104/27200 flush=1 in 2097ms (write=243ms wave=1854ms) strategy=baseline
    ```

    **They are indistinguishable.** Frame streaming is 242ms against 243ms; the waveform is 1854ms against 1854ms — to the millisecond. A partial refresh in the same session streams in 242ms too, so the streaming half is flat at ~242ms regardless of mode, which is what a healthy bit-banged transfer should look like.
  - **What that rules out, without a scope.** The first refresh is not short-changed on transfer time, so the frame is not being partially streamed. It is not short-changed on waveform time either, so the panel is not skipping or aborting the update — it runs the full 1.85s of drive. **The panel does the same electrical work on the refresh that does not commit as on the one that does.** That is inconsistent with a data-transfer bug and with a timing/settling bug, and consistent with the waveform being *the wrong waveform* — i.e. which LUT is loaded, not whether it ran. It moves strategy 3 (`reload-fast-lut`) from "plausible mechanism" to "the hypothesis the measurement points at".
  - **Still unmeasured, and only the panel can say it:** whether any strategy actually makes the first refresh commit. Nothing in serial distinguishes a committed refresh from an invisible one — that is exactly why this bug survived so long, and it is why the strategies need a human looking at the glass.
  - **Confirmed in passing:** the device mounts the card (SDHC 59,640MB) and the reader takes the **fill-and-wrap path** on it (`drew 2 ayahs, 0 lines, 302 glyphs` — `lines=0` means `used_layout` was false). That is this session's additive design working on real hardware: the old package on the card has no `LAYOUT_DATA`, and the device correctly declines the layout path rather than mis-rendering. The layout path itself is still unseen on the panel.
  - **Not shipped in the product build, deliberately.** Every strategy touches the refresh path of a display that currently works, and none of them can be verified without looking at the panel. `pio run` (the product env) is unchanged apart from the honest log line.


- **`LAYOUT_DATA`-driven line breaks — the Madinah mushaf's own, built and host-tested, rendered off the real package on the host; not yet seen on the panel. 2026-09-02.** The last of section 2's three open items, and the one this document said was blocked on data that did not exist.
  - **The premise was wrong, and that is the finding.** This doc said the item "depends on the page-boundary data below, which is still unsourced", and section 4 said closing it "needs the canonical Madinah line-break data, which nothing in this repo has yet". True about the repo; false about the world. quran.com's `/verses/by_page` returns a per-word `line_number` and groups verses by page — that *is* the Madinah mushaf's line breaks, and it is one endpoint away. 604 pages fetched in about ten minutes.
  - **Two data traps, both caught by checks rather than by luck.**
    - *The two sides disagree about what a word is.* Their per-page word units attach a waqf mark to the word before it (`"رَيْبَ ۛ"` is one unit there, two tokens here); their edition carries tajweed marks ours lacks (U+06ED 4,807 times against 99); a few verses differ only in combining-mark **order**; 5:52 has a spurious internal space and 11:13 differs orthographically. Zipping the two sequences gave **82,011 against 77,429** — a 4,582-word drift that would have misaligned the whole package after Al-Baqarah, which is precisely the phantom-word bug class [quran-content.md §10](quran-content.md) exists to warn about. Words are matched on their **consonantal skeleton** instead — every mark dropped, alef forms folded — with a greedy many-to-many walk that absorbs both their spurious splits and their space-joined units. Result: **6,236 of 6,236 verses, 82,011 of 82,011 words.**
    - *Their per-word `page_number` is sometimes wrong.* `by_page/121` groups verse 5:77 onto page 121 correctly but stamps every one of its words `page_number: 120` — a page whose first three lines are already occupied by 5:71 and 5:72. Trusting it put 5:77 back at the top of page 120 and made the corpus read backwards. **The validator written for this pass is what caught it**, not inspection: rule 17's ordering check rejected the package with "LAYOUT_DATA line 1779 starts at word 16370, before line 1778's 16473". The request's own page is authoritative; the fetcher now asserts `(page, line)` never decreases across the whole 604-page sweep, and re-running gave **0 backwards transitions, 0 non-contiguous line spans**.
  - **Our word sequence was never touched.** That was the constraint the whole aligner exists to satisfy: an aligner that "fixed" our splits to match theirs would have forced a re-shape and re-broken the shaped-vs-text alignment `build-full-quran-shaped.mjs` guards. The atlas, the shaped `WORD_INDEX` and the ayah-marker glyph ids are byte-identical before and after; only a `(page, line)` label is added alongside.
  - **The design decision was a measurement, not a preference.** A mushaf line has to fit 756 usable panel pixels. Measured across all 8,817 lines at the atlas's 36px: median **619px**, p90 **710**, p99 **789**, and **2.78% over**. The tempting move — re-rasterize the atlas smaller so every line fits — is ruled out by the same numbers: the widest line is **1,305px** and is still over at 28px, so a global downscale would cost harakat legibility on every line, on a panel this doc already records as fading thin strokes, and *still* not fix the outliers. So: keep 36px, honour the mushaf's breaks, and wrap the ~2% that overflow onto a second panel row.
  - **Additive, like the markers.** `ui::QuranScreen` takes the layout path only when `LAYOUT_DATA` is present and keeps fill-and-wrap otherwise — so the package on the card today still renders exactly the way that was confirmed on hardware, and if the new layout looks wrong on the panel the fix is swapping a card file, not reflashing. Host-tested by rendering the same fixture both ways.
  - **`net::ReadingProgress`'s on-disk format did not change**, deliberately: it was confirmed on hardware on 2026-09-01 and stores an *ayah* index. `QuranScreen::seekToAyah()` is the bridge — ayah index in, line cursor out — so the surah picker, the resume path and the saved position all keep speaking ayahs while the renderer pages by line.
  - **Two O(log n) accessors, because "never scan" is a rule.** `Reader::findLineByWordIndex()` and `findAyahByWordIndex()` binary-search `LAYOUT_DATA` and `AYAH_INDEX`. That is only correct if the records ascend, so what was a *writer discipline* in the spec is now **wire invariant rule 17** — enforced in `validate.ts`, not at device open, because proving it costs a full sweep of 8,820 records and that is exactly the open-time scan §1.1 forbids.
  - **Backward paging got simpler as a side effect.** On the layout path one line back is always a legal position, so the 32-deep history stack is a convenience rather than the only way back — which it had to be when line breaks were "wherever it filled" and the previous screen's start could not be computed.
  - **`PAGE_INDEX` is real now too**, and it had to be: `AYAH_INDEX` claiming page = surah id while `LAYOUT_DATA` claimed real mushaf pages would have been actively contradictory. 114 fabricated pages became **604 real ones**, contiguous, 15 lines each. That also closes section 4's "page-boundary fidelity" item.
  - **A latent converter bug surfaced with real data.** `quran-package.ts` keyed each `LAYOUT_DATA` line on the *ayah's* page rather than the *word's*. Harmless under placeholder one-page-per-surah data, silently wrong the moment an ayah straddles a page boundary — which real mushaf ayahs routinely do. `QuranSourceWordBox` gained a `page`, and the page's line count is now the highest line any word on it occupies rather than the highest line any ayah *starts* on.
  - **Seen, not just counted.** The `quran-package` preview scene renders a real `.qpk` through the actual `ui::QuranScreen`. Al-Fatihah comes out set the way the Madinah mushaf sets it: the basmalah alone on its line, then `ٱلْحَمْدُ لِلَّهِ رَبِّ ٱلْعَـٰلَمِينَ`, then two ayahs sharing the third. That is a structure fill-and-wrap could not have produced by accident.
  - **Verified on the shipped package:** `LAYOUT_DATA` 8,820 lines over 604 pages, line ids 1..15, ordering invariant holds end to end, line 0 starts at word 0 and the last line at word 82,008 of 82,011. Package 5.10 → 5.21 MB.
  - **Swept all 114 surahs, not the four that were looked at.** `RenderLayoutPath` has two silent `break`s -- a span past `kMaxWordsPerLine`, and a failed `getLine` -- either of which would show up as one blank or half-drawn surah and nothing else. Every surah's opening screen was rendered through the real screen code and asserted to draw at least one line and at least one glyph: **0 of 114 failed**, surah 9 (no basmalah) and surah 114 (last three lines of the mushaf) included.
  - **A resume bug found and fixed before it shipped.** On the layout path `next_ayah_index` only advances when an ayah *ends* on screen, so a screen in the middle of a long ayah -- 2:282 runs for pages -- reported the ayah the screen *started* in. Forward paging was persisting that as the reading position, so resuming a long ayah would silently reopen several screens back. Fixed by making forward paging derive the ayah anchor from the line cursor, exactly as backward paging already did (`QuranSyncAyahToLine`), which makes `next_ayah_index` purely informational on that path. The same slip was in the surah-label correction at the render call site: it reset the ayah anchor while only meaning to fix the surah name.
  - **A wrong claim in a comment, corrected by measuring it.** `kMarkerSlotWidth = 28` was documented as an upper bound over both marker paths. It is not: `gfx::Canvas::textWidth` is `n * (5 + 1) * scale - scale`, so the Latin fallback `"(7)"` at scale 2 is **34px** and `"(286)"` is **58px** -- both wider than the atlas glyph's 28. Budgeting 28 for a fallback marker would have measured a line shorter than it draws, and wrapped it in the wrong place. `MarkerSlotWidth()` now picks the same branch `DrawAyahMarker` will. It could not bite on the shipped package, where every marker resolves -- which is exactly why it was worth catching rather than leaving as a latent trap for the next package.
  - **`quran-full.qpk` changed bytes too** (`PAGE_INDEX` 114 → 604 records), keeping content id `fc7c07fe…` and `content_version` 1 -- the same situation recorded for the shaped package in the ayah-marker entry below, and harmless for the same reason: nothing decides *not* to reinstall on `(content_id, content_version)`, and `upload_manager.cpp`'s resume check also compares size and CRC, both of which changed. Recorded for both packages now, not just one.
  - **`run_host_tests.py` reported `test_ui FAILED` on two consecutive full runs and 26/26 with 0 failures when that suite is run alone.** Not a flaky test: the script's own retry loop printed `[WinError 4551] An Application Control policy has blocked this file` three times and gave up. Same environment problem that made `render_ui_preview` unrunnable from a temp directory and then from a path ending in `.exe` -- it currently only runs as `.pio/host-tests/preview/tests`, with no extension. Worth naming plainly because it makes "168/168" a claim about a suite that has to be coaxed into launching, not one that runs clean on demand.
  - **Touched:** `fetch_mushaf_layout.py` and `build_mushaf_layout.py` (both new), `build_full_quran_source.py`, `quran-package.ts`, `schema.ts`, `validate.ts` (rule 17), `build-full-quran-shaped.mjs`, `qpk_reader.{h,cpp}`, `ui/quran_screen.{h,cpp}`, `main.cpp`, `render_ui_preview.cpp`, the `MiniShapedQuran` fixture, and three docs. **Host tests 163 → 168, all passing; `pio run` clean; both packages rebuilt and revalidated.**

- **Uthmani ayah markers — built, host-tested, rendered off the real package on the host; not yet seen on the panel. 2026-09-02.** This was one of section 2's three open items, and it turned out to be far smaller than this document claimed, because the claim rested on an assumption nobody had checked.
  - **What this doc said, and why it was wrong.** The entry above read: U+06DD is a *combining enclosing* mark whose digits the shaper composes, so fixing it means shaping markers separately and adding them to the atlas. Half right. Shaping `U+06DD` + Arabic-Indic digits through HarfBuzz against the KFGQPC font produces **two** glyphs — a precomposed circled numeral *and* a bare empty circle, which would draw the mark twice. Shaping the **digits alone** (`U+0660`..`U+0669`, no U+06DD at all) produces **one** glyph: the font's own precomposed ornate circle with the number already inside it. Measured across every number an ayah can have: **all 286 shape to exactly one glyph, all 27×33 px at 36 px, all 286 distinct, no collisions.** So there was never a per-word run to build — one glyph per ayah *number*, 286 of them, full stop.
  - **That collapses the design.** Markers are dense and one glyph each, so they need no per-ayah field and no lookup table: the shaper mints them **contiguously and last**, and the device resolves ayah *n* as `first_glyph_id + (n - 1)`. The only thing that has to be recorded is where the range starts, which is two numbers — one `METADATA` key, `"1508:286"`.
  - **Nothing in the wire format changed.** No new section, no record-size change, no format version bump. `METADATA` is a key-value blob precisely so new fields never touch a struct, and an unknown key is skipped by both readers already. The three alternatives were all worse and are recorded in [qpk-format.md §9b](qpk-format.md) so this is not re-litigated: `AYAH_INDEX` is 24 bytes with no spare padding (unlike `.meta`'s offset 24, which is what made `expires_at` free); appending marker "words" to `WORD_INDEX` would break the shaped-vs-text positional alignment that `build-full-quran-shaped.mjs` exists to guard; a dedicated section is a format change for two numbers.
  - **The alignment invariant was preserved and then proved, not assumed.** The atlas was regenerated from scratch and diffed against the old one: **82,011 words byte-identical** — same glyph ids, same text, same ordering — and **all 1,507 pre-existing glyph records unchanged**. The 286 markers are purely additive, ids 1508..1793. The package grew 5.06MB → 5.10MB.
  - **Verified against the real Quran, not just the fixture.** On the built `quran-full-shaped.qpk`: the key reads back `"1508:286"`, all 286 marker ids resolve in `FONT_METADATA`, **zero** of them collide with the 1,507 glyph ids the words actually use, and the highest ayah number in all 6,236 ayahs is 286 — exactly covered, no gaps.
  - **Seen, not just counted.** `firmware/tools/render_ui_preview.cpp` gained a `quran-package` scene that opens a real `.qpk` off disk and renders it through the *actual* `ui::QuranScreen` — the same code the device runs. Al-Fatihah and An-Naba both come out with real ornate Uthmani marks between verses where `(1)`/`(2)` used to be. This is the strongest verification available while the card is in the device, and it is worth having permanently: until now the only way to see a screen was to flash hardware.
  - **The Latin `(7)` was kept, deliberately.** It is still what a package built before markers existed draws, and still what gets drawn when a package *claims* a marker range its atlas does not hold. A hole where a verse boundary should be would be worse than an obviously-Latin stand-in. Both paths are host-tested by glyph count: markers present blits exactly one more glyph per ayah than the same fixture without the key, and a package claiming `"900:3"` blits exactly the same as one claiming nothing.
  - **The parser refuses rather than half-succeeds.** `Reader::ayahMarkerGlyphs()` rejects an empty value, a missing field, a wrong separator, trailing junk, a zero count, and a range that would wrap past `u16` — eleven malformed values are host-tested. A partially-filled pair would not be an error on screen, it would be the *wrong glyph* on screen.
  - **Item 3 came along for free.** The rebuild produces the retitled package as a side effect, and the content id is unchanged (`fe9f9a8b…`, derived from title/language/script only), so the staged file is the same name — now carrying both the title fix and the markers. Still not on the card.
  - **Which packages actually have markers, so this is not read as universal.** Only `quran-full-shaped.qpk` (staged as `fe9f9a8b…`) — checked, not assumed. `al-fatihah-glyph-atlas.qpk` and `sdcard-staging/DEVICE/glyph-atlas-demo.qpk` are built from a *different* atlas (`data/all/glyph-atlas.json` is not what `build-fatihah-glyph-atlas.mjs` reads), still carry no marker key, and still draw the Latin `(n)`. That is the fallback working as designed, not a gap to close: the demo package exists to prove the blitter, not to be read from. (`sdcard-staging/DEVICE/glyph-atlas-demo.qpk` was deleted on 2026-09-14, after the `GLYPH_ATLAS_DEMO` build that opened it was removed; `al-fatihah-glyph-atlas.qpk` in `desktop/converter/examples` still shows the same thing.)
  - **`content_version` is still 1 on a package whose bytes changed** (5.06MB → 5.10MB, same `fe9f9a8b…`). Checked whether anything decides *not* to reinstall on `(content_id, content_version)`: the only comparison is `upload_manager.cpp`'s resume check, and it also compares `size` and `payload_crc32`, both of which changed — so a stale session is correctly discarded, not wrongly resumed. Left at 1 deliberately: the shaped build passes the source package's version through on purpose, and changing package identity fields is not something to do in a session that cannot verify on hardware. Recorded so the next pass does not re-derive it.
  - **`render_ui_preview.cpp` is now built by `run_host_tests.py`**, compile-only (it has its own `main()`, so it cannot join a suite, and nothing asserts on it). Without that it would rot silently the next time a `ui::` signature moves — which is exactly what happened to the whole test suite during the no-compiler period.
  - **Touched:** `shape_glyph_atlas.py` (marker pass, `--no-ayah-markers`), `packages/qpk-format` + `qpk_format.h` (`kAyahMarkerGlyphs = 10`), `quran-package.ts` (`extraMetadata`), `build-full-quran-shaped.mjs` (density + coverage checks), `qpk_reader.{h,cpp}`, `ui/quran_screen.cpp`, `render_ui_preview.cpp`, `run_host_tests.py`, the `MiniShapedQuran` fixture, and both docs. **Host tests 157 → 163, all passing; `pio run` clean; `packages/qpk-format` 23/23 and `desktop/converter` 44/44 still green.**
  - **One run of `run_host_tests.py` reported `test_qpk FAILED` and passed on re-run**, with no code change between them. Recorded rather than smoothed over: it is the Windows Application Control race the script's own docstring names, and it hit `render_ui_preview.cpp`'s binary in the same session hard enough that it had to be built under `.pio/host-tests/` to run at all — a temp-directory build was refused outright, repeatedly, not just on first launch. So "163/163" here means "163/163, once, after a known environment flake ate one suite", not "163/163 every time".

- **First real hardware verification of this whole session's work — 2026-09-01, the card is in the device and the device is on the bench.** Flashed the product build (`pio run -t upload`) and watched a live serial monitor while the surah picker, reading-position persistence and transfer-mode footer all got exercised by hand for the first time. This is the first entry in this doc that says "confirmed on hardware" and means it for today's features, not carried over from an earlier pass.
  - **Surah picker confirmed working, with real content.** Serial:
    `[quran] opened /LIBRARY/QURAN/fc7c07fe71673a560b9548a921f29aaf.qpk, 114
    surahs, 6236 ayahs` immediately followed by a **full** repaint
    (`ink=3720/27200 ... in 2096ms`, matching `EnterSurahPicker()`'s
    `g_force_full_refresh = true`) rather than jumping straight into surah
    1 -- and the person at the keyboard confirmed the picker screen itself
    "shows surah names", i.e. `app::kSurahNames` (the generated Latin-name
    table) is rendering real, non-blank, non-placeholder text on the real
    panel. This is also confirmation that `fc7c07fe…aaf` -- the older,
    unshaped, ambiguously-titled package §2's "Still open" table says is
    still the one on the card -- carries a complete, real `SURAH_INDEX`;
    the picker only needs that, not the glyph atlas `QuranScreen` needs, so
    it works on this package even though the shaped one is still staged,
    not copied.
  - **A real, minor rough edge found by this**: opening the Quran package
    logged `[E][vfs_api.cpp:105] open(): /sd/USER/progress.bin does not
    exist, no permits for creation` -- the ESP32 SD/VFS layer's own error
    log for `IStorage::read()` on a missing file, printed by the framework
    itself, not this codebase. Harmless: `LoadReadingProgress()` already
    treats a failed read as "no saved progress" and correctly fell through
    to the picker (there was nothing to resume, first time the file was
    ever touched). But it is unconditional noise on every fresh-content
    open until a progress record actually exists to be read successfully,
    worth knowing about before mistaking it for a real error in a future
    serial log.
  - **Transfer mode: the mechanism fired, the footer text itself not yet
    visually confirmed.** `[library] OK on Transfer Mode row`, then several
    `NO_AP_FOUND` retries (normal Wi-Fi association noise), then
    `[transfer] http server up, free_heap=162992`, then the very next
    repaint's ink count jumped from ~3071 to 3189/27200 -- consistent with
    `LibraryScreen`'s new `"Transfer mode: <ip>:<port>  hold OK to stop"`
    footer actually drawing (the old footer text was shorter) -- but this
    is inferred from ink delta, not read off the physical screen. **Still
    needs**: someone looking at the panel to confirm the IP:port string
    itself is legible and correct, not just "some text changed".
  - **Also confirmed, incidentally**: SD mount (`SDHC 59640 MB, layout=1
    write=1`), all four inputs (WHEEL/OK/EXIT/MENU) registering and
    triggering the right handler, and sleep/wake cycling correctly across
    three separate wakes.
  - **Not yet exercised this pass**: the new library icons (five rows
    render with icons now, per the earlier preview-tool confirmation, but
    nobody has looked at the physical panel to confirm they're legible at
    real e-ink contrast, not just in a PNG); the chunk-PUT diagnostics
    (needs a real upload attempt); actually reading a surah in
    `QuranScreen` from the picker (only entering the picker was tried).
- **`ui::QuranScreen` finally has a host test — built 2026-09-01, all 5 `ui::` screens now covered.** The gap this doc named a few entries ago ("what is missing is a *fixture*") is closed:
  `qpktest::BuildMiniShapedQuran()` (`test/test_qpk/qpk_test_package.h`, new)
  is a shaped variant of the existing `MiniQuran` fixture -- same 2-surah/
  5-ayah/12-word shape (so surah-boundary paging gets exercised, not just a
  single ayah), plus 3 synthetic glyph bitmaps (obviously not real Arabic,
  per this file's own rule) cycled across the 12 words, real
  `FONT_METADATA`/`ASSETS`/a shaped `WORD_INDEX`. Three new tests in
  `test/test_ui/`: a real render reports `ayahs_drawn > 0` and
  `glyphs_drawn > 0` (not just "didn't crash"), paging past surah 1's last
  ayah into surah 2 renders real glyphs there too, and the existing plain
  `MiniQuran` (no atlas) still correctly reports `unsupported`.
  - **A real, if narrow, C++11 bug hit and fixed while building the
    fixture**: passing a `static constexpr` class member straight to
    `std::vector::push_back(const uint8_t&)` ODR-uses it, which needs an
    out-of-class definition a shared header (included by every test suite's
    own translation unit) cannot cleanly provide before C++17's `inline`
    variables -- a real link error (`undefined reference to
    MiniShapedQuran::kGlyphWidth`), not a hypothetical one. Fixed by
    copying to a local before `push_back()`, the same fix pattern noted for
    future readers in the header comment beside it.
  - **Verified visually too**, via the new preview pipeline below: a
    `quran-shaped` scene in `tools/render_ui_preview.cpp` renders the
    fixture through the real `ui::QuranScreen::render()`, confirming by eye
    that the three ayahs' glyph clusters and `(1)`/`(2)`/`(3)` markers land
    in correct right-to-left order (ayah 1 rightmost/read-first through
    ayah 3 leftmost) -- something the numeric test assertions alone
    couldn't show.
  - **Still true**: nothing here has run on real hardware, and the fixture
    is synthetic, not the real shaped atlas -- this proves the mechanism,
    not that Al-Fatihah's actual glyph atlas draws correctly (that was
    already confirmed on real hardware in an earlier pass, see the glyph
    atlas entry below).
- **Library home screen: icons per row, and the transfer IP:port shown where transfer mode is actually toggled on — both built 2026-09-01, verified visually (a first for this project) but not on hardware.**
  - **The transfer address was already plumbed into `ui::LibraryState`
    (`transfer_address`, set by `main.cpp`'s `PollTransferMode()`) but
    `LibraryScreen::render()` never drew it** -- only `SelfTestScreen` did,
    reachable by pressing MENU away from the library, which is the product
    build's actual default screen and where Transfer Mode is actually
    toggled on (its trailing row). Fixed: the footer now shows
    `"Transfer mode: <ip>:<port>  hold OK to stop"` once connected, or the
    connecting hint while pending, with `status_message` still taking
    priority over both (same precedence every other footer branch here
    already has).
  - **Five new 16x16 1bpp icons**, one per category row plus Transfer Mode
    (`firmware/include/app/library_icons.h`, generated;
    `firmware/scripts/gen_library_icons.py`, new). Distinct, simple shapes
    chosen to survive the row height (23px) and the format's own hard
    lesson from this pass: curves do not render cleanly at 16x16 with PIL's
    `arc()`/cut-circle tricks -- two separate attempts (a wifi-arcs icon, a
    crescent-moon icon) broke up into disconnected dots and had to be
    redone as straight-line shapes (signal bars; a 4-point star). Wired into
    `ui::LibraryScreen::render()`'s categories view only, not the items
    view, where every row is already the same type.
  - **What actually made this possible: a real preview pipeline, not
    guessing.** `firmware/tools/render_ui_preview.cpp` (new) links the real
    `ui::LibraryScreen::render()` against a plain (rotation 0, no seam)
    host-built `gfx::Canvas` and dumps the raw framebuffer;
    `firmware/scripts/raw_canvas_to_png.py` (new) converts that to a real
    PNG; `firmware/scripts/preview_ui.py` (new) wraps both end to end,
    reusing `run_host_tests.py`'s compiler discovery. This did not exist
    before today because there was no host compiler to build it with --
    every icon and layout question up to now had to be answered by
    reasoning about coordinates and flashing real hardware to see the
    result. `gen_library_icons.py`'s own `preview` mode used the same
    approach at the single-icon level (a 12x zoomed sheet) before any shape
    was committed to the real header.
  - **Not verified**: nothing here has run on the physical panel. The
    preview pipeline renders the exact same `ui::LibraryScreen::render()`
    the device calls, through the same `gfx::Canvas`, so what it shows is
    the real logical layout -- but only a real e-ink refresh confirms ink
    actually lands where the buffer says it should.
- **Upload chunk-PUT diagnostics — added 2026-09-01, compiles clean, not yet
  run (needs the device on Wi-Fi).** docs/pending.md's own open question --
  "which of `HandleUploadChunk`'s three `kBadRequest` exits fires... one
  `Serial.printf` of the first line `ReadChunkedBody` actually reads would
  settle it" -- answered with instrumentation, not yet with data. Every
  distinct failure point now logs which one it is before returning: the
  missing/malformed `X-Qr-Offset` header (two separate log lines, one per
  cause), and, inside `ReadChunkedBody` itself, the first line it ever
  reads plus which of its three own `kBadRequest` returns fired (empty/
  malformed size line, a size line that is not valid hex, or a short body
  read with the exact byte count and `readBytes()` return value). The next
  real upload attempt against hardware will name the exact line, not just
  the error class.
- **Surah picker and reading-position persistence — built 2026-09-01, unverified on hardware or by host tests (card is in the device; no host C++ compiler on PATH this session, same two structural gaps as before).**
  - **`ui::SurahPickerScreen`** (`firmware/include/ui/surah_picker_screen.h`,
    `firmware/ui/surah_picker_screen.cpp`, new): a screen between the library
    browser and `ui::QuranScreen`. `main.cpp`'s `OpenQuran()` now lands here
    instead of jumping straight to surah 1; EXIT from the reader comes back
    here (not all the way to the library) so switching surahs does not mean
    re-opening the package -- it stays open the whole time either screen is
    up, same handle. Rows read live off `qpk::Reader::getSurah()` --
    `SURAH_INDEX` is addressed `[id-1]`, so each row is a direct-index
    lookup, not a scan, matching `library_index.h`'s "never scan for the
    reading position" rule.
    - **A correctness bug caught in review, fixed before this entry was
      written**: the row loop originally skipped a row outright when
      `getSurah(item + 1)` failed, assuming a well-formed package always has
      `SURAH_INDEX[i].surah_id == i + 1`. That is true by the format's
      contiguous-ids rule for slot *contents* the package builder wrote
      correctly, but `getSurah()`'s own consistency check (rule 14, exact
      same case `test_surah_index_slot_must_hold_that_surah` in
      `test_qpk` covers) is enforced lazily on each call, not eagerly at
      `open()` -- so a package that opened successfully can still fail
      `getSurah()` here, and the skip would silently make `rowCount()`
      disagree with what was actually drawn. Now draws a numbered
      `"N. (unavailable)"` fallback row instead, so the two can never
      disagree.
  - **Row labels needed a Latin name, and the QPK format could not gain one
    without regenerating the already-staged 5.3MB shaped package** (the card
    it would need to be re-verified on is inside the device). Fixed with a
    generated, compiled-in table instead:
    `tools/arabic-pager/gen_surah_names.py` reads the 114 chapters' plain-
    ASCII `name_simple` field out of `data/all/chapters.json` (already
    fetched) and writes `firmware/include/app/surah_names.h` -- same
    "GENERATED FILE, DO NOT EDIT" convention as `render_pages.py`'s page
    header. ~2.5KB against 55% flash. These are Latin transliterations for
    menu chrome, not the mushaf's Arabic surah names.
  - **`net::ReadingProgress`** (`firmware/include/net/reading_progress.h`,
    `firmware/storage/reading_progress.cpp`, new): `/USER/progress.bin`, not
    `/DEVICE/state.bin` -- `architecture.md` section 6 puts reading position
    under `/USER` specifically ("a package can be replaced without touching
    reading position"), which this pass's `docs/pending.md` wording had
    drifted from calling `state.bin`; corrected here, not left inconsistent.
    Same magic/version/CRC/`.tmp`-then-rename discipline as
    `net::LibraryIndex::save()`. One record, keyed on `content_id` (not a
    path or library-index position, both of which `rebuild()`/`prune()` can
    change under it) -- opening a different Quran package overwrites it,
    deliberately: this is where the reader is now, not a bookmark history.
  - **Wired to save on every way of leaving `ui::QuranScreen`**: EXIT (to
    the picker), MENU (to the library), and `GoToSleep()` -- not on every
    page turn, which would put an SD write on the tail of every 622ms
    partial refresh for no benefit. `OpenQuran()` resumes straight into the
    reader at the saved position when the saved `content_id` matches the
    package just opened; otherwise it opens the picker at surah 1.
  - **A real, pre-existing gap fixed along the way**: MENU from `kQuran`
    used to jump straight to the library without calling `CloseQuran()`,
    leaking the open file handle -- never surfaced because nothing had
    exercised that specific path. Now closes it, matching EXIT's existing
    behavior. Worth knowing the actual symptom this could produce:
    `hal::IStorage`'s handle pool is a fixed 4 (`architecture.md` section
    5a), so four MENU presses out of the reader without this fix would
    exhaust it -- at which point `g_storage.open()` starts returning
    `nullptr` for everything, including opening a book. If that has been
    seen before as an unexplained "won't open" after some browsing, this is
    the likely cause.
  - **A second correctness gap caught in review, fixed before this entry was
    written**: `OpenQuran()`'s resume path originally trusted a loaded
    `net::ReadingProgress` once `loadSurah(progress.surah_id)` succeeded.
    That only proves the surah exists, not that `progress.first_ayah_index`
    still lands inside it -- a CRC-clean but semantically stale record (e.g.
    saved under a different `AYAH_INDEX` layout) would otherwise resume onto
    a dead page with no way back but EXIT. Now also checks
    `getAyahByIndex(progress.first_ayah_index)` resolves and its
    `surah_id` matches, falling back to the picker if not -- the same direct
    check `getSurah()` itself already does for its own consistency rule.
  - **Verified**: the product build and all four demo variants
    (`NABA_DEMO`, `READER_DEMO`, `GLYPH_ATLAS_DEMO`, `DEV_AUTOPROVISION`)
    compile clean via `pio run` against the real ESP32-S3 toolchain, which
    *is* on PATH this session even though a host C++ compiler is not.
    **Not verified**: no host tests ran (10 new: 5 in `test_net` for
    `ReadingProgress`'s round-trip/corruption/versioning, plus a new
    `test_ui` suite -- the first host-tested `ui::` screen at the time this
    was written; the other three joined it, and a compiler, the same day --
    see the "No host C++ compiler" entry above) -- `pio test -e native`
    confirmed they were at least discovered and attempted to build even
    then, failing only on the missing-compiler gap, not a wiring problem.
    Nothing
    exercised on hardware: no picker screenshot, no confirmed resume across
    a real sleep/wake, no confirmed handle-leak fix.

- **Glyph atlas — both halves built and proven, 2026-08-31/09-01: real
  Arabic confirmed rendering on the physical panel via a real QPK-shaped
  package.** Desktop half scoped and finished first, deliberately, in its
  own pass (no host C++ compiler was on PATH this session, so a blitter
  written before real content existed would have been verifiable by
  neither host tests nor hardware —
  exactly the untested-code outcome avoided everywhere else this session).
  - `tools/arabic-pager/shape_glyph_atlas.py` (new): shapes real Arabic
    through the same proven HarfBuzz+FreeType path `render_pages.py` already
    uses (no new rasterization code, just serializing what `shape()`/
    `bitmap()` already compute instead of compositing it straight onto a
    page), and gets one non-obvious thing right that would otherwise have
    silently corrupted rendering — `qpk_format.h` documents
    `xAdvance`/`xOffset`/`yOffset` as **per-glyph-id constants**, but
    HarfBuzz's GPOS mark positioning is contextual (a harakah's offset
    depends on which base letter precedes it). Keying the atlas on raw
    HarfBuzz glyph id would store one offset per glyph and render every
    occurrence but one wrong. Fixed by keying atlas entries on
    `(font_glyph_id, xOffset, yOffset, xAdvance)` instead — minting a new
    atlas id whenever the same font glyph's shaped position actually
    differs, exactly what the format comment prescribes ("mint a distinct
    glyphId per visually distinct base+mark combination"). Run against
    Al-Fatihah: 29 words, 120 atlas entries from 58 distinct font glyph ids
    (so the dedup is doing real work, not a no-op), zero font-coverage gaps.
  - `desktop/converter/src/pipeline/glyph-atlas.ts` (new,
    `buildGlyphAtlasSections`): pure data transform, no I/O — packs the
    Python tool's shaped output into `FONT_METADATA` + `ASSETS` (bitmaps
    then glyph-id runs, back to back, matching `readAsset()`'s documented
    layout) + a shaped `WORD_INDEX`. Unit tested (3 new tests): section
    sizes are exact multiples of the wire record size, a full
    `writePackage`/`readPackage` round trip recovers every glyph's metrics,
    bitmap bytes and each word's glyph-id run byte-exact, and a word
    referencing an unknown glyph id is rejected at build time rather than
    written.
  - **A real bug found and fixed along the way, in `packages/qpk-format`
    itself, not just this new code**: `QpkPackage.checkIndexConsistency`'s
    deep sweep validated every `WORD_INDEX` entry's `textOffset`/
    `textLength` against `TEXT_DATA` unconditionally, even when
    `FLAG_SHAPED_TEXT_DATA` is set — where the format's own documented
    contract is that those spans point into `ASSETS` instead. A real shaped
    `WORD_INDEX` failed `readPackage()` outright until fixed; the existing
    `FLAG_SHAPED_TEXT_DATA` test never caught it because it has
    `FONT_METADATA`/`ASSETS` but no `WORD_INDEX` section, so the buggy
    `TEXT_DATA`-only branch never ran. Fixed in `reader.ts`, with a
    dedicated regression test added to `qpk-format`'s own suite (not just
    incidentally covered by the new converter test). The **C++ device-side
    reader did not have this bug** — `firmware/qpk/qpk_reader.cpp`'s
    `readAsset()` already validates against `ASSETS` specifically, and its
    own `checkIndexConsistency()` never attempted the equivalent eager
    `WORD_INDEX` sweep at all (by design — see its own Rule 13 comment: the
    device verifies only fixed-size sections at open, deferring full
    validation to each accessor).
  - `desktop/converter/scripts/build-fatihah-glyph-atlas.mjs` (new):
    assembles a real, valid QURAN package carrying the shaped atlas —
    `examples/al-fatihah-glyph-atlas.qpk`, **a separate file, not an update
    to `examples/surahs/al-fatihah.qpk`**, since that package already has
    a real (unshaped) `WORD_INDEX` with word boxes into `TEXT_DATA`, and
    `FLAG_SHAPED_TEXT_DATA` redefines `WORD_INDEX`'s meaning for the whole
    package — one package cannot carry both kinds at once. Passes
    `validatePackage()` (desktop + device profiles). A Node readback script
    confirmed coherence directly against the bytes: all 227 glyph
    references across the 29 words resolve to a real `FONT_METADATA`
    entry, all 120 atlas entries are actually referenced by some word (zero
    orphans), every bitmap read back its declared byte length with no short
    reads, and textually-identical repeated words in Al-Fatihah (e.g.
    "الرَّحْمَٰنِ الرَّحِيمِ", appearing in both the Bismillah and its own
    ayah) independently produced byte-identical glyph-id sequences —
    real confirmation the shaping is deterministic, not just structurally
    plausible.
  - **On-device blitter — built and confirmed rendering real Arabic on real
    hardware, 2026-08-31/09-01.** `firmware/include/qpk/glyph_blitter.h` +
    `firmware/qpk/glyph_blitter.cpp`: `MeasureGlyphRun` (sums `xAdvance`,
    no canvas needed — for laying a word out before drawing it) and
    `BlitGlyphRun` (`getGlyph()` + `readAsset()` per glyph id, ink-only —
    a bitmap's 0 bits are never drawn, so an overlapping harakah doesn't
    erase its base letter). A new `-DGLYPH_ATLAS_DEMO=1` build (same
    pattern as `NABA_DEMO`/`READER_DEMO`, not in the product build) opens
    `/DEVICE/glyph-atlas-demo.qpk` and lays its 29 words out right-to-left
    with simple line wrapping — no `LAYOUT_DATA`-driven real positions yet,
    that's still separate, unstarted work; this proves the glyph mechanism
    draws correctly, not a real reader's layout.
    - **Confirmed twice, independently**: a diagnostic read of glyph 1
      straight off the SD card on-device came back byte-identical to the
      desktop-side dump verified earlier (`00 00 00 00 00 00 00 01 80 00
      00 00 02 40 00 00...`), and the computed word bounding box
      (`x=[40,776] y=[40,128]`) was sane — spread across the visible
      panel in 3 lines, nothing degenerate. Then, visually: **real
      Arabic text confirmed readable on the physical panel.**
    - **A real, unresolved hardware/timing quirk found along the way**:
      the very first full refresh right after this demo's cold-boot
      init-clear does not visually commit, even though `flush()` reports
      success (`ink=3851/27200`, `flush=1`, normal ~2s duration) and the
      byte-level/bbox diagnostics above both check out — the panel showed
      plain white. A second full refresh (originally triggered by
      accident, via a long-press's unrelated `ToggleTransferMode()` side
      effect setting `g_dirty`; now a deliberate OK-click handler added
      for this build only, `main.cpp`'s `HandleEvent`) does commit
      correctly, every time. Root cause not identified — ruled out: glyph
      data correctness (byte-verified), word positioning (bbox verified),
      and it not being glyph-specific (an early debug build swapped the
      glyph content for a plain filled rectangle as a control test; same
      symptom, first refresh invisible, second visible). Every other demo
      this session (`NABA_DEMO`, the library browser, the book reader) has
      done a cold-boot-then-immediate-content full refresh with no such
      issue, so this isn't a generic "two full refreshes back to back"
      limitation — something specific to this render is triggering it,
      still unknown what. Tried a 150ms settle delay between this demo's
      SD-read burst and the flush (working hypothesis: a shared-SPI-bus or
      rail-settling effect specific to doing real SD I/O in the window
      between the display's own init-refresh and a content flush, which no
      other demo does) — inconclusive on the one test run before the
      session moved on to other work; not confirmed fixed or ruled out.
      **Paused, not abandoned**: needs slower iterative hardware testing
      (ideally a scope on the panel rail, per §5's existing "not yet
      measured" list) that a multi-tasking session isn't well suited to.
      **Later confirmation (2026-09-01)**: the 150ms settle delay did not
      fix it. Content only ever appears after a hard refresh (power cycle /
      reset) or after holding OK for a long time — a normal cold boot with
      no long-press still shows plain white on the first content flush,
      matching the original symptom exactly. This narrows it further: it
      isn't specific to the accidental-second-refresh path from
      `ToggleTransferMode()` that first surfaced it — any trigger that
      forces a *subsequent* full refresh after the invisible first one
      reveals the content, whether that's a long OK-press or a hard reset
      landing straight on a fresh cold boot (which also needs its own
      "second" refresh to commit, i.e. even the hard-refresh case isn't a
      single clean refresh — it's still masking the same underlying
      first-refresh-doesn't-commit bug, not an independent fix). Still
      unresolved; still needs the scope-on-the-rail investigation above.
      **Confirmed in the real product path too, 2026-09-01 -- this is not a
      `GLYPH_ATLAS_DEMO`-only quirk.** Reading an actual surah through the
      picker for the first time on real hardware reproduced it: entering
      `ui::QuranScreen` from `OpenSelectedSurah()` reported a normal full
      refresh (`ink=4580/27200 flush=1 in 2096ms`), but the panel kept
      showing the picker screen underneath -- the user's own words, "screen
      showed old content behind." Confirmed by reading the code, not just
      guessing: `Epd579Display::flush(kPartial)` silently escalates to a
      full `panel_.updateFull()` waveform whenever
      `partials_since_full_ >= app::kPartialsBeforeFullRefresh` (8) --
      ghosting-prevention logic already in place for an unrelated reason --
      but `main.cpp`'s log line prints the *requested* mode
      (`"partial"`), not what the display actually did, so this doesn't
      show up as `mode=full` in serial. The timestamps confirm it happened
      anyway: after the stale-content full refresh, 8 page-turns logged
      `mode=partial` at the normal ~621ms each, and the 9th
      (`ink=5078/27200 flush=1 in 2097ms` -- full-refresh duration, despite
      the "partial" label) is exactly where the periodic counter fires --
      and that is the page turn the user reported finally "changing" to
      correct content, after "8 rotary moved movement." Same underlying bug
      as above (a genuine full refresh does not visually commit the first
      time), a new way of observing it self-correct (the existing
      ghosting-prevention full refresh, not a deliberate second one), and
      new evidence of real product-facing severity: **every fresh entry
      into the reader from the picker currently shows the wrong screen for
      up to 8 page-turns**, not a one-off bring-up artifact. Still needs
      the same scope-on-the-rail investigation; this raises its priority.
    - **Not done, on purpose, per scope for this pass**: no real word-box
      geometry (`LAYOUT_DATA`-driven real positions are separate,
      unstarted work); only Al-Fatihah, not scaled to the rest of the
      Quran or made part of the normal build/product pipeline; the
      refresh quirk above is worked around for bring-up, not fixed.
- **The full Quran is now real content, 2026-09-01.** All 114 surahs, 6236
  ayahs (the canonical count, matched exactly), fetched in Uthmani script and
  packaged three ways: a text QURAN package, an aligned TRANSLATION, and a
  **shaped** QURAN package carrying a 1,507-entry HarfBuzz glyph atlas with
  full font coverage and zero warnings. 5 index entries of 96. What is still
  missing is unchanged and is the same thing as the item below: **nothing
  on-device consumes it.** Page rendering in An-Naba's layout works for any
  surah (`tools/arabic-pager/render_surah.py`), at 38px rather than the old
  44 -- the Uthmanic face is taller and 44 left a third of every page blank. See [quran-content.md](quran-content.md).
- **The Quran renders on the device -- built and confirmed on hardware,
  2026-09-01.** `ui::QuranScreen` (`firmware/include/ui/quran_screen.h`,
  `firmware/ui/quran_screen.cpp`) walks `AYAH_INDEX` -> `WORD_INDEX` ->
  `ASSETS` and blits pre-shaped glyph runs right-to-left. Serial from the real
  board, paging through Al-Baqarah:
  `[quran] surah=2 from ayah_index=7 drew 4 ayahs, 231 glyphs`, then
  `ayah_index=11 drew 2 ayahs, 232 glyphs`, each a 622ms partial refresh.
  `ayah_index=7` is exactly Al-Baqarah 1 (surah 1 holds indices 0..6), so the
  index arithmetic is right, and reaching surah 2 at all proves paging follows
  `AYAH_INDEX` across a surah boundary rather than stopping.
  - **Not a pure renderer, unlike every other screen.** Each glyph is a
    `readAsset()` off the card during the repaint, so the package stays
    **open** for as long as the screen is up -- where `OpenBook()` slurps text
    into RAM and closes immediately. `CloseQuran()` on EXIT releases the
    handle; opening twice closes first, so a handle cannot leak.
  - **Backward paging is a history stack, not a computation.** Where a screen
    ends depends on measured glyph widths, so the previous screen's start
    cannot be derived without laying it out. 32 remembered start indices.
  - **A package with no atlas says so.** The text `quran-full.qpk` has no
    `FONT_METADATA`/`ASSETS`, and `TEXT_DATA` is UTF-8 Arabic the 5x7 ASCII
    face cannot draw. Confirmed on hardware:
    `drew 0 ayahs, 0 glyphs (UNSUPPORTED PACKAGE)` with an on-screen
    explanation, rather than a blank screen.
  - **A UX bug this surfaced, fixed the same pass:** both QURAN packages were
    titled "The Holy Quran", so the two library rows were indistinguishable
    while only one could be drawn. The shaped build now titles itself "The
    Holy Quran (readable)" -- which also gives it a naturally distinct content
    id, replacing a hand-forced byte-xor.
  - **Still missing, and honest about it:** no surah picker (it opens at surah
    1 and you page forward), no `LAYOUT_DATA`-driven positions (simple
    right-to-left wrapping, not mushaf line breaks), and ayah numbers are drawn
    as `(7)` in the Latin face because U+06DD is a combining enclosing mark the
    per-word glyph runs do not carry.
- **Fold pages into QPK.** The An-Naba bitmaps are still a C array, not a
  package — that part is unchanged. `LAYOUT_DATA` itself is now implemented
  on both sides (writer in `desktop/converter`'s Quran and Book pipelines,
  reader in `packages/qpk-format` and `firmware/qpk`, rule 16 enforcing the
  section is present whenever `kFlagHasWordLayout` is set) and covered by
  host tests on both languages plus the golden-fixture cross-check. What is
  still missing is the on-device consumer: nothing blits from it yet.
- **Library browser is now two levels — built and confirmed live on hardware,
  2026-09-01.** The flat list was fine for six test packages and stops being
  fine the moment a real library arrives: scrolling past a hundred entries on
  a six-row screen with a wheel is not a menu. Top level is a **fixed** set of
  four categories (Quran, Books, Translations, Tafsir) plus the Transfer Mode
  row; OK on one drills into just that type. Fixed rather than derived from
  what is installed, so the menu keeps its shape as content comes and goes and
  an empty category still tells you where that content would go. Inside a
  category the type column is dropped and its width goes to the title, which
  is the thing that was actually being truncated. Back out with the `< Back`
  row or with EXIT -- EXIT already meant "go back" from the reader, and
  sleeping the device instead would be a surprising thing for a back button to
  do. `LibraryScreen::rowKind()` classifies a row for `main.cpp` so the
  renderer and the input handler cannot disagree about which row is what,
  which the old `selected == book_count` arithmetic invited. Selection and
  scroll reset on every level change, deliberately: carrying a row number from
  a 5-row menu into a 200-row list lands the cursor somewhere arbitrary. OK on
  an *empty* category sets a footer message instead of entering a view
  containing nothing but a Back row. Confirmed on hardware:
  `[library] OK on category Books (1)`, entered, EXIT returned.
  - **For Quran specifically:** the current content is one package per surah,
    so this level *is* the chapter list, and the header says "N chapters"
    rather than "N items". A third level -- chapters *within* one package --
    is possible in the format (`SURAH_INDEX` holds them) but not built, and
    would only matter once a package carries more than one surah. Nothing
    on-device can open a Quran package yet regardless.
  - **The hard limit this does not solve: `kMaxLibraryEntries = 96`**
    (`net/library_index.h`). A thousand books cannot be indexed today at any
    level of menu -- `LibraryIndex` is a fixed `LibraryEntry entries_[96]` in
    static RAM at 256 B each, so 96 costs 24 KB of 320 KB and 1000 would cost
    250 KB. Raising it is a real design change, not a constant bump: either
    move the array into PSRAM (8 MB is present and already used for the
    framebuffer) or stop holding the whole index in RAM and page it off the
    card. Worth deciding before the browser is asked to scale.
- **Library browser (first version) — built and confirmed live on hardware, 2026-08-31.**
  `ui::LibraryScreen` (`firmware/include/ui/library_screen.h`,
  `firmware/ui/library_screen.cpp`) lists `net::LibraryIndex` directly (it's
  host-testable already — no Arduino dependency — so the screen reads it
  rather than copying rows into state), plus one synthetic trailing "Transfer
  Mode" row so transfer mode finally has a real menu trigger, not just the
  encoder switch's long-press (which still works too, unchanged). MENU now
  toggles between this and the Milestone 1 self-test screen (`ScreenMode` in
  `main.cpp`) rather than the self-test screen being the only thing the
  product build ever shows. WHEEL moves the selection with scrolling; OK on
  the Transfer Mode row calls the existing `ToggleTransferMode()`. That used to
  **silently do nothing when no Wi-Fi credentials were saved** -- no footer
  message, no log line -- so the row read as broken rather than unavailable;
  found 2026-09-01 with the product build freshly flashed onto a device whose
  NVS held no SSID. **Fixed the same day:** it now sets the footer to
  "No saved Wi-Fi -- provision the device first" and logs
  `[transfer] refused: no saved Wi-Fi credentials`. The footer write is
  `#if`-guarded because `g_library_status` does not exist in the demo builds
  while `ToggleTransferMode()` does -- caught by building every variant, not
  just the product one. OK on a
  book sets a footer status message rather than silently doing nothing when
  there is genuinely nothing to open yet (Quran/Translation/Tafsir -- see
  below). Column layout bug fixed the same pass: the type column was only
  110px wide, and "TRANSLATION" at this font/scale needs 132px, so it always
  overlapped the size column regardless of title length -- widened to a
  144px field sized off `gfx::kGlyphWidth` rather than another guessed
  constant. Not real selection persistence: `g_library_selected` is a plain
  global, not `RTC_DATA_ATTR`, so every deep-sleep wake resets the cursor to
  item 0 -- distinct from (and not a substitute for) the still-open
  reading-position-persistence item below, not fixed here since nobody asked
  for it. OK on the trailing row starts a real transfer session
  (`[transfer] http server up`, closed cleanly by EXIT). The device is now
  running this product build rather than the `NABA_DEMO` bitmap demo
  (trivially reflashable back with `PLATFORMIO_BUILD_FLAGS="-DNABA_DEMO=1"`
  if wanted).
- **Opening a BOOK package — built and confirmed live on hardware,
  2026-08-31.** `qpk::LoadBookText` (`firmware/include/qpk/book_reader.h`,
  `firmware/qpk/book_reader.cpp`) concatenates a BOOK package's `TEXT_DATA`
  records (one per desktop-computed page, see `paginate.ts`'s
  device-independent 72-column grid) into a flat buffer, turning each
  record's internal `\n`s into spaces so `util::TextPager` re-wraps for
  this panel's real width instead of double-wrapping the desktop's
  already-wrapped lines, with a blank line between records as the closest
  structural break the format actually preserves. `main.cpp`'s
  `OpenBook()` reads the file via `hal::IStorage::open()` +
  `qpk::Reader`, loads it into a fixed 48 KB static buffer (no dynamic
  allocation, matching `qpk_reader.h`'s own rule; refuses gracefully via
  `BookLoadError::kTruncated` past that, not implemented for any book yet
  since the one on the card is ~1 KB), and reuses `READER_DEMO`'s existing
  `util::TextPager` + `ui::ReaderScreen` pair -- only the byte source
  changed, from flash to a real SD-card package. A new `ScreenMode::kReader`
  wires WHEEL to page-turn and EXIT back to the library ("EXIT goes back",
  literally the product copy in `app::kEmbeddedNote`). QURAN/TRANSLATION/
  TAFSIR selection still only shows the status message -- those need Arabic
  shaping, which is the separate, still-unbuilt glyph-atlas item above; this
  only covers plain-Latin BOOK content. **Confirmed live**: serial log read
  `[library] OK on row 6: "For Bushra" type=BOOK` then
  `[reader] opened /LIBRARY/BOOKS/85c9fc7c....qpk (1112 bytes, 8 pages)`,
  followed by a full repaint with far more ink than the list view (7539 vs
  ~4900/27200) -- and the user confirmed reading the actual note text on the
  panel and turning pages with the wheel.
  - **The paragraph-break gap above — fixed, 2026-08-31.** Two bugs in
    `paginate.ts`'s `paginateBlocks`, not one:
    1. The blank-line spacer was only inserted around *heading* blocks.
       Fixed by inserting one blank line between every pair of blocks,
       unconditionally — every `Block` is already one source paragraph
       (`txt.ts`'s `flushParagraph` joins a paragraph's lines before this
       ever runs), so a block boundary always is a paragraph break.
    2. Deeper bug the first fix alone didn't catch (verified empirically —
       the regenerated package still showed no blank lines at all): the
       line-emission loop had `if (line.text.length > 0)`, which silently
       dropped every blank-line entry from `pageLines` before `pageText()`
       (`model.ts`) ever saw it — `pageText()` reconstructs a page's
       `TEXT_DATA` record by joining `page.lines` with `\n`, so a line
       never in `page.lines` cannot become a break in the stored bytes,
       no matter how many blank entries `paginateBlocks` computed upstream.
       Fixed by pushing blank lines into `pageLines` too (as a zero-word
       `TextLine`, which `book-package.ts` already skips when emitting
       `LAYOUT_DATA` — that guard existed before this fix, for exactly this
       case, so nothing downstream needed to change).
    Verified three ways: `npm test` stays 41/41 (no fixture asserts an
    exact page count against text with real paragraph breaks); a synthetic
    long paragraph confirmed blank lines land between blocks only, never
    between a wrapped paragraph's own lines; and `for-bushra.qpk` was
    regenerated (`node scripts/regen-for-bushra.mjs`, same content id,
    confirming the title/author/language-derived id is unaffected) and its
    `TEXT_DATA` records read back with real `\n\n` between every original
    paragraph, matching `for-bushra.txt` exactly. The regenerated package
    is staged (`sdcard-staging/LIBRARY/BOOKS/85c9…qpk`) but **not yet
    copied to the physical card** — that still needs the card swapped into
    a reader on this PC, same as every other card update this session.
- **Reading position persistence — superseded 2026-09-01.** See the surah-
  picker/reading-position entry above (`net::ReadingProgress`,
  `/USER/progress.bin`, not `/DEVICE/state.bin` as this line used to say).
- **Desktop Tauri UI — built, 2026-09-01.** All four tabs over the existing
  `converter` and `device-client` libraries, which are unchanged: this was
  assembly, as scoped. What it took, and the one decision worth recording:
  - **`desktop/app-bridge/` (new)** — the app drives both TypeScript
    libraries as a **one-shot `node` subprocess per operation**, JSON in on
    stdin, JSON lines out on stdout (`{"type":"progress"|"result"|"error"}`).
    The alternative — running them in the webview — was rejected on a
    concrete blocker, not on taste: `yauzl` (EPUB) is Node-only, `pdfjs-dist`
    needs its legacy build, and `content-id.ts` hashes with a synchronous
    `node:crypto` call whose exact bytes the format, the firmware and the
    golden fixture all pin. Swapping it for WebCrypto would change a content
    id. Given Node is in the process tree for conversion regardless,
    `device-client` rides the same pipe rather than earning a second
    mechanism (a Rust HTTP proxy) purely to dodge the webview's CORS rules.
    One process per operation means no sidecar lifecycle, no port, and a
    wedged conversion cannot poison the next one; the cost is a ~100ms Node
    start against minutes of PDF parsing or radio time. 22 tests
    (`npm test --prefix desktop/app-bridge`), run against the *real*
    converter and device-client builds — only the device transport is faked,
    because there is no device.
  - **`desktop/src-tauri/src/bridge.rs` (new)** — spawn, stream, forward
    progress as Tauri events. **No new crates**: no HTTP client, no dialog
    plugin. One generic `bridge_call` command rather than one per operation,
    because the request shapes are the bridge's and the response shapes are
    the libraries', so a Rust copy of both would be a third declaration that
    could drift. 13 Rust tests total, including a real spawned round trip
    through `run.mjs` when Node is on PATH.
  - **`desktop/src/` (new UI)** — Dashboard, Library, Device, Converter.
    `bridge.ts` imports its result types from `desktop/converter` and
    `packages/protocol` (type-only, mapped to their built `.d.ts`) instead of
    redeclaring them, for the same anti-drift reason.
  - **A byte-identity check the UI's flow depends on**: "validate & preview"
    is a dry run and "convert & write" is the same conversion committed, so a
    test asserts the two produce the same size and content id. Without it,
    previewing before installing would be previewing something else.
  - **Verified**: `cargo test --lib` 13/13, `npm test --prefix
    desktop/app-bridge` 22/22, `tsc` clean, `vite build` clean, the built
    binary launches, and every screen was rendered and checked in both light
    and dark. **Not verified**: the webview `invoke` seam itself — the
    Rust↔Node half has a real integration test and the UI compiles, but no
    automated test drives a click through the native window.
  - **Known limitations, deliberately taken** (all in
    `desktop/README.md` too):
    - **Node on PATH plus this checkout on disk.** The bridge is a script in
      the repository and the libraries load from their built `dist/`. An
      installer means bundling Node and both builds as Tauri resources —
      separate work, not started. `QURAN_DEVICE_REPO` overrides the search.
      Named as a stopgap the way `TryWifiBypassFromSdCard()` is, not left
      silent: the Dashboard checks all of it on open and prints what to run.
    - **Paths are typed, not picked.** A native file dialog means
      `tauri-plugin-dialog` and a fresh crate; left as a decision rather than
      made silently. The Library tab's "Send to device" removes the retyping
      that mattered most.
    - **The pairing token sits in the webview's `localStorage`.** A real
      trust store (`device-client`'s `TrustStore`, keyed by device id, with
      OS keychain backing) belongs in Rust and is not this milestone.
    - **Everything on the Device tab except Identify is unproven against
      hardware**, for the reason in section 3 below, not because it is
      unfinished. The tab says so on screen rather than presenting an
      untested path as a working one.

**Done this pass:** `desktop/converter/src/pipeline/translation-package.ts` --
a standalone `TRANSLATION` package builder, closing qpk-format.md §13's open
question ("whether translations live in the Quran package or stay separate —
currently separate, joined by content_id in metadata key 9") from spec to
working code. `TRANSLATION_INDEX`/`TRANSLATION_DATA` wire-format support
already existed on both sides; only the desktop-side builder was missing.

The reader skeleton already exists and is not throwaway:
`util::TextPager` (wrap + paginate, zero-copy) and `ui::ReaderScreen`. Only
the source of the bytes changes — flash today, a QPK page cache then.

---

## 3. Never run, though written and compiled

These are implemented and host-tested but have never executed on hardware:

| Subsystem | State |
|---|---|
| Wi-Fi HTTP API | **proven, 2026-08-31** — see below |
| BLE provisioning | **partially proven, 2026-08-31** — see below |
| Resumable upload, backup, restore | still blocked — every endpoint but `GET /api/device/info` requires a pairing token, and BLE never delivered one (see below). **Also carries a real framing bug found 2026-09-01, see below** |
| mDNS discovery | behind an interface; only `ManualDiscovery` ships |

**Wi-Fi HTTP API, proven 2026-08-31:** with the device in transfer mode (via
the SD-card Wi-Fi bypass below) and its IP found via the host PC's ARP table
(`10.102.150.184` on the `Xiaomi` network — the device doesn't log its own
IP over serial, only on-screen), `curl http://10.102.150.184:8080/api/device/info`
returned a real response from real hardware over real Wi-Fi:
`{"protocolVersion":1,"deviceId":"38a0e28fcba4","name":"Quran Reader",
"model":"CrowPanel ESP32-S3 5.79in","firmwareVersion":"0.1.0-m1",
"maxChunkBytes":16384,"paired":false}`. `HttpServer` starts, serves, and
matches the spec's shape. `"paired":false` is `auth.h` working as designed —
every other endpoint (`/api/library/upload`, `/api/device/backup`, ...)
will refuse until a pairing token exists, which needs BLE provisioning to
actually finish (see below) since the SD-card bypass only carries Wi-Fi
credentials, not a token.

**BLE provisioning, what actually got proven:** a Python (`bleak`) client
paired with the device three separate times — `ESP_GAP_BLE_SEC_REQ_EVT` →
key exchange → `ESP_GAP_BLE_AUTH_CMPL_EVT` → `[ble] pairing ok`, exactly as
`docs/provisioning.md` specifies, Just Works, no MITM. **Not proven**: a
client completing the full write-SSID → write-passphrase → commit → read
Status flow. Every attempt hit the same Windows/WinRT-specific failure —
`pair()` drops the GATT session as a side effect of completing the OS-level
bond, and reconnecting afterward reliably times out before the device's
~20s idle-to-sleep window closes. This is a Windows BLE client-stack
timing issue, not a device-side bug — nothing in the device's own log
suggests a problem on that side. Worth retrying with a different client
(a phone's native BLE central, or a Linux/BlueZ client) before assuming
it needs a device-side fix.

**The chunk PUT is framed wrongly by the desktop client — found 2026-09-01,
not yet fixed.** Driving the new Device tab against a stand-in HTTP server
(one written to `protocol.md`, because the board is asleep and has never
issued a pairing token) recorded how `DeviceClient.uploadPackage` actually
frames a chunk on the wire:

```
content-length:    512
transfer-encoding: (absent)
```

`protocol.md`'s own section "The chunk-upload body is binary-unsafe as a plain
PUT" requires the opposite — **no `Content-Length`, `Transfer-Encoding:
chunked`** — because Arduino's `WebServer` auto-buffers a `Content-Length`
body through `String(plainBuf)`, which stops at the first `0x00`, and a QPK
package contains zero bytes as a matter of course. The firmware side is
written to that contract and only that: `HttpServer::ReadChunkedBody`
(`firmware/drivers/net/http_server.cpp`) unconditionally reads a hex
chunk-size line first (`strtol(line, &end, 16)`) and has no `Content-Length`
path at all.

Cause: `client.ts` passes the chunk as `body: slice`, a `Uint8Array`. A
fixed-length body makes `fetch` set `Content-Length` and skip chunked
encoding. So **every upload to real hardware would fail on its first chunk** —
never observed because uploads have never run against a device.

The fix is verified but not applied (it changes a Milestone 4 library, and it
cannot be confirmed against hardware until pairing works): pass a
`ReadableStream` body with `duplex: 'half'`, which is the standard way to send
a body of unknown length. Measured against a local server, side by side:

```
Uint8Array body (today)          content-length=6          transfer-encoding=(absent)
ReadableStream body (proposed)   content-length=(absent)   transfer-encoding=chunked
```

That is exactly the framing `ReadChunkedBody` expects. It wants a regression
test in `desktop/device-client`'s suite asserting the framing, not just the
bytes — the existing scripted server accepts either, which is why the suite is
green with the bug present.

**Upload still fails on real hardware, and a chunk PUT wedges the HTTP
server — found 2026-09-01, unresolved.** With a valid token and the device in
transfer mode, `/api/device/info`, `/api/device/status` and `/api/library` all
answered correctly from the real board. The chunk PUT did not:

- Through the fixed client: `HTTP 400` on the first chunk, at `sentBytes=0`,
  and with no `{"error":...}` body — so the client reported `HTTP_ERROR`
  rather than a protocol code.
- By hand with `curl` (same chunked framing): the request was sent in full
  (`upload completely sent off: 1036 bytes`) and the device **never
  responded at all**.
- Afterwards the whole HTTP server was dead: `/api/device/info`, which needs
  no auth, also stopped answering, while **ICMP ping still succeeded with 0%
  loss**. So the device was alive on Wi-Fi with its single-threaded
  `WebServer` stuck. The session had not expired (uptime ~11 min against a
  30-minute `kTransferSessionTimeoutMs`). Only a reset recovered it.

**Ruled out, by looking rather than assuming:** `X-Qr-Offset` *is* registered
with `collectHeaders()` (`http_server.cpp:97`), so `server_->header()` returning
empty is not the cause. The package is valid (`tools/package-inspector` reads
it), the token is correct, and `POST /api/library/upload` returns `201` with a
sane session — the failure is specific to the chunk PUT.

**Sharpened by a second session the same day.** The two framings fail
*differently*, reproducibly, which is the most useful fact so far:

| chunk PUT framing | result |
|---|---|
| `Transfer-Encoding: chunked` (the fixed client) | fast `{"error":"BAD_REQUEST"}` 400; **server survives** |
| `Content-Length` (the old client, reproduced with curl) | no response at all; **server wedges** — ICMP still 0% loss, HTTP dead until reset |

So the framing fix *is* right as far as it goes: chunked no longer hangs the
device, and the `400` now carries the protocol's own error code, meaning
`HandleUploadChunk` is entered and returns `kBadRequest` itself. The
Content-Length wedge is exactly the failure `protocol.md` predicted.

**A hypothesis checked and weakened, rather than assumed:** that this Arduino
core's `WebServer` swallows the chunked body before the handler runs. It does
not — `Parsing.cpp:177` only reads a body inside a branch gated on
`_clientContentLength`, so with the header absent the body genuinely is left on
the socket. Also ruled out: `X-Qr-Offset` is registered with
`collectHeaders()` (`http_server.cpp:97`).

**Still unknown:** which of `HandleUploadChunk`'s three `kBadRequest` exits
fires — the missing offset header, the `strtoull` parse, or `ReadChunkedBody`.
One `Serial.printf` of the first line `ReadChunkedBody` actually reads would
settle it.

**Resolved 2026-09-14 — confirmed on hardware.** That `printf` settled it:
`ReadChunkedBody first line: ""`, so the chunk-size line was gone before the
handler ran. The 2026-09-01 hypothesis above was right in spirit, wrong about
the line: `Parsing.cpp:177` does leave the body alone, but `_parseRequest` ends
with `client.flush()` (`Parsing.cpp:235`), and in core 2.0.14
`WiFiClient::flush()` empties the receive buffer. Every body byte that arrived
in the same TCP segment as the headers was discarded.

Hand-framed PUTs against the board showed it cleanly: a body sent 0.3 s after
the headers was read, the same request in one packet read nothing, and a
16,384-byte body lost its first segment (the handler started reading
mid-payload). `fetch` always sends headers and body together, so every chunk
PUT failed.

Fix: a patched copy of the library in `firmware/lib/WebServer/` with that one
call removed (see its `PATCHED.md`); PlatformIO builds it instead of the
framework's. All four hand-framed variants then passed, and a real photo went
through the desktop app's Photos tab in three chunks (`4000`, `4000`, `3b90`)
and was installed and shown. Package uploads use the same `ReadChunkedBody`,
so they are fixed by the same change, but a package install has not yet been
run on the board. **Update, same afternoon:** a 25.8 MB page-picture book
installed cleanly on the board, so the package path is confirmed too.

**Board drops off USB and Wi-Fi under load -- found 2026-09-14, open.** Twice
the whole board vanished (serial port gone, HTTP dead) until replugged: once
6.5 MB into an upload, once while turning pages. Suspect power first (cable,
Mac port, current during Wi-Fi + panel refresh), then the booster resistor.
After replugging, check the boot log's reset reason: POWERON means it lost
power, a brownout or panic reason means the board reset itself.

**The `setTimeout()` unit bug — fixed 2026-09-01, not yet verified on
hardware.** Confirmed by reading the Arduino core directly
(`WiFiClient.cpp`: `int WiFiClient::setTimeout(uint32_t seconds)` ends in
`tv.tv_sec = seconds;`, and the header names the parameter `seconds`): this
was a real bug, not just a "worth checking." `ReadChunkedBody`'s
`client.setTimeout(5000)` set an ~83-minute socket timeout instead of the
intended 5 seconds. Now `client.setTimeout(app::kChunkedBodyReadTimeoutSec)`
(`app_config.h`, new constant, `= 5`), with a comment at both the constant
and the call site so the same seconds-vs-milliseconds mismatch cannot recur
silently at a future call site — this was the only `.setTimeout(` call in
the firmware. Compiles clean via `pio run`. **Does not by itself explain**
the Content-Length wedge above (that path goes through the Arduino
`WebServer`'s own body buffering, not `ReadChunkedBody`) or the chunked
path's fast `400` (`HandleUploadChunk` returns before a timeout could ever
fire on a well-formed request) — it matters for a *stalled or slow* chunked
upload, where the device would previously have hung for up to 83 minutes
instead of failing in 5 seconds. Real, but doesn't close the chunk-PUT
investigation above; the `Serial.printf` diagnostic is still the next step
for that.

**What this means for the framing fix above:** it is correct against the
documented contract and verified end to end against a local server, but it did
**not** make uploads work on hardware, and it changed the failure from a fast
`400` to a hang. Do not treat the upload path as working.

**Chunk framing fixed, 2026-09-01.** `DeviceClient.uploadPackage` sent each
chunk as a `Uint8Array`, which `fetch` frames with `Content-Length` and no
`Transfer-Encoding` -- the opposite of what `HttpServer::ReadChunkedBody`
parses. Now a `ReadableStream` body with `duplex: 'half'`, built fresh per
attempt because a consumed stream cannot be retried. Two regression tests
assert the **framing** rather than the bytes, and were checked against the old
code first: they fail 2/14 with it and pass 14/14 with the fix, while the
other twelve pass either way -- which is precisely why the bug survived to be
found against real firmware. Still unproven on hardware: no upload has yet
reached a device.

**A compiled-in dev provisioning build, 2026-09-01.** `-DDEV_AUTOPROVISION=1`
provisions Wi-Fi *and* the pairing token from
`firmware/include/app/dev_secrets.h` at boot, so a test device needs neither
an SD-card shuffle nor a BLE client. The header is **gitignored**, with a
tracked `dev_secrets.example.h` alongside it; the flag is opt-in, and the
product image was checked to contain neither the passphrase nor the token
while the dev image contains both. It refuses to overwrite a device that is
already provisioned and holds a token, and says so on the console rather than
skipping silently. Never ship a build with this flag set: it puts the bearer
credential for every destructive endpoint into the firmware image.

**The SD-card bypass now carries a pairing token too — added 2026-09-01,
and it is a bigger downgrade than the Wi-Fi half.** `wifi.json` gained an
optional `"token"` key, read into `ProvisioningState::setPairingToken()`
before the commit, so `NetCredentials::save()` persists it exactly as BLE's
flow would. Two supporting changes: the bypass now runs **even when
credentials are already saved** (it used to be gated on `!has_saved_credentials`,
which made topping up a token on an already-provisioned device impossible
without erasing NVS), and a failed attempt reloads NVS so a bad file cannot
leave a working device unprovisioned. The file stays one-shot — removed on
read — so this cannot loop.

**Be clear about what this costs.** The Wi-Fi bypass put a network password on
a removable card; this puts *the bearer credential for every destructive
endpoint* there — upload, delete, and eventually restore. It is strictly worse
than BLE's `ESP_GATT_PERM_WRITE_ENCRYPTED` requirement, and it exists only
because the Windows BLE client cannot complete the GATT flow (above). The
mitigations are thin and worth naming rather than trusting: the file is
deleted on first read, and the token's *value* is never logged — only whether
one was supplied. **The real fix is still a working BLE client** (a phone or a
Linux/BlueZ central), which needs no firmware change at all; this is a
development shortcut, and any device provisioned this way should be
re-provisioned over BLE before it matters.

Not host-tested: `TryWifiBypassFromSdCard()` lives in `main.cpp` behind
Arduino dependencies, and no host C++ compiler was on PATH this session
(`run_host_tests.py`'s Windows note again). `setPairingToken()`'s own bounds
checking is covered by the existing `ProvisioningState` suite.

**How the device actually got Wi-Fi credentials, for now**: a dev-only
bypass added this session — `firmware/src/main.cpp`'s
`TryWifiBypassFromSdCard()`. If `/DEVICE/wifi.json` (plaintext
`{"ssid":"...","passphrase":"..."}`) is present at boot and no credentials
are saved yet, it connects directly and calls the same
commit-then-verify-then-persist path BLE's flow uses
(`NetCredentials::save()`), then deletes the file whether the attempt
succeeded or failed. Confirmed working end to end: boot → read file →
`WiFi.begin()` → associated in under a second → credentials persisted →
normal boot continued (BLE never engaged). **Not how the real product
should provision a device** — a plaintext Wi-Fi password sitting on a
removable SD card is a real downgrade from BLE's encrypted-link
requirement — but it unblocks testing the HTTP API and resumable upload
without fighting the BLE client issue above. Two real environmental
gotchas hit along the way, worth remembering: the ESP32's radio is
2.4GHz-only (a 5GHz-only or 5GHz-preferred network shows as
`NO_AP_FOUND` and looks identical to a wrong password), and a stale
Windows Wi-Fi scan cache can report a network absent when it has simply
changed channel/state — force a rescan (`netsh wlan disconnect`, then
rescan) before concluding a network isn't there.

---

## 4. Not actually cheap

Used to be titled "small and cheap." Two items that were (the QuadratureDecoder
deletion, the rail's Arabic surah name) are done; investigating the rest
found each blocked on something real rather than quick to fix — recorded here
so the next pass does not re-discover the same blocker.

- **Backup/restore are 501 stubs.** Not cheap: no packing format for `/USER` +
  `/DEVICE` state exists yet — this is a format-design task, not a stub fill-in.
- **`installed_at` now gets a real timestamp — resolved 2026-08-31.** Added
  `hal::IClock`/`drivers::SystemClock` (`firmware/include/hal/clock.h`,
  `firmware/drivers/net/system_clock.cpp`), following the same DI pattern as
  `IPower`/`IStorage`: a thin wrapper over ESP32 Arduino's built-in SNTP
  (`configTime()`/`time()`), no from-scratch UDP client needed.
  `PollTransferMode()` calls `g_clock.begin(app::kNtpServer)` the moment
  Wi-Fi connects, and `HttpServer::HandleUploadFinish` now passes
  `clock_->nowUnix()` (only when `synced()`, else still `0`) instead of the
  old hardcoded `0`. **Confirmed live on hardware**: serial log read
  `[clock] synced now_unix=1788195050` the same tick the HTTP server came
  up — decodes to 2026-08-31, matching the real date, synced against
  `pool.ntp.org` over the SD-card Wi-Fi bypass's connection in under a
  second. Best-effort by design: no sync yet (or ever, e.g. no internet
  behind the AP) still means `installed_at = 0`, same documented meaning as
  before.
  - **`expiresInSeconds` is now enforced, not just reported — done
    2026-09-01.** `UploadBegin` gained an `expires_at`, written into the
    `.meta` sidecar's **offset 24, which was already zero padding** between
    `payload_crc32` and the title — so the field moved nothing, changed no
    size, and needed no format version bump: a `.meta` written before it
    existed reads back 0, which is exactly the "no deadline" value. Checked
    in `beginUpload()` (an expired session is not resumed), `writeChunk()`
    and `finish()` — all three discard the session and return `kNoSession`,
    which is the recovery the desktop client already implements, so no new
    error code and no wire-format change. `finish()` refuses even when the
    bytes are complete and would verify: installing from a session that was
    reclaimable would make the deadline a fiction. The response now reports
    the **remaining** seconds rather than the constant, since a client told
    "900" on every resume cannot schedule around a deadline it is never
    given.
    - **Expiry is deliberately conservative in both directions**: a session
      expires only when a deadline is recorded *and* the device has a synced
      clock to judge it with. No clock means no deadline is written and none
      is enforced — the same best-effort rule `installed_at` already
      follows. Refusing an upload because we cannot prove it is *not* stale
      would be worse than keeping a few dead bytes.
    - **`now_unix` was appended as a defaulted parameter** on `beginUpload()`
      and `writeChunk()` rather than made required. That is a deliberate
      trade, not laziness: 20+ host-test call sites pass no clock, and with
      no host C++ compiler available this session (checked again — VS 2022 is
      installed without the C++ workload, no `cl.exe`, no MinGW) a required
      parameter would have meant editing every one of them blind. Defaulting
      to 0 keeps them compiling *and* keeps their behaviour byte-identical to
      before. Revisit when a compiler is available.
    - **Not host-tested.** Builds clean for product and both demo variants;
      the expiry paths have never executed. A test that fakes two timestamps
      across `beginUpload`/`writeChunk`/`finish` is the obvious next step and
      needs only a compiler, not hardware.
- **Justification.** Not cheap: lines are set flush right; a real mushaf
  justifies with kashida stretching, which is real typography work in
  `tools/arabic-pager`, not a quick fix.
- **Page-boundary fidelity — RESOLVED 2026-09-02.** This said closing the gap
  "needs the canonical Madinah line-break data, which nothing in this repo has
  yet". The data was never in the repo and never needed to be: quran.com's
  `/verses/by_page` serves a per-word `line_number`, and two new scripts fetch
  it and map it onto this repo's own word splits. Lines now break where the
  Madinah mushaf breaks them, `PAGE_INDEX` carries 604 real pages instead of
  114 fabricated ones, and the device lays out from `LAYOUT_DATA`. See
  section 2's "Done" list. Justification is a separate item and still open.

---

## 5. Measurements not yet taken

- **Sleep current.** Power the board from the BAT connector through a
  multimeter; deep sleep with both rails low should be tens of microamps.
  Milliamps means a rail is not actually off. Once `board::kBatteryAdcPin`
  (below) is resolved, the device also wakes itself every
  `app::kBatterySampleIntervalMs` (30 min) to sample the battery and go
  straight back to sleep (`firmware/include/util/battery.h`,
  `main.cpp`'s `SampleBattery`/`GoToSleep`) -- each wake is a brief full
  boot (~tens of mA for ~200ms), which averages to single-digit µA over the
  interval, the same order as the "tens of microamps" figure above. The
  measurement should account for this periodic wake, not just steady-state
  sleep between them.
- **Free internal heap during a transfer.** `WebServer` buffers a request body
  and then copies it into a `String`, so peak is roughly 2× the chunk size in
  internal RAM while Wi-Fi and LWIP are resident. `kMaxChunkBytes` (16 KB) is
  a single constant the device already advertises, so if it is tight the fix
  is a number.
- **Ghosting rate.** `kPartialsBeforeFullRefresh = 8` was chosen, not
  measured. Raising it means fewer full-screen flashes and more residue.
- **Quran page-turn time -- MEASURED 2026-09-21, on the board.** Paging
  forward through Al-Baqarah, one tap per page: **3.7-4.4 s to build the
  page** (2-3 ayahs, ~530-580 glyphs read off the card and blitted) and
  **0.65 s for the partial refresh** (copy 44 ms, panel 608 ms). So about
  4.5 s from finger to new page, and the panel is the *fast* part -- six
  times the cost is in `ui::QuranScreen::render` walking the glyph index.
  That is where to look if page turns are to get quicker; the touch path and
  the refresh add nothing measurable.
- **Panel rail current during `kFast`.** `kPartial`'s differential waveform
  leaves thin strokes (Arabic harakat, joins) faded and lets ghosting
  accumulate between the periodic full refreshes — visible as the previous
  page "mixing" with the new one. `updateFast()` (0xC7) is a full panel
  redraw with the vendor's quick waveform, so it should fix both without the
  `kFull` flash, and it is already wired in `Epd579Panel` — but switching
  `Repaint()`'s per-turn mode from `kPartial` to `kFast` produced a boot
  flicker with no serial output at all, consistent with a brownout from
  driving both cascaded SSD1683 controllers at once. Reverted to `kPartial`.
  Needs a scope/multimeter on the panel power rail (`kPanelPowerEn`) during a
  `kFast` update before trying again.
- **Library size ceiling is the index, not the card.** `library_index.h`'s
  `kMaxLibraryEntries = 96` is a fixed in-RAM array, not derived from
  anything about the SD card or RAM pressure — nobody has sized it against
  real content yet. A full Quran (Arabic-only, no font atlas — that pipeline
  doesn't exist yet, see §9a note in qpk-format.md) estimates to ~0.6-0.7MB
  from An-Naba's real measured text density (~77 bytes/ayah); a typical book
  is 1.5-5MB. 96 packages costs well under 500MB on a 64GB card, so the cap
  is arbitrary and raising it is a one-constant change whenever more than 96
  titles is wanted.

---

## 6. Known limitations, by milestone

Not repeated here — each milestone's limitations are recorded where they were
found:

| | |
|---|---|
| Milestone 1 — hardware | [development.md §7](development.md) |
| Milestone 2 — QPK format | [development.md §8](development.md) |
| Milestone 3 — conversion | [development.md §9](development.md) |
| Milestone 4 — transport | [development.md §10](development.md) |
| Hardware findings | [bring-up-log.md](bring-up-log.md) |
