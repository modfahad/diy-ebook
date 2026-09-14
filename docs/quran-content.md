# Putting the whole Quran on the device

How the 114 surahs get from quran.com onto the card, and the decisions that
shape it. Started 2026-09-01.

This is the content pipeline only. The *format* is
[qpk-format.md](qpk-format.md), the *converter* is
[conversion.md](conversion.md), and what is still outstanding is
[pending.md](pending.md).

---

## 1. The font, and why it unblocks this

`fonts/KFGQPC Uthmanic Script HAFS Regular.otf`, supplied 2026-09-01.

`pending.md` flagged one real risk before it arrived: *"some KFGQPC builds map
glyphs to private-use codepoints rather than standard Unicode Arabic"*, which
would render plausible-looking nonsense. Checked before anything was built on
it:

| Check | Result |
|---|---|
| Total mapped codepoints | 271 |
| Private-use (U+E000–U+F8FF) | **0** |
| U+06D6 (small high sad-lam-alef-maksura) | **present** |
| U+0670 superscript alef, U+06DD end-of-ayah, harakat | present |

Zero private-use mappings means it is addressed by standard Unicode, so the
existing shaping path (`shape_glyph_atlas.py`, HarfBuzz + FreeType) works
unchanged. **U+06D6 is the specific glyph that occurs 3× in An-Naba and was
being dropped** via `--drop-unsupported` with the old Naskh font. That gap is
now closed at the source.

The previous font stays in `fonts/` — it is what the existing An-Naba assets
were built with, and deleting it would make those unreproducible.

---

## 2. Uthmani, not imlaei

`fetch_verses.py` fetches the **imlaei** script. `fetch_all_surahs.py` fetches
**uthmani**, because the whole point of the KFGQPC Uthmanic font is rendering
exactly that orthography — the superscript alefs, the small high seen, the
pause marks. Fetching imlaei text and rendering it with an Uthmani font would
throw away the reason the font was obtained.

The older imlaei files under `data/` are left alone; they are what the An-Naba
demo assets were built from.

---

## 3. One package for all 114 surahs — not 228, and not 114

This is the decision that matters most, and it is forced from two directions
at once.

### The device's index has 96 slots

```c
constexpr uint16_t kMaxLibraryEntries = 96;   // net/library_index.h
```

`LibraryIndex` holds `LibraryEntry entries_[96]` in **static RAM** at 256
bytes each — 24 KB of the ESP32-S3's 320 KB. One package per surah plus a
companion translation would be **228 entries**, needing ~58 KB and, more to
the point, *nearly two and a half times the slots that exist*. The six test
surahs fit because six is small; 114 does not fit at all.

### The format addresses surahs by position

```
SURAH_INDEX[id - 1]     must be surah `id`
```

So one package can only hold **contiguous surah ids starting at 1**. The six
staged test surahs — {1, 103, 108, 112, 113, 114} — could not share a package
without renumbering, which is why each of them declares surah id 1 internally
and carries its real number only in the title. `pending.md` recorded that as
"undecided, deferred".

### Both problems have the same answer

A single QURAN package containing surahs **1..114 in sequence** is contiguous
from 1, so `SURAH_INDEX[id - 1]` is exactly right with no renumbering — and it
costs **one** index entry instead of 114. With a companion TRANSLATION package
that is **2 entries against a 96 limit**, with room to spare.

So the deferred question is not being deferred any more: it resolves to *one
package, real surah numbers, proper sequence*, which is what the format was
designed for all along.

**This supersedes the six single-surah test packages.** They were a scaffold
for proving the pipeline, and they carry the id-1 renumbering workaround. Once
the full package is validated they should come off the card.

---

## 4. Fetching

```bash
python tools/arabic-pager/fetch_all_surahs.py
```

228 API calls (114 Uthmani + 114 translation) plus **one** call for all
chapter metadata rather than 114 separate ones. Output:

```
tools/arabic-pager/data/all/uthmani-<n>.json    verse text, Uthmani script
tools/arabic-pager/data/all/en-<n>.json         Saheeh International (id 20)
tools/arabic-pager/data/all/chapters.json       all 114 names + metadata
```

