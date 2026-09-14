// Synthetic pagination for reflowable sources (TXT, EPUB).
//
// A reflowable document has no pages, so the converter invents a fixed grid
// and lays text out on it. The grid IS the layout coordinate space that ends
// up in the package, so word boxes are exact by construction rather than
// estimated -- unlike a PDF, where boxes come from measurement.
//
// The grid is deliberately generous and device-independent. Repaginating for
// a particular screen is the renderer's job, not the converter's; baking the
// device's 792x272 in here would make every package device-specific.

import type { ParsedPage, TextLine, WordBox } from '../model.js';

export interface Block {
  text: string;
  /** 0 = body text, 1..6 = heading level. */
  level: number;
}

export interface GridOptions {
  columns: number;
  rows: number;
  charWidth: number;
  lineHeight: number;
  margin: number;
}

export const DEFAULT_GRID: GridOptions = {
  columns: 72,
  rows: 36,
  charWidth: 10,
  lineHeight: 20,
  margin: 24,
};

export function gridPageSize(grid: GridOptions): { width: number; height: number } {
  return {
    width: grid.margin * 2 + grid.columns * grid.charWidth,
    height: grid.margin * 2 + grid.rows * grid.lineHeight,
  };
}

/** Greedy wrap. Words longer than the column count are hard-split. */
export function wrapText(text: string, columns: number): string[] {
  const words = text.split(/\s+/u).filter((w) => w.length > 0);
  if (words.length === 0) return [];

  const lines: string[] = [];
  let current = '';
  for (const word of words) {
    if (word.length > columns) {
      if (current) {
        lines.push(current);
        current = '';
      }
      for (let i = 0; i < word.length; i += columns) {
        lines.push(word.slice(i, i + columns));
      }
      continue;
    }
    const candidate = current ? `${current} ${word}` : word;
    if (candidate.length <= columns) {
      current = candidate;
    } else {
      lines.push(current);
      current = word;
    }
  }
  if (current) lines.push(current);
  return lines;
}

interface LaidOutLine {
  text: string;
  level: number;
}

/**
 * Lays blocks out on the grid and cuts pages.
 *
 * Heading lines are given a taller box than body lines, which is exactly the
 * signal `detectStructure` looks for -- so a source that already knows its
 * headings (EPUB `<h1>`) and one that has to be guessed at (a PDF) both go
 * through the same detector.
 */
export function paginateBlocks(blocks: Block[], grid: GridOptions = DEFAULT_GRID): ParsedPage[] {
  const { width, height } = gridPageSize(grid);
  const laidOut: LaidOutLine[] = [];

  // Every Block is already one paragraph -- txt.ts's flushParagraph joins a
  // source paragraph's lines into one Block before this ever sees it, so a
  // Block boundary IS a blank-line-in-the-source paragraph break. A blank
  // line between every pair of blocks (not just around headings) is what
  // reconstructs that -- getting this wrong is invisible in the desktop
  // preview, which still renders each Block as its own paragraph regardless,
  // but silently loses the break for anything that reads TEXT_DATA back as
  // flat text instead (see firmware/qpk/book_reader.cpp's on-device reader).
  let firstBlock = true;
  for (const block of blocks) {
    const columns = block.level > 0 ? Math.max(8, Math.floor(grid.columns * 0.7)) : grid.columns;
    const wrapped = wrapText(block.text, columns);
    if (wrapped.length === 0) continue;
    if (!firstBlock) {
      laidOut.push({ text: '', level: 0 });
    }
    for (const line of wrapped) laidOut.push({ text: line, level: block.level });
    firstBlock = false;
  }

  const pages: ParsedPage[] = [];
  let pageLines: TextLine[] = [];
  let row = 0;

  const flush = () => {
    pages.push({
      pageNumber: pages.length + 1,
      width,
      height,
      lines: pageLines,
    });
    pageLines = [];
    row = 0;
  };

  for (const line of laidOut) {
    // A heading occupies more vertical space, so it can push a page break.
    const rowsUsed = line.level > 0 ? 2 : 1;
    if (row + rowsUsed > grid.rows && pageLines.length > 0) flush();

    // Blank lines are pushed too, not just non-empty ones: they carry no
    // words (wordsForLine('', ...) is naturally empty, and book-package.ts
    // already skips emitting a LAYOUT_DATA record for a zero-word line), but
    // they DO carry the paragraph break itself -- pageText() (model.ts)
    // reconstructs a page's TEXT_DATA record by joining page.lines with
    // '\n', so a blank line dropped here is a break silently lost from
    // everything that reads that record back as flat text (this is exactly
    // what left firmware/qpk/book_reader.cpp's on-device reader unable to
    // tell two consecutive body paragraphs apart -- see docs/pending.md).
    const scale = line.level > 0 ? headingScale(line.level) : 1;
    const y = grid.margin + row * grid.lineHeight;
    const lineHeight = grid.lineHeight * scale;
    pageLines.push({
      y,
      height: lineHeight,
      text: line.text,
      words: wordsForLine(line.text, grid, y, lineHeight, scale),
    });
    row += rowsUsed;
  }

  if (pageLines.length > 0 || pages.length === 0) flush();
  return pages;
}

function headingScale(level: number): number {
  // h1 -> 2.0, h2 -> 1.7, h3 -> 1.4, deeper -> 1.2
  if (level <= 1) return 2;
  if (level === 2) return 1.7;
  if (level === 3) return 1.4;
  return 1.2;
}

function wordsForLine(
  text: string,
  grid: GridOptions,
  y: number,
  lineHeight: number,
  scale: number,
): WordBox[] {
  const charWidth = grid.charWidth * scale;
  const out: WordBox[] = [];
  for (const match of text.matchAll(/\S+/gu)) {
    const start = match.index ?? 0;
    out.push({
      text: match[0],
      x: Math.round(grid.margin + start * charWidth),
      y: Math.round(y),
      width: Math.round(match[0].length * charWidth),
      height: Math.round(lineHeight),
      // The grid is the coordinate space, so these positions are exact.
      estimated: false,
    });
  }
  return out;
}
