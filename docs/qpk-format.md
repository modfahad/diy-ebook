# QPK1 — device content package format

**Status: implemented — Phase 1, Milestone 2.**

| Implementation | Where | Notes |
|---|---|---|
| Device parser (read-only) | `firmware/qpk/` | no allocation, no scanning, `hal::IFile` |
| Desktop reader + writer | `packages/qpk-format/` | TypeScript, also the validator |
| Inspector CLI | `tools/package-inspector/` | dumps and validates a package |

The two implementations are kept honest by a golden fixture: the TypeScript
writer emits `packages/qpk-format/fixtures/mini-quran.qpk` plus a generated C++
header, and the firmware test suite both parses it and asserts it is **byte
identical** to what the C++ test builder produces for the same content.

`LAYOUT_DATA` (§9) is implemented on both sides (`Reader::getLayoutHeader`/
`getLine` in C++, `getLayoutHeader`/`getLine` in TypeScript) and produced from
real content: `desktop/converter`'s Quran and Book pipelines both derive line
records from their own word boxes (`quran-package.ts`, `book-package.ts`) and
emit them whenever `kFlagHasWordLayout` is set — enforced by rule 16, so the
flag can no longer be set without the section it promises. There is still no
on-device consumer: nothing blits from `LAYOUT_DATA` yet, only reads it back
in tests and the inspector.

`FONT_METADATA` / `ASSETS` (§9a) are **fully exercised end to end as of
2026-09-01**, and this paragraph used to say the opposite. The chain is:
`tools/arabic-pager/shape_glyph_atlas.py` shapes real Arabic through
HarfBuzz+FreeType; `desktop/converter`'s `buildGlyphAtlasSections` packs the
result into `FONT_METADATA` + `ASSETS` + a shaped `WORD_INDEX`; and
`ui::QuranScreen` on the device reads it back through
`Reader::getGlyph`/`readAsset` and blits it. The whole Quran — 114 surahs,
82,011 words, 1,507 deduplicated atlas entries — is packaged this way and
renders on real hardware. See [quran-content.md](quran-content.md).

End-of-ayah markers are real Uthmani glyphs as of 2026-09-02 (§9b), not the
Latin `(n)` stand-in.

**`LAYOUT_DATA` is consumed by the renderer as of 2026-09-02**, and carries
the Madinah mushaf's own line breaks — 8,820 line records over 604 pages.
`ui::QuranScreen` lays out whole mushaf lines from it and only decides how to
fit each onto the panel; the fill-and-wrap path it replaces is kept for
packages built without a layout. This paragraph used to say placement was
"not driven by the format"; what remains undriven is *justification* — lines
are set flush right, where a mushaf stretches them with kashida.

---

## 1. Design constraints

1. **Never scan.** The device must resolve `surah/ayah/word` to a byte offset
   through fixed-size index records, never by searching text.
2. **Never parse PDF on the device.** All layout is precomputed on the desktop.
3. **Validate before trusting.** A truncated, half-written or corrupt package
   must be detected and rejected, not crash the reader.
4. **One container, two shapes.** Quran packages and generic books share the
   header, section table and checksum machinery, and differ only in which
   sections they carry. Generic books are *not* forced into Quran structures.
5. **Random access from SD.** Every structure is addressed by absolute file
   offset so a section can be read with one seek.

## 2. Conventions

- All integers are **little-endian**, fixed width, unsigned unless stated.
- All offsets are **absolute, from the start of the file**, in bytes.
- All sections start on a **4-byte boundary**; padding bytes are `0x00`.
- Checksums are **CRC-32/ISO-HDLC** (poly `0xEDB88320`, init `0xFFFFFFFF`,
  final XOR `0xFFFFFFFF`) — the same CRC-32 as zlib/PNG.
- Strings are UTF-8, stored in a data section, referenced by
  `(offset, length)`. They are **not** NUL-terminated.
- Index records are fixed size so that record *i* is at
  `section.offset + i * record_size`. No record contains a pointer to another
  record's byte offset; they refer to each other by **record index**.

## 3. File layout

```
+-------------------------------+  0
| HEADER                        |  64 bytes, fixed
+-------------------------------+  64
| SECTION TABLE                 |  section_count * 32 bytes
+-------------------------------+
| section payloads, in any      |
| order, each 4-byte aligned    |
+-------------------------------+  package_size
```

