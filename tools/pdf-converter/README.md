# Document converter CLI

**Status: implemented — Phase 1, Milestone 3.**

Headless front end to `desktop/converter`. Converts PDF / EPUB / TXT / Quran
JSON into a QPK1 package, validates it, and optionally previews a page.
**Nothing is written unless validation passes.**

```bash
npm install --prefix desktop/converter
npm run build --prefix desktop/converter

node tools/pdf-converter/convert.mjs book.pdf --title "A Book" --preview 1
node tools/pdf-converter/convert.mjs mushaf.json -o quran.qpk
```

| Option | Effect |
|---|---|
| `-o`, `--out <file>` | output path (default: input with a `.qpk` extension) |
| `--title`, `--author`, `--language` | metadata overrides |
| `--content-version <n>` | revision number; bump it to update an installed copy in place |
| `--no-words` | omit `WORD_INDEX` — smaller package, no word highlighting |
| `--single-chapter` | skip structure detection |
| `--preview <n>` | print page *n* read back out of the finished package |
| `--dry-run` | convert and validate, write nothing |
| `--json` | machine-readable output |

Exit codes: `0` written and valid, `1` conversion or validation failed, `2`
usage or the converter is not built.

The **content id** is derived from the work's identity — type, title, author,
language — not from its bytes. Re-converting a corrected copy of the same book
therefore keeps the same id, so bumping `--content-version` makes the device
replace the installed copy rather than shelving a second one next to it.

Despite the directory name it is not PDF-specific; the input format is sniffed
from the file's magic bytes, falling back to the extension.
