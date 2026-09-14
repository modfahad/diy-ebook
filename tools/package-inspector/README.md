# Package inspector

**Status: implemented — Phase 1, Milestone 2.**

Dumps a QPK package's header, section table, metadata and a sample of its
index records, and reports whether it validates. When a package the device
rejects looks fine to the desktop, this is the tool that says which rule broke.

```bash
npm install --prefix packages/qpk-format
npm run build --prefix packages/qpk-format

node tools/package-inspector/inspect.mjs packages/qpk-format/fixtures/mini-quran.qpk
```

Exit codes: `0` valid, `1` invalid (the failing rule is printed), `2` usage or
missing build.

`--quiet-index` skips the sample index dump.
