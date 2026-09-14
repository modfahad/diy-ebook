// The pipeline, end to end.
//
//   input -> detect kind -> parse -> structure -> layout -> indexes
//         -> QPK1 bytes -> validate -> preview
//
// Content type is auto-detected but always overridable, as the spec requires:
// "The desktop application should automatically determine whether the imported
// document is Quran or generic book. The user should also be able to manually
// select the content type."

import { parseEpub } from './sources/epub.js';
import { parsePdf } from './sources/pdf.js';
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

  const document = await parseDocument(data, kind, options);
  const result = buildBookPackage(document, options);
  return { ...result, document };
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
