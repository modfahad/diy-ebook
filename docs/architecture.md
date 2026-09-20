# Architecture

Status: **Phase 1, Milestones 1-4 complete.** The firmware bring-up layer, the
QPK1 content package, the document conversion pipeline and the desktop-to-device
transport are implemented. Everything else in this document is either a stable
interface those milestones already commit to, or an explicitly deferred
decision.

---

## 1. Why the layering looks like this

The product has to survive two later phases that historically force rewrites:

- **Phase 2 (Hifz)** adds new *content types* and new *screens*, not new
  hardware.
- **Phase 3 (AI)** adds *network sessions* and *word-level highlighting*, which
  needs the renderer to be able to answer "where is word N on this page".

So the seams are drawn between *hardware*, *content*, and *presentation*, and
Milestone 1 puts real code on the hardware side of every seam even though it
only needs a self-test screen today.

```
        ui/                     application screens (pure render + state)
         |
    hal::IDisplay  hal::IInput  hal::IStorage  hal::IPower
         |             |            |             |
   drivers/epd   drivers/input  storage/      power/
         |             |            |             |
         +-------------+------------+-------------+
                       |
            board/board_crowpanel_579.h        <- the only file with GPIOs
```

Rules that hold from now on:

1. No file outside `board_crowpanel_579.h` contains a GPIO number.
2. UI code talks to `hal::` interfaces, never to a driver type.
3. Decode/policy logic that does not need hardware lives in `util/` as
   header-only code, so it is unit-testable on the host. Debouncing, quadrature
   decoding, rotary coalescing and the sleep policy are all there.

## 2. Directory map

| Path | Contents | State |
|---|---|---|
| `firmware/include/board/` | pin map, panel geometry, wake mask | done |
| `firmware/include/hal/` | `IDisplay`, `IInput`, `IStorage`, `IPower` | done |
| `firmware/include/util/` | debounce, rotary, idle policy (header-only, pure) | done |
| `firmware/include/gfx/` | 1bpp `Canvas`, 5x7 font | done |
| `firmware/drivers/epd/` | GDEY075T7 `IDisplay` impl over GxEPD2 | ported, not yet run on the panel |
| `firmware/drivers/input/` | `IInput` impl | done |
| `firmware/drivers/gfx/` | canvas + font implementation | done |
| `firmware/storage/` | SD `IStorage` impl | done |
| `firmware/power/` | `IPower` impl, EXT1 deep sleep | done |
| `firmware/ui/` | self-test screen | done |
| `firmware/qpk/` | QPK1 validating parser | done |
| `packages/qpk-format/` | TypeScript QPK1 reader/writer + validator | done |
| `tools/package-inspector/` | package dump + validation CLI | done |
| `desktop/converter/` | PDF/EPUB/TXT + Quran JSON -> QPK1 | done |
| `tools/pdf-converter/`, `tools/quran-validator/` | conversion and validation CLIs | done |
| `firmware/wifi/`, `firmware/ble/` | transport, provisioning logic (pure, host-tested) | Milestone 4, done |
| `firmware/drivers/net/` | HTTP server, BLE GATT, NVS credentials (Arduino-dependent glue over the above) | Milestone 4, done -- not run on hardware |
| `desktop/app-bridge/` | one-shot Node subprocess the app drives both TypeScript libraries through | Milestone 5, done |
| `desktop/src*` | Tauri UI: Dashboard, Library, Device, Converter | Milestone 5, done |
| `tools/arabic-pager/` | fetch, shape (HarfBuzz+FreeType) and render Quran content | Milestone 5, done |
| `firmware/ui/quran_screen.cpp` | on-device Quran reader: blits shaped glyph runs from the atlas | Milestone 5, done -- confirmed on hardware |
| `ai-server/` | optional speech / alignment | Phase 3 |

## 3. Hardware facts that shaped the design

