# Command-line tools

Thin CLIs over the TypeScript libraries in `packages/` and `desktop/`. Each one
is a few dozen lines: the logic lives in the libraries so the Tauri UI
(Milestone 5) can call exactly the same code.

| Tool | Status | What it does |
|---|---|---|
| [`package-inspector/`](package-inspector/README.md) | M2 | dumps and validates a QPK package |
| [`pdf-converter/`](pdf-converter/README.md) | M3 | PDF/EPUB/TXT/Quran JSON → validated QPK1 |
| [`quran-validator/`](quran-validator/README.md) | M3 | validates a Quran import source or a finished package |
| [`device-cli/`](device-cli/README.md) | M4 | talks to a device: info, status, library, resumable upload |
| [`arabic-pager/`](arabic-pager/README.md) | M5 | fetches the Quran and a translation, shapes Arabic through HarfBuzz+FreeType, renders panel pages, and builds the glyph atlas |

Each needs its library built first; the individual READMEs give the exact
commands.
