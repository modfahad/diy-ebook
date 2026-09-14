// A PDF's pages as pictures, the way an e-reader shows a PDF.
//
// Re-flowing a PDF's extracted text loses its layout: columns, tables,
// figures, headings. So, for PDFs, the app also renders every page with
// pdf.js and stores the pictures in the package (docs/qpk-format.md 9d); the
// device's reader then shows the page exactly as printed.
//
// Each page is trimmed to its content (the printed margins waste most of a
// small screen), scaled to fit the device's portrait reader, 480x800, and
// turned into black and white with only the mid-tones dithered, so text stays
// crisp. This runs in the webview because that is where a canvas is.

import * as pdfjs from "pdfjs-dist";
import workerUrl from "pdfjs-dist/build/pdf.worker.min.mjs?url";
import {
  PAGE_IMAGE_HEIGHT,
  PAGE_IMAGE_WIDTH,
  packBits,
  pageImageBytes,
  unpackBits,
} from "../../packages/qpk-format/src/format.ts";
import { ditherToBits } from "../device-client/src/photo.ts";

pdfjs.GlobalWorkerOptions.workerSrc = workerUrl;

type PdfDocument = Awaited<ReturnType<typeof pdfjs.getDocument>["promise"]>;
type PdfPage = Awaited<ReturnType<PdfDocument["getPage"]>>;

export interface RenderOptions {
  /** Crop each page to its printed content before scaling. Default true. */
  trimMargins?: boolean;
  onProgress?: (done: number, total: number) => void;
  /** Checked between pages; aborting rejects with a "cancelled" error. */
  signal?: AbortSignal;
  /**
   * Pages rendered at once. pdf.js parses in its own worker, so several pages
   * overlap usefully; each holds one large canvas, which is what caps it.
   * Default 4.
   */
  concurrency?: number;
}

export const RENDER_CANCELLED = "PDF rendering was cancelled";

/** Every page of the PDF, each a PackBits-compressed 480x800 1bpp bitmap, in page order. */
export async function renderPdfPages(data: Uint8Array, options: RenderOptions = {}): Promise<Uint8Array[]> {
  const doc = await pdfjs.getDocument({ data, isEvalSupported: false }).promise;
  try {
    const total = doc.numPages;
    const pages: Uint8Array[] = new Array<Uint8Array>(total);
    let next = 1;
    let done = 0;
    const lane = async () => {
      while (next <= total) {
        if (options.signal?.aborted) throw new Error(RENDER_CANCELLED);
        const n = next++;
        const page = await doc.getPage(n);
        pages[n - 1] = await renderPage(page, options.trimMargins ?? true);
        page.cleanup();
        done++;
        options.onProgress?.(done, total);
      }
    };
    const lanes = Math.max(1, Math.min(options.concurrency ?? 4, total));
    await Promise.all(Array.from({ length: lanes }, lane));
    return pages;
  } finally {
    await doc.destroy();
  }
}

function context(canvas: HTMLCanvasElement): CanvasRenderingContext2D {
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  if (!ctx) throw new Error("this webview has no 2D canvas");
  return ctx;
}

async function renderPage(page: PdfPage, trimMargins: boolean): Promise<Uint8Array> {
  const W = PAGE_IMAGE_WIDTH;
  const H = PAGE_IMAGE_HEIGHT;

  // Rendered well above the target size, so trimming still leaves detail to
  // scale down from and thin strokes average into grey instead of vanishing.
  const base = page.getViewport({ scale: 1 });
  const scale = 2.5 * Math.min(W / base.width, H / base.height);
  const viewport = page.getViewport({ scale });
  const source = document.createElement("canvas");
  source.width = Math.ceil(viewport.width);
  source.height = Math.ceil(viewport.height);
  const sourceContext = context(source);
  sourceContext.fillStyle = "#fff";
  sourceContext.fillRect(0, 0, source.width, source.height);
  await page.render({ canvasContext: sourceContext, viewport }).promise;

  const crop =
    (trimMargins ? contentBox(sourceContext, source.width, source.height) : null) ?? {
      x: 0,
      y: 0,
      w: source.width,
      h: source.height,
    };

  const target = document.createElement("canvas");
  target.width = W;
  target.height = H;
  const targetContext = context(target);
  targetContext.fillStyle = "#fff";
  targetContext.fillRect(0, 0, W, H);
  const fit = Math.min(W / crop.w, H / crop.h);
  const dw = crop.w * fit;
  const dh = crop.h * fit;
  targetContext.imageSmoothingEnabled = true;
  targetContext.imageSmoothingQuality = "high";
  targetContext.drawImage(source, crop.x, crop.y, crop.w, crop.h, (W - dw) / 2, (H - dh) / 2, dw, dh);

  const { data } = targetContext.getImageData(0, 0, W, H);
  return packBits(ditherToBits(data, W, H));
}

/** The bounding box of everything that is not near-white, padded a little; null for a blank page. */
function contentBox(
  ctx: CanvasRenderingContext2D,
  width: number,
  height: number,
): { x: number; y: number; w: number; h: number } | null {
  const { data } = ctx.getImageData(0, 0, width, height);
  let left = width;
  let top = height;
  let right = -1;
  let bottom = -1;
  for (let y = 0; y < height; y += 2) {
    for (let x = 0; x < width; x += 2) {
      const i = (y * width + x) * 4;
      if ((data[i] ?? 255) < 230 || (data[i + 1] ?? 255) < 230 || (data[i + 2] ?? 255) < 230) {
        if (x < left) left = x;
        if (x > right) right = x;
        if (y < top) top = y;
        if (y > bottom) bottom = y;
      }
    }
  }
  if (right < 0) return null;
  const pad = Math.round(Math.max(width, height) * 0.015);
  left = Math.max(0, left - pad);
  top = Math.max(0, top - pad);
  right = Math.min(width - 1, right + pad);
  bottom = Math.min(height - 1, bottom + pad);
  return { x: left, y: top, w: right - left + 1, h: bottom - top + 1 };
}

export function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

/** A page picture back as an image URL, for the Converter's preview. */
export function pageImageDataUrl(page: Uint8Array): string {
  const W = PAGE_IMAGE_WIDTH;
  const H = PAGE_IMAGE_HEIGHT;
  const bits = unpackBits(page, pageImageBytes(W, H));
  const canvas = document.createElement("canvas");
  canvas.width = W;
  canvas.height = H;
  const ctx = context(canvas);
  const image = ctx.createImageData(W, H);
  const stride = Math.ceil(W / 8);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const ink = ((bits[y * stride + (x >> 3)] ?? 0) >> (7 - (x & 7))) & 1;
      const at = (y * W + x) * 4;
      const value = ink ? 0 : 255;
      image.data[at] = value;
      image.data[at + 1] = value;
      image.data[at + 2] = value;
      image.data[at + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
  return canvas.toDataURL("image/png");
}
