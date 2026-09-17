import assert from 'node:assert/strict';
import test from 'node:test';

import { MetadataKey, PackageType, SectionId, readPackage } from '@quran-device/qpk-format';

import { convert, convertAndValidate, detectInputKind, readDocumentDetails } from '../src/convert.js';
import { buildBookPackage } from '../src/pipeline/book-package.js';
import { contentIdToHex, deriveContentId } from '../src/pipeline/content-id.js';
import { buildQuranPackage } from '../src/pipeline/quran-package.js';
import { buildTranslationPackage } from '../src/pipeline/translation-package.js';
import { previewPage } from '../src/pipeline/preview.js';
import { validatePackage } from '../src/pipeline/validate.js';
import { detectStructure } from '../src/pipeline/structure.js';
import { runToBox, groupIntoLines, type Matrix } from '../src/sources/pdf.js';
import { parsePdf } from '../src/sources/pdf.js';
import { parseEpub } from '../src/sources/epub.js';
import { parseTxt } from '../src/sources/txt.js';
import { splitRunIntoWords } from '../src/model.js';
import { validateQuranSource } from '../src/quran/schema.js';
import { makeEpub } from './fixtures/make-epub.js';
import { makePdf } from './fixtures/make-pdf.js';
import { miniQuranSource } from './fixtures/mini-quran-source.js';

const near = (actual: number, expected: number, tolerance = 0.01) =>
  assert.ok(
    Math.abs(actual - expected) <= tolerance,
    `expected ${expected} +/- ${tolerance}, got ${actual}`,
  );

// ---------------------------------------------------------------------------
// The coordinate transform -- this milestone's highest-risk pure code
// ---------------------------------------------------------------------------

test('runToBox flips PDF y-up into layout y-down', () => {
  // Composed transform for a 12pt run at PDF (72, 690) on a 792pt-tall page:
  // viewport [1,0,0,-1,0,792] x item [12,0,0,12,72,690].
  const tx: Matrix = [12, 0, 0, -12, 72, 102];
  const box = runToBox(tx, 103.38);
  near(box.x, 72);
  near(box.y, 90); // baseline 102 minus the 12pt ascent
  near(box.width, 103.38);
  near(box.height, 12);
  near(box.angle, 0);
});

test('runToBox handles a 90-degree page', () => {
  // Same run on a page with /Rotate 90: the viewport transform is [0,1,1,0,0,0],
  // so the baseline runs DOWN the page and ascenders point left. A naive
  // "top = f - height" would put this box 90 points too high and 12 too wide.
  const tx: Matrix = [0, 12, 12, 0, 700, 72];
  const box = runToBox(tx, 90.06);
  near(box.x, 688); // 700 - 12 ascent
  near(box.y, 72);
  near(box.width, 12);
  near(box.height, 90.06);
  near(box.angle, 90);
});

test('runToBox handles 180 and 270 degree pages', () => {
  const upsideDown = runToBox([-12, 0, 0, 12, 200, 100], 60);
  near(upsideDown.x, 140);
  near(upsideDown.y, 100);
  near(upsideDown.width, 60);
  near(upsideDown.height, 12);

  const quarter = runToBox([0, -12, -12, 0, 200, 300], 60);
  near(quarter.x, 200);
  near(quarter.y, 240);
  near(quarter.width, 12);
  near(quarter.height, 60);
});

test('word boxes from a multi-word run are flagged as estimated', () => {
  const exact = splitRunIntoWords('Chapter', 10, 20, 70, 12);
  assert.equal(exact.length, 1);
  assert.equal(exact[0]!.estimated, false);

  const split = splitRunIntoWords('one two', 10, 20, 70, 12);
  assert.equal(split.length, 2);
  assert.equal(split[0]!.text, 'one');
  assert.equal(split[1]!.text, 'two');
  assert.ok(split.every((w) => w.estimated));
  // Apportioned by character count across "one two" (7 chars, 10pt each).
  near(split[0]!.x, 10);
  near(split[1]!.x, 50);
});

test('groupIntoLines merges words that overlap vertically', () => {
  const lines = groupIntoLines([
    { text: 'b', x: 30, y: 10, width: 10, height: 12, estimated: false },
    { text: 'a', x: 10, y: 11, width: 10, height: 12, estimated: false },
    { text: 'c', x: 10, y: 40, width: 10, height: 12, estimated: false },
  ]);
  assert.equal(lines.length, 2);
  assert.equal(lines[0]!.text, 'a b'); // reordered left to right
  assert.equal(lines[1]!.text, 'c');
});

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

