# Conversion pipeline

**Status: implemented — Phase 1, Milestone 3.**

Turns documents into installable QPK1 packages. Lives in
`desktop/converter/` as a library with no UI dependency; the Tauri app
(Milestone 5) drives it, and `tools/pdf-converter` and `tools/quran-validator`
are thin CLIs over the same code.

```
input bytes
   |
   v  detectInputKind (magic first, extension second)
   |
   +-- PDF ---> pdfjs text runs -> coordinate transform -> word boxes
   +-- EPUB --> ZIP -> OPF spine -> XHTML blocks -> synthetic grid
   +-- TXT ---> paragraphs -> synthetic grid
   |                 |
   |                 v  ParsedDocument (pages, lines, word boxes)
   |                 v  structure detection (chapters / sections)
   |                 v  buildBookPackage  -> QPK1 BOOK
   |
   +-- Quran JSON -> schema validation -> buildQuranPackage -> QPK1 QURAN
                       |
                       v  validatePackage (desktop profile AND device profile)
                       v  previewPage (read back OUT of the package)
                       v  install
```

---

## 1. The decision that shapes everything: two pipelines, not one

**Generic books are parsed. The Quran is imported.**

Section 12 of the product spec warns against assuming a PDF contains clean
machine-readable Quran text, and rule 17 makes correctness here
non-negotiable. Heuristic text extraction — run splitting, line grouping,
ligature and diacritic handling, right-to-left reordering — is *good enough*
for a novel and categorically not good enough for scripture. A layout
heuristic that drops a diacritic produces a package that validates perfectly
and is wrong.

So:

| | Generic book | Quran |
|---|---|---|
| Input | PDF, EPUB, TXT | a structured JSON source (§3) |
| Text comes from | extraction heuristics | the publisher, verified |
| Word boxes | measured or apportioned | supplied, or absent |
| Failure mode | a wrong line break | rejected before it is written |

`convert()` **refuses** `contentType: 'QURAN'` for a PDF/EPUB/TXT input, with
an error that says why. That is not a missing feature; it is the feature.

No Quran data is bundled with this repository. `metadata.source` exists so the
provenance of the text travels inside the package.

Word coordinates are an *optional enrichment layer* on a Quran import: keyed by
`surah:ayah:word`, validated against the text, and applied only after the text
itself validates. A package can carry word text with no coordinates — that is
enough for Hifz word hiding, and the `HAS_WORD_LAYOUT` flag is left clear so
nothing downstream claims highlighting it cannot do.

## 2. PDF: the two things that are actually hard

### 2.1 Coordinates

PDF user space is y-up from the bottom left. `WORD_INDEX` is y-down from the
top left. `/Rotate` (0/90/180/270, common in scans) turns the whole page. Get
this wrong and you get plausible-looking numbers that put every highlight in
the wrong place, and nothing downstream can tell.

`runToBox()` in `src/sources/pdf.ts` is the single place this happens. It
composes the item transform with pdfjs's viewport transform (which already
carries the flip, the scale and the rotation) and then builds an axis-aligned
box **from four corners** rather than from `(e, f - height)`. The naive form is
right only for unrotated text: on a 90-degree page the baseline runs down the
page and the ascenders point sideways. All four rotations have direct unit
tests with hand-computed expectations.

The layout coordinate space is the page's own viewport at scale 1, and its
dimensions are recorded in the conversion report. The renderer scales; it never
assumes the device's 792x272.

`WordRecord.x/y/width/height` are `u16`. Anything beyond 65535 is clamped, and
the count of clamped coordinates is reported as a warning rather than wrapping
silently.

### 2.2 Words

`getTextContent()` returns **text runs, not words**, and runs split
unpredictably. A run with no internal whitespace is one word and its box is
exact. A run like `"the quick brown fox"` has only one known advance width, so
it is apportioned by character count and every box from it is flagged
`estimated`.

The report gives `estimatedWordFraction`, and the CLI prints it. A document at
80% estimated will highlight approximately, and you should know that before
installing rather than after.

A PDF where no page yields text is reported as probably scanned. **OCR is not
implemented**; it remains the optional step the spec describes.

## 3. The Quran import schema

Defined and validated in `desktop/converter/src/quran/schema.ts`. A worked
example is in `desktop/converter/examples/quran-source.example.json`
(synthetic placeholder content).

