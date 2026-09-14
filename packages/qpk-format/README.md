# @quran-device/qpk-format

TypeScript reader and writer for the QPK1 device content package format.

This is the desktop-side counterpart to `firmware/qpk/`. Both implement
[docs/qpk-format.md](../../docs/qpk-format.md); the cross-language contract is
enforced by a golden fixture that the C++ test suite parses **and** compares
byte for byte against its own builder.

## Use

```bash
npm install --prefix packages/qpk-format
npm run build  --prefix packages/qpk-format
npm test       --prefix packages/qpk-format
```

```ts
import { readPackage, writePackage, PackageType, SectionId } from '@quran-device/qpk-format';

const bytes = writePackage({
  type: PackageType.Quran,
  metadata: [[MetadataKey.Title, 'Mushaf']],
  sections: [/* ... */],
});

const pkg = readPackage(bytes);       // throws QpkError with a specific code
const ayah = pkg.getAyah(2, 255);     // direct index resolution, no scanning
const word = pkg.getWord(2, 255, 7);  // x/y/width/height for AI highlighting
```

## Reader strictness

`readPackage` defaults to the **validator** profile: every section checksum,
the payload checksum, and a full index-consistency sweep. The device
deliberately does less at open (index-section checksums only, sampled index
checks) because it cannot afford to read a whole package to show one page.
Pass `{ verifyChecksums: false, deepIndexCheck: false }` to reproduce the
device's behaviour.

## The golden fixture

`scripts/build-fixture.ts` emits two artifacts from the same bytes:

| Path | Consumer |
|---|---|
| `fixtures/mini-quran.qpk` | the package inspector, future desktop tests |
| `firmware/test/test_qpk/golden_fixture.h` | the firmware test suite |

Regenerate after any format change, and commit both:

```bash
npm run fixture --prefix packages/qpk-format
```

A diff in either file means the two writers drifted; the C++ byte-identity
test will then fail and say so.

## Placeholder content only

Every string in `src/mini-quran.ts` is obviously-synthetic placeholder text.
No Quranic text is fabricated anywhere in this repository — real mushaf text
needs a verified source and belongs to the converter milestone.
