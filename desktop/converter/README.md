# @quran-device/converter

**Status: implemented — Phase 1, Milestone 3.**

The document conversion pipeline: PDF / EPUB / TXT and structured Quran sources
in, validated QPK1 packages out. A library with **no Tauri or React import** —
the desktop UI (Milestone 5) drives it, and so do the CLIs in `tools/`.

Full design notes: [docs/conversion.md](../../docs/conversion.md).

## Use

```bash
npm install --prefix desktop/converter
npm test    --prefix desktop/converter
```

```ts
import { convertAndValidate, previewPage } from '@quran-device/converter';

const result = await convertAndValidate(bytes, { filename: 'book.pdf', title: 'A Book' });
if (!result.validation.ok) throw new Error(result.validation.errors.join('; '));

console.log(result.report);          // pages, chapters, words, estimated fraction
console.log(previewPage(result.bytes, 1));
```

## The one rule worth knowing up front

**Quran packages are only built from a structured JSON source**, never from a
PDF. Passing `contentType: 'QURAN'` with a PDF throws. Extracting Quranic text
with layout heuristics risks silently corrupting scripture, and no downstream
validation can recover from that — see
[docs/conversion.md §1](../../docs/conversion.md).

The schema is in `src/quran/schema.ts`; a worked (synthetic) example is
`examples/quran-source.example.json`.

## Layout

| Path | Role |
|---|---|
| `src/model.ts` | the neutral `ParsedDocument` every source produces |
| `src/sources/pdf.ts` | pdfjs extraction + the coordinate transform |
| `src/sources/epub.ts` | ZIP → OPF spine → XHTML blocks |
| `src/sources/txt.ts`, `paginate.ts` | plain text and the synthetic grid |
| `src/pipeline/structure.ts` | chapter / section detection |
| `src/pipeline/book-package.ts` | → QPK1 `BOOK` |
| `src/pipeline/quran-package.ts` | → QPK1 `QURAN` |
| `src/pipeline/validate.ts` | desktop **and** device validation profiles |
| `src/pipeline/preview.ts` | reads back out of the finished package |

## Dependencies

`pdfjs-dist` (word-level boxes are the reason), `yauzl` and `fast-xml-parser`
(EPUB), plus `@quran-device/qpk-format`. Kept here rather than in
`packages/qpk-format`, whose near-zero dependency footprint matters because the
firmware parser has to stay byte-compatible with it.
