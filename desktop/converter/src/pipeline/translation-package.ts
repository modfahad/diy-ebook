// TranslationSource -> QPK1 TRANSLATION package.
//
// Kept separate from QURAN packages (docs/qpk-format.md section 13's open
// question, resolved: "currently separate, joined by content_id in metadata
// key 9"). TRANSLATION_INDEX is addressed the same way AYAH_INDEX is --
// directly by ayah index in canonical order (surah ascending, then ayah) --
// so a translation package only makes sense read alongside the QURAN package
// whose ayah ordering it was built against; MetadataKey.AlignedContentId
// records which one that is.

import {
  MetadataKey,
  PackageType,
  SectionId,
  TextPool,
  encodeTranslation,
  writePackage,
} from '@quran-device/qpk-format';

import { deriveContentId, contentIdToHex } from './content-id.js';

export interface TranslationSourceAyah {
  surah: number;
  ayah: number;
  text: string;
}

export interface TranslationSourceMetadata {
  title: string;
  author?: string;
  language: string;
  source?: string;
}

export interface TranslationConversionOptions {
  /** The QURAN package's content id this translation aligns to (rule: MetadataKey.AlignedContentId). */
  alignedContentId: Uint8Array;
  contentId?: Uint8Array;
  contentVersion?: number;
  converterVersion?: string;
}

export interface TranslationConversionResult {
  bytes: Uint8Array;
  packageSize: number;
  ayahCount: number;
  warnings: string[];
}

export function buildTranslationPackage(
  ayahs: TranslationSourceAyah[],
  metadata: TranslationSourceMetadata,
  options: TranslationConversionOptions,
): TranslationConversionResult {
  const warnings: string[] = [];
  if (ayahs.length === 0) {
    throw new Error('translation source has no ayahs');
  }

  // Canonical order, matching how buildQuranPackage orders AYAH_INDEX: surah
  // ascending, then ayah ascending. TRANSLATION_INDEX must line up 1:1 with
  // the QURAN package's AYAH_INDEX by position, or a reader joining the two
  // by ayah index reads someone else's ayah.
  const sorted = [...ayahs].sort((a, b) => a.surah - b.surah || a.ayah - b.ayah);

  const text = new TextPool();
  const records = sorted.map((ayah) => {
    const ref = text.add(ayah.text);
    return encodeTranslation({ textOffset: ref.offset, textLength: ref.length });
  });

  const total = records.reduce((n, r) => n + r.length, 0);
  const payload = new Uint8Array(total);
  let at = 0;
  for (const record of records) {
    payload.set(record, at);
    at += record.length;
  }

  const metaEntries: Array<[MetadataKey, string]> = [
    [MetadataKey.Title, metadata.title],
    [MetadataKey.Language, metadata.language],
    [MetadataKey.BuildTimestamp, new Date().toISOString()],
    [MetadataKey.ConverterVersion, options.converterVersion ?? '0.1.0'],
    [MetadataKey.AlignedContentId, contentIdToHex(options.alignedContentId)],
  ];
  if (metadata.author) metaEntries.push([MetadataKey.Author, metadata.author]);
  if (metadata.source) metaEntries.push([MetadataKey.Source, metadata.source]);

  const contentId =
    options.contentId ??
    deriveContentId({
      type: PackageType.Translation,
      title: metadata.title,
      author: metadata.author,
      language: metadata.language,
    });

  const bytes = writePackage({
    type: PackageType.Translation,
    contentVersion: options.contentVersion ?? 1,
    contentId,
    metadata: metaEntries,
    sections: [
      { id: SectionId.TranslationIndex, payload, count: records.length },
      { id: SectionId.TranslationData, payload: text.bytes(), count: 0 },
    ],
  });

  return {
    bytes,
    packageSize: bytes.length,
    ayahCount: records.length,
    warnings,
  };
}