## 4. Header (64 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | `magic` | `'Q','P','K','1'` (0x314B5051 read LE) |
| 4 | 2 | `format_version` | `1` |
| 6 | 2 | `header_size` | `64`; lets a v2 header grow |
| 8 | 2 | `package_type` | see 4.1 |
| 10 | 2 | `section_count` | 1..255 |
| 12 | 4 | `flags` | see 4.2 |
| 16 | 8 | `package_size` | total file size in bytes |
| 24 | 16 | `content_id` | unique per package (UUIDv4 bytes) |
| 40 | 4 | `content_version` | monotonic, bumped by the publisher |
| 44 | 4 | `section_table_offset` | normally `64` |
| 48 | 4 | `section_table_length` | `section_count * 32` |
| 52 | 4 | `payload_crc32` | CRC-32 of bytes `[header_size, package_size)` |
| 56 | 4 | `reserved` | `0` |
| 60 | 4 | `header_crc32` | CRC-32 of bytes `[0, 60)` |

### 4.1 `package_type`

| Value | Meaning | Required sections |
|---:|---|---|
| 1 | `QURAN` | METADATA, SURAH_INDEX, PAGE_INDEX, AYAH_INDEX, TEXT_DATA |
| 2 | `BOOK` | METADATA, CHAPTER_INDEX, PAGE_INDEX, TEXT_INDEX, TEXT_DATA |
| 3 | `TRANSLATION` | METADATA, TRANSLATION_INDEX, TRANSLATION_DATA |
| 4 | `TAFSIR` | METADATA, TRANSLATION_INDEX, TRANSLATION_DATA |

### 4.2 `flags`

| Bit | Meaning |
|---:|---|
| 0 | has word-level layout (`WORD_INDEX` + `LAYOUT_DATA` present) |
| 1 | text is right-to-left |
| 2 | package is a delta/patch over `content_version - 1` (reserved) |
| 3 | `WORD_INDEX`'s `(text_offset, text_length)` are pre-shaped glyph runs into `ASSETS`, not raw UTF-8 into `TEXT_DATA` — see §9a. Requires `FONT_METADATA` and `ASSETS` (rule 15). |
| 4..31 | reserved, must be `0` |

## 5. Section table entry (32 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 2 | `section_id` | see 5.1 |
| 2 | 2 | `section_version` | `1` |
| 4 | 8 | `offset` | absolute, 4-byte aligned |
| 12 | 8 | `length` | payload bytes, excluding alignment padding |
| 20 | 4 | `count` | record count, or `0` for blob sections |
| 24 | 4 | `crc32` | CRC-32 of `[offset, offset+length)` |
| 28 | 2 | `record_size` | fixed record size, or `0` if variable |
| 30 | 2 | `flags` | reserved, `0` |

Entries are sorted by ascending `section_id`, so the parser can binary-search
the table. A `section_id` may appear at most once.

### 5.1 Section IDs

| ID | Name | Applies to | Record size |
|---:|---|---|---:|
| 1 | `METADATA` | all | variable |
| 2 | `SURAH_INDEX` | Quran | 24 |
| 3 | `JUZ_INDEX` | Quran | 8 |
| 4 | `HIZB_INDEX` | Quran | 8 |
| 5 | `RUB_INDEX` | Quran | 8 |
| 6 | `PAGE_INDEX` | Quran, Book | 16 |
| 7 | `AYAH_INDEX` | Quran | 24 |
| 8 | `WORD_INDEX` | Quran | 16 |
| 9 | `SAJDAH_INDEX` | Quran | 8 |
| 10 | `TEXT_DATA` | all | 0 (blob) |
| 11 | `LAYOUT_DATA` | Quran, Book | 0 (blob) |
| 12 | `TRANSLATION_INDEX` | Translation, Tafsir | 8 |
| 13 | `TRANSLATION_DATA` | Translation, Tafsir | 0 (blob) |
| 14 | `FONT_METADATA` | all | 12 |
| 15 | `ASSETS` | all | 0 (blob) |
| 16 | `CHAPTER_INDEX` | Book | 24 |
| 17 | `SECTION_INDEX` | Book | 16 |
| 18 | `TEXT_INDEX` | Book | 12 |
| 19 | `COVER` | all, optional | 0 (blob) |
| 20 | `PAGE_IMAGE_INDEX` | Book, optional | 16 |
| 21 | `PAGE_IMAGE_DATA` | Book, with 20 | 0 (blob) |