```jsonc
{
  "schemaVersion": 1,
  "metadata": {
    "title": "...",
    "script": "uthmani",         // optional, drives font selection later
    "language": "ar",
    "publisher": "...",
    "source": "..."              // provenance; strongly recommended
  },
  "surahs": [
    { "id": 1, "name": "...", "revelationPlace": "meccan", "hasBismillah": true }
  ],
  "ayahs": [
    { "surah": 1, "ayah": 1, "page": 1, "line": 1, "text": "...", "words": ["...", "..."] }
  ],
  "pages":  [ { "page": 1, "lineCount": 15, "juz": 1 } ],   // optional
  "juz":    [ { "id": 1, "surah": 1, "ayah": 1 } ],          // optional
  "hizb":   [ /* same shape */ ],                            // optional
  "rub":    [ /* same shape */ ],                            // optional
  "sajdah": [ { "surah": 32, "ayah": 15, "kind": "obligatory" } ], // optional
  "layout": {                                                // optional
    "pageWidth": 1000,
    "pageHeight": 1400,
    "words": [ { "surah": 1, "ayah": 1, "word": 0, "x": 100, "y": 260, "width": 110, "height": 50, "line": 1 } ]
  }
}
```

### An invariant the writer establishes

`buildQuranPackage` emits `AYAH_INDEX` in **canonical order** — surah
ascending, then ayah ascending — regardless of the order the source lists them
in. That makes a page's ayahs *contiguous* in the ayah index, which is what
lets both the preview and (in Milestone 5) the renderer walk a page as
`firstAyahIndex + i` for `ayahCount` records instead of scanning.

It holds across a surah boundary too: a page carrying the end of surah *N* and
the start of surah *N+1* still has its ayahs adjacent. The round-trip test
asserts exactly that case.

### What validation enforces, and why

Every rule exists because the package format's direct access depends on it:

| Rule | Why |
|---|---|
| surah ids contiguous from 1 | `SURAH_INDEX` is addressed as `[id - 1]` |
| ayah numbers contiguous from 1 within each surah | `first_ayah_index + (ayah - 1)` must land on the right record |
| pages contiguous from 1 | `PAGE_INDEX` is addressed as `[page - 1]` |
| juz/hizb/rub ids contiguous from 1 | same, for their indexes |
| every range and sajdah points at an ayah that exists | otherwise the index dangles |
| layout boxes reference existing ayahs and word positions | a coordinate for a word that is not there is a bug in the source |

Warnings, not errors:

- `metadata.source` empty — the package will carry no provenance.
- `words` do not rejoin to `text` after whitespace normalisation — usually a
  bad word split, occasionally a legitimate difference in how the source
  represents them. Flagged, not blocked.

What validation **cannot** check is whether the text is correct. That is the
publisher's responsibility, which is exactly why the Quran path takes a
verified source instead of guessing.

## 4. Structure detection (generic books)

Two signals, combined rather than chosen between:

- **typographic** — a line meaningfully taller than the document's body text.
  Free from EPUB (`<h1>`..`<h6>` are scaled during pagination) and available in
  a well-made PDF.
- **lexical** — `Chapter 4`, `II.`, an isolated all-caps line. The only signal
  when a PDF sets headings at body size.

Levels are rebased so the shallowest heading found becomes level 1: a document
whose only headings are `<h3>` still has chapters.

A document where neither signal fires is **not** an error. It becomes a
single-chapter package with a warning, because a book with no detectable
headings still has to install and read.

## 5. Validation and preview happen before install

`validatePackage()` runs two profiles:

1. **Desktop** — every section checksum, the payload checksum, and a full
   index-consistency sweep.
2. **Device** — exactly what the firmware does at open (index-section
   checksums, sampled index checks).

Both, because a package that passes the strict check but fails the device's
would install and then be rejected on the device, which is the worst possible
time to discover it.

`previewPage()` reads back **out of the finished package**, not out of the
in-memory `ParsedDocument`. Previewing the source shows what the converter
meant; previewing the package shows what the device will actually get.

The CLI writes nothing unless validation passes.

## 6. Not implemented

- **OCR.** A scanned PDF is detected and reported, not processed.
- **Right-to-left reordering** for generic books. The `rightToLeft` flag is
  recorded in the package; the converter does not reorder runs.
- **Tables, figures, footnotes.** Everything is text.
- **`LAYOUT_DATA` has no on-device consumer yet.** The section itself is
  written now, alongside `WORD_INDEX` (both Quran and Book pipelines derive
  line records from their own word boxes), and the firmware can read it back
  -- but nothing on the device blits from it. That is the renderer's job,
  Milestone 5.
- **Multi-column PDFs.** Line grouping is by vertical overlap, so two columns
  merge into one line. Detecting columns needs a horizontal gap analysis that
  is worth doing against real documents, not synthetic ones.
