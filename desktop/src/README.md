# Desktop UI

**Status: implemented — Milestone 5.**

React + TypeScript. Four tabs: Dashboard, Library, Device, Converter.

| File | What it is |
|---|---|
| `App.tsx` | shell and tab switching |
| `Dashboard.tsx` | toolchain check, library/device summary, honest per-tab status |
| `Library.tsx` | scans a folder of `.qpk` packages, validates and forwards them |
| `Device.tsx` | identity, status, installed content, resumable upload, delete, abort |
| `Converter.tsx` | the conversion pipeline: validate and preview, then write |
| `bridge.ts` | typed wrappers over the two Tauri commands |
| `settings.ts` | the handful of things remembered between runs |
| `format.ts`, `ui.tsx` | shared formatters and presentational pieces |
| `App.css` | the whole stylesheet: plain CSS, light and dark from the OS |

`bridge.ts` **imports** its result types from `desktop/converter` and
`packages/protocol` rather than redeclaring them — type-only imports, mapped in
`tsconfig.json` to those packages' built `.d.ts`, so nothing is bundled and
nothing can drift from the shapes actually crossing the wire.

No component library and no state manager. The app is four tabs of forms and
tables; the state that outlives a tab switch is five strings in
`localStorage`.
