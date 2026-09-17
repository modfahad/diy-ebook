// The pipeline, end to end.
//
//   input -> detect kind -> parse -> structure -> layout -> indexes
//         -> QPK1 bytes -> validate -> preview
//
// Content type is auto-detected but always overridable, as the spec requires:
// "The desktop application should automatically determine whether the imported
// document is Quran or generic book. The user should also be able to manually
// select the content type."

import { parseEpub, readEpubDetails } from './sources/epub.js';
import { parsePdf, readPdfDetails, type DocumentDetails } from './sources/pdf.js';
import { parseTxt } from './sources/txt.js';
import { buildBookPackage, type BookConversionOptions } from './pipeline/book-package.js';
import { buildQuranPackage, type QuranConversionOptions } from './pipeline/quran-package.js';
import { validatePackage } from './pipeline/validate.js';
import type { ConversionResult } from './pipeline/report.js';
import type { ParsedDocument } from './model.js';
import { validateQuranSource, type QuranSource } from './quran/schema.js';

export type InputKind = 'txt' | 'pdf' | 'epub' | 'quran-json';

/**
 * Sniffs the input format from its bytes, falling back to the file extension.
 * Magic numbers first: a mislabelled extension is common, a forged PDF header
 * is not.
 */
export function detectInputKind(data: Uint8Array, filename?: string): InputKind {
  if (startsWith(data, [0x25, 0x50, 0x44, 0x46])) return 'pdf'; // %PDF
  if (startsWith(data, [0x50, 0x4b, 0x03, 0x04])) return 'epub'; // ZIP

  const head = new TextDecoder('utf-8', { fatal: false })
    .decode(data.subarray(0, 512))
    .trimStart();
  if (head.startsWith('{') && /"schemaVersion"\s*:/u.test(head)) return 'quran-json';

  const extension = filename?.toLowerCase().split('.').pop();
  if (extension === 'pdf') return 'pdf';
  if (extension === 'epub') return 'epub';
  if (extension === 'json') return 'quran-json';
  return 'txt';
}

function startsWith(data: Uint8Array, bytes: number[]): boolean {
  if (data.length < bytes.length) return false;
  return bytes.every((byte, i) => data[i] === byte);
}

export interface ConvertOptions extends BookConversionOptions, QuranConversionOptions {
  /** Override the sniffed input format. */
  inputKind?: InputKind;
  /**
   * Force the output content type. Quran output is only possible from a
   * structured Quran source -- see src/quran/schema.ts for why.
   */
  contentType?: 'BOOK' | 'QURAN';
  filename?: string;
}

export interface FullConversion extends ConversionResult {
  /** The intermediate document, for a source-side preview. Absent for Quran JSON. */
  document?: ParsedDocument;
}

export async function parseDocument(
  data: Uint8Array,
  kind: InputKind,
  options: ConvertOptions = {},
): Promise<ParsedDocument> {
  switch (kind) {
    case 'pdf':
      return parsePdf(data);
    case 'epub':
      return parseEpub(data);
    case 'txt':
      return parseTxt(new TextDecoder().decode(data), {
        ...(options.title ? { title: options.title } : {}),
        ...(options.author ? { author: options.author } : {}),
        ...(options.language ? { language: options.language } : {}),
      });
    case 'quran-json':
      throw new Error('a Quran JSON source is not a paginated document');
    default:
      throw new Error(`unsupported input kind "${String(kind)}"`);
  }
}

export async function convert(
  data: Uint8Array,
  options: ConvertOptions = {},
): Promise<FullConversion> {
  const kind = options.inputKind ?? detectInputKind(data, options.filename);

  if (kind === 'quran-json') {
    if (options.contentType === 'BOOK') {
      throw new Error(
        'a Quran JSON source cannot be converted to a BOOK package; ' +
          'drop the contentType override',
      );
    }
    const source = JSON.parse(new TextDecoder().decode(data)) as QuranSource;
    return buildQuranPackage(source, options);
  }

  if (options.contentType === 'QURAN') {
    throw new Error(
      'Quran packages are only built from a structured Quran JSON source. ' +
        'Extracting Quranic text from a PDF with layout heuristics risks corrupting ' +
        'scripture; see desktop/converter/src/quran/schema.ts.',
    );
  }

  const parsed = await parseDocument(data, kind, options);
  const fallback = options.title || parsed.title ? undefined : titleFromFilename(options.filename);
  const document = fallback ? { ...parsed, title: fallback } : parsed;
  const result = buildBookPackage(document, options);
  return { ...result, document };
}

/**
 * The details a picked file will be converted with unless someone changes
 * them: what the document says about itself, and for the title, the file name
 * when it says nothing -- exactly what `convert` falls back to. Reads metadata
 * only (a PDF's info, an EPUB's OPF), never the pages, so it is quick enough
 * to run as soon as a book is chosen. `untitled` marks a title that came from
 * the file name, so a form can ask for a real one.
 */
export async function readDocumentDetails(
  data: Uint8Array,
  filename?: string,
): Promise<DocumentDetails & { kind: InputKind; untitled?: true }> {
  const kind = detectInputKind(data, filename);
  let details: DocumentDetails = {};
  if (kind === 'pdf') details = await readPdfDetails(data);
  else if (kind === 'epub') details = await readEpubDetails(data);
  const title = details.title ?? titleFromFilename(filename);
  return { kind, ...details, ...(title ? { title } : {}), ...(details.title ? {} : { untitled: true as const }) };
}

/**
 * The title a book gets when neither the caller nor the document names one.
 * Without it every such book is "Untitled", and since the title is part of the
 * content id (content-id.ts), two untitled books would share one id -- the
 * second would replace the first on the device.
 */
export function titleFromFilename(filename: string | undefined): string | undefined {
  const stem = (filename ?? '')
    .replace(/^.*[\\/]/u, '')
    .replace(/\.[^.]+$/u, '')
    .replace(/[_\s]+/gu, ' ')
    .trim();
  return stem === '' ? undefined : stem;
}

/** Convert, then validate what came out. Nothing installs without this. */
export async function convertAndValidate(
  data: Uint8Array,
  options: ConvertOptions = {},
): Promise<FullConversion & { validation: ReturnType<typeof validatePackage> }> {
  const result = await convert(data, options);
  const validation = validatePackage(result.bytes);
  return { ...result, validation };
}

export { validateQuranSource };
