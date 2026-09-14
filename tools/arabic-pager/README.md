# Arabic pager

**Status: working — the first piece of the Milestone 5 shaping stage.**

Shapes and rasterizes Arabic into device-ready 1-bit pages, on the desktop.

```bash
pip install uharfbuzz freetype-py pillow fonttools

python tools/arabic-pager/render_pages.py \
  fonts/KFGQPC-UthmanTahaNaskh-Regular.ttf \
  tools/arabic-pager/data/an-naba-imlaei.json \
  --translation tools/arabic-pager/data/an-naba-en.json \
  --px 36 --tr-px 14 --line-spacing 1.24 --drop-unsupported \
  --png-dir tools/arabic-pager/preview
```

Drop `--translation` for Arabic only; then `--px 44 --lines 3` is the layout
that fills the panel.

## Why the desktop does this and the device does not

Arabic needs contextual shaping (every letter has isolated, initial, medial
and final forms), mark positioning for the harakat, ligatures, and
right-to-left layout. That is HarfBuzz's work, and porting it to an ESP32 is
not a weekend.

So the desktop shapes once and the device blits finished pixels. For a mushaf
that is not a compromise but the correct design: the page layout is fixed, so
there is nothing to re-flow on device. This is what
[qpk-format.md §9a](../../docs/qpk-format.md) has always specified; this tool
is its first working half.

## The lesson that cost an hour

**Do not pass `init`, `medi` or `fina` as HarfBuzz features.** They are
contextual and applied automatically. Naming them forces them on for *every*
glyph regardless of position, which produces wrong letter forms that still
look like Arabic at a glance. The bug is visible only if you can read the
result — the letters come out unjoined.

```
correct   ids=[80, 72, 253, 81, 590, ...]
forced    ids=[80, 247, 253, 81, 241, ...]   <- wrong
```

Shape with `features=None` and let HarfBuzz do its job.

## Font coverage is a hard stop

The tool refuses to render if the font lacks any codepoint in the text, and
names every one. Silently dropping a character would alter scripture without
saying so; rendering `.notdef` would put boxes on the page. Neither is
acceptable by default.

`--drop-unsupported` proceeds, but only after listing exactly what it omits.

### Choosing a text variant

Measured against **KFGQPC Uthman Taha Naskh Regular** for An-Naba:

| Variant | Vowelled | Font gaps |
|---|---|---|
| `uthmani` | yes | **U+0671 ALEF WASLA ×23** — a *letter*, so unusable |
| `uthmani_simple` | **no** | unusable for recitation |
| **`imlaei`** | yes | U+06D6 ×3 — a pause annotation |
| `imlaei_simple` | **no** | unusable for recitation |

`imlaei` is the choice: complete letters, complete harakat, and the only gap
is three instances of one optional waqf sign. A font carrying U+06D6 would
close even that — the gap is the font's, not the text's.

## Text provenance

`data/an-naba-imlaei.json` came from the quran.com API v4
(`/quran/verses/imlaei?chapter_number=78`) and records its source inside the
file. No Quranic text is written from memory anywhere in this repository.

## Output

- `--png-dir` — one PNG per page at the panel's exact 792×272, for inspection
  before anything reaches the device
- `--header` — 1 bit per pixel, row-major, MSB first, 1 = ink. It was blitted
  by the `-DNABA_DEMO=1` build, which was removed on 2026-09-14; nothing in
  the firmware reads it any more

## Page furniture

Three things beyond the text itself, all measured against the panel's real
792×272 rather than guessed.

### Ayah numbers are drawn, not typeset

The default is a small round medallion with **Western digits**: ① ② ③.

That started as a legibility preference and turned out to be a rendering win.
The mushaf end-of-ayah mark `۝` is a single dense glyph, and at 44 px on a
1-bit panel with no greyscale it thresholds into a blob. A drawn circle stays
crisp at any size because it is geometry, not an outline being rasterized —
and it is narrower, which buys back line width.

`--arabic-numbers` restores `۝` with Arabic-Indic digits. That is a real
preference for a mushaf and should not require a code change.

| Flag | Default | Effect |
|---|---|---|
| `--number-scale` | `0.62` | medallion diameter as a fraction of glyph size |
| `--number-font` | auto | face for the digits (Arial → Segoe → DejaVu → Liberation) |
| `--arabic-numbers` | off | revert to `۝` + Arabic-Indic digits |

### The vertical surah rail

`--sidebar-width` (default 52, `0` disables) puts the surah's identity down
the left edge: **name, number, verse count**, reading bottom-to-top.

Built horizontally and rotated 90° with `expand=True` — deliberately *not*
per-character rotation, which destroys letter spacing and kerning. The number
and count come from the data (the number is parsed from the first
`verse_key`, the count is `len(verses)`), so pointing the tool at another
surah updates them automatically; the name does not, since it is not
derivable from the verses JSON — pass `--arabic-name` (and `--name`,
`--meaning`) alongside it.

The name itself is shaped Arabic, not a transliteration: `--arabic-name`
(default `النبأ`) goes through the same `shape()`/HarfBuzz call as the body
text, and the resulting glyph bitmaps are rasterized into a standalone tile
(`arabic_tile()`) rather than painted onto the page directly, since the rail
is composited separately and rotated as a whole. `--name`/`--meaning` still
supply the transliteration and English gloss set after it in Latin.