Unknown section IDs are **skipped, not rejected** — that is the forward
compatibility hinge. Unknown *required* sections cannot exist because the
required set is keyed on `package_type`.

## 6. METADATA (id 1)

A packed sequence of key/value records, so new fields never change a struct:

```
u16 key_id
u16 value_length
u8  value[value_length]      // UTF-8, no NUL
                             // padded to a 4-byte boundary
```

| `key_id` | Meaning |
|---:|---|
| 1 | title |
| 2 | author |
| 3 | language (BCP-47, e.g. `ar`, `en`, `ur`) |
| 4 | publisher |
| 5 | source description |
| 6 | build timestamp (RFC 3339) |
| 7 | converter version |
| 8 | script / mushaf name (e.g. `uthmani`, `indopak`) |
| 9 | `content_id` of the package this translation is aligned to |
| 10 | end-of-ayah marker glyph range, `"<first_glyph_id>:<count>"` — see §9b |

Padding rules, stated because they are the easy thing to get wrong:

- `value_length` counts the value bytes only; it **excludes** the padding.
- The next record starts at the next 4-byte boundary after
  `4 + value_length`.
- A record whose header or value would run past the end of the section makes
  the section invalid; the parser stops and reports `kIndexInconsistent`
  rather than returning a truncated value.
- Trailing zero padding of fewer than 4 bytes at the end of the section is
  permitted and ignored.

## 7. Quran index records

### 7.1 `SURAH_INDEX` — 24 bytes, `count` = 114

| Off | Size | Field |
|---:|---:|---|
| 0 | 2 | `surah_id` (1..114) |
| 2 | 2 | `ayah_count` |
| 4 | 4 | `first_ayah_index` — index into `AYAH_INDEX` |
| 8 | 2 | `first_page` |
| 10 | 2 | `last_page` |
| 12 | 2 | `first_juz` |
| 14 | 1 | `revelation_place` (0 = Meccan, 1 = Medinan) |
| 15 | 1 | `has_bismillah` |
| 16 | 4 | `name_offset` — into `TEXT_DATA` |
| 20 | 2 | `name_length` |
| 22 | 2 | reserved |

### 7.2 `AYAH_INDEX` — 24 bytes

| Off | Size | Field |
|---:|---:|---|
| 0 | 2 | `surah_id` |
| 2 | 2 | `ayah_number` (1-based within the surah) |
| 4 | 2 | `page` |
| 6 | 2 | `line` — first line on that page |
| 8 | 2 | `word_count` |
| 10 | 2 | `flags` (bit 0 = starts a juz, bit 1 = sajdah) |
| 12 | 4 | `text_offset` — into `TEXT_DATA` |
| 16 | 4 | `text_length` |
| 20 | 4 | `first_word_index` — into `WORD_INDEX` |

### 7.3 `WORD_INDEX` — 16 bytes

Word IDs are **implicit**: the word's ID is its index within the ayah, i.e.
`record_index - ayah.first_word_index`. This is the ID the AI server returns in
Phase 3.

| Off | Size | Field |
|---:|---:|---|
| 0 | 4 | `text_offset` — into `TEXT_DATA` |
| 4 | 2 | `text_length` |
| 6 | 2 | `x` — pixels from the left of the page |
| 8 | 2 | `y` |
| 10 | 2 | `width` |
| 12 | 2 | `height` |
| 14 | 2 | `line_id` |

`width`/`height` are 16-bit, not 8-bit: the layout coordinate space is the
source page's, which for a mushaf scan is commonly 1000–2000 px wide, and a
single word can exceed 255 px there.

`x`/`y`/`width`/`height` are in the package's own layout coordinate space, whose
page dimensions are declared in `LAYOUT_DATA`. The renderer scales; it does not
assume the panel's 792x272.

### 7.4 `PAGE_INDEX` — 16 bytes

