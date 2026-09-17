// EPUB source.
//
// An EPUB is a ZIP holding XHTML. The reading order comes from the OPF spine,
// not from the ZIP's entry order, so the manifest has to be resolved properly
// -- reading entries as they happen to appear produces a scrambled book.
//
// Headings are taken from the markup (<h1>..<h6>) rather than guessed, which
// is the one real advantage EPUB has over PDF here.

import { XMLParser } from 'fast-xml-parser';
import { unzipSync } from 'fflate';

import type { ParsedDocument } from '../model.js';
import type { DocumentDetails } from './pdf.js';
import { DEFAULT_GRID, type Block, type GridOptions, paginateBlocks } from './paginate.js';

type ZipEntries = Map<string, Uint8Array>;

// fflate is pure JS, so EPUBs open the same way in Node and in the Android
// app (docs/android.md). An EPUB is small enough to unzip whole.
async function readZip(data: Uint8Array): Promise<ZipEntries> {
  let files: Record<string, Uint8Array>;
  try {
    files = unzipSync(data);
  } catch (error) {
    throw new Error(
      `could not open the EPUB archive: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
  const entries: ZipEntries = new Map();
  for (const [name, bytes] of Object.entries(files)) {
    if (!name.endsWith('/')) entries.set(name, bytes);
  }
  return entries;
}

const utf8 = new TextDecoder('utf-8');

const xml = new XMLParser({
  ignoreAttributes: false,
  attributeNamePrefix: '@',
  isArray: (name) => name === 'item' || name === 'itemref',
});

function resolveRelative(base: string, target: string): string {
  if (target.startsWith('/')) return target.slice(1);
  const baseDir = base.includes('/') ? base.slice(0, base.lastIndexOf('/')) : '';
  const parts = (baseDir ? `${baseDir}/${target}` : target).split('/');
  const out: string[] = [];
  for (const part of parts) {
    if (part === '.' || part === '') continue;
    if (part === '..') out.pop();
    else out.push(part);
  }
  return out.join('/');
}

/** Strips XHTML down to blocks, keeping heading levels. */
export function xhtmlToBlocks(html: string): Block[] {
  const body = /<body[^>]*>([\s\S]*?)<\/body>/iu.exec(html)?.[1] ?? html;
  const withoutInvisible = body
    .replace(/<script[\s\S]*?<\/script>/giu, '')
    .replace(/<style[\s\S]*?<\/style>/giu, '');

  const blocks: Block[] = [];
  // Split on block-level boundaries, remembering heading levels.
  const pattern = /<(h[1-6]|p|div|li|blockquote|section|article)\b[^>]*>([\s\S]*?)<\/\1>/giu;
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(withoutInvisible)) !== null) {
    const tag = (match[1] ?? '').toLowerCase();
    const inner = match[2] ?? '';
    // Nested block containers are handled by their own matches.
    if ((tag === 'div' || tag === 'section' || tag === 'article') && /<(h[1-6]|p|li)\b/iu.test(inner)) {
      continue;
    }
    const text = decodeEntities(inner.replace(/<[^>]+>/gu, ' ')).replace(/\s+/gu, ' ').trim();
    if (text.length === 0) continue;
    const level = /^h([1-6])$/u.exec(tag) ? Number(tag.slice(1)) : 0;
    blocks.push({ text, level });
  }

  if (blocks.length === 0) {
    const text = decodeEntities(withoutInvisible.replace(/<[^>]+>/gu, ' '))
      .replace(/\s+/gu, ' ')
      .trim();
    if (text.length > 0) blocks.push({ text, level: 0 });
  }
  return blocks;
}

function decodeEntities(text: string): string {
  return text
    .replace(/&#x([0-9a-f]+);/giu, (_, hex: string) => String.fromCodePoint(parseInt(hex, 16)))
    .replace(/&#(\d+);/gu, (_, dec: string) => String.fromCodePoint(Number(dec)))
    .replace(/&lt;/gu, '<')
    .replace(/&gt;/gu, '>')
    .replace(/&quot;/gu, '"')
    .replace(/&apos;/gu, "'")
    .replace(/&nbsp;/gu, ' ')
    .replace(/&amp;/gu, '&');
}

export interface EpubParseOptions {
  grid?: GridOptions;
}

/**
 * An EPUB's title, author and language from its OPF alone -- no chapter is
 * read, so this is quick enough to run as soon as a book is picked.
 */
export async function readEpubDetails(data: Uint8Array): Promise<DocumentDetails> {
  const { pkg } = openPackage(await readZip(data));
  const meta = pkg.metadata ?? {};
  const details: DocumentDetails = {};
  const title = firstString(meta['dc:title'] ?? meta['title']);
  const author = firstString(meta['dc:creator'] ?? meta['creator']);
  const language = firstString(meta['dc:language'] ?? meta['language']);
  if (title) details.title = title;
  if (author) details.author = author;
  if (language) details.language = language;
  return details;
}

/** The OPF package document an EPUB's container points at, parsed. */
// The parser's output is untyped XML, read defensively by every caller.
function openPackage(entries: ZipEntries): { rootPath: string; pkg: any } {
  const containerRaw = entries.get('META-INF/container.xml');
  if (!containerRaw) {
    throw new Error('not an EPUB: META-INF/container.xml is missing');
  }
  const container = xml.parse(utf8.decode(containerRaw));
  const rootPath: string | undefined =
    container?.container?.rootfiles?.rootfile?.['@full-path'];
  if (!rootPath) {
    throw new Error('not an EPUB: the container names no OPF root file');
  }
  const opfRaw = entries.get(rootPath);
  if (!opfRaw) throw new Error(`EPUB is missing its OPF at ${rootPath}`);
  const opf = xml.parse(utf8.decode(opfRaw));
  return { rootPath, pkg: opf?.package ?? {} };
}

export async function parseEpub(
  data: Uint8Array,
  options: EpubParseOptions = {},
): Promise<ParsedDocument> {
  const entries = await readZip(data);
  const warnings: string[] = [];

  const { rootPath, pkg } = openPackage(entries);

  const meta = pkg.metadata ?? {};
  const title = firstString(meta['dc:title'] ?? meta['title']);
  const author = firstString(meta['dc:creator'] ?? meta['creator']);
  const language = firstString(meta['dc:language'] ?? meta['language']);

  const manifest = new Map<string, string>();
  for (const item of pkg.manifest?.item ?? []) {
    const id = item?.['@id'];
    const href = item?.['@href'];
    if (typeof id === 'string' && typeof href === 'string') {
      manifest.set(id, resolveRelative(rootPath, href));
    }
  }

  const spine: string[] = [];
  for (const ref of pkg.spine?.itemref ?? []) {
    const idref = ref?.['@idref'];
    if (typeof idref === 'string') {
      const href = manifest.get(idref);
      if (href) spine.push(href);
      else warnings.push(`spine references unknown manifest id "${idref}"`);
    }
  }
  if (spine.length === 0) {
    throw new Error('EPUB spine is empty: there is no reading order to follow');
  }

  const blocks: Block[] = [];
  for (const href of spine) {
    const raw = entries.get(href);
    if (!raw) {
      warnings.push(`spine item "${href}" is missing from the archive`);
      continue;
    }
    blocks.push(...xhtmlToBlocks(utf8.decode(raw)));
  }

  if (blocks.length === 0) warnings.push('no readable text found in the spine');

  return {
    source: 'epub',
    title,
    author,
    language,
    pages: paginateBlocks(blocks, options.grid ?? DEFAULT_GRID),
    warnings,
  };
}

export interface EpubCover {
  /** The picture's bytes exactly as stored in the archive (JPEG, PNG, ...). */
  bytes: Uint8Array;
  mediaType: string;
}

/**
 * The EPUB's cover picture, or null when it declares none. EPUB 3 marks it
 * with a manifest item's `properties="cover-image"`, EPUB 2 with
 * `<meta name="cover" content="<manifest id>">`; failing both, an image item
 * whose id says "cover" is the common convention. Decoding is left to the
 * caller -- the desktop app's webview -- since nothing here can decode images.
 */
export async function extractEpubCover(data: Uint8Array): Promise<EpubCover | null> {
  const entries = await readZip(data);
  const containerRaw = entries.get('META-INF/container.xml');
  if (!containerRaw) return null;
  const rootPath: unknown = xml.parse(utf8.decode(containerRaw))?.container?.rootfiles
    ?.rootfile?.['@full-path'];
  if (typeof rootPath !== 'string') return null;
  const opfRaw = entries.get(rootPath);
  if (!opfRaw) return null;
  const pkg = xml.parse(utf8.decode(opfRaw))?.package ?? {};

  const items: Array<Record<string, unknown>> = Array.isArray(pkg.manifest?.item)
    ? pkg.manifest.item
    : [];
  const text = (item: Record<string, unknown> | undefined, key: string): string =>
    typeof item?.[key] === 'string' ? (item[key] as string) : '';
  const isImage = (item: Record<string, unknown> | undefined): boolean =>
    text(item, '@media-type').startsWith('image/');

  let item = items.find((candidate) =>
    text(candidate, '@properties').split(/\s+/u).includes('cover-image'),
  );
  if (!item) {
    const metas: unknown = pkg.metadata?.meta;
    const list = (Array.isArray(metas) ? metas : metas ? [metas] : []) as Array<
      Record<string, unknown>
    >;
    const coverId = text(list.find((meta) => text(meta, '@name') === 'cover'), '@content');
    if (coverId !== '') item = items.find((candidate) => text(candidate, '@id') === coverId);
  }
  if (!item || !isImage(item)) {
    item = items.find((candidate) => isImage(candidate) && /cover/iu.test(text(candidate, '@id')));
  }
  if (!item || !isImage(item)) return null;

  const href = text(item, '@href');
  if (href === '') return null;
  const bytes =
    entries.get(resolveRelative(rootPath, href)) ??
    entries.get(resolveRelative(rootPath, decodeURIComponent(href)));
  return bytes ? { bytes: new Uint8Array(bytes), mediaType: text(item, '@media-type') } : null;
}

function firstString(value: unknown): string | undefined {
  if (typeof value === 'string') return value.trim() || undefined;
  if (typeof value === 'number') return String(value);
  if (Array.isArray(value)) return firstString(value[0]);
  if (value && typeof value === 'object') {
    const text = (value as Record<string, unknown>)['#text'];
    if (typeof text === 'string') return text.trim() || undefined;
  }
  return undefined;
}
