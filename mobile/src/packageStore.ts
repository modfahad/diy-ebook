// The phone's own library: packages kept in the app's documents folder
// (`library/`), added from the system file picker or written by the
// Converter tab. Reading a package's header, title and cover is plain
// qpk-format in the app; full validation goes to the render worker.

import * as DocumentPicker from 'expo-document-picker';
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
  error?: string;
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

/** Copies packages the user picks into the library; returns how many. */
export async function addFromPicker(): Promise<number> {
  const result = await DocumentPicker.getDocumentAsync({ multiple: true, copyToCacheDirectory: true });
  if (result.canceled) return 0;
  let added = 0;
  for (const asset of result.assets) {
    const bytes = await readBytes(new File(asset.uri));
    saveToLibrary(bytes, asset.name);
    added++;
  }
  return added;
}