| Off | Size | Field |
|---:|---:|---|
| 0 | 2 | `page_number` |
| 2 | 2 | `ayah_count` |
| 4 | 4 | `first_ayah_index` |
| 8 | 4 | `first_word_index` |
| 12 | 1 | `line_count` |
| 13 | 1 | `juz` |
| 14 | 2 | `flags` |

### 7.5 `JUZ_INDEX` / `HIZB_INDEX` / `RUB_INDEX` — 8 bytes

| Off | Size | Field |
|---:|---:|---|
| 0 | 2 | `id` (1-based) |
| 2 | 2 | `first_page` |
| 4 | 4 | `first_ayah_index` |

### 7.6 `SAJDAH_INDEX` — 8 bytes

| Off | Size | Field |
|---:|---:|---|
| 0 | 4 | `ayah_index` |
| 4 | 2 | `page` |
| 6 | 1 | `kind` (0 = recommended, 1 = obligatory) |
| 7 | 1 | reserved |

## 8. Generic book records

Deliberately *not* the Quran structures.

### 8.1 `CHAPTER_INDEX` — 24 bytes

| Off | Size | Field |
|---:|---:|---|
| 0 | 4 | `chapter_number` |
| 4 | 4 | `first_page` |
| 8 | 4 | `first_section_index` — into `SECTION_INDEX` |
| 12 | 4 | `title_offset` — into `TEXT_DATA` |
| 16 | 2 | `title_length` |
| 18 | 2 | `depth` (0 = top level) |
| 20 | 4 | `parent_index` (`0xFFFFFFFF` = none) |

### 8.2 `SECTION_INDEX` — 16 bytes

| Off | Size | Field |
|---:|---:|---|
| 0 | 4 | `first_page` |
| 4 | 4 | `title_offset` |
| 8 | 2 | `title_length` |
| 10 | 2 | `depth` |
| 12 | 4 | `first_text_index` |

### 8.3 `TEXT_INDEX` — 12 bytes

One record per page.

| Off | Size | Field |
|---:|---:|---|
| 0 | 4 | `text_offset` — into `TEXT_DATA` |
| 4 | 4 | `text_length` |
| 8 | 2 | `page_number` |
| 10 | 2 | `flags` |

## 9. LAYOUT_DATA

Blob section beginning with a fixed header, followed by per-line records:

```
u16 layout_version        // 1
u16 page_width            // layout coordinate space
u16 page_height
u16 line_count_total
u32 line_records_offset   // relative to the section start
```

Line record (12 bytes): `page u16`, `line_id u16`, `y u16`, `height u16`,
`first_word_index u32`.

Line records are written in ascending `(page, line_id)` order, and therefore
in ascending `first_word_index` order. **As of 2026-09-02 this is a wire
invariant, not just a writer discipline** (rule 17, §11) — it used to be the
latter, with the stated reason "nothing reads `LAYOUT_DATA` at runtime yet to
demand one". Something does now: `ui::QuranScreen` resolves a reading
position to the line it sits on every time the reader opens, and
`Reader::findLineByWordIndex()` **binary-searches** the records to do it,
which §1.1's "never scan" rule requires and which is only correct if the
order holds.

There is still no `page -> first_line_index` field. It is not needed: the
renderer pages by line, not by page, so the entry point is a word index and
the binary search answers it in O(log n).

The invariant is enforced by the **writer** (`validate.ts`), not at device
open time, and that asymmetry is deliberate: proving it costs a full sweep of
every line record — 9,060 of them for the full Quran — which is exactly the
open-time scan §1.1 forbids. A violated invariant cannot crash the device
(`getLine` is bounds-checked regardless); it would silently draw the wrong
line, which is why the check lives where a full pass is free.

`y`/`height` are the vertical union of that line's word boxes (min `y` to
max `y + height`) when derived from per-word boxes (`quran-package.ts`), or
carried straight from the source's own per-line geometry when it has one
(`book-package.ts`, from `TextLine.y`/`.height`) — either way, a renderer
reads them as one committed value, never re-derives them.

## 9a. FONT_METADATA and ASSETS — the Arabic glyph atlas

**Specified, Milestone 5. Nothing yet produces real content for these
sections** — no desktop shaping tool, no glyph-atlas generator, no on-device
renderer. What follows is the wire format those tools will target, plus the
reader/writer support for it on both sides (`Reader::getGlyph`/`readAsset`,
TypeScript `getGlyph`/`assetBytes`), proven with synthetic test packages, not
real ones.

