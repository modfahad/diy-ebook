// Reads the finished package back and checks it by real surah number -- the
// property the one-package decision rests on. Run after build-full-quran.mjs.
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readPackage } from '@quran-device/qpk-format';

const here = dirname(fileURLToPath(import.meta.url));
const file = process.argv[2] ?? join(here, '..', 'examples', 'quran-full.qpk');
const pkg = readPackage(new Uint8Array(readFileSync(file)));

for (const id of [1, 2, 18, 112, 113, 114]) {
  const s = pkg.getSurah(id);
  console.log(
    `surah ${String(id).padStart(3)}  ${pkg.text(s.nameOffset, s.nameLength).padEnd(20)} ayahs=${String(s.ayahCount).padStart(3)}`,
  );
}
console.log('');
for (const [si, ai] of [[1, 1], [1, 7], [112, 1], [114, 6]]) {
  const s = pkg.getSurah(si);
  const a = pkg.getAyahByIndex(s.firstAyahIndex + ai - 1);
  console.log(`  ${si}:${a.ayahNumber} (surahId=${a.surahId})  ${pkg.text(a.textOffset, a.textLength)}`);
}
