# Hardware bring-up log

**First run on real hardware: 2026-08-30.** Board: Elecrow CrowPanel ESP32
5.79" E-Paper HMI, on COM6 via its CH340 USB-serial bridge.
**Second session: 2026-08-31** — the SD card, recorded in §6.

Everything before this date was written against datasheets and Elecrow's
published examples. This is the record of what the board actually did — what
held, what broke, and what is still untested. It exists because the interesting
part of bring-up is never the parts that worked.

---

## 1. Verified on hardware

| Subsystem | Result | Evidence |
|---|---|---|
| PSRAM | ✅ 8 MB, framebuffer allocated into it | `psram_fb=1 psram_total=8386263` |
| E-Ink panel | ✅ dual-SSD1683 init and refresh | `init=1`, full refresh 2096–2098 ms |
| Partial refresh | ✅ ~3.4× faster than full | 621 ms vs 2097 ms |
| Canvas → panel mapping | ✅ text renders correctly across the seam | legible Arabic and Latin on the glass |
| MENU / EXIT buttons | ✅ | `[input] MENU click`, `[input] EXIT click` |
| Encoder push (OK) | ✅ | `[input] OK click` |
| Wheel rotation | ✅ **after a decoder fix** — see §2.1 | `[input] WHEEL rotate delta=±1` |
| Deep sleep | ✅ | `[power] idle, sleeping` |
| Wake from button/wheel | ✅ EXT1 on GPIO 1,2,4,5,6 | `wake=button/rotary gpio=4` |
| Wake-cause reporting | ✅ names the exact GPIO | `gpio=4` = encoder B |
| Boot with **no** SD card | ✅ degrades, still boots and renders | `[sd] no card / wiring` then a normal render |
| RTC state across sleep | ✅ once fixed — see §2.2 | page number survives a wake |

### Memory

| Build | RAM | Flash |
|---|---|---|
| Product (both radios linked) | 26.6% | 41.2% |
| An-Naba demo (15 page bitmaps compiled in) | 32.1% | 54.8% |

Flash rises because the demo's page bitmaps ship *inside the image*. That is
the cost of running with no SD card; real content lives on the card.

---

## 2. What broke, and what it cost to find

Four bugs. Only one was a wrong guess about the hardware — the other three
were software, and two of them were introduced during this session.

### 2.1 The wheel is not a quadrature encoder

**The last `TODO(hw)` from Milestone 1, and the one that mattered.**

Elecrow names the two rotary channels **PRV** (GPIO 6) and **NEXT** (GPIO 4),
which is not how a quadrature encoder is normally labelled, but the board
documentation never settled it. Rather than guess, both interpretations were
built and run side by side behind `BOARD_ENCODER_QUADRATURE`, with both
counters logged live. The build shipped with the quadrature interpretation
selected — the wrong one.

One detent produced:

```
[enc] A=1 B=1 quad=0 pulse=0
[enc] A=1 B=0 quad=0 pulse=0     <- B alone drops; A never moves
[enc] A=1 B=0 quad=0 pulse=1     <- only the pulse decoder counts it
[enc] A=1 B=1 quad=0 pulse=1     <- B returns high
```

A quadrature encoder moves both channels in a phase-shifted pair. This one
drives exactly one line per detent: `A=0` steps backwards, `B=0` forwards.
Later in the same log the quadrature decoder *invented* a detent from
`A=1 B=1`, which is the failure mode you would expect from feeding it a signal
it was never designed for.

`BOARD_ENCODER_QUADRATURE=0` is now the default.

**Why it was hard to see:** MENU, EXIT and the wheel *press* all worked, so
the symptom read as "the wheel is broken" rather than "the wheel is
mis-decoded". The clue was `wake=button/rotary gpio=4` — a pin that can wake
an ESP32 from deep sleep is unquestionably switching, so the wiring was never
in doubt. Only the interpretation was.

**What made it cheap to fix:** the losing decoder was already written, already
tested, and already selectable. The decision to build both rather than pick one
cost an afternoon in Milestone 1 and saved a redesign here.

### 2.2 Page state was not in RTC memory

`g_naba_page` and `g_reader_page` were plain globals, so every deep-sleep wake
silently rewound the reader to page 1. Every other piece of UI state on the
self-test screen already lived in `RTC_DATA_ATTR`; these were simply missed.

Combined with §2.3 and the documented "first detent after a wake is consumed by
the wake itself", the user-visible symptom was **"pages are not changing"** —
three unrelated causes producing one indistinguishable result.

### 2.3 A 20-second idle timeout is hostile to reading