### Why shaping is not the device's job

Arabic script is cursive and contextual: a letter takes a different glyph
form depending on its neighbors, letters combine into ligatures, and Uthmani
Quranic text carries dense diacritics (harakat) that have to be positioned
relative to the base letter. Doing that at render time — on a bit-banged
ESP32-S3, with no font-rendering library, no shaping engine, and nothing of
the kind anywhere in this codebase — contradicts design constraint §1.2:
*"Never parse PDF on the device. All layout is precomputed on the desktop."*
The same principle extends here: shaping happens once, on the desktop, at
conversion time. The device's job is to look up a glyph ID and blit a bitmap,
exactly as mechanical as resolving `surah/ayah/word` to a byte offset already
is.

Concretely: a shaping tool (not yet built) turns each word's Unicode text into
a sequence of glyph IDs, mints one glyph ID per visually distinct shape
(including base+diacritic combinations that need their own position), and
rasterizes each shape once into a shared bitmap atlas. The device never sees
raw Arabic text for a package built this way — only fully-resolved lookups.

### FONT_METADATA (id 14) — glyph atlas index, 12 bytes/record

Fixed-size index, same discipline as `WORD_INDEX`. Unlike every other index in
this format, records are **not** guaranteed sorted or dense: `glyph_id` is
whatever the shaping tool assigned, so a reader searches by value rather than
indexing directly.

| Off | Size | Field |
|---:|---:|---|
| 0 | 2 | `glyph_id` — arbitrary, unique within the package |
| 2 | 4 | `bitmap_offset` — into `ASSETS` |
| 6 | 1 | `width` (px) |
| 7 | 1 | `height` (px) |
| 8 | 1 | `x_advance` (signed) — pen movement after drawing |
| 9 | 1 | `x_offset` (signed) — pen position to bitmap left edge |
| 10 | 1 | `y_offset` (signed) — line baseline to bitmap top edge |
| 11 | 1 | reserved |

**Deliberate simplification**: `x_advance`/`x_offset`/`y_offset` are constants
per `glyph_id`, not per occurrence. This format has no mechanism for
context-dependent mark repositioning (the same diacritic drawn slightly
differently depending on which base letter it follows). The shaping tool is
expected to mint a distinct `glyph_id` per visually distinct base+mark
combination instead of trying to reposition one mark glyph on the fly. This
trades atlas size (more glyph IDs) for a device-side format with zero
positioning logic — the right trade for a lookup-and-blit device.

### ASSETS (id 15) — glyph bitmaps, blob

Every glyph's bitmap, packed back to back, referenced by `FONT_METADATA`'s
`bitmap_offset`. Same blob discipline as `TEXT_DATA`: checksum deferred to
install time (rule 13), not verified eagerly at open.

Each bitmap is `width` x `height`, **1bpp, MSB-first, each row padded to a
whole byte** — `ceil(width / 8) * height` bytes, starting at `bitmap_offset`.
Same convention as the firmware's existing fixed Latin font
(`gfx::font5x7`), so both can share one blit primitive. **Bit `1` is ink**
(the foreground pixel gets drawn); bit `0` is transparent, not "background
white" — this is what lets a diacritic's bounding box overlap a base letter's
without one clobbering the other. This is the opposite of `gfx::Canvas`'s own
pixel convention (there, bit `1` is white) — a blitter composes one onto the
other, it does not reinterpret one as the other.

### How a shaped package's `WORD_INDEX` differs

Flag bit 3, `kFlagShapedTextData` (§4.2), changes what `WORD_INDEX`'s
`(text_offset, text_length)` mean, and *only* `WORD_INDEX`'s:

- **Normally**: an offset and byte length into `TEXT_DATA`, raw UTF-8.
- **Under the flag**: an offset and byte length into **`ASSETS`**, holding
  `text_length / 2` glyph IDs (`u16`, little-endian) in left-to-right visual
  order — already reordered for RTL, so the device walks the sequence
  forward regardless of the script's logical direction. Render it by walking
  the sequence, looking up each ID in `FONT_METADATA`, and blitting at
  `pen + (x_offset, y_offset)`, then advancing the pen by `x_advance`.