**Resumable by design.** A file that exists *and parses with a non-empty
payload* is skipped; a file truncated by an interrupted run fails that check
and is refetched. Re-running is cheap and safe, which matters when the
alternative is hammering a public API 228 times to recover from one dropped
connection.

Translations are copyrighted works. Nothing is committed to the repository —
the fetch exists so a specific person can put one on their own device, and the
provenance travels inside each file.

---

## 5. Indexing

The single QURAN package carries, per
[qpk-format.md](qpk-format.md):

- `SURAH_INDEX` — 114 records in sequence, each with the surah's Arabic name,
  ayah count, revelation place, and first-ayah offset. Addressed `[id - 1]`.
- `AYAH_INDEX` — every ayah of every surah, in order, each pointing at its
  text span and carrying its surah id and ayah number.
- `PAGE_INDEX` / `TEXT_DATA` — page records and the text pool behind them.

The companion TRANSLATION package carries `TRANSLATION_INDEX` /
`TRANSLATION_DATA`, joined to the Quran package by
`MetadataKey.AlignedContentId`, exactly as the six test pairs already do.

### Page numbers are real Madinah pagination — as of 2026-09-02

This section used to say the opposite, and the sentence it turned on was
wrong: "the canonical line-break data is not in this repository." It was not
in the repository, but it was one API call away. quran.com's
`/verses/by_page` returns a per-word `line_number` and the endpoint's own
page grouping, which together *are* the Madinah mushaf's line breaks.

Ayah `page`/`line` now carry the page and line the ayah's first word actually
occupies: **604 pages, contiguous, 15 lines each**, replacing "page = surah
id, line = ayah number". `PAGE_INDEX` went from 114 fabricated pages to 604
real ones.

Two things had to be got right for this to be trustworthy rather than
plausible, and both are written up in
`tools/arabic-pager/build_mushaf_layout.py`:

- **The two sides disagree about what a word is.** Their per-page word units
  attach a waqf mark to the preceding word; ours split them apart. Naively
  zipping the sequences gave 82,011 against 77,429 — a 4,582-word drift that
  would have misaligned everything after Al-Baqarah. Words are matched on
  their consonantal skeleton instead, and our word splits are left completely
  untouched, so the atlas and the shaped `WORD_INDEX` are byte-identical
  before and after.
- **Their per-word `page_number` is sometimes wrong.** `by_page/121` groups
  verse 5:77 correctly onto page 121 but stamps its words `page_number: 120`
  — a page whose first three lines are already occupied. Trusting that field
  put 5:77 back at the top of page 120 and made the corpus read backwards.
  The request's own page is authoritative; the fetcher now asserts that
  `(page, line)` never decreases across the whole sweep.

---

## 6. Battery percentage — not possible on this board revision

Recorded here because it was asked for alongside this work.

`util::BatteryPercentFromMillivolts()` already exists and is host-testable: a
piecewise-linear single-cell LiPo discharge curve, plus a self-calibrating
remaining-runtime estimator that derives from observed percentage history
rather than a guessed average current.

It cannot be fed. `board::kBatteryAdcPin = -1`, and that is **not a
placeholder waiting to be filled in** — Elecrow's schematic for this board was
read directly and the battery block is connector → protection diode → 4054A
charger → PMOS load switch into the 3.3 V rail, with **no resistor divider on
the `BAT` net anywhere**. Every other functional GPIO on the sheet carries a
descriptive net name; `BAT` only ever appears as a bare power-rail label.

So a real battery percentage needs a **hardware modification** — solder a
divider from `BAT` to a spare ADC-capable pin (`IO14`/`IO15`/`IO38` look
unused on the header) and re-measure. The software half is already written and
waiting for a number. See [pending.md](pending.md) §1.

---

## 7. What was built, and how it was checked

```bash
python tools/arabic-pager/fetch_all_surahs.py
python tools/arabic-pager/build_full_quran_source.py
node --max-old-space-size=4096 desktop/converter/scripts/build-full-quran.mjs
node --max-old-space-size=4096 desktop/converter/scripts/verify-full-quran.mjs
```

| | |
|---|---|
| QURAN package | **3.89 MB**, content id `fc7c07fe71673a560b9548a921f29aae` |
| TRANSLATION package | **0.88 MB**, `9ae1ea89601231cd13677d7632480ab8`, aligned to the above |
| Surahs | 114 |
| Ayahs | **6236** — the canonical count, matched exactly |
| Words | 82,121 (`WORD_INDEX`) |
| Library index cost | **2 entries of 96** |