test('a PDF is parsed into pages, lines and words', async () => {
  const pdf = makePdf([
    {
      width: 612,
      height: 792,
      items: [
        { text: 'Chapter One', x: 72, y: 720, size: 18 },
        { text: 'the quick brown fox', x: 72, y: 690, size: 12 },
        { text: 'jumps over the lazy dog', x: 72, y: 674, size: 12 },
      ],
    },
  ]);

  const doc = await parsePdf(pdf);
  assert.equal(doc.pages.length, 1);
  const page = doc.pages[0]!;
  assert.equal(page.width, 612);
  assert.equal(page.height, 792);
  assert.equal(page.lines.length, 3);
  assert.equal(page.lines[0]!.text, 'Chapter One');
  assert.equal(page.lines[1]!.text, 'the quick brown fox');
  // The heading is set larger, which is what structure detection keys on.
  assert.ok(page.lines[0]!.height > page.lines[1]!.height);
});

test('a rotated PDF page keeps its text inside the viewport', async () => {
  const pdf = makePdf([
    { width: 612, height: 792, rotate: 90, items: [{ text: 'rotated text', x: 72, y: 700, size: 12 }] },
  ]);
  const doc = await parsePdf(pdf);
  const page = doc.pages[0]!;
  // A 90-degree page presents as landscape.
  assert.equal(page.width, 792);
  assert.equal(page.height, 612);
  for (const line of page.lines) {
    for (const word of line.words) {
      assert.ok(word.x >= 0 && word.x + word.width <= page.width, `x out of range: ${word.x}`);
      assert.ok(word.y >= 0 && word.y + word.height <= page.height, `y out of range: ${word.y}`);
    }
  }
});

test('a PDF with no extractable text is reported, not silently accepted', async () => {
  const pdf = makePdf([{ width: 612, height: 792, items: [] }]);
  const doc = await parsePdf(pdf);
  assert.equal(doc.pages.length, 1);
  assert.ok(doc.warnings.some((w) => w.includes('scanned PDF')));
});

test('plain text is paginated and its headings are found', () => {
  const doc = parseTxt(
    ['Chapter One', '', 'Body text of the first chapter.', '', 'Chapter Two', '', 'More body text.'].join(
      '\n',
    ),
    { title: 'A Book' },
  );
  assert.ok(doc.pages.length >= 1);
  const headings = detectStructure(doc);
  assert.deepEqual(
    headings.filter((h) => h.level === 1).map((h) => h.title),
    ['Chapter One', 'Chapter Two'],
  );
});

test('an EPUB is read in spine order, not archive order', async () => {
  const epub = makeEpub({
    title: 'Spine Test',
    author: 'A Writer',
    language: 'en',
    shuffleManifest: true, // archive holds the chapters back to front
    chapters: [
      { href: 'ch1.xhtml', title: 'First Chapter', paragraphs: ['alpha text'] },
      { href: 'ch2.xhtml', title: 'Second Chapter', paragraphs: ['beta text'] },
    ],
  });

  const doc = await parseEpub(epub);
  assert.equal(doc.title, 'Spine Test');
  assert.equal(doc.author, 'A Writer');
  assert.equal(doc.language, 'en');

  const allText = doc.pages
    .flatMap((page) => page.lines.map((line) => line.text))
    .join('\n');
  assert.ok(allText.indexOf('First Chapter') < allText.indexOf('Second Chapter'));
  assert.ok(allText.includes('alpha text'));

  const headings = detectStructure(doc);
  assert.deepEqual(headings.map((h) => h.title), ['First Chapter', 'Second Chapter']);
});

// ---------------------------------------------------------------------------
// Book packages
// ---------------------------------------------------------------------------