**`AYAH_INDEX` (and every other section's text references) are unaffected —
always raw UTF-8 in `TEXT_DATA`, flag or no flag.** This is deliberate, not an
oversight: search, translation alignment, and any future text-to-speech need
the actual Unicode text, which a glyph-ID sequence cannot provide (it is
meaningful only against the specific atlas it was shaped for). Word-level
rendering and ayah-level text are different consumers with different needs,
so they read from different sections rather than one field trying to serve
both.

A package can therefore carry both: raw UTF-8 in `TEXT_DATA` for
non-rendering purposes, and a parallel shaped glyph run in `ASSETS` for
`WORD_INDEX` to point at. Rule 15 (§11) only checks that `FONT_METADATA` and
`ASSETS` are present when the flag is set — it does not and cannot check that
every `WORD_INDEX` entry's glyph IDs actually resolve, since that is exactly
the kind of full-package scan §1.1 rules out at open time. A corrupt or
half-built glyph run surfaces the first time something tries to render that
specific word, as `Error::kOutOfRange` from `getGlyph`.

## 9b. End-of-ayah markers

**Implemented end to end as of 2026-09-02** — shaped by
`tools/arabic-pager/shape_glyph_atlas.py`, written by
`desktop/converter/scripts/build-full-quran-shaped.mjs`, read by
`Reader::ayahMarkerGlyphs()` and drawn by `ui::QuranScreen`. Host-tested on
both sides; not yet seen on the physical panel.

A mushaf ends each verse with an ornate circled numeral. In the KFGQPC
Uthmanic Script font that mark is **one precomposed glyph per ayah number**,
reached by shaping the Arabic-Indic digits (`U+0660`..`U+0669`) **alone**.
This was measured, not assumed: all 286 numbers an ayah can have shape to
exactly one glyph, every one 27×33 px at 36 px, all 286 distinct, no
collisions. Prefixing `U+06DD` — the obvious reading of "end-of-ayah mark",
and what this document previously assumed was required — shapes to *two*
glyphs, the composed numeral plus a bare empty circle, which draws the marker
twice.

Because the marker is one glyph and the numbers are dense, no per-ayah field
is needed. The shaper mints the 286 glyphs **contiguously and last**, and one
`METADATA` key records where they start:

```
key 10  ->  "<first_glyph_id>:<count>"      e.g. "1508:286"
```

The marker for ayah number *n* is atlas glyph id `first_glyph_id + (n - 1)`,
for *n* in `1..count`.

**Why METADATA and not a new section or an `AYAH_INDEX` field.**
`AYAH_INDEX` is 24 bytes with no spare padding, so a marker reference there
would mean a record-size change and a format version bump. Appending marker
"words" to `WORD_INDEX` would break the invariant that the shaped and text
packages index the same words positionally — the check
`build-full-quran-shaped.mjs` runs to guard the phantom-word bug class
(quran-content.md §10). `METADATA` is a key-value blob whose whole purpose is
that new fields never change a struct (§6): an unknown key is skipped by
every existing reader on both sides. So this is additive — no section, no
record-size change, no version bump, and a package built before markers
existed still opens and renders.

**Both sides must degrade, not fail.** `Reader::ayahMarkerGlyphs()` returns
false for an absent key, a malformed value, a zero count, or a range that
would wrap past `u16` — a half-parsed range would have the renderer asking
`getGlyph()` for ids the writer never minted, which is a *wrong glyph on
screen*, not an error. `ui::QuranScreen` keeps the Latin `(7)` stand-in for
every one of those cases and for a claimed id the atlas does not actually
hold: a gap where a verse boundary should be is worse than an
obviously-Latin placeholder.

Nothing validates at open time that the claimed range resolves. That is the
same deliberate choice §9a makes for `WORD_INDEX` glyph runs — checking it
would be exactly the full-package scan §1.1 rules out — so a mismatched
build surfaces as the fallback being drawn, not as a rejected package.

## 9c. COVER (id 19) — cover picture

Optional in every package type (added 2026-09-14). The device's Books grid and
the desktop Library tab show it; a package without one gets a drawn cover
(book icon and title). Readers that predate it skip it like any unknown
section, so adding a cover never breaks an older device.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `QCV1` |
| 4 | 2 | width, u16 = 108 |
| 6 | 2 | height, u16 = 144 |
| 8 | 1 | bits per pixel = 2 |
| 9 | 7 | reserved, zero |
| 16 | 3888 | pixels: 108 × 144 at 2 bpp |

Pixels are row-major, four to a byte, most significant first; 0 = black,
1 = dark grey, 2 = light grey, 3 = white — the panel's four greys, packed the
same way as the home screen's photos. The section is exactly 3,904 bytes;
anything else is rejected by the desktop validator and ignored by the device.

The desktop app builds it (cropping the picture to 3:4 and dithering to four
greys), because that is where images can be decoded: the converter takes the
finished payload as `cover`. Its source is the EPUB's declared cover image, or
any picture the user picks.

## 9d. PAGE_IMAGE_INDEX (id 20) and PAGE_IMAGE_DATA (id 21) — page pictures

Optional in a Book (added 2026-09-14). Extracting a PDF's text and re-flowing
it loses the page's own layout — columns, tables, figures, headings. A package
carrying page pictures keeps it exactly: the desktop app renders every PDF
page the way an e-reader does, and the device's reader shows those pictures
instead of re-flowed text. The text sections are still written, so the
package stays searchable and its chapters still list.

Each page is a `width × height` bitmap at 1 bit per pixel: row-major,
`ceil(width / 8)` bytes a row, most significant bit first, **1 = black**. It is
compressed with PackBits and the pages are stored end to end in
`PAGE_IMAGE_DATA`.

PackBits: a header byte `n`, then `n + 1` literal bytes when `n` is 0–127, or
one byte repeated `257 − n` times when `n` is 129–255. 128 is never written.

`PAGE_IMAGE_INDEX`, 16 bytes a record, one per page in reading order:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | offset into `PAGE_IMAGE_DATA` |
| 4 | 4 | compressed length |
| 8 | 2 | width = 480 |
| 10 | 2 | height = 800 |
| 12 | 4 | reserved, zero |

480×800 is the device's reader held upright (portrait). Both sections must be
present together; every record must lie inside `PAGE_IMAGE_DATA` and decode to
exactly `ceil(width / 8) × height` bytes — the desktop validator checks every
page, the device checks each one as it reads it.

## 10. Access paths the format guarantees

Every one of these is O(1) or O(log n), never a scan:

```
getSurah(id)              SURAH_INDEX[id - 1]
getAyah(surah, ayah)      AYAH_INDEX[ SURAH_INDEX[surah-1].first_ayah_index
                                      + (ayah - 1) ]
getWord(surah, ayah, w)   WORD_INDEX[ getAyah(surah,ayah).first_word_index + w ]
getPage(n)                PAGE_INDEX[n - 1]
getJuz(id)                JUZ_INDEX[id - 1]
page -> ayahs             PAGE_INDEX[n].first_ayah_index, .ayah_count
page -> words             PAGE_INDEX[n].first_word_index
juz  -> page              JUZ_INDEX[id - 1].first_page
```

The Phase 3 AI response `{surah, ayah, word_id, status}` therefore resolves to
on-screen pixels with two array lookups and no search.

## 11. Parser validation rules

A conforming parser MUST reject the package if any of these fail, and MUST
report *which* one failed:

1. File is at least `header_size` bytes.
2. `magic` equals `QPK1`.
3. `format_version` is understood.
4. `header_size >= 64` and `header_size <= package_size`.
5. `header_crc32` matches bytes `[0, 60)`.
6. `package_size` equals the actual file size on disk.
7. `section_table_offset + section_table_length <= package_size`, and
   `section_table_length == section_count * 32`.
8. For every section: `offset` is 4-byte aligned,
   `offset >= section_table_offset + section_table_length`, and
   `length <= package_size - offset`. **Bounds must be checked in subtraction
   form**: `offset + length` overflows on a hostile file, which is exactly the
   case this rule exists to catch.
9. No two sections overlap. Entries are ordered by `section_id`, *not* by
   `offset`, so overlap detection cannot assume offset ordering — with at most
   18 sections an O(n²) pairwise check is the right implementation.
10. Section IDs are unique and strictly ascending.
11. For fixed-size sections: `length == (uint64_t)count * record_size`.
    `count` is 32-bit and `record_size` 16-bit, so the product must be computed
    in 64-bit or it wraps.
12. Every section required by `package_type` is present.
13. Checksums, split by cost:
    - **At open:** the `crc32` of every fixed-size *index* section is verified
      eagerly. Together they are tens of kilobytes at most, and they are the
      structures every later bounds check trusts.
    - **Not at open:** blob sections (`TEXT_DATA`, `TRANSLATION_DATA`,
      `ASSETS`) are *not* checksummed on open — `TEXT_DATA` is the large one,
      and reading it would be exactly the full scan §9 forbids. Their
      integrity is established once, at install time, by `payload_crc32`.
    - `verifySection()` is exposed so a validator or the inspector can check a
      blob on demand.
14. Index consistency, checked on a bounded sample at open and fully by the
    desktop validator:
    - `SURAH_INDEX[i].first_ayah_index + ayah_count <= AYAH_INDEX.count`
    - `AYAH_INDEX[i].first_word_index + word_count <= WORD_INDEX.count`
    - every `text_offset + text_length <= TEXT_DATA.length`
    - `PAGE_INDEX` page numbers are strictly ascending from 1
15. If flag bit 3 (`kFlagShapedTextData`, §4.2) is set, both `FONT_METADATA`
    and `ASSETS` are present. Presence only — this does not (and at open
    time, cannot cheaply) check that every `WORD_INDEX` glyph run actually
    resolves; see §9a.
16. If flag bit 0 (`kFlagHasWordLayout`, §4.2) is set, `LAYOUT_DATA` is
    present. The flag's own documented meaning is "`WORD_INDEX` +
    `LAYOUT_DATA` present"; this is what makes that true rather than
    aspirational. Presence only, same reasoning as rule 15 — the device does
    not walk every line record at open to confirm each `first_word_index`
    resolves.
17. `LAYOUT_DATA` line records ascend by `(page, line_id)` and by
    `first_word_index`, and the last record's `first_word_index` is inside
    `WORD_INDEX`. **Writer-enforced, not parser-enforced** — see §9 for why
    the device does not re-verify it at open time.

Failures must be surfaced as a typed error, never a silent fallback: a package
that fails validation is not installed and never becomes an active library item.

## 12. Atomic installation

Because power loss during install is an expected event on a battery device:

```
upload  -> /DEVICE/uploads/<content_id>.part   (+ .meta alongside it)
verify  -> header, section table, payload_crc32, full structural validation
rename  -> /LIBRARY/<type>/<content_id>.qpk
update  -> /LIBRARY/library_index.bin          (written .tmp then renamed)
delete  -> the .meta
```

In-progress transfers live under `/DEVICE/uploads`, **not** in `/LIBRARY`, so
nothing that walks the library tree can see a partial package.

`.part` files are **not** deleted at boot, and this is a deliberate reversal of
the original plan: the resume point is the `.part`'s size on disk, so keeping
it is exactly what lets a transfer survive a device reset. What is swept at
boot is `.part` files with no matching `.meta` — those can never be resumed or
finished, so they are pure leaked space. See `firmware/wifi/README.md`.

### The invariant, stated honestly

> **After boot recovery, every entry in `library_index.bin` names a package
> file that exists and parses.**

Not "at every instant". Replacing an installed package means removing the old
file and then renaming the new one in, and FAT offers no way to make those one
operation — power lost between them leaves the index naming a file that is
gone. That window cannot be closed, so it is *repaired* instead:
`LibraryIndex::prune()` runs at boot and drops entries whose package is missing
or unreadable.

Both halves are tested by cutting power at every write of a full
upload-and-install — once for a fresh install, once for replacing an installed
package, which is the only path where the window exists
(`firmware/test/test_net/`).

## 13. Open questions

- Whether `TEXT_DATA` should be compressed per page. Compression trades SD reads
  for CPU and PSRAM; decide with a measurement, not an assumption. (Milestone 2)
- Whether translations live in the Quran package or stay separate. Currently
  separate (`package_type` 3), joined by `content_id` in metadata key 9.
  (Milestone 2)
- Font/glyph atlas production, Milestone 5: §9a specifies the wire format, but
  three real pieces of tooling remain unbuilt — a desktop Arabic shaping
  stage, a glyph-atlas rasterizer, and the on-device blitter that consumes
  them. None are a format question anymore; all three are implementation.
