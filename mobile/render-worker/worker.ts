// The render worker: a hidden WebView page doing the work that needs a
// browser (docs/android.md).
//
// React Native has no canvas and cannot run pdf.js, so this page -- bundled
// by scripts/build-render-worker.mjs into src/render/workerHtml.generated.ts
// -- decodes pictures, renders PDF pages, and runs the whole converter
// (TXT, EPUB and PDF) exactly as the desktop's bridge does. The app side is
// src/render/RenderWorker.tsx.
//
// Messages are JSON strings. Big byte arrays go as base64 and are split into
// pieces both ways, so no single injectJavaScript or postMessage is huge:
//   app -> page:  __renderWorker.put(key, piece)   then
//                 __renderWorker.run({ id, command, args })
//   page -> app:  { id, kind: 'progress', progress }
//                 { id, kind: 'piece', piece }          (zero or more)
//                 { id, kind: 'done', result } | { id, kind: 'error', message }
// A result whose `bytesPieces` flag is set had its `bytes` field sent as the
// pieces before it.

import * as pdfjs from 'pdfjs-dist/legacy/build/pdf.mjs';
import * as pdfjsWorker from 'pdfjs-dist/legacy/build/pdf.worker.mjs';

import {
  convertAndValidate,
  extractEpubCover,
  previewPage,
  validatePackage,
  type ConvertOptions,
} from '@quran-device/converter';
import {
  COVER_HEIGHT,
  COVER_WIDTH,
  PAGE_IMAGE_HEIGHT,
  PAGE_IMAGE_WIDTH,
  encodeCover,
  packBits,
} from '@quran-device/qpk-format';
import {
  PHOTO_HEIGHT,
  PHOTO_WIDTH,
  coverCrop,
  ditherToBits,
  ditherToLevels,
} from '@quran-device/device-client';

// pdf.js's worker runs on this page's own thread: one fewer file to ship,
// the same arrangement as the desktop's standalone bridge.
(globalThis as unknown as { pdfjsWorker: unknown }).pdfjsWorker = pdfjsWorker;

declare global {
  interface Window {
    ReactNativeWebView?: { postMessage(message: string): void };
    __renderWorker: {
      put(key: string, piece: string): void;
      run(request: WorkerRequest): void;
    };
  }
}

interface WorkerRequest {
  id: number;
  command: string;
  args: Record<string, unknown>;
}

const PIECE_CHARS = 512 * 1024;

// --- base64 -------------------------------------------------------------------