test('a text document round-trips through a BOOK package', async () => {
  const source = [
    'Chapter One',
    '',
    'The first chapter says something short.',
    '',
    'Chapter Two',
    '',
    'The second chapter says something else.',
  ].join('\n');

  const result = await convertAndValidate(new TextEncoder().encode(source), {
    filename: 'book.txt',
    title: 'Round Trip',
    author: 'Test',
    language: 'en',
  });

  assert.ok(result.validation.ok, result.validation.errors.join('; '));
  const pkg = readPackage(result.bytes);
  assert.equal(pkg.header.packageType, PackageType.Book);
  assert.equal(pkg.metadata(MetadataKey.Title), 'Round Trip');
  assert.equal(pkg.metadata(MetadataKey.Author), 'Test');

  assert.equal(pkg.recordCount(SectionId.ChapterIndex), 2);
  const chapter = pkg.getChapter(0);
  assert.equal(pkg.text(chapter.titleOffset, chapter.titleLength), 'Chapter One');

  const first = pkg.getTextRecord(0);
  assert.ok(pkg.text(first.textOffset, first.textLength).includes('first chapter'));

  // A book carries no Quran structures.
  assert.equal(pkg.section(SectionId.SurahIndex), undefined);
  assert.equal(pkg.section(SectionId.AyahIndex), undefined);
  assert.equal(pkg.getPage(1).ayahCount, 0);
});

test('a document with no headings still produces a valid single-chapter package', () => {
  const doc = parseTxt('just one long paragraph of body text with nothing that looks like a heading.');
  const { bytes, report } = buildBookPackage(doc, { title: 'Headless' });
  assert.equal(report.chapterCount, 1);
  assert.ok(report.warnings.some((w) => w.includes('no chapter headings')));
  const pkg = readPackage(bytes);
  assert.equal(pkg.recordCount(SectionId.ChapterIndex), 1);
  assert.equal(pkg.text(pkg.getChapter(0).titleOffset, pkg.getChapter(0).titleLength), 'Headless');
});

test('a book with word boxes derives LAYOUT_DATA from its own lines', () => {
  const doc = parseTxt('just one long paragraph of body text with nothing that looks like a heading.');
  const { bytes } = buildBookPackage(doc, { title: 'Layout Test' });
  const pkg = readPackage(bytes);

  const linesWithWords = doc.pages.flatMap((p) => p.lines).filter((l) => l.words.length > 0);
  const header = pkg.getLayoutHeader();
  assert.equal(header.lineCountTotal, linesWithWords.length);

  const first = pkg.getLine(0);
  assert.equal(first.page, doc.pages[0]!.pageNumber);
  assert.equal(first.y, linesWithWords[0]!.y);
  assert.equal(first.height, linesWithWords[0]!.height);
  // The line's first word must be the same word WORD_INDEX would give a
  // renderer walking that page directly.
  assert.equal(
    pkg.getWordByIndex(first.firstWordIndex).textOffset,
    pkg.getWordByIndex(pkg.getPage(1).firstWordIndex).textOffset,
  );
});

test('oversized coordinates are clamped and counted, not silently wrapped', () => {
  const doc = parseTxt('one two three');
  // Push a word past the u16 layout field.
  doc.pages[0]!.lines[0]!.words[0]!.x = 70000;
  const { bytes, report } = buildBookPackage(doc, { title: 'Big page' });
  assert.equal(report.clampedCoordinates, 1);
  assert.ok(report.warnings.some((w) => w.includes('clamped')));
  const pkg = readPackage(bytes);
  assert.equal(pkg.getWordByIndex(0).x, 0xffff);
});

test('word layout can be left out entirely', () => {
  const doc = parseTxt('Chapter One\n\nsome body text');
  const { bytes } = buildBookPackage(doc, { includeWordLayout: false });
  const pkg = readPackage(bytes);
  assert.equal(pkg.section(SectionId.WordIndex), undefined);
  assert.equal(pkg.header.flags & 1, 0);
});

// ---------------------------------------------------------------------------
// Quran source validation and packaging
// ---------------------------------------------------------------------------

test('a well-formed Quran source validates', () => {
  const result = validateQuranSource(miniQuranSource());
  assert.deepEqual(result.errors, []);
});

test('non-contiguous surah ids are rejected', () => {
  const source = miniQuranSource();
  source.surahs[1]!.id = 3;
  source.ayahs = source.ayahs.map((a) => (a.surah === 2 ? { ...a, surah: 3 } : a));
  const result = validateQuranSource(source);
  assert.ok(result.errors.some((e) => e.includes('surah ids must be contiguous')));
});

