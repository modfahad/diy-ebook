// PDF text extraction via pdfjs-dist.
//
// Two things here are easy to get wrong and are therefore isolated and tested
// directly:
//
//  1. The coordinate transform. PDF user space is y-up from the bottom left;
//     the layout space is y-down from the top left, and `page.rotate` (0/90/
//     180/270, common in scans) rotates the whole thing. pdfjs gives us the
//     viewport transform that composes all of it; `runToBox` applies it and
//     returns an axis-aligned box.
//
//  2. Word segmentation. getTextContent() returns *text runs*, not words, and
//     runs split unpredictably. See splitRunIntoWords in model.ts.

import type { ParsedDocument, ParsedPage, TextLine, WordBox } from '../model.js';
import { splitRunIntoWords } from '../model.js';

/** A 2x3 affine transform as pdfjs represents it: [a, b, c, d, e, f]. */
export type Matrix = [number, number, number, number, number, number];

export interface RunBox {
  x: number;
  y: number;
  width: number;
  height: number;
  /** Rotation of the text baseline in degrees, 0 for normal horizontal text. */
  angle: number;
}

/**
 * Converts one text run into an axis-aligned layout-space box.
 *
 * `tx` is the item transform already composed with the viewport transform
 * (pdfjs `Util.transform(viewport.transform, item.transform)`), so it maps
 * text space straight to y-down device space. `advance` is the run's width in
 * viewport units, which is what getTextContent reports at scale 1.
 *
 * The box is built from the four corners rather than from `(tx[4], tx[5] - h)`
 * because the latter is only correct for unrotated text: on a 90-degree page
 * the baseline runs down the page and the ascenders point sideways.
 */
export function runToBox(tx: Matrix, advance: number): RunBox {
  const angle = Math.atan2(tx[1], tx[0]);
  const fontHeight = Math.hypot(tx[2], tx[3]);

  // Along the baseline, and from the baseline towards the ascenders.
  const alongX = Math.cos(angle) * advance;
  const alongY = Math.sin(angle) * advance;
  const upX = -Math.sin(angle) * fontHeight;
  const upY = -Math.cos(angle) * fontHeight;

  const originX = tx[4];
  const originY = tx[5];
  const xs = [originX, originX + alongX, originX + upX, originX + alongX + upX];
  const ys = [originY, originY + alongY, originY + upY, originY + alongY + upY];

  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);

  return {
    x: minX,
    y: minY,
    width: maxX - minX,
    height: maxY - minY,
    angle: (angle * 180) / Math.PI,
  };
}

/** Groups word boxes into lines by vertical overlap, then orders them. */
export function groupIntoLines(words: WordBox[]): TextLine[] {
  if (words.length === 0) return [];

  const sorted = [...words].sort((a, b) => a.y - b.y || a.x - b.x);
  const lines: WordBox[][] = [];
  let current: WordBox[] = [sorted[0]!];
  let currentTop = sorted[0]!.y;
  let currentBottom = sorted[0]!.y + sorted[0]!.height;

  for (let i = 1; i < sorted.length; i++) {
    const word = sorted[i]!;
    const overlap =
      Math.min(currentBottom, word.y + word.height) - Math.max(currentTop, word.y);
    // More than half of the shorter box overlapping means "same line". This
    // tolerates the baseline jitter that superscripts and inline sizes cause.
    const threshold = Math.min(word.height, currentBottom - currentTop) * 0.5;
    if (overlap >= threshold) {
      current.push(word);
      currentTop = Math.min(currentTop, word.y);
      currentBottom = Math.max(currentBottom, word.y + word.height);
    } else {
      lines.push(current);
      current = [word];
      currentTop = word.y;
      currentBottom = word.y + word.height;
    }
  }
  lines.push(current);

  return lines.map((lineWords) => {
    const ordered = [...lineWords].sort((a, b) => a.x - b.x);
    const top = Math.min(...ordered.map((w) => w.y));
    const bottom = Math.max(...ordered.map((w) => w.y + w.height));
    return {
      y: top,
      height: bottom - top,
      text: ordered.map((w) => w.text).join(' '),
      words: ordered,
    };
  });
}

interface PdfjsModule {
  getDocument(params: unknown): { promise: Promise<PdfjsDocument> };
  Util: { transform(a: number[], b: number[]): number[] };
}

interface PdfjsDocument {
  numPages: number;
  getPage(n: number): Promise<PdfjsPage>;
  getMetadata(): Promise<{ info?: Record<string, unknown> }>;
  destroy(): Promise<void>;
}

interface PdfjsPage {
  rotate: number;
  getViewport(params: { scale: number }): { width: number; height: number; transform: number[] };
  getTextContent(): Promise<{ items: PdfjsTextItem[] }>;
}

interface PdfjsTextItem {
  str?: string;
  width?: number;
  transform?: number[];
}

let cachedModule: PdfjsModule | undefined;

async function loadPdfjs(): Promise<PdfjsModule> {
  if (!cachedModule) {
    // The legacy build is the one that runs without a DOM.
    cachedModule = (await import('pdfjs-dist/legacy/build/pdf.mjs')) as unknown as PdfjsModule;
  }
  return cachedModule;
}

export interface PdfParseOptions {
  /** Stop after this many pages. Useful for a fast preview. */
  maxPages?: number;
}

export async function parsePdf(
  data: Uint8Array,
  options: PdfParseOptions = {},
): Promise<ParsedDocument> {
  const pdfjs = await loadPdfjs();
  const warnings: string[] = [];

  const doc = await pdfjs.getDocument({
    data,
    isEvalSupported: false,
    useSystemFonts: false,
    // Errors only. pdfjs warns about font data and DOM polyfills it needs for
    // *rendering*; we only ever ask for text, and the noise would end up in
    // the converter CLI's output.
    verbosity: 0,
  }).promise;

  let title: string | undefined;
  let author: string | undefined;
  try {
    const meta = await doc.getMetadata();
    const info = meta.info ?? {};
    if (typeof info['Title'] === 'string' && info['Title'].trim()) title = info['Title'].trim();
    if (typeof info['Author'] === 'string' && info['Author'].trim()) author = info['Author'].trim();
  } catch {
    warnings.push('PDF metadata could not be read');
  }

  const limit = Math.min(doc.numPages, options.maxPages ?? doc.numPages);
  const pages: ParsedPage[] = [];
  let emptyPages = 0;

  for (let n = 1; n <= limit; n++) {
    const page = await doc.getPage(n);
    const viewport = page.getViewport({ scale: 1 });
    const content = await page.getTextContent();

    const words: WordBox[] = [];
    for (const item of content.items) {
      const text = item.str ?? '';
      if (text.trim().length === 0) continue;
      if (!item.transform) continue;
      const tx = pdfjs.Util.transform(viewport.transform, item.transform) as Matrix;
      const box = runToBox(tx, item.width ?? 0);
      words.push(...splitRunIntoWords(text, box.x, box.y, box.width, box.height));
    }

    if (words.length === 0) emptyPages++;

    pages.push({
      pageNumber: n,
      width: Math.round(viewport.width),
      height: Math.round(viewport.height),
      lines: groupIntoLines(words),
    });
  }

  await doc.destroy();

  if (emptyPages === pages.length && pages.length > 0) {
    warnings.push(
      'no extractable text on any page: this looks like a scanned PDF, ' +
        'which needs the optional OCR step',
    );
  } else if (emptyPages > 0) {
    warnings.push(`${emptyPages} of ${pages.length} pages have no extractable text`);
  }

  return { source: 'pdf', title, author, pages, warnings };
}