`kIdleSleepMs = 20000` is right for a self-test screen and wrong for a page
someone is looking at. Demo builds now idle for five minutes.

### 2.4 Repainting when nothing changed

The panel was being refreshed for events that changed no pixels:

| Trigger | Why it repainted |
|---|---|
| Rotating at page 1 or the last page | `g_dirty` was set *before* the page was clamped |
| Pressing the wheel | set `g_dirty` though a mushaf page has nothing to redraw |
| Pressing MENU | forced a **full** refresh — a 2 s white flash — with nothing to redraw |
| Pressing EXIT | slept the device; the next touch woke it into a full clear-and-redraw |

This compounded badly. `kPartialsBeforeFullRefresh = 8` forces a full-screen
flash every eighth refresh to clear ghosting, and **wasted refreshes counted
toward that tally** — so idle spinning at a page boundary triggered the big
flash for nothing.

EXIT was the worst of the four: sleeping is cheap, but **waking is expensive**,
because the panel driver re-runs its clear-and-full-refresh on every wake. One
accidental EXIT cost a two-second white flash the next time anything was
touched. In demo builds EXIT no longer sleeps.

---

## 3. Flicker: what is a bug and what is physics

After §2.4, the remaining flashes are all deliberate:

- **Every wake flashes white.** `Epd579Display::begin()` runs the panel's
  clear-and-full-refresh on each wake. A partial refresh needs the
  controller's *previous* framebuffer as reference and deep sleep destroys it;
  27 KB does not fit the ESP32-S3's ~8 KB RTC slow memory
  ([architecture.md §4.2](architecture.md)). The fix already exists —
  `Epd579Display::setRestorePath()` persists the frame to SD and restores it on
  wake — **but it needs a card**, and every log line in this session reads
  `restored=0`.
- **Every 8th page turn is a full refresh** to clear accumulated ghosting.
  `kPartialsBeforeFullRefresh` is the single constant that trades flash
  frequency against ghosting.

---

## 4. Not verified

| Item | Why |
|---|---|
| **SD card** | Superseded by §6 — a card is now in the slot and answers on the bus, but it is exFAT and will not mount. Still blocks: real content, the wake-without-flashing path, and `state.bin` persistence. |
| Wi-Fi / HTTP API | Needs provisioning; transfer mode is reachable (long-press the wheel) but has never been driven. |
| BLE provisioning | GATT wiring compiled, never exercised. |
| Battery | This board revision publishes no battery-sense GPIO. |
| Sleep current | Needs a multimeter in series with the BAT connector. |
| Rotary debounce under fast spin | Only slow, deliberate turns were tested. |

---

## 5. Method note

Two throwaway-looking pieces of instrumentation did most of the work here, and
both are worth keeping:

- **`[enc] A=.. B=.. quad=.. pulse=..`** logged the raw pin levels *alongside
  both decoders' counters*. That single line is what turned "the wheel doesn't
  work" into "the pulse decoder is the right one" in one capture.
- **`[epd] repaint mode=.. ink=N/27200 flush=.. in Nms`** separates three
  otherwise identical white screens: the renderer drew nothing (`ink=0`), the
  renderer drew but the panel did not show it (`flush=0`), and the panel really
  refreshed (`flush=1 in ~2000ms`). A flush that "succeeds" in a few
  milliseconds means BUSY was never observed and nothing reached the glass.

The general lesson: when a device has one output and it is a slow one, the
serial log has to carry enough state to distinguish failures that look
identical on the glass. Both of these were added *after* guessing wrong once.

---

## 6. Second session, 2026-08-31: the SD card

A card was in the slot for the first time. It does not mount — but the reason
turned out to be worth three separate corrections to what this log and
[pending.md](pending.md) claimed.

### 6.1 The missing COM port was a tooling artifact