test('a gap in ayah numbering is rejected', () => {
  const source = miniQuranSource();
  source.ayahs = source.ayahs.filter((a) => !(a.surah === 1 && a.ayah === 2));
  const result = validateQuranSource(source);
  assert.ok(result.errors.some((e) => e.includes('ayah numbers must be contiguous')));
});

test('a page with no ayahs is rejected', () => {
  const source = miniQuranSource();
  source.ayahs = source.ayahs.map((a) => (a.page === 2 ? { ...a, page: 3 } : a));
  const result = validateQuranSource(source);
  assert.ok(result.errors.some((e) => e.includes('pages must be contiguous')));
});

test('layout boxes must reference words that exist', () => {
  const source = miniQuranSource();
  source.layout!.words.push({
    surah: 1,
    ayah: 1,
    word: 99,
    x: 0,
    y: 0,
    width: 1,
    height: 1,
  });
  const result = validateQuranSource(source);
  assert.ok(result.errors.some((e) => e.includes('layout references word 99')));
});

test('a words array that does not rejoin to the text is warned about', () => {
  const source = miniQuranSource();
  // Drop the layout: this case is about the text/words relationship, and a
  // shorter words array would legitimately orphan its coordinate boxes.
  delete source.layout;
  source.ayahs[0]!.words = ['completely', 'different'];
  const result = validateQuranSource(source);
  assert.deepEqual(result.errors, []);
  assert.ok(result.warnings.some((w) => w.includes('do not rejoin to text')));
});

test('a Quran source round-trips through a QURAN package', () => {
  const { bytes, report } = buildQuranPackage(miniQuranSource());
  const validation = validatePackage(bytes);
  assert.ok(validation.ok, validation.errors.join('; '));

  const pkg = readPackage(bytes);
  assert.equal(pkg.header.packageType, PackageType.Quran);
  assert.equal(report.chapterCount, 2);
  assert.equal(report.pageCount, 3);

  const surah = pkg.getSurah(2);
  assert.equal(surah.ayahCount, 3);
  assert.equal(pkg.text(surah.nameOffset, surah.nameLength), 'PLACEHOLDER-SURAH-TWO');
  assert.equal(surah.revelationPlace, 1); // medinan

  const ayah = pkg.getAyah(2, 2);
  assert.equal(ayah.page, 3);
  assert.equal(ayah.wordCount, 3);
  assert.equal(pkg.text(ayah.textOffset, ayah.textLength), 'W2-2-0 W2-2-1 W2-2-2');
  // Ayah 2:2 starts juz 2.
  assert.equal(ayah.flags & 1, 1);

  const word = pkg.getWord(2, 2, 1);
  assert.equal(pkg.text(word.textOffset, word.textLength), 'W2-2-1');
  assert.equal(word.x, 220);

  // A page's ayahs are CONTIGUOUS in AYAH_INDEX, because the writer emits
  // canonical order (surah ascending, then ayah). The renderer and the preview
  // both walk a page by `firstAyahIndex + i`, so this invariant is load-bearing
  // -- page 2 here holds the end of surah 1 and the start of surah 2.
  const page2 = pkg.getPage(2);
  assert.equal(page2.ayahCount, 2);
  const onPage2 = [
    pkg.getAyahByIndex(page2.firstAyahIndex),
    pkg.getAyahByIndex(page2.firstAyahIndex + 1),
  ];
  assert.deepEqual(
    onPage2.map((a) => `${a.surahId}:${a.ayahNumber}`),
    ['1:3', '2:1'],
  );
  assert.ok(onPage2.every((a) => a.page === 2));

  assert.equal(pkg.getJuz(2).firstPage, 3);
  assert.equal(pkg.recordCount(SectionId.SajdahIndex), 1);
  assert.equal(pkg.getSajdah(0).kind, 0);
});

