// A minimal, hand-built PDF writer for test fixtures.
//
// Generating the fixture is deliberate: a committed binary PDF would be a
// thing nobody can reason about when a test fails. This produces a file small
// enough to read in a hex dump, and lets a test ask for exactly the awkward
// case it needs -- a rotated page, an empty page, a page with no text.

export interface PdfTextItem {
  text: string;
  /** PDF user space: origin bottom-left, y up. */
  x: number;
  y: number;
  size: number;
}

export interface PdfPageSpec {
  width: number;
  height: number;
  /** 0, 90, 180 or 270. */
  rotate?: number;
  items: PdfTextItem[];
}

function escapeText(text: string): string {
  return text.replace(/([()\\])/gu, '\\$1');
}

export function makePdf(pages: PdfPageSpec[]): Uint8Array {
  if (pages.length === 0) throw new Error('a PDF needs at least one page');

  const objects: Array<string | { stream: string }> = [];
  const firstPageObject = 4;
  const kids = pages.map((_, i) => `${firstPageObject + i * 2} 0 R`).join(' ');

  objects[1] = '<</Type/Catalog/Pages 2 0 R>>';
  objects[2] = `<</Type/Pages/Kids[${kids}]/Count ${pages.length}>>`;
  objects[3] = '<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>';

  pages.forEach((page, i) => {
    const pageObject = firstPageObject + i * 2;
    const contentObject = pageObject + 1;
    const rotate = page.rotate ? `/Rotate ${page.rotate}` : '';
    objects[pageObject] =
      `<</Type/Page/Parent 2 0 R/MediaBox[0 0 ${page.width} ${page.height}]` +
      `${rotate}/Resources<</Font<</F1 3 0 R>>>>/Contents ${contentObject} 0 R>>`;
    objects[contentObject] = {
      stream: page.items
        .map(
          (item) =>
            `BT /F1 ${item.size} Tf ${item.x} ${item.y} Td (${escapeText(item.text)}) Tj ET`,
        )
        .join('\n'),
    };
  });

  let out = '%PDF-1.4\n';
  const offsets: number[] = [];
  for (let n = 1; n < objects.length; n++) {
    const object = objects[n]!;
    offsets[n] = out.length;
    if (typeof object === 'string') {
      out += `${n} 0 obj\n${object}\nendobj\n`;
    } else {
      out +=
        `${n} 0 obj\n<</Length ${object.stream.length}>>\nstream\n` +
        `${object.stream}\nendstream\nendobj\n`;
    }
  }

  const xrefAt = out.length;
  out += `xref\n0 ${objects.length}\n0000000000 65535 f \n`;
  for (let n = 1; n < objects.length; n++) {
    out += `${String(offsets[n]).padStart(10, '0')} 00000 n \n`;
  }
  out += `trailer\n<</Size ${objects.length}/Root 1 0 R>>\nstartxref\n${xrefAt}\n%%EOF\n`;

  return new TextEncoder().encode(out);
}