The previous session recorded that no serial port was visible from the dev
machine, and guessed at a missing CH34x driver. The driver was fine. The
query was wrong: `Win32_SerialPort` under-reports USB-serial bridges, and
enumerating PnP devices instead finds the board immediately.

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match 'COM\d+' }
```

```
USB-SERIAL CH340 (COM6)   Status: OK   USB\VID_1A86&PID_7523
```

Worth keeping as the first thing to try, because the wrong query produces a
confident, plausible, and entirely false hardware conclusion.

### 6.2 The card is exFAT

The product build (no `NABA_DEMO`, no `READER_DEMO`) reported:

```
[   382][E][sd_diskio.cpp:805] sdcard_mount(): f_mount failed: (13) There is no valid FAT volume
[sd] no card / wiring
```

The self-test row said wiring; the core said filesystem. **The core was
right.** FatFs only reaches `FR_NO_FILESYSTEM (13)` *after* `disk_initialize`
has completed the card's CMD0/ACMD41 handshake, so reaching that error is
itself proof that the card is present, powered, and talking. `kSdPowerEn` (42),
CS (10) and the SPI wiring are therefore all verified — by a failure.

The card was later confirmed exFAT. The Arduino framework cannot read it: see
§6.4.

### 6.3 Ruling out the SPI clock first

`FR_NO_FILESYSTEM` has two causes that look identical. The card really has no
FAT volume — or the card initialises fine at 400 kHz and the *first full-speed
read of sector 0* comes back garbled, so FatFs finds no BPB and reports the
same 13. `kSdSpiHz` is 40 MHz, well inside the range where that is plausible.

Dropping `kSdSpiHz` to 4 MHz and reflashing produced a **byte-identical**
failure, which killed the clock hypothesis in one cycle and left the
filesystem as the only remaining cause. The constant was reverted.

Incidental finding: `ff_sd_initialize` clamps any frequency above 25 MHz, so
`kSdSpiHz = 40000000` has always run at 25 MHz. The comment in
`board_crowpanel_579.h` weighing 40 MHz against Elecrow's 80 MHz is describing
a choice the core overrides.

### 6.4 exFAT is not reachable from this framework

Not a matter of a config flag:

- `FF_FS_EXFAT` is **hardcoded** to `0` in the SDK's `ffconf.h`, not bound to a
  `CONFIG_` macro the way its neighbours are. There is no sdkconfig knob.
- `libfatfs.a` ships **prebuilt** with the Arduino framework, so editing that
  header changes nothing — the exFAT code paths are not in the binary.

Reaching exFAT would mean rebuilding the IDF from source, or replacing the
`SD.h` backend with SdFat. Neither is worth it: the largest file this device
will ever hold is a few megabytes against exFAT's 4 GB threshold, and FAT32
works on cards of any size — the 32 GB ceiling is in Microsoft's *format
dialog*, not in FAT32. **The card should be formatted FAT32**, which on a card
over 32 GB needs guiformat or Rufus rather than the Windows dialog, and not
the SD Association's formatter (it writes exFAT above 32 GB by spec).

### 6.5 The fix: an error string that stops blaming the wiring

`SD.begin()` returns a bool, collapsing "no card on the bus" and "card answers
but carries no FAT volume" into one failure — and `SdStorage::begin()` reported
`"no card / wiring"` for both. That sends you looking for a hardware fault when
the real fix is a reformat.

`SD.cardType()` cannot disambiguate: `SD.cpp` sets `_pdrv = 0xFF` on any
`begin()` failure, so it returns `CARD_NONE` regardless. But FatFs's result
code already separates the two — `FR_NOT_READY (3)` for a card that never
answered, `FR_NO_FILESYSTEM (13)` for one that did. `sd_diskio.h` is public in
the SD library, so `ClassifyMountFailure()` re-runs the mount one layer down
purely to read that `FRESULT`, then unwinds everything it allocated. It runs on
the failure path only.

| `FRESULT` | Row reads | Meaning |
|---|---|---|
| `FR_NO_FILESYSTEM` | `Card: present, no FAT` | card is fine, reformat it |
| `FR_NOT_READY` | `Card: no card / wiring` | nothing answered on the bus |
| `FR_DISK_ERR` | `Card: card read error` | answered, reads failing |
| other | `Card: mount failed` | |

Both of the first two branches were verified on hardware — the card happened
to be out of the slot for one boot, which tested the `FR_NOT_READY` path for
free.

Two things this cost:

- **Strings are capped at 19 characters.** The `Card:` row starts at x=560 and
  advances 12 px per character on a 792 px panel, so `"card present, no FAT"`
  (20) silently lost its final `T`. `setPixel` discards out-of-range pixels via
  `mapPixel`, so there is no overrun — just a missing letter, which is the kind
  of defect that survives review. The cap is now a comment above the switch.
- **A cardless boot is ~900 ms slower**, because the CMD0 retry timeout is paid
  twice: SD bring-up goes from ~0.65 s to ~1.6 s. Nothing is paid when a card
  is present, since the probe never runs on the success path and the
  `FR_NO_FILESYSTEM` path returns immediately.

### 6.6 Still unverified

Mount, read, write, and removal mid-write remain untested — everything above is
a *failure* path. The card must be FAT32 before `BringUpStorage()` can append
its first line to `/DEVICE/selftest.log` and prove the card is writable rather
than merely present.