test('a Quran source with word boxes derives LAYOUT_DATA line geometry', () => {
  const { bytes } = buildQuranPackage(miniQuranSource());
  const pkg = readPackage(bytes);

  const header = pkg.getLayoutHeader();
  assert.equal(header.pageWidth, 1000);
  assert.equal(header.pageHeight, 1400);
  assert.equal(header.lineCountTotal, 6);

  // Every ayah in the fixture is its own line (see mini-quran-source.ts), so
  // this is one line record per ayah, in (page, lineId) order -- not ayah
  // order, which is why 1:3 (page 2, line 1) sorts before 2:1 (page 2,
  // line 3) even though it is a different surah.
  const lines = Array.from({ length: header.lineCountTotal }, (_, i) => pkg.getLine(i));
  assert.deepEqual(
    lines.map((l) => [l.page, l.lineId]),
    [
      [1, 1],
      [1, 2],
      [2, 1],
      [2, 3],
      [3, 1],
      [3, 2],
    ],
  );
  // y/height come from the union of that line's word boxes (y=200+line*60,
  // height=50 for every word in this fixture -- see mini-quran-source.ts).
  assert.deepEqual(
    lines.map((l) => [l.y, l.height]),
    [
      [260, 50],
      [320, 50],
      [260, 50],
      [380, 50],
      [260, 50],
      [320, 50],
    ],
  );
  // firstWordIndex must land on the same word WORD_INDEX itself resolves to
  // for that ayah -- this is the join a renderer actually depends on.
  assert.deepEqual(
    lines.map((l) => l.firstWordIndex),
    [0, 3, 5, 9, 11, 14],
  );
  assert.equal(pkg.getWordByIndex(lines[3]!.firstWordIndex).textOffset,
    pkg.getWord(2, 1, 0).textOffset);
});

test('a Quran source without coordinates still carries word text, and says so', () => {
  const source = miniQuranSource();
  delete source.layout;
  const { bytes, report } = buildQuranPackage(source);
  const pkg = readPackage(bytes);

  assert.ok(pkg.section(SectionId.WordIndex));
  // No coordinates means the word-layout flag must NOT be set.
  assert.equal(pkg.header.flags & 1, 0);
  assert.equal(pkg.getWord(1, 1, 0).x, 0);
  assert.ok(report.warnings.some((w) => w.includes('no coordinates')));
});

test('building a Quran package from an invalid source throws rather than emitting', () => {
  const source = miniQuranSource();
  source.ayahs[0]!.text = '';
  assert.throws(() => buildQuranPackage(source), /does not validate/u);
});

test('a translation package aligns to its Quran package by content id, and round-trips', () => {
  const quranSource = miniQuranSource();
  const { bytes: quranBytes } = buildQuranPackage(quranSource);
  const quranPkg = readPackage(quranBytes);

  const { bytes, ayahCount } = buildTranslationPackage(
    [
      { surah: 1, ayah: 1, text: 'Translation of 1:1' },
      { surah: 1, ayah: 2, text: 'Translation of 1:2' },
      { surah: 1, ayah: 3, text: 'Translation of 1:3' },
      { surah: 2, ayah: 1, text: 'Translation of 2:1' },
      { surah: 2, ayah: 2, text: 'Translation of 2:2' },
      { surah: 2, ayah: 3, text: 'Translation of 2:3' },
    ],
    { title: 'Test Translation', author: 'Tester', language: 'en' },
    { alignedContentId: quranPkg.header.contentId },
  );

  const validation = validatePackage(bytes);
  assert.ok(validation.ok, validation.errors.join('; '));

  const pkg = readPackage(bytes);
  assert.equal(pkg.header.packageType, PackageType.Translation);
  assert.equal(ayahCount, 6);
  assert.equal(pkg.metadata(MetadataKey.AlignedContentId), contentIdToHex(quranPkg.header.contentId));

  // TRANSLATION_INDEX lines up with the Quran package's AYAH_INDEX by
  // position -- canonical order, surah ascending then ayah ascending.
  const ayah = quranPkg.getAyahByIndex(3); // 2:1
  assert.equal(ayah.surahId, 2);
  assert.equal(ayah.ayahNumber, 1);
  const translation = pkg.getTranslation(3);
  assert.equal(pkg.translationText(translation.textOffset, translation.textLength), 'Translation of 2:1');
});

// ---------------------------------------------------------------------------
// Orchestration
// ---------------------------------------------------------------------------

test('input format is sniffed from the bytes', () => {
  assert.equal(detectInputKind(makePdf([{ width: 100, height: 100, items: [] }])), 'pdf');
  assert.equal(
    detectInputKind(makeEpub({ title: 't', author: 'a', language: 'en', chapters: [] })),
    'epub',
  );
  assert.equal(
    detectInputKind(new TextEncoder().encode('{"schemaVersion": 1, "metadata": {}}')),
    'quran-json',
  );
  assert.equal(detectInputKind(new TextEncoder().encode('just words')), 'txt');
  // Magic beats the extension: a mislabelled file is common.
  assert.equal(
    detectInputKind(makePdf([{ width: 100, height: 100, items: [] }]), 'book.txt'),
    'pdf',
  );
});