All resolved from Elecrow's published example sources for this exact board
([Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792](https://github.com/Elecrow-RD/CrowPanel-ESP32-5.79-E-paper-HMI-Display-with-272-792)),
not guessed. Provenance for each value is recorded in the board header.

### 3.1 The panel: a GDEY075T7 in the CrowPanel's socket

The board shipped with a 5.79" panel built from two cascaded SSD1683
controllers (792x272, with an 8-column dead gap in controller RAM). That panel
has been replaced with a Good Display **GDEY075T7-T01**: 7.5", **800x480**,
black/white, a single **UC8179** controller, on the same 24-pin FPC socket.

Consequences baked into the code:

- `drivers::Epd750Display` drives it through GxEPD2's `GxEPD2_750_GDEY075T7`.
  UI code still draws on `gfx::Canvas` (48,000 bytes, in PSRAM); `flush()`
  copies it into GxEPD2's buffer with `drawInvertedBitmap()` and calls
  `display()` (full) or `display(true)` (partial). GxEPD2 needs Arduino, the
  canvas does not, which is what keeps every `ui::` screen host-testable.
- Orientation is GxEPD2's `setRotation(board::kDisplayRotation)`, not the
  canvas's; the canvas is always in logical coordinates.
- `gfx::Canvas` keeps its seam option for two-controller panels, but this panel
  passes 0. `Canvas::clippedPixels()` counts ink drawn off the 800x480 canvas;
  host tests assert it stays 0 with worst-case content and the repaint log
  prints it.
- **Hardware risk:** the CrowPanel's booster ballast resistor is fitted for
  the SSD1683 panel, and UC8179 panels usually want the other value. A faint,
  blotchy or half-refreshed image points at that resistor before firmware.

### 3.2 Panel on hardware SPI, SD on its own bus

The panel uses the ESP32-S3's global `SPI` object (FSPI). GxEPD2's `init()`
starts it on the S3's default pins (SCK 12, MISO 13, MOSI 11), after which
`Epd750Display::begin()` calls `SPI.end()` and `SPI.begin(12, -1, 11, 45)`
to put it on the CrowPanel's panel pins. The Elecrow-derived bit-banged
driver is gone.

GPIO 13 is also the SD card's MISO. On the pinned Arduino core
(espressif32@6.5.0, core 2.0.14) that brief default attach does not take the
pin away from the SD bus; on core 3.x its peripheral manager would, so moving
to core 3 means calling `SPI.begin(12, -1, 11, 45)` *before* `init()`.

The SD card sits on a **separate** HSPI bus (39/13/40/10), so there is no
cross-peripheral CS discipline to get wrong.

### 3.3 Two power-enable rails

- `GPIO7` = panel power, must be HIGH before any EPD GPIO is driven.
- `GPIO42` = SD power, must be HIGH before `SD.begin()`.

Both are driven LOW before deep sleep.

### 3.4 Deep sleep and wake — the decision that had to be settled now

ESP32-S3 EXT1 wake only works on **RTC-capable GPIOs, which are GPIO0..GPIO21**.

The five front-panel inputs are GPIO 1, 2, 4, 5, 6 — all inside that range, and
all idle HIGH with on-board pull-ups and go LOW when actuated. Therefore:

> A single `ESP_EXT1_WAKEUP_ANY_LOW` mask covers MENU, EXIT, the encoder switch
> **and** both encoder channels, with no trigger-mode conflict.

`ANY_LOW` really is "any" here, which is worth stating because it is not true
on every ESP32: the original ESP32 only offers `ALL_LOW`/`ANY_HIGH`, and a mask
of five pins under `ALL_LOW` would mean "wake when all five are pressed at
once" — i.e. never. From the S2 onwards the enum is
`ESP_EXT1_WAKEUP_ANY_LOW = 0`, `ESP_EXT1_WAKEUP_ANY_HIGH = 1`, with `ALL_LOW`
kept only as a deprecated alias of `ANY_LOW`. Confirmed against
`framework-arduinoespressif32/tools/sdk/esp32s3/include/esp_hw_support/include/esp_sleep.h`.

`board_crowpanel_579.h` carries `static_assert`s on every wake pin so that
moving a pin out of the RTC range fails the build rather than silently
disabling wake.

**A quadrature encoder cannot be decoded through deep sleep**: the transition
that caused the wake is already gone by the time code runs. The decision, made
explicitly:

> Both encoder channels are wake sources, and **the first detent after a wake
> is consumed by the wake itself**. The device wakes and repaints, but does not
> also advance the page by one.

The alternative (wake only on the encoder push switch and the two buttons)
would make a spin of the wheel do nothing at all on a sleeping device, which
reads as a broken product.

### 3.5 Pad holding

`GPIO7` (panel power) is RTC-capable, so it is latched LOW through deep sleep
with `gpio_hold_en()` + `gpio_deep_sleep_hold_en()`, and released in
`PowerManager::begin()` on the next boot. Forgetting the release would leave
the panel rail stuck off.

**Known limitation:** `GPIO42` (SD power) is a plain digital pad and *cannot* be
held through deep sleep, so it floats while asleep. The card's idle draw is
small; the actual cost is unmeasured until hardware is in hand.

## 4. Power model

```
   deep sleep  (E-Ink retains the page, ESP32 off, Wi-Fi off, BLE off,
       |        both rails low)
       |  EXT1 ANY_LOW on {MENU, EXIT, ENC_SW, ENC_A, ENC_B}
       v
   PowerManager::begin()      release pad holds, read wake cause, bump RTC
       |                       boot counter
       v
   InputManager::begin()      sample pins before anything else runs
       |
       v
   Display::begin()           rail up -> GxEPD2 init -> SPI remap  (idempotent)
   Storage::begin()           rail up -> mount
       |
       v
   render + flush             one E-Ink refresh
       |
       v
   event loop                 poll @2 ms, coalesce rotary, repaint on change
       |
       |  IdlePolicy: 20 s with no input, nothing busy, no pin held
       v
   Display::end()  Storage::end()  -> rails low -> esp_deep_sleep_start()
```

Two guards in `util::IdlePolicy` exist because both failure modes are easy to
hit and hard to debug on a battery device:

- `busy` — never sleep mid-refresh or mid-mount.
- `input_held` — never sleep while a wake-source pin is still LOW, or the
  device wakes again the instant it sleeps.

### 4.1 Refresh policy

`RefreshMode::kPartial` is the default for content changes. Partial refreshes
accumulate ghosting, so `Epd750Display` forces a full refresh every
`kPartialsBeforeFullRefresh` (8) partials. MENU forces a full refresh on demand.

Rotary input is **coalesced** (`util::RotaryCoalescer`): detents accumulate
until the user pauses for 140 ms or 6 detents pile up, then a single
`kRotate` event with a summed delta is emitted. A fast spin costs one refresh,
not one per click. A direction reversal discards the queue so a correction does
not overshoot twice.

### 4.2 The wake flash, and the saved-frame restore

A partial refresh needs the controller's *previous* image as its reference,
and after deep sleep that reference is gone. The 800x480x1bpp frame is 48 KB,
which does **not** fit the ESP32-S3's ~8 KB RTC slow memory, so the last frame
pushed to the panel is written to `/DEVICE/framebuffer.bin` after every
repaint.

On a real wake (not a cold boot) `Epd750Display::begin()` reads it back, calls
GxEPD2's `init()` with `initial = false`, and writes the frame into the
controller as the previous image (`epd2.writeImageToPrevious()`). The first
repaint can then be partial, with no white flash. With no usable frame -- cold
boot, no card, or a file of the wrong size (including one saved by the 5.79"
build) -- it calls `init(115200, true, 2, false)` and the first repaint is a
full refresh.

### 4.3 Transfer mode: the second power state

Milestone 4 breaks an assumption every earlier milestone was built on. The
model above is "wake, render, sleep": the device is awake for a second or two
at a time and `IdlePolicy` puts it back to sleep after 20 s of no input. An
upload takes **minutes**, with no button presses at all — and spec sections 6
and 7 equally forbid the other easy answer, a permanently associated Wi-Fi
connection.

So there is a second, explicitly entered state:

```
   deep sleep
       |  long-press the encoder switch (stopgap for "user selects Connect")
       v
   CONNECTING              Wi-Fi associating with the saved credentials
       |
       v
   TRANSFER MODE          Wi-Fi ON, mDNS advertising, HTTP server listening,
       |                  IdlePolicy::setBusy(true) for the whole session
       |
       |  upload / delete / backup, minutes at a time
       |
       |  EXIT, or the session timeout expires (app::kTransferSessionTimeoutMs)
       v
   Wi-Fi OFF -> setBusy(false) -> normal idle -> deep sleep
```

Three consequences that are design constraints, not details:

- **It is always user-initiated.** A provisioned device brings up neither radio
  at boot. `HttpServer::begin()` is called from `main.cpp`'s
  `PollTransferMode`, entered by a long-press on the encoder switch
  (`ToggleTransferMode`) — a stopgap for the menu action Milestone 5 will
  replace it with, not a redesign of this rule. See
  [development.md §10](development.md) item 1. The rule itself is the
  reader-facing promise: the radio is on because you asked.
- **`setBusy(true)` is the mechanism, but the mode is its own concept.**
  Conflating "mid-refresh" with "in a transfer session" would confuse the
  library UI in Milestone 5, so the app carries an explicit mode rather than
  inferring it from the busy flag.
- **Progress repaints must be throttled.** A partial refresh costs ~1 s and
  every eighth is a full one (§4.1). Repainting per chunk would make the
  transfer slower than the radio. The status screen repaints on percentage
  bands and a minimum interval, never per chunk.

**This is the largest sustained power draw the device will ever have.** Wi-Fi
for minutes dwarfs everything the reading path does, which is exactly why it
is gated behind a deliberate user action rather than left running.

BLE and Wi-Fi are **never up at once**: `BleProvisioning::end()` deinitialises
the stack before Wi-Fi is brought up for a transfer session. Both radios
resident together is the classic way to run an ESP32-S3 out of heap.

### 4.4 Table-clock mode: the home screen

`app::kTableClockMode` (on) turns the device into a USB-powered table clock.
It boots to `ui::HomeScreen`: the clock, date and a month calendar on the
left, and a 400x480 photo in four greys on the right. It never deep-sleeps,
brings Wi-Fi and the HTTP API up at boot, and reconnects every
`app::kWifiReconnectIntervalMs` if the network drops. MENU cycles
home -> library -> self-test; EXIT from the top of a menu returns home.

**Time.** SNTP over Wi-Fi, shown in the POSIX TZ rule the desktop app sends
(`POST /api/device/time`, stored in `/DEVICE/timezone.txt`). The desktop
derives the rule from its own clock, DST transitions included
(`device-client/src/photo.ts` `posixTimeZone`), so the device needs no
time-zone database.

**Photos.** The desktop app's Photos tab crops a picture to 400x480, dithers
it to the panel's four greys and uploads the finished file
(`/api/photos/<name>/chunk`, `/finish`); `net::PhotoStore` keeps them as
`/PHOTOS/<name>.g4` (a 16-byte `QPH1` header plus 2 bits per pixel). They are
shown in name order.

**Refreshes** (`PollHomeScreen` in main.cpp):

- a full 4-grey frame (~3.7 s, flashes) when the screen is entered, when the
  date changes, when the clock first syncs, when photos or the time zone
  change, and every `app::kPhotoIntervalMinutes` (10) with the next photo --
  at least 15 s apart, so a burst of uploads is not a burst of flashes;
- on every other minute, only the clock digits' rectangle, as a black/white
  window update (~0.6 s, `Epd750Display::flushWindow`).

GxEPD2_4G cannot do the second on its own: its first black/white write after a
grey refresh clears both controller RAM planes and forces a full black/white
refresh, wiping the photo. `flushWindow` instead sends the fast OTP partial
waveform with "partial in" (0x91) around the clock rectangle only. That
sequence was run on the panel as variant B1 of `src/grey_test.cpp`
(`pio run -e grey_test`), where all four candidates completed without BUSY
timeouts; **whether B1 leaves the grey photo visibly untouched has not been
confirmed by eye.** If it does not, A1 (a grey window update, ~2.8 s) is the
fallback in the same experiment.

## 5. Rotary encoder: resolved on hardware

**It is not a quadrature encoder.** Settled 2026-08-30 on the real board.

Elecrow's example names the two rotary channels **PRV** and **NEXT** and polls
each independently, which is not how a quadrature encoder behaves — but the
board documentation did not settle it, so rather than guess, both
interpretations were built and run side by side:

- `util::QuadratureDecoder` — Gray-code state machine with detent detection.
- `util::PulseDecoder` — one falling edge on a channel is one step in that
  direction.

A build flag picked which one drove the UI, and both counters were logged
live. Turning the wheel one detent produced:

```
[enc] A=1 B=1 quad=0 pulse=0
[enc] A=1 B=0 quad=0 pulse=0     <- B alone drops; A never moves
[enc] A=1 B=0 quad=0 pulse=1     <- only the pulse decoder counts it
[enc] A=1 B=1 quad=0 pulse=1     <- B returns high
```

A quadrature encoder moves both channels in a phase-shifted pair. This one
drives exactly one line per detent, so the pulse interpretation is correct.
`util::PulseDecoder` is now the only decoder built; `util::QuadratureDecoder`
and the build flag that chose between them were deleted along with the
comparison logging once the question was settled.

One thing this cost, worth recording: the wheel did nothing at all until this
was settled, while MENU, EXIT and the wheel *press* all worked — so the
symptom pointed at the wheel being broken rather than at the decoder being the
wrong one. GPIO 4 (`kEncoderB`, "NEXT") waking the device from deep sleep was
the clue that the wiring was fine.

## 5a. Content packages

The QPK1 container is specified in [qpk-format.md](qpk-format.md). Three
architectural points belong here rather than there:

**Storage grew a file handle.** `hal::IStorage::read(path, ...)` opens and
closes the file per call, which is right for `state.bin` and catastrophic for a
parser that resolves an ayah by reading a 24-byte record. `IStorage::open()`
now returns an `hal::IFile` held open for the package's lifetime, drawn from a
fixed pool of 4 handles inside `SdStorage` — no heap. `IFile::readExact()`
loops over short reads centrally, so no call site can silently truncate a
record.

**The parser is host-testable because it never touches Arduino.** It reads
through `hal::IFile`, so the same code runs against an SD card on the device
and a `qpk::MemoryFile` in the tests. That is what makes a corruption matrix
practical: build a valid package, flip one thing, assert the specific rule.

**Checksum cost is split deliberately.** The device verifies only the
fixed-size index sections when it opens a package — tens of kilobytes, and they
are the structures every later bounds check trusts. Blob sections
(`TEXT_DATA`) are not verified at open, because reading one would be exactly
the full scan the format exists to avoid; their integrity is established once,
at install time, via `payload_crc32`. The desktop reader verifies everything,
because it can afford to and it is the side that catches a bad transfer.

**Two writers, one format.** `packages/qpk-format` (TypeScript) and the C++
test builder both emit QPK1. A golden fixture generated by the TypeScript
writer is committed as a binary and as a generated C++ header; the firmware
test suite parses it and asserts it is byte-identical to the C++ builder's
output. Without that, "both sides implement the spec" would be an assumption.

## 5b. Getting content in

The conversion pipeline is designed in [conversion.md](conversion.md). One
decision belongs in the architecture record rather than only there:

> **Generic books are parsed. The Quran is imported.**
>
> PDF/EPUB/TXT go through extraction heuristics into a `BOOK` package. Quran
> packages are built only from a structured, verified JSON source, and
> `convert()` refuses to produce one from a PDF.

The reason is that heuristic extraction is good enough for a novel and
categorically not good enough for scripture: a heuristic that drops a diacritic
produces a package that validates perfectly and is wrong. Spec section 12
already warns against assuming a PDF holds clean machine-readable Quran text;
this makes that warning structural instead of advisory. Word coordinates are an
optional enrichment layer on top of a validated text source.

The converter is a library with no Tauri or React dependency. The CLIs drive it
and, since Milestone 5, so does the desktop app -- through
`desktop/app-bridge`, a one-shot Node subprocess, rather than by being ported
into the webview. `desktop/app-bridge/README.md` records why: `yauzl` is
Node-only, `pdfjs-dist` needs its legacy build, and content ids come from a
synchronous `node:crypto` hash the format and the firmware both pin.

## 5c. Touch: the same queue, one finger

The -T01 panel carries a GT911 capacitive layer on its own ribbon (the pins
are in `board_crowpanel_579.h`; the bench steps are board-test-checklist.md
section 11). **Nothing on the glass reacts to it yet** — what exists is the
input path, built the same shape as every other input:

```
GT911 over I2C -> drivers::TouchGt911 (hal::ITouchPanel)
                    -> drivers::InputManager::pumpTouch
                       -> util::TapTracker          (pure, host-tested)
                          -> hal::InputEvent{kTouch, kClick|kLongPress, x, y}
```

Four decisions worth keeping:

- **One queue, not two.** Touch events come off `nextEvent()` beside MENU,
  EXIT and the wheel, so `lastActivityMs()` and the idle/sleep policy keep
  working untouched. A parallel touch path would have forked the sleep logic,
  which is the part of this firmware least able to afford a second copy.
- **One finger.** The chip reports five; the UI acts on the first. A palm on
  the glass cannot fire a second action.
- **Taps, not gestures.** A refresh costs about half a second, so anything
  wanting continuous feedback — drag-scrolling, swipe-to-page — would feel
  broken. `util::TapTracker` classifies down / tap / long press / up, and a
  finger that slides more than `app::kTouchSlopPx` is none of them (it is
  still activity, so the device does not sleep under a moving finger).
- **Additive, never load-bearing.** If the GT911 does not answer — not
  fitted, not wired, ribbon reversed — `begin()` logs it once and the front
  panel behaves exactly as it always has. The buttons are the primary input;
  touch is the addition.

**Sleeping, and waking on a tap, are the same decision from opposite ends.**
The GT911 draws about 8 mA scanning and about 100 uA asleep, and a chip that
is asleep cannot notice a finger. So `app::kTouchSleepWithScreen` (on) tells
it to sleep when the device does -- nothing has to wake it again, because
`begin()` toggles RST on every wake, which is the datasheet's way back --
while `app::kWakeOnTouch` (off) leaves it scanning instead and adds its INT
line to the EXT1 wake mask (`board::kWakeMaskWithTouch`; INT is RTC-capable,
which is what makes this possible at all).

Waking on a tap stays off until the bench confirms one thing, which the setup
screen now prints live: **INT must idle HIGH and go LOW under a finger.** EXT1
here is ANY_LOW, so the opposite polarity is a device that wakes the instant
it sleeps, forever. The pin also keeps its RTC pull-up through sleep, because
the GT911 leaves INT high-impedance between signals, and a floating pin in an
ANY_LOW mask is that same failure by another route.

## 5d. The options menu: one meaning for a held OK

OK used to mean something different on every screen, and the glass never said
which. Click was "open" in the library, "text size" in a book, "go to page" in
a picture book. Hold was "save this place", or "delete this bookmark", or
"turn transfer mode on", by screen. Holding EXIT factory reset the device,
with no warning and no confirmation.

**Hold OK now opens `ui::OptionsMenu` on every screen**: that screen's actions,
listed by name. The wheel moves, OK chooses, EXIT closes, and because the rows
are 40 px tall and the full width of the rules, a tap chooses too — the first
thing on this device a finger can actually do. The old hold meanings are gone;
each is a row where it belongs.

- **A modal, not a `ScreenMode`.** The screen underneath keeps all of its
  state, so closing the menu is a repaint rather than a re-entry — it matters
  most for the Quran reader, whose position is expensive to rebuild.
- **`build()` is a pure function of a `MenuContext`.** Which rows exist
  depends on the screen and on six flags (translation? transfer on? any
  bookmarks? …), all of which main.cpp already knows. That is what makes the
  whole menu host-testable without a device.
- **Destructive rows ask.** `buildConfirm()` turns Factory reset into a
  two-row question with the highlight on "No, keep everything". Nothing else
  asks, so ordinary rows stay one press.
- **Every list can be left.** `AddTail()` always appends "Close this menu",
  and "Sleep now" except in table-clock mode, where the device never sleeps.
- **`rowAt()` is the touch half.** Taps on the margins are ignored rather
  than closing the menu: until the touch orientation is confirmed on real
  glass, a stray mapping should cost nothing.

Taps act on three more screens, and only ever do what a button already does:

- **A library row or shelf tile opens it** (`ui::LibraryScreen::rowAt`,
  which handles the list and the covers, and includes a tile's title strip
  because it reads as part of the same thing). One tap opens rather than
  selecting and waiting for a second: each extra step is another half-second
  refresh, and EXIT undoes a mis-tap.
- **The left and right edges of a reading screen turn the page**
  (`util::PageTapDelta`, `app::kTouchPageEdgePx` = 240 px each side). The
  320 px band down the middle deliberately does nothing: a page turn costs
  half a second and the reader's place, and a hand holding the device rests
  near the centre.
- **The saved places list and the surah picker open a tapped row too**
  (`ui::BookmarksScreen::rowAt`, `ui::SurahPickerScreen::rowAt`). The
  bookmarks list keeps no scroll position of its own -- it derives one from
  the selection -- so its hit test asks `scrollTop()` rather than assuming the
  list starts at the top.
- **A page turn by tap goes through `HandleRotate()`**, the wheel's own
  per-screen logic, moved out of `HandleEvent` rather than copied. The clamps,
  the surah boundaries and the Quran's history stack were settled on real
  hardware; there should only ever be one of them.

A hold on the glass is deliberately NOT the options menu -- a thumb resting
while reading would open it. Hold OK does that.

## 5e. Which way up, and which way round: the setup screen

Two facts about this device cannot be known at build time. Whether the board
sits upside down in its case is a `TODO(hw)` in the board header. Which way
round the touch layer reports its axes is worse: a laminated panel can swap or
mirror them, the spec sheet does not say, and there are eight possibilities.

`ui::SetupScreen` (Options -> "Screen and touch setup", from the clock screen
or the hardware test screen) settles both on the device:

- **MENU turns the picture** 180 degrees, applied to the panel immediately,
  because looking at it is the only way to tell which way is up.
- **OK, or the wheel, steps through the eight touch orientations.**
- **Two targets, not one.** A single corner cannot distinguish swapped axes
  from straight ones -- (0,0) maps to (0,0) either way. Target 2 sits in the
  opposite top corner, which a swap sends to the bottom of the screen, so
  filling in both means the orientation is genuinely right. Changing the
  orientation clears both, because those hits belonged to the old one.
- **EXIT saves and leaves.** Nothing is written until then, which is what
  makes trying all eight safe.

It is deliberately not a list of rows: a wrong touch orientation is exactly
the situation where a list cannot be used, so every control is a physical
button and touch only ever reports where it thinks the finger went.

`util::ScreenSetup` is the saved form -- one line of text, `rotation=2
touch=5`, in `/DEVICE/screen.txt`, read at boot before the panel comes up. It
is text because the person most likely to read it is someone debugging a
device with a card reader and no compiler. Anything unparseable falls back to
the board header's defaults rather than to an unusable screen.

The two settings stopped being compile-time constants to make this work:
`drivers::Epd750Display::setRotation()` replaces the four places that read
`board::kDisplayRotation` (including the partial-window and restored-frame
paths, which map canvas coordinates to panel-native ones), and
`drivers::InputManager::setTouchOrientation()` replaces the touch mapping's
three flags. Both keep the board header's values as their defaults, so a
device with no `/DEVICE/screen.txt` behaves exactly as it did before.

## 6. Storage layout

Committed now (spec sections 15/16) so later milestones do not each invent
paths. Milestone 1 creates the directories and writes `selftest.log`; nothing
else uses them yet.

```
/DEVICE/   state.bin  settings.bin  selftest.log
/USER/     bookmarks.bin  progress.bin  goals.bin  mistakes.bin  history.bin
/LIBRARY/  library_index.bin
           QURAN/  BOOKS/  TRANSLATIONS/  TAFSIR/
```

User state is separate from content so a package can be replaced without
touching reading position, and so a backup is a small, well-defined set of
files.

## 7. Deferred decisions (not silently made)

| Question | Status |
|---|---|
| Battery sense GPIO | **Unknown.** Elecrow publishes none. `kBatteryAdcPin = -1`; `IPower::batteryAvailable()` returns false. Resolve from the schematic before implementing. |
| Encoder is quadrature or pulse | Both implemented, resolved on hardware. |
| PSRAM is octal or quad | `qio_opi` assumed (N8R8). Self-test screen reports PSRAM pass/fail; flip to `qio_qspi` if it fails. |
| Serial is UART bridge or native USB | UART0 assumed (`ARDUINO_USB_CDC_ON_BOOT=0`), matching Elecrow's CH34x driver instructions. |
| SD clock rate | 40 MHz, conservative. Elecrow's demo asks for 80 MHz. |
| Display rotation | `board::kDisplayRotation` = 0 (GxEPD2 `setRotation`). **TODO(hw):** confirm against the enclosure; 2 if upside down. |
| Hardware SPI for the panel | Done with the GDEY075T7 port: GxEPD2 on the global SPI, remapped to 12/11/45. |
| Panel booster resistor | **Unresolved hardware risk.** Fitted for the SSD1683 panel; UC8179 panels usually want the other value. Suspect it first if the image is faint or half-refreshed. |
| QPK1 binary layout | Specified in [qpk-format.md](qpk-format.md) and implemented on both sides. |
| Compressing `TEXT_DATA` | Deferred: it trades SD reads for CPU and PSRAM, and the decision needs a measurement on real content. |
| `LAYOUT_DATA` internals | Implemented on both sides and **consumed by the on-device renderer since 2026-09-02**: `ui::QuranScreen` lays out whole Madinah-mushaf lines from it (8,820 line records over 604 real pages) and only decides how to fit each onto the panel. The fill-and-wrap path it replaced is kept for packages carrying no layout. This row previously said the page-boundary data was "still unsourced" -- it was not in the repo, but quran.com's `/verses/by_page` serves it per word; see [quran-content.md](quran-content.md). |
| Desktop/device protocol | Implemented, Milestone 4 — [protocol.md](protocol.md). Not run on hardware; no transcript fixture yet (see that doc). |
| BLE provisioning GATT | Implemented, Milestone 4 — [provisioning.md](provisioning.md). Not run on hardware; no pairing/bonding configured. |
