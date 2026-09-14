// ParsedDocument -> QPK1 BOOK package.
//
// Generic books deliberately do NOT reuse the Quran structures (spec section
// 11): no surah, no ayah, no juz. A book is chapters, sections, pages and
// text, and its indexes say exactly that.

import {
  FLAG_HAS_WORD_LAYOUT,
  FLAG_RIGHT_TO_LEFT,
  MetadataKey,
  PackageType,
  SectionId,
  TextPool,
  encodeBookSection,
  encodeChapter,
  encodeLayoutData,
  encodePage,
  encodeTextRecord,
  encodeWord,
  NO_PARENT,
  PAGE_IMAGE_HEIGHT,
  PAGE_IMAGE_WIDTH,
  decodeCover,
  encodePageImageRecord,
  pageImageBytes,
  unpackBits,
  writePackage,
  type LineRecord,
} from '@quran-device/qpk-format';

import { estimatedWordFraction, pageText, type ParsedDocument } from '../model.js';
import { deriveContentId } from './content-id.js';
import { detectStructure, type DetectedHeading } from './structure.js';
import {
  clampCoordinate,
  emptyReport,
  type ConversionResult,
} from './report.js';

export interface BookConversionOptions {
  title?: string;
  author?: string;
  language?: string;
  publisher?: string;
  source?: string;
  contentId?: Uint8Array;
  contentVersion?: number;
  /** Emit WORD_INDEX. Default true; turn it off for a text-only package. */
  includeWordLayout?: boolean;
  rightToLeft?: boolean;
  /** Skip structure detection and treat the whole document as one chapter. */
  singleChapter?: boolean;
  converterVersion?: string;
  /**
   * A finished COVER section payload (qpk-format's encodeCover). Built by the
   * caller because turning a picture into one needs an image decoder, which
   * the desktop app's webview has and this library does not.
   */
  cover?: Uint8Array;
  /**
   * Page pictures (qpk-format.md 9d): one PackBits-compressed 1bpp bitmap per
   * source page, each pageImageWidth x pageImageHeight (default 480x800).
   * Rendered by the caller for the same reason as `cover`: drawing a PDF page
   * needs a canvas.
   */
  pageImages?: Uint8Array[];
  pageImageWidth?: number;
  pageImageHeight?: number;
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

export function buildBookPackage(
  doc: ParsedDocument,
  options: BookConversionOptions = {},
): ConversionResult {
  const report = emptyReport('BOOK');
  report.warnings.push(...doc.warnings);

  if (doc.pages.length === 0) {
    throw new Error('cannot build a package from a document with no pages');
  }

  const text = new TextPool();
  const counter = { clamped: 0 };
  const includeWords = options.includeWordLayout ?? true;

  // --- headings -------------------------------------------------------------
  const headings: DetectedHeading[] = options.singleChapter ? [] : detectStructure(doc);
  const chapters = headings.filter((h) => h.level === 1);
  const sections = headings.filter((h) => h.level > 1);

  if (chapters.length === 0) {
    report.warnings.push(
      'no chapter headings were detected; the whole document becomes one chapter',
    );
  }

  // --- per-page text, word boxes and indexes --------------------------------
  const textRecords: Uint8Array[] = [];
  const pageRecords: Uint8Array[] = [];
  const wordRecords: Uint8Array[] = [];
  const lineRecords: LineRecord[] = [];

  let firstWordIndex = 0;
  for (const page of doc.pages) {
    const body = text.add(pageText(page));
    textRecords.push(
      encodeTextRecord({
        textOffset: body.offset,
        textLength: body.length,
        pageNumber: page.pageNumber,
        flags: 0,
      }),
    );

    const wordsOnPage = includeWords ? page.lines.flatMap((line) => line.words) : [];
    let lineId = 0;
    if (includeWords) {
      for (const line of page.lines) {
        const lineFirstWordIndex = wordRecords.length;
        for (const word of line.words) {
          const ref = text.add(word.text);
          wordRecords.push(
            encodeWord({
              textOffset: ref.offset,
              textLength: ref.length,
              x: clampCoordinate(word.x, counter),
              y: clampCoordinate(word.y, counter),
              width: clampCoordinate(word.width, counter),
              height: clampCoordinate(word.height, counter),
              lineId: Math.min(lineId, 0xffff),
            }),
          );
        }
        // A line with no words (a blank line detected by structure, not
        // text) contributes no geometry a renderer could use; skip it rather
        // than emit a record no WORD_INDEX range will ever point at.
        if (line.words.length > 0) {
          lineRecords.push({
            page: page.pageNumber,
            lineId: Math.min(lineId, 0xffff),
            y: clampCoordinate(line.y, counter),
            height: clampCoordinate(line.height, counter),
            firstWordIndex: lineFirstWordIndex,
          });
        }
        lineId++;
      }
    }

    pageRecords.push(
      encodePage({
        pageNumber: page.pageNumber,
        // A book has no ayahs. These fields stay zero rather than being
        // repurposed -- the device must not read Quran meaning out of a book.
        ayahCount: 0,
        firstAyahIndex: 0,
        firstWordIndex,
        lineCount: Math.min(page.lines.length, 255),
        juz: 0,
        flags: 0,
      }),
    );
    firstWordIndex += wordsOnPage.length;
    report.wordCount += wordsOnPage.length;
  }

  // --- chapter and section indexes -----------------------------------------
  const sectionRecords: Uint8Array[] = sections.map((heading) => {
    const title = text.add(heading.title);
    return encodeBookSection({
      firstPage: doc.pages[heading.pageIndex]!.pageNumber,
      titleOffset: title.offset,
      titleLength: title.length,
      depth: heading.level - 1,
      firstTextIndex: heading.pageIndex,
    });
  });

  const chapterRecords: Uint8Array[] = [];
  if (chapters.length === 0) {
    const title = text.add(options.title ?? 'Body');
    chapterRecords.push(
      encodeChapter({
        chapterNumber: 1,
        firstPage: doc.pages[0]!.pageNumber,
        firstSectionIndex: 0,
        titleOffset: title.offset,
        titleLength: title.length,
        depth: 0,
        parentIndex: NO_PARENT,
      }),
    );
  } else {
    chapters.forEach((heading, index) => {
      const title = text.add(heading.title);
      // Index of this chapter's first section. A chapter with no sections
      // after it gets a one-past-the-end sentinel rather than 0, which would
      // otherwise point at the FIRST chapter's first section.
      const found = sections.findIndex(
        (s) =>
          s.pageIndex > heading.pageIndex ||
          (s.pageIndex === heading.pageIndex && s.lineIndex > heading.lineIndex),
      );
      const firstSection = found < 0 ? sections.length : found;
      chapterRecords.push(
        encodeChapter({
          chapterNumber: index + 1,
          firstPage: doc.pages[heading.pageIndex]!.pageNumber,
          firstSectionIndex: firstSection,
          titleOffset: title.offset,
          titleLength: title.length,
          depth: 0,
          parentIndex: NO_PARENT,
        }),
      );
    });
  }

  // --- assemble -------------------------------------------------------------
  const metadata: Array<[MetadataKey, string]> = [];
  const title = options.title ?? doc.title;
  const author = options.author ?? doc.author;
  const language = options.language ?? doc.language;
  if (title) metadata.push([MetadataKey.Title, title]);
  if (author) metadata.push([MetadataKey.Author, author]);
  if (language) metadata.push([MetadataKey.Language, language]);
  if (options.publisher) metadata.push([MetadataKey.Publisher, options.publisher]);
  metadata.push([MetadataKey.Source, options.source ?? `${doc.source} import`]);
  metadata.push([MetadataKey.BuildTimestamp, new Date().toISOString()]);
  metadata.push([MetadataKey.ConverterVersion, options.converterVersion ?? '0.1.0']);

  let flags = 0;
  if (includeWords && wordRecords.length > 0) flags |= FLAG_HAS_WORD_LAYOUT;
  if (options.rightToLeft) flags |= FLAG_RIGHT_TO_LEFT;

  const sectionsToWrite = [
    { id: SectionId.ChapterIndex, payload: concat(chapterRecords), count: chapterRecords.length },
    { id: SectionId.PageIndex, payload: concat(pageRecords), count: pageRecords.length },
    { id: SectionId.TextIndex, payload: concat(textRecords), count: textRecords.length },
    { id: SectionId.TextData, payload: text.bytes(), count: 0 },
  ];
  if (sectionRecords.length > 0) {
    sectionsToWrite.push({
      id: SectionId.SectionIndex,
      payload: concat(sectionRecords),
      count: sectionRecords.length,
    });
  }
  if (wordRecords.length > 0) {
    sectionsToWrite.push({
      id: SectionId.WordIndex,
      payload: concat(wordRecords),
      count: wordRecords.length,
    });
  }
  // FLAG_HAS_WORD_LAYOUT's documented meaning (qpk-format.md 4.2) is
  // "WORD_INDEX + LAYOUT_DATA present" -- the reader rejects the flag
  // without this section (rule 16), so it is not conditioned separately.
  if (flags & FLAG_HAS_WORD_LAYOUT) {
    // A separate counter: report.clampedCoordinates and its warning are about
    // word coordinates specifically (checked below), and a page dimension
    // clamping is a different, vanishingly unlikely event that must not
    // silently inflate that count.
    const pageDimensionClamps = { clamped: 0 };
    sectionsToWrite.push({
      id: SectionId.LayoutData,
      payload: encodeLayoutData(
        lineRecords,
        clampCoordinate(doc.pages[0]!.width, pageDimensionClamps),
        clampCoordinate(doc.pages[0]!.height, pageDimensionClamps),
      ),
      count: 0,
    });
  }

  if (options.cover) {
    decodeCover(options.cover); // throws on a malformed payload before it is packaged
    sectionsToWrite.push({ id: SectionId.Cover, payload: options.cover, count: 0 });
  }

  if (options.pageImages && options.pageImages.length > 0) {
    const width = options.pageImageWidth ?? PAGE_IMAGE_WIDTH;
    const height = options.pageImageHeight ?? PAGE_IMAGE_HEIGHT;
    const bytesPerPage = pageImageBytes(width, height);
    const records: Uint8Array[] = [];
    let offset = 0;
    for (const image of options.pageImages) {
      unpackBits(image, bytesPerPage); // throws on a malformed page before it is packaged
      records.push(encodePageImageRecord({ offset, length: image.length, width, height }));
      offset += image.length;
    }
    sectionsToWrite.push({ id: SectionId.PageImageIndex, payload: concat(records), count: records.length });
    sectionsToWrite.push({ id: SectionId.PageImageData, payload: concat(options.pageImages), count: 0 });
    if (options.pageImages.length !== doc.pages.length) {
      report.warnings.push(
        `${options.pageImages.length} page pictures but ${doc.pages.length} text pages; ` +
          'the reader shows the pictures, so search results may point at a different page',
      );
    }
  }

  // Derived from the work's identity when the caller does not supply one.
  // An all-zero id would make every book collide with every other book: the
  // library index and the upload session are both keyed on it.
  const contentId =
    options.contentId ??
    deriveContentId({
      type: PackageType.Book,
      title: title ?? 'Untitled',
      author,
      language,
    });

  const bytes = writePackage({
    type: PackageType.Book,
    flags,
    contentVersion: options.contentVersion ?? 1,
    contentId,
    metadata,
    sections: sectionsToWrite,
  });

  report.packageSize = bytes.length;
  report.pageCount = doc.pages.length;
  report.chapterCount = chapterRecords.length;
  report.sectionCount = sectionRecords.length;
  report.layoutSpace = { width: doc.pages[0]!.width, height: doc.pages[0]!.height };
  report.estimatedWordFraction = estimatedWordFraction(doc);
  report.clampedCoordinates = counter.clamped;

  if (counter.clamped > 0) {
    report.warnings.push(
      `${counter.clamped} word coordinates exceeded the 16-bit layout field and were ` +
        'clamped; the source page is larger than the format can address',
    );
  }
  if (report.estimatedWordFraction > 0.5 && report.wordCount > 0) {
    report.warnings.push(
      `${Math.round(report.estimatedWordFraction * 100)}% of word boxes are estimated ` +
        'rather than measured; word-level highlighting will be approximate',
    );
  }

  return { bytes, report };
}