test('a QURAN package cannot be forced out of a PDF', async () => {
  const pdf = makePdf([
    { width: 612, height: 792, items: [{ text: 'some text', x: 72, y: 700, size: 12 }] },
  ]);
  await assert.rejects(
    () => convert(pdf, { contentType: 'QURAN' }),
    /structured Quran JSON source/u,
  );
});

test('a Quran JSON source converts through the same entry point', async () => {
  const json = new TextEncoder().encode(JSON.stringify(miniQuranSource()));
  const result = await convert(json, { filename: 'mushaf.json' });
  assert.equal(result.report.contentType, 'QURAN');
  assert.equal(readPackage(result.bytes).header.packageType, PackageType.Quran);
});

// ---------------------------------------------------------------------------
// Preview -- read back out of the package, not out of the parser
// ---------------------------------------------------------------------------

test('a book page previews from the finished package', async () => {
  const source = ['Chapter One', '', 'Alpha beta gamma delta.'].join('\n');
  const { bytes } = await convert(new TextEncoder().encode(source), {
    filename: 'b.txt',
    title: 'Preview Test',
  });

  const preview = previewPage(bytes, 1);
  assert.equal(preview.pageNumber, 1);
  assert.equal(preview.heading, 'Chapter One');
  assert.ok(preview.text.includes('Alpha beta gamma delta.'));
  assert.ok(preview.words.length > 0);
  assert.equal(preview.words[0]!.text, 'Chapter');
});

test('a Quran page previews with its ayah numbering', () => {
  const { bytes } = buildQuranPackage(miniQuranSource());
  const preview = previewPage(bytes, 3);
  assert.equal(preview.pageCount, 3);
  assert.equal(preview.heading, 'PLACEHOLDER-SURAH-TWO');
  assert.ok(preview.text.startsWith('2:2  '));
  assert.equal(preview.words.length, 5); // 2:2 has 3 words, 2:3 has 2
});

test('previewing a page that does not exist is an error, not an empty page', async () => {
  const { bytes } = await convert(new TextEncoder().encode('short'), { filename: 'b.txt' });
  assert.throws(() => previewPage(bytes, 99), /out of range/u);
});

// ---------------------------------------------------------------------------
// Validation gate
// ---------------------------------------------------------------------------

test('validation reports both desktop and device verdicts', async () => {
  const { bytes } = await convert(new TextEncoder().encode('Chapter One\n\nbody'), {
    filename: 'b.txt',
    title: 'Gate',
  });
  const result = validatePackage(bytes);
  assert.ok(result.ok);
  assert.equal(result.summary?.type, 'BOOK');
  assert.ok(result.summary!.sections.some((s) => s.name === 'TextIndex'));
});

test('a corrupted package fails validation with the format’s own error code', async () => {
  const { bytes } = await convert(new TextEncoder().encode('Chapter One\n\nbody'), {
    filename: 'b.txt',
  });
  const damaged = Uint8Array.from(bytes);
  damaged[damaged.length - 1] = (damaged[damaged.length - 1]! ^ 0xff) & 0xff;
  const result = validatePackage(damaged);
  assert.equal(result.ok, false);
  assert.ok(result.errors[0]!.includes('CHECKSUM'));
});

// ---------------------------------------------------------------------------
// Content identity
// ---------------------------------------------------------------------------

test('every package gets a content id derived from the work it is', () => {
  const one = buildBookPackage(parseTxt('alpha'), { title: 'Book One', author: 'A' });
  const two = buildBookPackage(parseTxt('beta'), { title: 'Book Two', author: 'A' });

  const idOne = readPackage(one.bytes).header.contentId;
  const idTwo = readPackage(two.bytes).header.contentId;

  // An all-zero id would make every book collide: the library index and the
  // upload session are both keyed on this.
  assert.notEqual(contentIdToHex(idOne), '0'.repeat(32));
  assert.notEqual(contentIdToHex(idOne), contentIdToHex(idTwo));
});

