// Adding many books to the library at once.
//
// One file at a time, and each the way the Converter tab would do it: a .qpk
// is copied as it is; a PDF, EPUB or TXT is converted, validated and written
// straight into the library folder, with whatever details were filled in for
// it. A detail left empty is not sent, so the document's own title, author and
// language still apply -- and a book with no title of its own is titled from
// its file name by the converter.

import { convert, coverSource, importPackage, libraryOutputPath, readInputFile } from "./bridge";
import { base64ToBytes, coverPayloadBase64, pictureToCover } from "./cover";
import { bytesToBase64, renderPdfPages } from "./pdfPages";
import { baseName } from "./format";

/** What the file picker offers. Quran JSON stays in the Converter tab. */
export const BOOK_EXTENSIONS = ["qpk", "pdf", "epub", "txt"];

/** What a person can set for one book before it is converted. */
export interface BookDetails {
  title: string;
  author: string;
  language: string;
  /** Cover levels from a chosen picture; without one an EPUB's own cover is used. */
  cover: Uint8Array | null;
}

export interface AddOptions {
  /** PDFs as page pictures, as the Converter tab's "Keep the PDF's page layout". */
  keepPdfLayout: boolean;
  details?: BookDetails;
  signal?: AbortSignal;
  onStage?: (stage: string) => void;
}

export interface AddResult {
  path: string;
  /** False when the package was already in the library and nothing was written. */
  added: boolean;
}

export function bookKind(path: string): string {
  return /\.([^.\\/]+)$/u.exec(path)?.[1]?.toLowerCase() ?? "";
}

export async function addToLibrary(
  source: string,
  libraryDir: string,
  { keepPdfLayout, details, signal, onStage }: AddOptions,
): Promise<AddResult> {
  const kind = bookKind(source);
  if (kind === "qpk") {
    onStage?.("copying");
    const result = await importPackage(source, libraryDir);
    return { path: result.path, added: result.copied };
  }
  if (!BOOK_EXTENSIONS.includes(kind)) {
    throw new Error(`.${kind} files are not books this app can convert`);
  }

  // A chosen picture wins; otherwise an EPUB's own cover, when it declares
  // one. A cover that cannot be used is not worth failing the book over: the
  // device draws one instead.
  let cover = details?.cover ? coverPayloadBase64(details.cover) : undefined;
  if (!cover && kind === "epub") {
    onStage?.("reading the cover");
    try {
      const found = await coverSource(source);
      if (found) {
        const bytes = base64ToBytes(found.data);
        cover = coverPayloadBase64(
          await pictureToCover(new Blob([bytes.buffer as ArrayBuffer], { type: found.mediaType })),
        );
      }
    } catch {
      cover = undefined;
    }
  }

  let pageImages: string[] | undefined;
  if (kind === "pdf" && keepPdfLayout) {
    onStage?.("reading the PDF");
    const file = await readInputFile(source);
    const pages = await renderPdfPages(base64ToBytes(file.data), {
      trimMargins: true,
      signal,
      onProgress: (done, total) => onStage?.(`rendering page ${done} of ${total}`),
    });
    pageImages = pages.map(bytesToBase64);
  }

  const title = details?.title.trim() ?? "";
  const author = details?.author.trim() ?? "";
  const language = details?.language.trim() ?? "";
  const stem = title !== "" ? title : baseName(source).replace(/\.[^.]+$/u, "");
  const output = await libraryOutputPath(libraryDir, "BOOK", stem);
  const result = await convert(
    {
      input: source,
      output,
      dryRun: false,
      ...(title !== "" ? { title } : {}),
      ...(author !== "" ? { author } : {}),
      ...(language !== "" ? { language } : {}),
      ...(cover ? { cover } : {}),
      // A picture book's reader never uses word coordinates (see Converter.tsx).
      ...(pageImages ? { pageImages, includeWordLayout: false } : {}),
    },
    (progress) => onStage?.(String(progress.stage)),
  );
  return { path: result.output, added: true };
}