Sections in the Quran package: `METADATA`, `SURAH_INDEX` (114 × 24 B),
`PAGE_INDEX` (114 × 16 B), `AYAH_INDEX` (6236 × 24 B), `WORD_INDEX`
(82,121 × 16 B), `TEXT_DATA` (2.6 MB). `FLAGS 0x0002 (rtl)`.

Checked four independent ways, not one:

1. **Ayah count against the canon.** 6236 is externally verifiable and is
   asserted twice — in the Python assembler and again in the Node builder,
   which is a different process re-reading the file the first one wrote.
2. **Per-surah count against chapter metadata.** Each surah's fetched verse
   count is checked against `verses_count` from the independent `/chapters`
   endpoint, so a truncated response cannot become a silently short surah.
3. **`validatePackage()` and `tools/quran-validator`** — both the desktop and
   device profiles. `VALID`.
4. **Readback by real surah number** (`verify-full-quran.mjs`): surahs 1, 2,
   18, 112, 113, 114 resolve through `SURAH_INDEX[id - 1]` with correct Arabic
   names and canonical ayah counts (7, 286, 110, 4, 5, 6), and sampled ayahs
   come back with `surahId` equal to the **real chapter number** — 112, 114 —
   not the `1` the old single-surah packages were forced to declare.

### The six single-surah packages are gone

`sdcard-staging/LIBRARY/{QURAN,TRANSLATIONS}` held six QURAN + TRANSLATION
pairs, each declaring surah id 1. They are superseded and were deleted:
staging now holds the full pair plus the existing book, **4 packages against
the 96-entry index limit**. `build-test-surahs.mjs` is left in place — it is
the reproducible record of how those were made.

---

## 8. Two things this does NOT solve

### 34 translation verses contain Arabic script

The Saheeh International text writes the honorific after the Prophet's name in
Arabic letters (`صلى الله عليه وسلم`) inside otherwise-English prose, in 34
verses — 2:23, 2:62, 2:146, 3:68, 3:152, 3:193, 4:47, 5:69, 7:184, 9:40, 9:74,
10:38 and more. The panel's Latin face is ASCII-only, so those glyphs cannot
be drawn, and the translation package is not rendered with the Arabic font.

**Left as-is, deliberately.** The options are to substitute an English
rendering ("(peace be upon him)"), to drop the phrase, or to build mixed-script
rendering. The first two edit the text of a religious translation, which is not
a decision to make silently in a batch job. Recorded here for whoever decides.

Everything else that could reach an ASCII-only face **was** fixed: U+02BF and
U+02BE, the ayn and hamza transliteration half-rings, are MODIFIER LETTERS
rather than combining marks, so the existing NFKD pass never touched them and
68 of them would have arrived undrawable. They now fold to `'`. After that,
`other non-ASCII codepoints remaining: none`.

### Pagination is the real mushaf — since 2026-09-02

`page` and `line` were the surah id and the ayah number: contiguous from 1,
honest about being a placeholder, and not mushaf pagination. They are now the
Madinah mushaf's own 604 pages and 15 lines per page, and the shaped package
carries a `LAYOUT_DATA` section with **8,820 line records** that the device
lays text out from. See the section above for how the data was sourced and
aligned, and [qpk-format.md §9](qpk-format.md) for the wire invariant those
records now have to satisfy.

---

## 9. The glyph atlas scales — measured, not assumed

The worry going in was that pre-shaping every word would explode: Al-Fatihah's
29 words needed 120 atlas entries, and the full corpus has 82,011. Linear
growth would have made this impossible and forced an on-device shaping design.

**It does not grow linearly.** Atlas entries are keyed on
`(font_glyph_id, xOffset, yOffset, xAdvance)`, so they saturate as the corpus
covers the font's real repertoire:

| Corpus | Words | Atlas entries | Font glyph ids |
|---|---|---|---|
| Al-Fatihah | 29 | 124 | 61 |
| **All 114 surahs** | **82,011** | **1,507** | **343** |

2,800× the words for 12× the atlas. Measured with:

```bash
python tools/arabic-pager/shape_glyph_atlas.py \
  "fonts/KFGQPC Uthmanic Script HAFS Regular.otf" <verses.json> <atlas.json>
```

Byte cost of a full shaped package:

| Part | Size |
|---|---|
| Glyph bitmaps (1,507 glyphs @ 36px) | **42.4 KB** |
| `FONT_METADATA` (1,507 × 16 B) | 24.1 KB |
| Glyph runs (581,336 refs × 2 B, avg 7.1/word) | 1.11 MB |
| `WORD_INDEX` (82,011 × 16 B) | 1.25 MB |
| **Total** | **~2.42 MB** |

The bitmaps — the part that intuitively should dominate — are 42 KB, under 2%.
The cost is in the per-word glyph runs, which is inherent to pre-shaping.

**Font coverage across the whole Quran: zero warnings.** The shaper reports
every unsupported codepoint and refuses by default; run against all 6236 ayahs
it reported none. `--drop-unsupported` was never needed. That is the KFGQPC
font validated on the real corpus, not just on Al-Fatihah.

### Still to build

The shaped package itself. It must be a **separate** package from
`quran-full.qpk`, because `FLAG_SHAPED_TEXT_DATA` redefines `WORD_INDEX` for
the whole package — a package cannot carry both a text `WORD_INDEX` and a
shaped one. That is the same reason `al-fatihah-glyph-atlas.qpk` is a separate
file from `examples/surahs/al-fatihah.qpk`. Cost: 2 more index entries of 96,
and ~2.4 MB.

---

## 10. A phantom-word bug, caught by cross-checking two producers

The text package counted **82,121** words; the shaper counted **82,011**. A
110-word disagreement between two independent producers of the same corpus.

Cause: the quran.com Uthmani text carries a **leading space on the first ayah
of 110 of the 114 surahs**. `text.split(' ')` turns that into an empty first
element — a phantom word — while the shaper skips empties.

Why it mattered: the text package's `WORD_INDEX` and the shaped package's are
positionally aligned. A phantom word in one and not the other shifts every
subsequent word in that surah by one, which would have silently misaligned
word-level highlighting and Hifz word-hiding for essentially the whole Quran —
and it would have validated cleanly, because nothing in the format says a word
cannot be empty.

Fixed by stripping the ayah text and filtering empties in
`build_full_quran_source.py`. Both producers now report 82,011, and the
invariant is asserted in **both** — a hard failure in the Python assembler and
an explicit `82011` check in `build-full-quran.mjs` — so it cannot drift back
unnoticed.

This is the case for having two independent producers of the same data. Nothing
else in the pipeline would have caught it.

---

## 11. The shaped package — built 2026-09-01

```bash
python tools/arabic-pager/shape_glyph_atlas.py \
  "fonts/KFGQPC Uthmanic Script HAFS Regular.otf" \
  tools/arabic-pager/data/all/all-verses.json \
  tools/arabic-pager/data/all/glyph-atlas.json
node --max-old-space-size=6144 desktop/converter/scripts/build-full-quran-shaped.mjs
```

`examples/quran-full-shaped.qpk`, **5.06 MB**, content id
`fc7c07fe71673a560b9548a921f29aaf`, `FLAGS 0x000a` (rtl + shaped).

| Section | Records | Bytes |
|---|---|---|
| `SURAH_INDEX` | 114 | 2,736 |
| `PAGE_INDEX` | 114 | 1,824 |
| `AYAH_INDEX` | 6,236 | 149,664 |
| `WORD_INDEX` (shaped: glyph runs) | 82,011 | 1,312,176 |
| `TEXT_DATA` | — | 2,614,904 |
| `FONT_METADATA` | 1,507 | 18,084 |
| `ASSETS` (bitmaps + glyph-id runs) | — | 1,206,106 |

It carries `TEXT_DATA` and `AYAH_INDEX` as well as the atlas, so it is
self-contained: only `WORD_INDEX`'s *meaning* changes under
`FLAG_SHAPED_TEXT_DATA`, not the rest of the package. Its content id
deliberately differs from the text package's in the last byte — they are two
distinct installable items, and a shared id would make the device treat one as
a replacement for the other.

### The alignment check is the point of this script

The text package and the shaped package index the same 82,011 words
*positionally*. A one-word drift misaligns every word-level feature across the
whole Quran and still validates. So the builder refuses to write unless:

1. the shaped word count equals the text package's `WORD_INDEX` count;
2. the shaper reported **no** font-coverage warnings;
3. the word **text** matches the source at spread-out probe positions
   (0, 1, 6, 100, 5000, 40000, 82010) — because two different splits can agree
   on the total and disagree on the words.

All three pass: `alignment ok: 82011 words, 7 text probes matched`.

### Staged

`sdcard-staging/LIBRARY` now holds 5 packages, ~10 MB, **5 index entries of
96**: the Quran text package, the shaped package, the aligned translation, the
existing book, and `fahad_testing.qpk`.

> `fahad_testing.qpk` sits directly in `LIBRARY/` rather than
> `LIBRARY/BOOKS/`. `LibraryIndex::rebuild()` scans the four content
> subdirectories, so a package at the `LIBRARY/` root is not indexed and will
> not appear on the device.

### The device reads this — confirmed 2026-09-01

`ui::QuranScreen` opens this package and blits its glyph runs. From the real
board, paging through Al-Baqarah:

```
[quran] surah=2 from ayah_index=7  drew 4 ayahs, 231 glyphs
[quran] surah=2 from ayah_index=11 drew 2 ayahs, 232 glyphs
```

`ayah_index=7` is Al-Baqarah 1 exactly (surah 1 occupies indices 0..6), and
reaching surah 2 at all proves paging follows `AYAH_INDEX` across a surah
boundary. Each screen is a 622ms partial refresh.

**The text package is not readable and says so.** `quran-full.qpk` has no
`FONT_METADATA`/`ASSETS`, and its `TEXT_DATA` is UTF-8 Arabic that the device's
5x7 ASCII face cannot draw. Opening it produces
`drew 0 ayahs, 0 glyphs (UNSUPPORTED PACKAGE)` and an on-screen explanation
rather than a blank panel. That is why the shaped build titles itself "The Holy
Quran (readable)" — with both titled the same, the two library rows were
indistinguishable while only one could be drawn.

Still open: a surah picker (it opens at surah 1 and pages forward),
`LAYOUT_DATA`-driven line breaks rather than fill-and-wrap, and Uthmani ayah
markers instead of `(7)` in the Latin face. See [pending.md](pending.md) §2.

---

## 12. Screen layout — An-Naba's, for any surah

```bash
python tools/arabic-pager/make_render_translations.py   # once, after the corpus build
python tools/arabic-pager/render_surah.py 1             # any surah, 1..114
python tools/arabic-pager/render_surah.py 114 --out somewhere
```

`render_surah.py` wraps `render_pages.py` and pulls the surah's name, Arabic
name, meaning and ayah count from the fetched chapter metadata, so nothing is
retyped per surah. Output is the same layout An-Naba established: the ayah
right-aligned with its number in a circle, the English translation beneath it,
a rule between blocks, and the rotated Arabic surah name in the left rail
alongside `<meaning> <number> <N> Ayat`.

Previews committed: `tools/arabic-pager/preview-quran-1/` (Al-Fatihah, 3 pages)
and `preview-quran-114/` (An-Nas, 2 pages), rendered at the real panel size of
792x272.

### The glyph size had to change, and it was measured

`render_pages.py` defaults to **44px**, tuned for the old Naskh face. The
KFGQPC Uthmanic face is taller at the same nominal size, and at 44 only **two**
ayah+translation blocks fit a 792x272 page — the bottom third of every page
sits blank, and Al-Fatihah spills to 4 pages. At **38px** three blocks fit, the
page fills, and Al-Fatihah is 3 pages, matching An-Naba's density.

So `render_surah.py` defaults to 38, not 44, and says why. This is the kind of
thing that only shows up by rendering a page and looking at it; the shaping was
correct at both sizes.

### A note on `--translation`'s input shape

`render_pages.py --translation` expects the *old* `fetch_translation.py` output:
`{"verses":[{"verse_key":"1:1","text":"..."}]}`. The bulk fetch stores the raw
API payload instead, which has neither `verse_key` nor the cleaning applied.
`make_render_translations.py` bridges them, emitting one file per surah from the
**cleaned** corpus — so the pages render the same text the TRANSLATION package
carries, not a separately-cleaned copy that could drift from it.
