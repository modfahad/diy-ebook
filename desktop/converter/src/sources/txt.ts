// Plain-text source.
//
// Blank lines separate paragraphs. A line that looks like a heading (short,
// isolated, and matching a chapter-ish pattern) is promoted to level 1 here so
// the structure detector has something to work with; everything else is body
// text and gets whatever the detector can infer later.

import type { ParsedDocument } from '../model.js';
import { DEFAULT_GRID, type Block, type GridOptions, paginateBlocks } from './paginate.js';

const HEADING_PATTERNS: RegExp[] = [
  /^\s*(chapter|part|section|book|appendix|preface|introduction|epilogue)\b/iu,
  /^\s*\d+\s*[.)]\s+\S/u,
  /^\s*[IVXLCDM]+\s*[.)]\s+\S/u,
];

export function looksLikeHeading(line: string): boolean {
  const trimmed = line.trim();
  if (trimmed.length === 0 || trimmed.length > 80) return false;
  if (HEADING_PATTERNS.some((pattern) => pattern.test(trimmed))) return true;
  // Short, all upper case, and not a sentence.
  if (
    trimmed.length <= 60 &&
    trimmed === trimmed.toUpperCase() &&
    /[A-Z]/u.test(trimmed) &&
    !/[.!?]$/u.test(trimmed)
  ) {
    return true;
  }
  return false;
}

export interface TxtParseOptions {
  title?: string;
  author?: string;
  language?: string;
  grid?: GridOptions;
}

export function parseTxt(text: string, options: TxtParseOptions = {}): ParsedDocument {
  const normalised = text.replace(/\r\n?/gu, '\n');
  const rawLines = normalised.split('\n');
  const warnings: string[] = [];

  const blocks: Block[] = [];
  let paragraph: string[] = [];

  const flushParagraph = () => {
    if (paragraph.length === 0) return;
    blocks.push({ text: paragraph.join(' '), level: 0 });
    paragraph = [];
  };

  for (let i = 0; i < rawLines.length; i++) {
    const line = rawLines[i] ?? '';
    if (line.trim().length === 0) {
      flushParagraph();
      continue;
    }
    const isolatedBefore = i === 0 || (rawLines[i - 1] ?? '').trim().length === 0;
    const isolatedAfter =
      i === rawLines.length - 1 || (rawLines[i + 1] ?? '').trim().length === 0;
    if (isolatedBefore && isolatedAfter && looksLikeHeading(line)) {
      flushParagraph();
      blocks.push({ text: line.trim(), level: 1 });
      continue;
    }
    paragraph.push(line.trim());
  }
  flushParagraph();

  if (blocks.length === 0) {
    warnings.push('the file contained no text');
  }

  const pages = paginateBlocks(blocks, options.grid ?? DEFAULT_GRID);
  return {
    source: 'txt',
    title: options.title,
    author: options.author,
    language: options.language,
    pages,
    warnings,
  };
}
