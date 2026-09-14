// QuranSource -> QPK1 QURAN package.
//
// The ordering rules here are what make the device's direct access work, and
// they are checked by validateQuranSource before anything is written:
//
//   SURAH_INDEX[id - 1]     must be surah `id`
//   AYAH_INDEX               canonical order: surah ascending, then ayah
//   PAGE_INDEX[page - 1]     must be page `page`
//   WORD_INDEX               ayah order, then word order within the ayah
//
// Get any of those wrong and the parser rejects the package rather than
// serving the wrong ayah -- which is the point of validating on both sides.

import {
  FLAG_HAS_WORD_LAYOUT,
  FLAG_RIGHT_TO_LEFT,
  MetadataKey,
  PackageType,
  SectionId,
  TextPool,
  encodeAyah,
  encodeLayoutData,
  encodePage,
  encodeRange,
  encodeSajdah,
  encodeSurah,
  encodeWord,
  writePackage,
  type LineRecord,
} from '@quran-device/qpk-format';

import {
  isValid,
  validateQuranSource,
  type QuranSource,
  type QuranSourceAyah,
  type QuranSourceRange,
} from '../quran/schema.js';
import { clampCoordinate, emptyReport, type ConversionResult } from './report.js';
import { deriveContentId } from './content-id.js';

// Ayah flag bits, mirroring qpk_format.h.
const AYAH_FLAG_STARTS_JUZ = 1 << 0;
const AYAH_FLAG_SAJDAH = 1 << 1;

