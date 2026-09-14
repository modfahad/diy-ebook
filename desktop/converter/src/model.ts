// The neutral document model every source produces and every downstream stage
// consumes. PDF, EPUB and TXT differ only in how they fill this in.
//
// Coordinates are in the *layout coordinate space*, which is the source page's
// own space (PDF points for a PDF, a synthetic grid for reflowable formats),
// y-down with the origin at the top left. The renderer scales; nothing here
// assumes the device's 792x272.

export type SourceKind = 'txt' | 'pdf' | 'epub' | 'quran-json';

export interface WordBox {
  text: string;
  x: number;
  y: number;
  width: number;
  height: number;
  /**
   * True when the box was apportioned across a multi-word text run rather
   * than measured. PDFs that emit one run per word give exact boxes; a run
   * like "the quick brown fox" has to be split proportionally. The
   * conversion report says how much of a document is estimated, because
   * word-level AI highlighting is only as good as this.
   */
  estimated: boolean;
}

export interface TextLine {
  /** Top of the line in layout space. */
  y: number;
  height: number;
  text: string;
  words: WordBox[];
}

export interface ParsedPage {
  /** 1-based, contiguous. */
  pageNumber: number;
  width: number;
  height: number;
  lines: TextLine[];
}

export interface ParsedDocument {
  source: SourceKind;
  // `| undefined` on purpose: exactOptionalPropertyTypes is on, and a source
  // that simply has no title assigns undefined rather than omitting the key.
  title?: string | undefined;
  author?: string | undefined;
  language?: string | undefined;
  pages: ParsedPage[];
  /** Non-fatal problems worth showing before install. */
  warnings: string[];
}

export function pageText(page: ParsedPage): string {
  return page.lines.map((line) => line.text).join('\n');
}

export function documentWordCount(doc: ParsedDocument): number {
  let n = 0;
  for (const page of doc.pages) {
    for (const line of page.lines) n += line.words.length;
  }
  return n;
}

export function estimatedWordFraction(doc: ParsedDocument): number {
  let total = 0;
  let estimated = 0;
  for (const page of doc.pages) {
    for (const line of page.lines) {
      for (const word of line.words) {
        total++;
        if (word.estimated) estimated++;
      }
    }
  }
  return total === 0 ? 0 : estimated / total;
}

/**
 * Splits a text run into word boxes.
 *
 * A run with no internal whitespace is one word and its box is exact. A run
 * with spaces has to be apportioned: only the run's total advance width is
 * known, so width is shared out by character count (spaces included). That is
 * an approximation and is flagged as such rather than quietly presented as a
 * measurement.
 */
export function splitRunIntoWords(
  text: string,
  x: number,
  y: number,
  width: number,
  height: number,
): WordBox[] {
  const trimmed = text.replace(/\s+$/u, '');
  if (trimmed.length === 0) return [];

  if (!/\s/u.test(trimmed)) {
    return [{ text: trimmed, x, y, width, height, estimated: false }];
  }

  const perChar = width / trimmed.length;
  const out: WordBox[] = [];
  let cursor = 0;
  for (const match of trimmed.matchAll(/\S+/gu)) {
    const start = match.index ?? cursor;
    const wordText = match[0];
    out.push({
      text: wordText,
      x: x + start * perChar,
      y,
      width: wordText.length * perChar,
      height,
      estimated: true,
    });
    cursor = start + wordText.length;
  }
  return out;
}