function toBase64(bytes: Uint8Array): string {
  let binary = '';
  for (let i = 0; i < bytes.length; i += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(binary);
}

function fromBase64(text: string): Uint8Array {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

// --- messaging -----------------------------------------------------------------

function send(message: unknown): void {
  window.ReactNativeWebView?.postMessage(JSON.stringify(message));
}

const incoming = new Map<string, string[]>();
/** Bytes kept between commands, so a page turn does not resend a 20 MB PDF. */
const slots = new Map<string, Uint8Array>();

function takeBytes(key: unknown): Uint8Array {
  if (typeof key !== 'string') throw new Error('a bytes key is required');
  const pieces = incoming.get(key);
  if (pieces) {
    incoming.delete(key);
    return fromBase64(pieces.join(''));
  }
  const kept = slots.get(key);
  if (kept) return kept;
  throw new Error(`nothing was sent under "${key}"`);
}

function sendBytesThenDone(id: number, bytes: Uint8Array, rest: Record<string, unknown>): void {
  const text = toBase64(bytes);
  for (let at = 0; at < text.length; at += PIECE_CHARS) {
    send({ id, kind: 'piece', piece: text.slice(at, at + PIECE_CHARS) });
  }
  send({ id, kind: 'done', result: { ...rest, bytesPieces: true } });
}

// --- pictures --------------------------------------------------------------------

function canvas2d(width: number, height: number): [HTMLCanvasElement, CanvasRenderingContext2D] {
  const canvas = document.createElement('canvas');
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext('2d', { willReadFrequently: true });
  if (!context) throw new Error('this WebView has no 2D canvas');
  context.fillStyle = '#fff';
  context.fillRect(0, 0, width, height);
  context.imageSmoothingEnabled = true;
  context.imageSmoothingQuality = 'high';
  return [canvas, context];
}

/** A picture cropped from its centre to width x height's shape and scaled, as RGBA. */
async function pictureRgba(
  bytes: Uint8Array,
  mediaType: string,
  width: number,
  height: number,
): Promise<Uint8ClampedArray> {
  const bitmap = await createImageBitmap(new Blob([bytes.slice().buffer], { type: mediaType || 'image/*' }));
  try {
    const target = width / height;
    let crop = { sx: 0, sy: 0, sw: bitmap.width, sh: bitmap.height };
    if (width === PHOTO_WIDTH && height === PHOTO_HEIGHT) {
      crop = coverCrop(bitmap.width, bitmap.height);
    } else if (bitmap.width / bitmap.height > target) {
      const sw = Math.max(1, Math.round(bitmap.height * target));
      crop = { sx: Math.floor((bitmap.width - sw) / 2), sy: 0, sw, sh: bitmap.height };
    } else {
      const sh = Math.max(1, Math.round(bitmap.width / target));
      crop = { sx: 0, sy: Math.floor((bitmap.height - sh) / 2), sw: bitmap.width, sh };
    }
    const [, context] = canvas2d(width, height);
    context.drawImage(bitmap, crop.sx, crop.sy, crop.sw, crop.sh, 0, 0, width, height);
    return context.getImageData(0, 0, width, height).data;
  } finally {
    bitmap.close();
  }
}

// --- PDF page pictures (mirrors desktop/src/pdfPages.ts) ---------------------------

type PdfDocument = Awaited<ReturnType<typeof pdfjs.getDocument>['promise']>;
type PdfPage = Awaited<ReturnType<PdfDocument['getPage']>>;

async function renderPdfPages(
  data: Uint8Array,
  trimMargins: boolean,
  onProgress: (done: number, total: number) => void,
): Promise<Uint8Array[]> {
  // pdf.js takes ownership of the buffer it is given; keep the caller's intact.
  const doc = await pdfjs.getDocument({ data: data.slice(), isEvalSupported: false }).promise;
  try {
    const total = doc.numPages;
    const pages = new Array<Uint8Array>(total);
    let next = 1;
    let done = 0;
    // Two at a time: a phone has less memory for the large source canvases.
    const lane = async () => {
      while (next <= total) {
        const n = next++;
        const page = await doc.getPage(n);
        pages[n - 1] = await renderPage(page, trimMargins);
        page.cleanup();
        onProgress(++done, total);
      }
    };
    await Promise.all(Array.from({ length: Math.min(2, total) }, lane));
    return pages;
  } finally {
    await doc.destroy();
  }
}

async function renderPage(page: PdfPage, trimMargins: boolean): Promise<Uint8Array> {
  const W = PAGE_IMAGE_WIDTH;
  const H = PAGE_IMAGE_HEIGHT;
  const base = page.getViewport({ scale: 1 });
  const viewport = page.getViewport({ scale: 2.5 * Math.min(W / base.width, H / base.height) });
  const [source, sourceContext] = canvas2d(Math.ceil(viewport.width), Math.ceil(viewport.height));
  await page.render({ canvasContext: sourceContext, viewport }).promise;

  const crop = (trimMargins ? contentBox(sourceContext, source.width, source.height) : null) ?? {
    x: 0,
    y: 0,
    w: source.width,
    h: source.height,
  };
  const [, target] = canvas2d(W, H);
  const fit = Math.min(W / crop.w, H / crop.h);
  const dw = crop.w * fit;
  const dh = crop.h * fit;
  target.drawImage(source, crop.x, crop.y, crop.w, crop.h, (W - dw) / 2, (H - dh) / 2, dw, dh);
  source.width = 0; // release the large canvas now, not at garbage collection
  return packBits(ditherToBits(target.getImageData(0, 0, W, H).data, W, H));
}

function contentBox(
  context: CanvasRenderingContext2D,
  width: number,
  height: number,
): { x: number; y: number; w: number; h: number } | null {
  const { data } = context.getImageData(0, 0, width, height);
  let left = width;
  let top = height;
  let right = -1;
  let bottom = -1;
  for (let y = 0; y < height; y += 2) {
    for (let x = 0; x < width; x += 2) {
      const i = (y * width + x) * 4;
      if (data[i]! < 230 || data[i + 1]! < 230 || data[i + 2]! < 230) {
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

// --- commands ----------------------------------------------------------------------

type Handler = (args: Record<string, unknown>, id: number) => Promise<unknown>;

function isPdf(bytes: Uint8Array): boolean {
  return bytes[0] === 0x25 && bytes[1] === 0x50 && bytes[2] === 0x44 && bytes[3] === 0x46;
}

const COMMANDS: Record<string, Handler> = {
  async ping() {
    return { pdfjs: pdfjs.version, canvas: Boolean(document.createElement('canvas').getContext('2d')) };
  },

  /** Keep bytes under a name for later commands (e.g. the converter's input). */
  async keep(args) {
    const bytes = takeBytes(args.key);
    slots.set(String(args.slot), bytes);
    return { slot: args.slot, bytes: bytes.length };
  },

  /** A home-screen photo's RGBA (400x480, centre crop); the app dithers it. */
  async photoRgba(args) {
    const rgba = await pictureRgba(takeBytes(args.key), String(args.mediaType ?? ''), PHOTO_WIDTH, PHOTO_HEIGHT);
    return { rgba: toBase64(new Uint8Array(rgba.buffer, rgba.byteOffset, rgba.byteLength)) };
  },

  /** A picture as cover levels (108x144, four greys). */
  async coverFromPicture(args) {
    const rgba = await pictureRgba(takeBytes(args.key), String(args.mediaType ?? ''), COVER_WIDTH, COVER_HEIGHT);
    const levels = ditherToLevels(rgba, COVER_WIDTH, COVER_HEIGHT, { contrast: 0.15 });
    return { levels: toBase64(levels) };
  },

  /** The cover an EPUB declares, as cover levels; null when it has none. */
  async epubCover(args) {
    const cover = await extractEpubCover(takeBytes(args.key));
    if (!cover) return null;
    const rgba = await pictureRgba(cover.bytes, cover.mediaType, COVER_WIDTH, COVER_HEIGHT);
    return { levels: toBase64(ditherToLevels(rgba, COVER_WIDTH, COVER_HEIGHT, { contrast: 0.15 })) };
  },

  /**
   * Convert, validate, preview -- the desktop bridge's convert command. The
   * finished package is kept in the "package" slot for later page previews
   * and sent back as bytes.
   */
  async convert(args, id) {
    const data = takeBytes(args.key);
    const options = (args.options ?? {}) as ConvertOptions & Record<string, unknown>;
    const convertOptions: ConvertOptions = { ...options };
    if (typeof options.cover === 'string') {
      convertOptions.cover = encodeCover(fromBase64(options.cover as unknown as string));
    }
    if (args.keepLayout === true && isPdf(data)) {
      const pages = await renderPdfPages(data, args.trimMargins !== false, (done, total) =>
        send({ id, kind: 'progress', progress: { stage: 'rendering', done, total } }),
      );
      convertOptions.pageImages = pages;
      // A picture book's reader never uses word coordinates.
      convertOptions.includeWordLayout = false;
    }
    send({ id, kind: 'progress', progress: { stage: 'converting' } });
    const result = await convertAndValidate(data, convertOptions);
    let preview: unknown = null;
    if (result.validation.ok && result.report.pageCount > 0) {
      const page = Math.min(Math.max(Number(args.previewPage) || 1, 1), result.report.pageCount);
      try {
        preview = previewPage(result.bytes, page);
      } catch (error) {
        preview = { error: error instanceof Error ? error.message : String(error) };
      }
      slots.set('package', result.bytes);
    }
    sendBytesThenDone(id, result.bytes, { report: result.report, validation: result.validation, preview });
    return undefined;
  },

  /** One page of a package: its text preview, plus the page picture if it has one. */
  async preview(args) {
    return previewPage(takeBytes(args.key ?? 'package'), Math.max(1, Number(args.page) || 1));
  },

  async validate(args) {
    return validatePackage(takeBytes(args.key));
  },
};

window.__renderWorker = {
  put(key, piece) {
    const pieces = incoming.get(key) ?? [];
    pieces.push(piece);
    incoming.set(key, pieces);
  },
  run(request) {
    const handler = COMMANDS[request.command];
    if (!handler) {
      send({ id: request.id, kind: 'error', message: `unknown command "${request.command}"` });
      return;
    }
    handler(request.args ?? {}, request.id).then(
      (result) => {
        if (result !== undefined) send({ id: request.id, kind: 'done', result });
      },
      (error: unknown) =>
        send({ id: request.id, kind: 'error', message: error instanceof Error ? error.message : String(error) }),
    );
  },
};

send({ id: 0, kind: 'ready' });
