// Structure detection: turning a flat page-and-line document into chapters
// and sections.
//
// Two signals, deliberately combined rather than chosen between:
//
//   * typographic -- a line noticeably taller than the document's body text.
//     This is what EPUB gives for free (the paginator scales <h1>..<h6>) and
//     what a well-made PDF gives too.
//   * lexical -- "Chapter 4", "II.", an isolated all-caps line. This is the
//     only signal available when a PDF sets every heading in the body size.
//
// A document where neither fires is not an error. It becomes a single
// chapter, because a book with no detected headings still has to install and
// read; refusing it would be the converter deciding what counts as a book.

import type { ParsedDocument } from '../model.js';

export interface DetectedHeading {
  /** 1 = chapter, 2+ = nested section. */
  level: number;
  title: string;
  /** 0-based index into doc.pages. */
  pageIndex: number;
  /** 0-based index into that page's lines. */
  lineIndex: number;
}

const LEXICAL_PATTERNS: RegExp[] = [
  /^\s*(chapter|part|section|book|appendix|preface|introduction|epilogue|foreword)\b/iu,
  /^\s*\d+\s*[.)]\s+\S/u,
  /^\s*[IVXLCDM]{1,7}\s*[.)]\s+\S/u,
];

const MAX_HEADING_CHARS = 90;

export function looksLexicallyLikeHeading(text: string): boolean {
  const trimmed = text.trim();
  if (trimmed.length === 0 || trimmed.length > MAX_HEADING_CHARS) return false;
  if (LEXICAL_PATTERNS.some((pattern) => pattern.test(trimmed))) return true;
  if (
    trimmed.length <= 60 &&
    trimmed === trimmed.toUpperCase() &&
    /\p{Lu}/u.test(trimmed) &&
    !/[.!?]$/u.test(trimmed)
  ) {
    return true;
  }
  return false;
}

/** The most common line height in the document: its body text size. */
export function bodyLineHeight(doc: ParsedDocument): number {
  const histogram = new Map<number, number>();
  for (const page of doc.pages) {
    for (const line of page.lines) {
      const bucket = Math.round(line.height);
      histogram.set(bucket, (histogram.get(bucket) ?? 0) + 1);
    }
  }
  let best = 0;
  let bestCount = -1;
  for (const [height, count] of histogram) {
    if (count > bestCount || (count === bestCount && height < best)) {
      best = height;
      bestCount = count;
    }
  }
  return best;
}

export interface StructureOptions {
  /** A line this many times the body height counts as a heading. */
  heightRatio?: number;
  /** Ignore the lexical signal (useful when a document is full of numbered lists). */
  lexicalSignal?: boolean;
}

export function detectStructure(
  doc: ParsedDocument,
  options: StructureOptions = {},
): DetectedHeading[] {
  const heightRatio = options.heightRatio ?? 1.25;
  const useLexical = options.lexicalSignal ?? true;
  const body = bodyLineHeight(doc);
  const headings: DetectedHeading[] = [];

  for (let pageIndex = 0; pageIndex < doc.pages.length; pageIndex++) {
    const page = doc.pages[pageIndex]!;
    for (let lineIndex = 0; lineIndex < page.lines.length; lineIndex++) {
      const line = page.lines[lineIndex]!;
      const text = line.text.trim();
      if (text.length === 0 || text.length > MAX_HEADING_CHARS) continue;

      const ratio = body > 0 ? line.height / body : 1;
      const bigger = ratio >= heightRatio;
      const lexical = useLexical && looksLexicallyLikeHeading(text);
      if (!bigger && !lexical) continue;

      headings.push({
        level: bigger ? levelFromRatio(ratio) : 1,
        title: text,
        pageIndex,
        lineIndex,
      });
    }
  }

  return normaliseLevels(headings);
}

function levelFromRatio(ratio: number): number {
  if (ratio >= 1.85) return 1;
  if (ratio >= 1.55) return 2;
  if (ratio >= 1.3) return 3;
  return 4;
}

/**
 * Rebases levels so the shallowest heading found is level 1. A document whose
 * only headings are `<h3>` still has chapters, not a tree that starts three
 * levels down.
 */
function normaliseLevels(headings: DetectedHeading[]): DetectedHeading[] {
  if (headings.length === 0) return headings;
  const shallowest = Math.min(...headings.map((h) => h.level));
  return headings.map((h) => ({ ...h, level: h.level - shallowest + 1 }));
}
