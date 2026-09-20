# Board test checklist

Everything built since the last flash, to try on the board in one sitting.
Written 2026-09-14. Go top to bottom: the early items are what the later ones
need (a flashed board, Wi-Fi, a pairing token).

For each item: **what to do**, then **what should happen**. Note anything that
does not, with the serial log around it.

## Results

**2026-09-14 evening**, firmware built 22:28 flashed over `/dev/cu.usbserial-110`:
- 1 Boot and power: boots; `resets.log` and `alive.bin` written (the second boot
  reported the first one's `last_alive`). Opening the serial port resets this
  board (logged as `power-on`), so keep one monitor open rather than reopening.
  Setup takes 7.8 s, 7 s of it between the reset record and the time-zone load
  (library index / orphan sweep / credentials) -- not yet broken down.
- 2 Home screen: confirmed by eye -- grey frame in 3.5 s, minute updates leave
  the photo intact, clock synced (LOC-5:30).
- 3 Launcher and Books shelf: confirmed by eye, **grey covers included** --
  one grey refresh per shelf page, title strips update fast, covers keep their
  greys.
- Wi-Fi: the device was on the phone hotspot (10.35.x) while the Mac was on
  192.168.1.x; the desktop tests need both on the same network.
- 4–6 (firmware built 23:28 the day before): picture book resume, Go to page,
  bookmarks, text size, translation reader all worked. Slow: the 913-page book
  opened in 4.4 s every time; Al-Baqarah built in 2,643 ms.

**2026-09-15 00:01 build** (speed-ups + translation surah list):
- Book open: first open 13 ms parse + 3,983 ms index checksums; the next open
  **14 ms** (`checksums passed on an earlier open`).
- Translation: Aal-Imran (40.8 KB) built in **51 ms**, Al-Fatiha in 8 ms (was
  62 ms); the aligned Quran's first open spends 1.1 s on checksums, once.
- Translation buttons: OK → surah list → OK picked surah 3; hold OK cycled
  size; bookmarks saved "Surah 3, page 1".
- Photos over Wi-Fi from the app: installed and deleted a photo.
- **The board dropped off USB at 00:15:37**, seconds after the photo delete,
  as it did twice on 2026-09-14. Cause still open (power suspected); check the
  reset reason after replugging.

## 0. Before you start

- Close any serial monitor, then flash over USB:
  ```bash
  cd ~/Documents/epaper/firmware && ~/Library/Python/3.9/bin/pio run -e crowpanel_579 -t upload --upload-port /dev/cu.usbserial-10
  ```
- Open the monitor:
  ```bash
  ~/Library/Python/3.9/bin/pio device monitor -p /dev/cu.usbserial-10 -b 115200 --dtr 0 --rts 0
  ```
- Use a short, good USB cable straight into the computer (not a hub): the
  board dropped off USB and Wi-Fi together twice before.

## 1. Boot and power

| Do | Expect |
|---|---|
| Watch the log from reset | `boot=N reset=<reason>` appended to `/DEVICE/resets.log`; home screen (clock + photo) |
| Unplug, wait 5 s, replug; then desktop Device tab → Identify | "Last restart" shows `power-on` |
| If the board drops off USB again | Next boot's reset reason (`brownout` means power) |

## 2. Home screen (table clock)

| Do | Expect |
|---|---|
| Wait for a minute change | Only the clock digits update (fast); the photo's greys stay intact (open item: B1 window update) |
| Wait 10 minutes, or upload a photo | Full 4-grey frame with the next photo |
| Look at the photo and text closely | Greys look right, text sharp, no ghosting |

## 3. Launcher and Books shelf

| Do | Expect |
|---|---|
| Open the library | Eight tiles: Quran, Books, Translations, Tafsir / Wi-Fi, Clock, Device, Bookmarks; selected tile inverted |
| Open Books | Covers in two rows of five, "< Back" first, "page 1 of 2" if more than nine books |
| **New: grey covers** | Entering the shelf (or a new shelf page) is one grey refresh (a few seconds, flashes); covers show real greys |
| Turn the wheel within a page | The selected book's title turns white-on-black; only the title strips update, fast, and the covers keep their greys |
| Turn past the last tile of a page | Another grey refresh with the next page of covers |
| Leave the shelf | The next screen is a clean full refresh (no grey left behind) |
| If grey covers misbehave | Set `app::kShelfGreyCovers = false` (app_config.h) to go back to dithered black-and-white covers |

## 4. Page-picture books (PDF with "keep layout")

| Do | Expect |
|---|---|
| Open a picture book | Log `[library] opened <path> in Nms` -- should now be well under the old ~9 s |
| Turn pages | Right way up (else flip `kPortraitClockwise`); page number "N / total" bottom-right |
| OK | "Go to page" box with the chapter name; wheel = ±10 pages; MENU = next chapter; OK jumps; EXIT closes |
| Restart the board, reopen the book | Opens on the page you left |

## 5. Bookmarks

| Do | Expect |
|---|---|
| Hold OK while reading (picture book, text book, Quran) | Picture page shows a ribbon top-right; Quran status line says "Bookmarked ..." |
| Home → Bookmarks tile | "Continue" first, then bookmarks newest first; OK opens at that place |
| Hold OK on a bookmark | It is deleted |
| Hold OK on the launcher (not reading) | Still toggles transfer mode |

## 6. Text books and translations

| Do | Expect |
|---|---|
| Open a text book, press OK | Text size cycles x2 / x3 / x4, re-paginated near the same place; footer says `OK=size EXIT=back` |
| Open a TRANSLATION package | One surah at a time as numbered verses; wheel runs into the next surah; no "No reader" message |
| **Speed-up:** open the 913-page book twice | First open logs `[qpk] ... index checksums passed in Nms` (about the old 4.4 s); every open after that logs `(checksums passed on an earlier open)` and should take well under a second |
| **Speed-up:** open Al-Baqarah in a translation | `[translation] surah 2: ... built in Nms` well under the old 2,643 ms |
| In a translation: OK | Surah list ("Jump to Surah", 114 rows with ayah counts), the surah being read selected; wheel moves, OK opens that surah, EXIT returns to the page you were on, MENU leaves to the library |
| In a translation: hold OK | Text size x2 / x3 / x4 |
| In a translation: MENU | Bookmark; footer says "Bookmarked ..." until the next page turn |
| Footer hint in a translation | `OK=surah hold=size MENU=mark` |

## 7. Wi-Fi, desktop app

| Do | Expect |
|---|---|
| Desktop Device tab: Identify, Status, Library | Values, including "Last restart" |
| Upload a package | Installs; log `[transfer] installed ...` |
| Photos tab: upload a photo, send time zone | Photo appears within seconds; clock uses local time |

## 8. Firmware over Wi-Fi

| Do | Expect |
|---|---|
| Build, then desktop Device tab → Update firmware → pick `firmware/.pio/build/crowpanel_579/firmware.bin` | Progress bar, "installed", board restarts a second later |
| After the restart | Boots the new image; reset reason `software`; Identify still works |
| Pick something that is not firmware | Refused before sending |

## 9. Base64 chunk bodies (what the Android app will use)

No phone needed: this sends a package the way the phone does.

```bash
cd ~/Documents/epaper/desktop/device-client && node -e "import('./dist/src/index.js').then(async ({DeviceClient})=>{const fs=await import('node:fs');const c=new DeviceClient({host:process.argv[1],token:process.argv[2],chunkEncoding:'base64'});console.log(await c.uploadPackage(new Uint8Array(fs.readFileSync(process.argv[3])),{onProgress:p=>process.stdout.write('\r'+p.sentBytes)}))})" <device-ip> <token> ../converter/examples/for-bushra.qpk
```

Expect `installed: true`; the log shows the chunks arriving as base64 without errors.

## 10. Still open from before

- Rotation against the enclosure (`kDisplayRotation`).
- Quran page-turn time after the glyph index (read it from the log).
- Booster resistor: if an image looks faint or half-refreshed, suspect it first.

## 11. Touch (GDEY075T7-T01's GT911) — added 2026-09-19

**Run on the board 2026-09-19 (Mac build):** GT911 found at 0x5D, id "911",
fw 0x1060, res 800x480; taps and drags track, and the point data starts at
0x814F (confirmed from the `bytes=` log). Still open: the orientation flags.
It is a separate build and does not touch the product firmware.

The product firmware now reads touch too (architecture.md 5c), so after this
section passes, flash the normal build and watch the log: `[touch] GT911 at
0x5D ...` at boot, then `[input] TOUCH click at (x,y)` for each tap.

Wiring first. The touch layer has its own 6-pin ribbon; the CrowPanel has no
socket for it, so it needs a 6-pin FPC breakout wired to the 2x10 header.
The ribbon's pin order is from the spec sheet's page 5 drawing; check it there
before powering up, because swapping 3.3V and GND can destroy the GT911. The
GPIOs are our choice and can be changed in `board_crowpanel_579.h`:

| Touch FPC pin | Signal | CrowPanel header |
|---|---|---|
| 1 | GND | GND |
| 2 | VCC | 3V3 (not 5V) |
| 3 | RESET | GPIO 18 |
| 4 | INT | GPIO 17 |
| 5 | SDA | GPIO 15 |
| 6 | SCL | GPIO 16 |

```bash
pio run -d firmware -e touch_test -t upload && pio device monitor -d firmware -b 115200
```

| Do | Expect |
|---|---|
| Boot | The glass shows `TOUCH TEST GT911` and `addr 0x5D id "911" ... res 800x480`. If it says `GT911 NOT FOUND`, the I2C scan line under it lists what did answer (nothing = wiring/power) |
| Tap box 1 (top left) | Box 1 fills. If another box fills, or none does, press EXIT for the next orientation (0–7) and tap again. Keep going until all five boxes fill where you tap, then copy that orientation's three flags into `kTouchSwapXY / kTouchInvertX / kTouchInvertY` |
| First tap, in the log | A `down ... bytes=` line. The first byte is a small track id (0–4) and the next two the x position — confirmed on this board 2026-09-19, so `kRegPoints = 0x814F` is right. Bytes shifted by one would mean 0x8150 |
| Drag a finger | A line follows about half a second behind; that lag is the panel's refresh |
| Two to five fingers | One line per finger |
| OK or MENU | Clears the glass |

## 12. The options menu — added 2026-09-20

**Not compiled and not run yet.** Works without touch: the wheel and OK drive
it. Do this part even if the touch panel is not wired.

| Do | Expect |
|---|---|
| Hold OK on the library's shelf list | A list: Open this shelf, Transfer mode, Hardware test, Sleep now, Close this menu. The title says "Library options" |
| Open a shelf, hold OK again | The first row now says "Open this book", and "Back to all shelves" has appeared |
| Turn the wheel | The highlight moves, and wraps past either end |
| Press EXIT | The menu closes and the library comes back unchanged |
| Hold OK in a book, choose "Save this place" | "Bookmark saved" as before — this is what holding OK used to do silently |
| Hold OK with a bookmark selected in Saved places | "Delete this place" is there; with nothing selected it is not |
| Hardware test → hold OK → Factory reset | A question, with "No, keep everything" highlighted. EXIT closes it and erases nothing |
| Hold EXIT anywhere | **Nothing happens** (it used to wipe the device). The log says so |
| With touch working: tap a row | That row runs. Tapping the margins does nothing |
| Hold OK on the clock screen | The menu draws over the photo; closing it brings the photo back |

## 13. Screen and touch setup — added 2026-09-20

**Not compiled and not run yet.** This is now the fastest way to find the
touch orientation: it replaces editing `board_crowpanel_579.h` and reflashing.

| Do | Expect |
|---|---|
| Hardware test screen → hold OK → "Screen and touch setup" | A screen with two boxes near the top, "1" on the left and "2" on the right |
| **First**, press MENU until the writing is the right way up | The whole picture turns 180° each press. Do this before the touch part: turning it afterwards clears the boxes, because a flip moves every position under a touch layer that did not move |
| Tap box 1 | It fills in solid, and a cross marks where the tap was read |
| Tap box 2 | It fills in too, and the screen says "Both boxes hit — this is the one" |
| If the wrong box fills, or neither does | Press OK for the next of the eight ways round, then tap both boxes again |
| Press EXIT | Saves and returns to the hardware test screen. The log shows `[setup] saved /DEVICE/screen.txt: rotation=0 touch=5` |
| Restart the device | The saved orientation is still in force; the log shows `[setup] rotation=0 touch=5` at boot |
| Pull the card and read `/DEVICE/screen.txt` | One line: `rotation=0 touch=5` |
| With a rotated picture, turn a few book pages | Partial refreshes still look right — rotation touches the window-refresh path |
| Delete `/DEVICE/screen.txt` and restart | Back to the build-time defaults, log says "(defaults, nothing saved yet)" |

## 14. Tapping to open and to turn pages — added 2026-09-20

**Not compiled and not run yet.** Do section 13 first: these all depend on the
touch orientation being right.

| Do | Expect |
|---|---|
| Tap a tile on the library's front screen (Books, Quran, Device…) | That tile opens. This screen is tiles, not rows — check each one opens what its label says |
| Tap a row in the Quran or Translations list | It opens, same as highlighting it and pressing OK |
| Tap a book's cover on the Books shelf | That book opens. Tapping its title underneath does the same |
| Tap the header or the footer of the library | Nothing happens |
| Scroll a long list down, then tap the top row | The row you tapped opens — not the one that was there before scrolling |
| In a book, tap the right-hand edge | Next page |
| In a book, tap the left-hand edge | Previous page |
| Tap the middle of the page | **Nothing** — that band is for the hand holding the device |
| Same three taps in a picture book and in the Quran | Same behaviour; the Quran goes forward and back by screen |
| Tap a row in Saved places | It opens that place, same as OK |
| Scroll Saved places past the twelfth row, then tap the top row | The row you tapped opens, not the one that used to be there |
| Tap a surah in the surah list | That surah opens — check the number matches, an off-by-one would open its neighbour |
| Tap the top strip (above the header rule) on any screen | The options menu opens. This is how a finger leaves a book: the menu has Close this book, Library and the rest |
| Tap the top strip on a library list | The menu opens — it must not select the first row, which starts below it at y=84 |
| Rest a thumb on the glass for a second while reading | Nothing opens. The options menu is hold OK or the top strip, not hold-on-glass |

## 15. Touch and sleep - added 2026-09-20

**Not compiled and not run yet.** Section 13 first.

| Do | Expect |
|---|---|
| On the setup screen, read the "Touch INT line" row with nothing touching the glass | It should say **HIGH**. If it says LOW untouched, stop - do not turn on tap-to-wake, and say so |
| Tap and hold a finger on the glass, watching that row | It should flip to **LOW** while held. HIGH-when-touched is the opposite polarity, and needs EXT1 changed rather than the flag flipped |
| Let the device sleep (EXIT, or the idle timer) | The log shows `[touch] asleep` just before it goes |
| Tap the glass while it is asleep | **Nothing happens** - correct today. A button wakes it |
| Wake it with a button, then tap | Touch works again at once: the reset on wake is what brings the chip back |
| Measure sleep current at the BAT connector | About what it was before touch existed. Milliamps means the chip did not sleep |
| **Only if INT behaved above:** set `app::kWakeOnTouch = true`, rebuild, sleep it | A tap wakes the device, and the log says `[power] woken by a tap on the glass` |
| With tap-to-wake on, leave it asleep a minute untouched | It stays asleep. Waking immediately and repeatedly means INT is floating or inverted - turn the flag back off |
| With tap-to-wake on, measure sleep current again | Higher, by the chip's scanning draw. That is the trade; decide it with a real battery in hand |
