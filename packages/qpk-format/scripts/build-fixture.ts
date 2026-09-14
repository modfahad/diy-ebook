// Emits the golden fixture in two forms:
//
//   packages/qpk-format/fixtures/mini-quran.qpk   binary, for the inspector
//                                                 and future desktop tests
//   firmware/test/test_qpk/golden_fixture.h       the same bytes as a C++
//                                                 array, so the firmware test
//                                                 suite can parse what the
//                                                 TypeScript writer produced
//
// The C++ suite asserts both that the golden package parses and that it is
// byte-identical to what the C++ builder produces for the same content. That
// pair is the cross-language contract. Regenerate with:
//
//     npm run fixture --prefix packages/qpk-format
//
// and commit the result; a diff in either file means the two writers drifted.

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { buildMiniQuran } from '../src/mini-quran.js';
import { readPackage } from '../src/reader.js';

const here = dirname(fileURLToPath(import.meta.url));
const packageRoot = join(here, '..', '..');          // packages/qpk-format
const repoRoot = join(packageRoot, '..', '..');

const bytes = buildMiniQuran();

// Never ship a fixture that does not validate.
readPackage(bytes);

const binaryPath = join(packageRoot, 'fixtures', 'mini-quran.qpk');
mkdirSync(dirname(binaryPath), { recursive: true });
writeFileSync(binaryPath, bytes);

const lines: string[] = [];
for (let i = 0; i < bytes.length; i += 12) {
  const chunk = [...bytes.subarray(i, i + 12)].map(
    (b) => `0x${b.toString(16).padStart(2, '0').toUpperCase()}`,
  );
  lines.push('    ' + chunk.join(', ') + ',');
}

const header = `// GENERATED FILE -- DO NOT EDIT.
//
// Produced by packages/qpk-format/scripts/build-fixture.ts from
// buildMiniQuran(). Regenerate with:
//
//     npm run fixture --prefix packages/qpk-format
//
// This is a QPK1 package written by the TypeScript writer. The firmware test
// suite parses it and also compares it byte for byte against the C++ builder,
// which is what keeps the two implementations of the format honest.

#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t kGoldenPackageSize = ${bytes.length};

const uint8_t kGoldenPackage[kGoldenPackageSize] = {
${lines.join('\n')}
};
`;

const headerPath = join(repoRoot, 'firmware', 'test', 'test_qpk', 'golden_fixture.h');
writeFileSync(headerPath, header);

console.log(`wrote ${binaryPath} (${bytes.length} bytes)`);
console.log(`wrote ${headerPath}`);