test('re-converting the same work keeps its identity, so it updates in place', () => {
  const first = buildBookPackage(parseTxt('the original text'), {
    title: 'A Book',
    author: 'Someone',
    language: 'en',
  });
  // Same work, corrected text, next revision.
  const second = buildBookPackage(parseTxt('the corrected text'), {
    title: 'A Book',
    author: 'Someone',
    language: 'en',
    contentVersion: 2,
  });

  assert.equal(
    contentIdToHex(readPackage(first.bytes).header.contentId),
    contentIdToHex(readPackage(second.bytes).header.contentId),
    'an edit must be an update, not a second copy in the library',
  );
  assert.equal(readPackage(second.bytes).header.contentVersion, 2);
});

test('identity fields are separated, so concatenations cannot collide', () => {
  const a = deriveContentId({ type: PackageType.Book, title: 'ab', author: 'c' });
  const b = deriveContentId({ type: PackageType.Book, title: 'a', author: 'bc' });
  assert.notEqual(contentIdToHex(a), contentIdToHex(b));
});

test('an untitled book is titled from its file name, so two of them are two works', async () => {
  const one = await convert(new TextEncoder().encode('alpha'), { filename: 'C:\\books\\first_book.txt' });
  const two = await convert(new TextEncoder().encode('beta'), { filename: 'second-book.txt' });

  assert.equal(readPackage(one.bytes).metadata(MetadataKey.Title), 'first book');
  assert.equal(readPackage(two.bytes).metadata(MetadataKey.Title), 'second-book');
  assert.notEqual(
    contentIdToHex(readPackage(one.bytes).header.contentId),
    contentIdToHex(readPackage(two.bytes).header.contentId),
    'two untitled books must not share the "Untitled" id',
  );
});

test("a title from the caller or the document wins over the file name", async () => {
  const given = await convert(new TextEncoder().encode('alpha'), { filename: 'file.txt', title: 'Given' });
  assert.equal(readPackage(given.bytes).metadata(MetadataKey.Title), 'Given');

  const epub = makeEpub({
    title: 'From The EPUB',
    author: 'A Writer',
    language: 'en',
    chapters: [{ href: 'ch1.xhtml', title: 'One', paragraphs: ['alpha text'] }],
  });
  const declared = await convert(epub, { filename: 'something-else.epub' });
  assert.equal(readPackage(declared.bytes).metadata(MetadataKey.Title), 'From The EPUB');
});

const onePage = [{ width: 300, height: 400, items: [{ text: 'Hello', x: 20, y: 360, size: 12 }] }];

test("a PDF's details come from its metadata, without reading pages", async () => {
  const pdf = makePdf(onePage, { Title: 'Riyad as-Salihin', Author: 'An-Nawawi', Lang: 'ar' });
  const details = await readDocumentDetails(pdf, 'scan_0042.pdf');
  assert.deepEqual(details, { kind: 'pdf', title: 'Riyad as-Salihin', author: 'An-Nawawi', language: 'ar' });
  // Reading details must not consume the bytes the conversion needs next.
  assert.equal(detectInputKind(pdf), 'pdf');
});

test('a PDF with no metadata is titled from its file name, as convert would title it', async () => {
  const details = await readDocumentDetails(makePdf(onePage), 'C:\\books\\my_book.pdf');
  assert.deepEqual(details, { kind: 'pdf', title: 'my book' });
});

test("an EPUB's details come from its OPF", async () => {
  const epub = makeEpub({
    title: 'From The OPF',
    author: 'A Writer',
    language: 'en',
    chapters: [{ href: 'ch1.xhtml', title: 'One', paragraphs: ['alpha text'] }],
  });
  assert.deepEqual(await readDocumentDetails(epub, 'x.epub'), {
    kind: 'epub',
    title: 'From The OPF',
    author: 'A Writer',
    language: 'en',
  });
});

test('a TXT has only the title its file name gives it', async () => {
  const details = await readDocumentDetails(new TextEncoder().encode('Chapter One\n\nbody'), 'notes.txt');
  assert.deepEqual(details, { kind: 'txt', title: 'notes' });
});

test('an explicit content id still wins', () => {
  const explicit = new Uint8Array(16).fill(0x5a);
  const { bytes } = buildBookPackage(parseTxt('x'), { title: 'T', contentId: explicit });
  assert.deepEqual(readPackage(bytes).header.contentId, explicit);
});
