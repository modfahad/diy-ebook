// Adding many books to the phone's library at once -- the Android twin of
// desktop/src/addBooks.ts.
//
// One file at a time, each the way the Converter tab would do it with its
// defaults: a .qpk is copied as it is; a PDF, EPUB or TXT is converted and
// validated in the render worker and saved only if it passes. A package whose
// content id and version are already in the library is not saved again --
// content ids come from the content, so converting the same book twice is
// caught too.

import * as DocumentPicker from 'expo-document-picker';
import { File } from 'expo-file-system';

import { packageIdentity, readBytes, saveToLibrary } from './packageStore';
import type { RenderWorker, WorkerProgress } from './render/RenderWorker';

export const BOOK_EXTENSIONS = ['qpk', 'pdf', 'epub', 'txt'];

export interface PickedBook {
  name: string;
  uri: string;
}

export interface AddResult {
  /** The library file name, or the name of the copy already there. */
  name: string;
  /** False when the same package was already in the library. */
  added: boolean;
}

interface ConvertReply {
  bytes: Uint8Array;
  validation: { ok: boolean; errors: string[] };
}

/**
 * Lets the user pick any number of files. No MIME filter: Android has no type
 * for .qpk, so filtering would hide the packages; unsupported files are
 * reported per file instead.
 */
export async function pickBooks(): Promise<PickedBook[]> {
  const result = await DocumentPicker.getDocumentAsync({ multiple: true, copyToCacheDirectory: true });
  if (result.canceled) return [];
  return result.assets.map((asset) => ({ name: asset.name, uri: asset.uri }));
}

function extension(name: string): string {
  return /\.([^.]+)$/u.exec(name)?.[1]?.toLowerCase() ?? '';
}

function describeProgress(progress: WorkerProgress): string {
  if (progress.stage === 'rendering' && progress.total) {
    return `rendering page ${progress.done} of ${progress.total}`;
  }
  return progress.stage;
}

/** `known` holds the identities already in the library; it is updated as books are added. */
export async function addToLibrary(
  book: PickedBook,
  worker: RenderWorker,
  known: Set<string>,
  { keepPdfLayout, onStage }: { keepPdfLayout: boolean; onStage?: (stage: string) => void },
): Promise<AddResult> {
  const kind = extension(book.name);
  if (!BOOK_EXTENSIONS.includes(kind)) {
    throw new Error(`${kind ? `.${kind}` : 'this'} file is not a book the app can convert`);
  }
  onStage?.('reading');
  const source = await readBytes(new File(book.uri));
  const stem = book.name.replace(/\.[^.]+$/u, '').slice(0, 60);

  let bytes = source;
  if (kind !== 'qpk') {
    const options: Record<string, unknown> = { filename: book.name };
    if (kind === 'epub') {
      // A cover that cannot be used is not worth failing the book over.
      onStage?.('reading the cover');
      try {
        const found = await worker.call<{ levels: string } | null>('epubCover', {}, { bytes: { key: source } });
        if (found) options.cover = found.levels;
      } catch {
        // the device draws a cover instead
      }
    }
    const reply = await worker.call<ConvertReply>(
      'convert',
      { options, keepLayout: kind === 'pdf' && keepPdfLayout, trimMargins: true, previewPage: 1 },
      { bytes: { key: source }, onProgress: (progress) => onStage?.(describeProgress(progress)) },
    );
    if (!reply.validation.ok) {
      throw new Error(`does not validate: ${reply.validation.errors.join('; ')}`);
    }
    bytes = reply.bytes;
  }

  const identity = packageIdentity(bytes);
  if (!identity) throw new Error('not a QPK package');
  if (known.has(identity)) return { name: stem, added: false };
  const file = saveToLibrary(bytes, `${stem}.qpk`);
  known.add(identity);
  return { name: file.name, added: true };
}
