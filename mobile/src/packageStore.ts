// The phone's own library: packages kept in the app's documents folder
// (`library/`), added from the system file picker or written by the
// Converter tab. Reading a package's header, title and cover is plain
// qpk-format in the app; full validation goes to the render worker.

import { Directory, File, Paths } from 'expo-file-system';

import {
  COVER_HEIGHT,
  COVER_WIDTH,
  MetadataKey,
  PackageType,
  SectionId,
  decodeCover,
  readPackage,
} from '@quran-device/qpk-format';

import { levelsPngDataUrl } from './png';

export interface LibraryPackage {
  file: File;
  name: string;
  size: number;
  title?: string;
  author?: string;
  language?: string;
  type?: string;
  contentVersion?: number;
  pageCount?: number;
  /** PNG data URL of the COVER section, when the package has one. */
  cover?: string;
  /** Content id and version (packageIdentity), for spotting the same package twice. */
  identity?: string;
  error?: string;
}

/**
 * A package's content id and version, read straight from the 64-byte header
 * (docs/qpk-format.md 4); null when the bytes are not a QPK1 package. Two files
 * with the same identity are the same package.
 */
export function packageIdentity(bytes: Uint8Array): string | null {
  if (bytes.length < 64 || bytes[0] !== 0x51 || bytes[1] !== 0x50 || bytes[2] !== 0x4b || bytes[3] !== 0x31) {
    return null;
  }
  let id = '';
  for (const byte of bytes.subarray(24, 40)) id += byte.toString(16).padStart(2, '0');
  const version = new DataView(bytes.buffer, bytes.byteOffset + 40, 4).getUint32(0, true);
  return `${id}@${version}`;
}

const TYPE_NAMES: Record<number, string> = {
  [PackageType.Quran]: 'QURAN',
  [PackageType.Book]: 'BOOK',
  [PackageType.Translation]: 'TRANSLATION',
  [PackageType.Tafsir]: 'TAFSIR',
};

export function libraryDirectory(): Directory {
  const directory = new Directory(Paths.document, 'library');
  directory.create({ idempotent: true });
  return directory;
}

export async function readBytes(file: File): Promise<Uint8Array> {
  return new Uint8Array(await file.arrayBuffer());
}

/** Header, metadata and cover -- no checksum sweep, so a list stays quick. */
export function describePackage(file: File, bytes: Uint8Array): LibraryPackage {
  const entry: LibraryPackage = { file, name: file.name, size: bytes.length };
  const identity = packageIdentity(bytes);
  if (identity) entry.identity = identity;
  try {
    const pkg = readPackage(bytes, { verifyChecksums: false });
    entry.type = TYPE_NAMES[pkg.header.packageType] ?? `type ${pkg.header.packageType}`;
    entry.contentVersion = pkg.header.contentVersion;
    const title = pkg.metadata(MetadataKey.Title);
    const author = pkg.metadata(MetadataKey.Author);
    const language = pkg.metadata(MetadataKey.Language);
    if (title) entry.title = title;
    if (author) entry.author = author;
    if (language) entry.language = language;
    const pages = pkg.section(SectionId.PageIndex);
    if (pages) entry.pageCount = pkg.recordCount(SectionId.PageIndex);
    const cover = pkg.section(SectionId.Cover);
    if (cover) {
      try {
        const levels = decodeCover(bytes.subarray(cover.offset, cover.offset + cover.length));
        entry.cover = levelsPngDataUrl(COVER_WIDTH, COVER_HEIGHT, levels);
      } catch {
        // A malformed cover is not worth refusing the package over.
      }
    }
  } catch (error) {
    entry.error = error instanceof Error ? error.message : String(error);
  }
  return entry;
}

export async function listLibrary(): Promise<LibraryPackage[]> {
  const entries: LibraryPackage[] = [];
  for (const item of libraryDirectory().list()) {
    if (!(item instanceof File) || !/\.qpk$/iu.test(item.name)) continue;
    entries.push(describePackage(item, await readBytes(item)));
  }
  return entries.sort((a, b) => (a.title ?? a.name).localeCompare(b.title ?? b.name));
}

/** A file name that does not clash with one already in the library. */
function freeName(directory: Directory, wanted: string): string {
  const base = wanted.replace(/[^\w.-]+/gu, '_').replace(/\.qpk$/iu, '') || 'package';
  let name = `${base}.qpk`;
  for (let n = 2; new File(directory, name).exists; n++) name = `${base}-${n}.qpk`;
  return name;
}

export function saveToLibrary(bytes: Uint8Array, wantedName: string): File {
  const directory = libraryDirectory();
  const file = new File(directory, freeName(directory, wantedName));
  file.create();
  file.write(bytes);
  return file;
}
