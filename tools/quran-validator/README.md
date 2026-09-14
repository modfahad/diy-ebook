# Quran / package validator CLI

**Status: implemented — Phase 1, Milestone 3.**

Validates either a Quran import source (the JSON schema in
`desktop/converter/src/quran/schema.ts`) or a finished QPK package.

```bash
npm install --prefix desktop/converter
npm run build --prefix desktop/converter

node tools/quran-validator/validate.mjs mushaf.json
node tools/quran-validator/validate.mjs quran.qpk
node tools/quran-validator/validate.mjs quran.qpk --json
```

For a **source** it checks the structural rules the package format's direct
access depends on: contiguous surah ids, contiguous ayah numbers within each
surah, contiguous pages from 1, and layout boxes that reference ayahs and words
that exist. It cannot check that the text is correct — that is the publisher's
responsibility, which is why `metadata.source` is recorded in the package.

For a **package** it runs both profiles: the strict desktop one (every
checksum, full index sweep) and the device's, so a package that would install
and then be rejected on the device is caught here instead.

Exit codes: `0` valid, `1` invalid (problems are listed), `2` usage or the
converter is not built.