export interface QuranConversionOptions {
  contentId?: Uint8Array;
  contentVersion?: number;
  converterVersion?: string;
  /**
   * Extra METADATA entries, merged with the ones derived from `source`.
   *
   * Exists for facts a *variant* of this package knows and the source does
   * not -- today only `MetadataKey.AyahMarkerGlyphs`, which the shaped build
   * (scripts/build-full-quran-shaped.mjs) learns from the glyph atlas, long
   * after the source JSON was written. METADATA is the right home for it:
   * it is a key-value blob, so an unknown key is skipped by every existing
   * reader on both sides, which is what keeps this additive -- no section,
   * no record-size change, no format version bump.
   *
   * Keys here override a derived key of the same id.
   */
  extraMetadata?: Iterable<readonly [MetadataKey | number, string]>;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

function key(surah: number, ayah: number): string {
  return `${surah}:${ayah}`;
}

export function buildQuranPackage(
  source: QuranSource,
  options: QuranConversionOptions = {},
): ConversionResult {
  const validation = validateQuranSource(source);
  if (!isValid(validation)) {
    throw new Error(
      `the Quran source does not validate:\n  ${validation.errors.join('\n  ')}`,
    );
  }

  const report = emptyReport('QURAN');
  report.warnings.push(...validation.warnings);

  const text = new TextPool();
  const counter = { clamped: 0 };

  const surahs = [...source.surahs].sort((a, b) => a.id - b.id);
  const ayahs = [...source.ayahs].sort((a, b) => a.surah - b.surah || a.ayah - b.ayah);

  // --- flags that live on individual ayahs ---------------------------------
  const juzStarts = new Map<string, number>();
  for (const range of source.juz ?? []) juzStarts.set(key(range.surah, range.ayah), range.id);
  const sajdahAyahs = new Set((source.sajdah ?? []).map((s) => key(s.surah, s.ayah)));

  // --- word boxes ----------------------------------------------------------
  const boxes = new Map<
    string,
    {
      x: number;
      y: number;
      width: number;
      height: number;
      line: number;
      // Explicitly `| undefined`, not just optional: exactOptionalPropertyTypes
      // is on, and a word without its own page really does carry undefined
      // here rather than omitting the key.
      page: number | undefined;
    }
  >();
  for (const box of source.layout?.words ?? []) {
    boxes.set(`${box.surah}:${box.ayah}:${box.word}`, {
      x: box.x,
      y: box.y,
      width: box.width,
      height: box.height,
      line: box.line ?? 0,
      page: box.page,
    });
  }

  // --- TEXT_DATA, AYAH_INDEX, WORD_INDEX -----------------------------------
  const ayahIndexOf = new Map<string, number>();
  const ayahRecords: Uint8Array[] = [];
  const wordRecords: Uint8Array[] = [];
  const pageFirstAyah = new Map<number, number>();
  const pageFirstWord = new Map<number, number>();
  const pageLineCount = new Map<number, number>();
  const pageJuz = new Map<number, number>();

  // --- LAYOUT_DATA: one line record per (page, line) that has a real word
  // box. Geometry is the vertical union of that line's word boxes, not a
  // value the source hands us directly -- a word-box source (the only kind
  // `boxes` is built from) never states line height on its own.
  const lineAgg = new Map<
    string,
    { page: number; lineId: number; yTop: number; yBottom: number; firstWordIndex: number }
  >();

  const surahNameRefs = surahs.map((surah) => text.add(surah.name));

  let currentJuz = 1;
  ayahs.forEach((ayah, index) => {
    ayahIndexOf.set(key(ayah.surah, ayah.ayah), index);

    const juzId = juzStarts.get(key(ayah.surah, ayah.ayah));
    if (juzId !== undefined) currentJuz = juzId;

    const textRef = text.add(ayah.text);
    const firstWordIndex = wordRecords.length;

    for (let w = 0; w < (ayah.words?.length ?? 0); w++) {
      const wordRef = text.add(ayah.words![w]!);
      const box = boxes.get(`${ayah.surah}:${ayah.ayah}:${w}`);
      const globalWordIndex = wordRecords.length;
      const y = box ? clampCoordinate(box.y, counter) : 0;
      const height = box ? clampCoordinate(box.height, counter) : 0;
      const lineId = box ? Math.min(box.line, 0xffff) : (ayah.line ?? 0);
      wordRecords.push(
        encodeWord({
          textOffset: wordRef.offset,
          textLength: wordRef.length,
          x: box ? clampCoordinate(box.x, counter) : 0,
          y,
          width: box ? clampCoordinate(box.width, counter) : 0,
          height,
          lineId,
        }),
      );

      if (box) {
        // The WORD's page, not the ayah's: an ayah that straddles a page
        // boundary has words on both, and keying on the ayah's page would
        // file the overflow under the wrong one.
        const wordPage = box.page ?? ayah.page;
        // A page's line count is the highest line any WORD on it occupies.
        // Derived here rather than from `ayah.line` below, which only knows
        // where each ayah starts and undercounts every page whose last ayah
        // begins near its top.
        pageLineCount.set(wordPage, Math.max(pageLineCount.get(wordPage) ?? 0, lineId));
        const lineKey = `${wordPage}:${lineId}`;
        const existing = lineAgg.get(lineKey);
        if (existing) {
          existing.yTop = Math.min(existing.yTop, y);
          existing.yBottom = Math.max(existing.yBottom, y + height);
          existing.firstWordIndex = Math.min(existing.firstWordIndex, globalWordIndex);
        } else {
          lineAgg.set(lineKey, {
            page: wordPage,
            lineId,
            yTop: y,
            yBottom: y + height,
            firstWordIndex: globalWordIndex,
          });
        }
      }
    }

    let flags = 0;
    if (juzId !== undefined) flags |= AYAH_FLAG_STARTS_JUZ;
    if (sajdahAyahs.has(key(ayah.surah, ayah.ayah))) flags |= AYAH_FLAG_SAJDAH;

    ayahRecords.push(
      encodeAyah({
        surahId: ayah.surah,
        ayahNumber: ayah.ayah,
        page: ayah.page,
        line: ayah.line ?? 0,
        wordCount: ayah.words?.length ?? 0,
        flags,
        textOffset: textRef.offset,
        textLength: textRef.length,
        firstWordIndex,
      }),
    );

    if (!pageFirstAyah.has(ayah.page)) {
      pageFirstAyah.set(ayah.page, index);
      pageFirstWord.set(ayah.page, firstWordIndex);
      pageJuz.set(ayah.page, currentJuz);
    }
    // Fallback for a source with no word boxes; a source with them has
    // already set a truer count per word, above.
    pageLineCount.set(ayah.page, Math.max(pageLineCount.get(ayah.page) ?? 0, ayah.line ?? 0));
  });

  // --- SURAH_INDEX ---------------------------------------------------------
  const surahRecords = surahs.map((surah, i) => {
    const own = ayahs.filter((a) => a.surah === surah.id);
    const first = own[0];
    const last = own[own.length - 1];
    const firstIndex = ayahIndexOf.get(key(surah.id, 1));
    if (!first || !last || firstIndex === undefined) {
      throw new Error(`surah ${surah.id} has no ayahs`);
    }
    return encodeSurah({
      surahId: surah.id,
      ayahCount: own.length,
      firstAyahIndex: firstIndex,
      firstPage: first.page,
      lastPage: last.page,
      firstJuz: juzOfAyah(source.juz, surah.id, 1) ?? 1,
      revelationPlace: surah.revelationPlace === 'medinan' ? 1 : 0,
      hasBismillah: surah.hasBismillah === false ? 0 : 1,
      nameOffset: surahNameRefs[i]!.offset,
      nameLength: surahNameRefs[i]!.length,
    });
  });

  // --- PAGE_INDEX ----------------------------------------------------------
  const pageCount = Math.max(...ayahs.map((a) => a.page));
  const declaredPages = new Map((source.pages ?? []).map((p) => [p.page, p]));
  const pageRecords: Uint8Array[] = [];
  for (let page = 1; page <= pageCount; page++) {
    const declared = declaredPages.get(page);
    const firstAyah = pageFirstAyah.get(page);
    const firstWord = pageFirstWord.get(page);
    // validateQuranSource rejects page gaps, but this function is exported and
    // callable directly. Writing `undefined` here would become a 0 in the
    // record -- a package that validates and serves the wrong ayah, which is
    // exactly the failure this pipeline exists to prevent.
    if (firstAyah === undefined || firstWord === undefined) {
      throw new Error(`page ${page} has no ayahs; pages must be contiguous from 1`);
    }
    pageRecords.push(
      encodePage({
        pageNumber: page,
        ayahCount: ayahs.filter((a) => a.page === page).length,
        firstAyahIndex: firstAyah,
        firstWordIndex: firstWord,
        lineCount: Math.min(declared?.lineCount ?? pageLineCount.get(page) ?? 0, 255),
        juz: Math.min(declared?.juz ?? pageJuz.get(page) ?? 0, 255),
        flags: 0,
      }),
    );
  }

  // --- JUZ / HIZB / RUB / SAJDAH -------------------------------------------
  const rangeSection = (ranges: QuranSourceRange[] | undefined): Uint8Array | undefined => {
    if (!ranges || ranges.length === 0) return undefined;
    const sorted = [...ranges].sort((a, b) => a.id - b.id);
    return concat(
      sorted.map((range) => {
        const index = ayahIndexOf.get(key(range.surah, range.ayah))!;
        return encodeRange({
          id: range.id,
          firstPage: ayahs[index]!.page,
          firstAyahIndex: index,
        });
      }),
    );
  };

  const juzPayload = rangeSection(source.juz);
  const hizbPayload = rangeSection(source.hizb);
  const rubPayload = rangeSection(source.rub);

  const sajdahPayload =
    (source.sajdah ?? []).length > 0
      ? concat(
          [...(source.sajdah ?? [])]
            .sort(
              (a, b) =>
                ayahIndexOf.get(key(a.surah, a.ayah))! - ayahIndexOf.get(key(b.surah, b.ayah))!,
            )
            .map((sajdah) => {
              const index = ayahIndexOf.get(key(sajdah.surah, sajdah.ayah))!;
              return encodeSajdah({
                ayahIndex: index,
                page: ayahs[index]!.page,
                kind: sajdah.kind === 'obligatory' ? 1 : 0,
              });
            }),
        )
      : undefined;

  // --- assemble ------------------------------------------------------------
  const metadata: Array<[MetadataKey, string]> = [
    [MetadataKey.Title, source.metadata.title],
    [MetadataKey.BuildTimestamp, new Date().toISOString()],
    [MetadataKey.ConverterVersion, options.converterVersion ?? '0.1.0'],
  ];
  if (source.metadata.language) metadata.push([MetadataKey.Language, source.metadata.language]);
  if (source.metadata.publisher) metadata.push([MetadataKey.Publisher, source.metadata.publisher]);
  if (source.metadata.source) metadata.push([MetadataKey.Source, source.metadata.source]);
  if (source.metadata.script) metadata.push([MetadataKey.Script, source.metadata.script]);
  for (const [key, value] of options.extraMetadata ?? []) {
    const existing = metadata.findIndex((entry) => entry[0] === key);
    if (existing >= 0) metadata[existing] = [key as MetadataKey, value];
    else metadata.push([key as MetadataKey, value]);
  }

  // The word-layout flag means "these boxes are real". A package can carry
  // word text for Hifz without any coordinates, and must not claim otherwise.
  let flags = FLAG_RIGHT_TO_LEFT;
  if (source.layout && source.layout.words.length > 0) flags |= FLAG_HAS_WORD_LAYOUT;

  const sections = [
    { id: SectionId.SurahIndex, payload: concat(surahRecords), count: surahRecords.length },
    { id: SectionId.PageIndex, payload: concat(pageRecords), count: pageRecords.length },
    { id: SectionId.AyahIndex, payload: concat(ayahRecords), count: ayahRecords.length },
    { id: SectionId.TextData, payload: text.bytes(), count: 0 },
  ];
  if (juzPayload) {
    sections.push({ id: SectionId.JuzIndex, payload: juzPayload, count: juzPayload.length / 8 });
  }
  if (hizbPayload) {
    sections.push({ id: SectionId.HizbIndex, payload: hizbPayload, count: hizbPayload.length / 8 });
  }
  if (rubPayload) {
    sections.push({ id: SectionId.RubIndex, payload: rubPayload, count: rubPayload.length / 8 });
  }
  if (sajdahPayload) {
    sections.push({
      id: SectionId.SajdahIndex,
      payload: sajdahPayload,
      count: sajdahPayload.length / 8,
    });
  }
  if (wordRecords.length > 0) {
    sections.push({
      id: SectionId.WordIndex,
      payload: concat(wordRecords),
      count: wordRecords.length,
    });
  }
  // FLAG_HAS_WORD_LAYOUT's documented meaning (qpk-format.md 4.2) is
  // "WORD_INDEX + LAYOUT_DATA present" -- the reader rejects the flag
  // without this section (rule 16), so it is not conditioned separately.
  if (flags & FLAG_HAS_WORD_LAYOUT) {
    const lineRecords: LineRecord[] = [...lineAgg.values()]
      .sort((a, b) => a.page - b.page || a.lineId - b.lineId)
      .map((line) => ({
        page: line.page,
        lineId: line.lineId,
        y: line.yTop,
        height: line.yBottom - line.yTop,
        firstWordIndex: line.firstWordIndex,
      }));
    // A separate counter: report.clampedCoordinates and its warning are about
    // word coordinates specifically (checked below), and a page dimension
    // clamping is a different, vanishingly unlikely event that must not
    // silently inflate that count.
    const pageDimensionClamps = { clamped: 0 };
    sections.push({
      id: SectionId.LayoutData,
      payload: encodeLayoutData(
        lineRecords,
        clampCoordinate(source.layout!.pageWidth, pageDimensionClamps),
        clampCoordinate(source.layout!.pageHeight, pageDimensionClamps),
      ),
      count: 0,
    });
  }

  // See content-id.ts: the script is part of a mushaf's identity, because two
  // scripts of the same text are two different works to a reader.
  const contentId =
    options.contentId ??
    deriveContentId({
      type: PackageType.Quran,
      title: source.metadata.title,
      language: source.metadata.language,
      script: source.metadata.script,
    });

  const bytes = writePackage({
    type: PackageType.Quran,
    flags,
    contentVersion: options.contentVersion ?? 1,
    contentId,
    metadata,
    sections,
  });

  report.packageSize = bytes.length;
  report.pageCount = pageCount;
  report.chapterCount = surahRecords.length;
  report.sectionCount = 0;
  report.wordCount = wordRecords.length;
  report.layoutSpace = source.layout
    ? { width: source.layout.pageWidth, height: source.layout.pageHeight }
    : { width: 0, height: 0 };
  report.estimatedWordFraction = 0; // supplied coordinates are never estimated
  report.clampedCoordinates = counter.clamped;

  if (wordRecords.length > 0 && !source.layout) {
    report.warnings.push(
      'word text is present but no coordinates were supplied: Hifz word hiding will ' +
        'work, word-level highlighting will not',
    );
  }
  if (counter.clamped > 0) {
    report.warnings.push(
      `${counter.clamped} word coordinates exceeded the 16-bit layout field and were clamped`,
    );
  }

  return { bytes, report };
}

function juzOfAyah(
  juz: QuranSourceRange[] | undefined,
  surah: number,
  ayah: number,
): number | undefined {
  return juz?.find((range) => range.surah === surah && range.ayah === ayah)?.id;
}

export type { QuranSource, QuranSourceAyah };