Wrapping subtracts the rail from the usable width. Without that the Arabic
would be painted underneath it.

### Bilingual layout

`--translation <json>` switches the pager from continuous line flow to
**per-ayah blocks**: the Arabic, then its meaning in a smaller line beneath.

Blocks are packed whole and never split across a page. A page break between an
ayah and its own translation is the one layout error a bilingual mushaf must
not make, so the packer measures each block's full height and starts a new page
rather than orphaning the meaning.

The alignments are deliberately opposed — Arabic flush right, English flush
left. The asymmetry is what makes the two scripts read as separate voices
instead of one muddled column.

| Flag | Default | Effect |
|---|---|---|
| `--translation` | none | JSON of an English translation |
| `--tr-px` | `15` | translation glyph size |
| `--tr-gap` | `8` | space after each ayah block |
| `--line-spacing` | `1.28` | Arabic line pitch multiplier |

`--line-spacing` is the lever that decides ayat per page, and it matters more
than font size: harakat sit above the letters so it must exceed 1.0, but the
original 1.42 wasted a third of the glass. At 1.24 An-Naba fits 3 ayat per
page in 15 pages instead of 2 in 22.

## Fetching a translation

```bash
python tools/arabic-pager/fetch_translation.py 78 20 \
  tools/arabic-pager/data/an-naba-en.json
```

Arguments are chapter, quran.com translation id, and output path. The tool
strips HTML footnote markers (the notes are not in that payload, so a dangling
superscript only confuses) and normalises curly quotes to ASCII, because the
Latin face is used at small sizes on a 1-bit panel.

English translations available on quran.com include Saheeh International (20),
Abdel Haleem (85), Usmani (84), Pickthall (19) and Yusuf Ali (22).

**Translations are copyrighted works.** The fetched file records its name,
author and source URL, and says so. Fine for a personal device; check the
licence before redistributing.

## Limitations

- **Pre-rendered pages, not reflowable text.** Changing the glyph size means
  re-running this tool, not a button on the device.
- **No justification.** Lines are packed greedily and set flush right; a real
  mushaf justifies with kashida stretching.
- **No page-boundary awareness.** Lines break where they fill, not where the
  Madinah mushaf breaks them.
- These pages are not yet a QPK package — they are a C array. Folding them
  into `LAYOUT_DATA`/`ASSETS` is the rest of Milestone 5.
- **Pages ship inside the firmware image.** 15 bilingual pages take flash from
  41% to 55%. That is the cost of having no SD card; real content belongs on
  the card.
- **The ayah translation is Latin-only.** The surah rail's name is shaped
  Arabic (`--arabic-name`, routed through the same HarfBuzz/FreeType pipeline
  as the body text); the per-ayah translation text itself is not Arabic and
  has no reason to be.

---

## The scripts

| Script | What it does |
|---|---|
| `fetch_verses.py` | one surah's Arabic (imlaei), the original An-Naba path |
| `fetch_translation.py` | one surah's translation, with the ASCII cleaning rules |
| `fetch_chapter_meta.py` | one surah's name/ayah count/revelation place |
| **`fetch_all_surahs.py`** | all 114 surahs (Uthmani) + translations + metadata, resumable |
| **`build_full_quran_source.py`** | assembles them into one `QuranSource` + a cleaned translation |
| **`make_render_translations.py`** | re-shapes the cleaned translation into what `render_pages.py --translation` expects |
| `shape_glyph_atlas.py` | HarfBuzz-shapes words into a deduplicated glyph atlas, plus one precomposed end-of-ayah marker glyph per ayah number (docs/qpk-format.md 9b) |
| `render_pages.py` | rasterizes panel pages (792x272), optionally with translation |
| **`render_surah.py`** | `render_pages.py` for any surah by number, metadata filled in |

The bold ones were added for the full-Quran build.

## The whole Quran

```bash
python tools/arabic-pager/fetch_all_surahs.py
python tools/arabic-pager/build_full_quran_source.py
python tools/arabic-pager/make_render_translations.py
python tools/arabic-pager/render_surah.py 1        # any surah, 1..114
```

Then the packages, in `desktop/converter/scripts/`. Full sequence and the
reasoning: [docs/quran-content.md](../../docs/quran-content.md), commands only:
[docs/development.md §5f](../../docs/development.md).

**Two things worth knowing before changing any of this:**

- **Use the Uthmanic font for Uthmani text.** `fetch_all_surahs.py` fetches the
  **uthmani** script, not imlaei, because `fonts/KFGQPC Uthmanic Script HAFS
  Regular.otf` exists to render exactly that orthography. It has full coverage
  of the corpus — the whole Quran shapes with zero warnings and
  `--drop-unsupported` is never needed, which was not true of the older Naskh
  face (it lacks U+06D6).
- **38px, not 44.** `render_pages.py`'s 44 default was tuned for the Naskh
  face. The Uthmanic one is taller: at 44 only two ayah+translation blocks fit
  a page and the bottom third is blank. `render_surah.py` defaults to 38, which
  fits three and matches An-Naba's density.

## Data is not committed

`data/all/` and the `preview-quran-*/` directories are gitignored. Quranic text
and translations are fetched, never stored in this repository.
