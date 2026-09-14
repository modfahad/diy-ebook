// Validation and preview of an emitted package.
//
// The spec requires both before installing to a device. Validation is not a
// formality here: the desktop is the last place a bad package can be caught
// cheaply, and it checks strictly more than the device does.

import {
  FLAG_HAS_WORD_LAYOUT,
  MetadataKey,
  PackageType,
  QpkError,
  SectionId,
  decodeCover,
  decodePageImageRecord,
  packageTypeName,
  pageImageBytes,
  unpackBits,
  readPackage,
  sectionName,
  type QpkPackage,
} from '@quran-device/qpk-format';

export interface ValidationResult {
  ok: boolean;
  /** Why it failed, in the format's own error vocabulary. */
  errors: string[];
  warnings: string[];
  summary?: PackageSummary;
}

export interface PackageSummary {
  type: string;
  packageSize: number;
  contentVersion: number;
  contentId: string;
  metadata: Record<string, string>;
  sections: Array<{ id: number; name: string; length: number; records: number }>;
  pageCount: number;
  chapterCount: number;
  ayahCount: number;
  wordCount: number;
  hasWordLayout: boolean;
}

export function validatePackage(bytes: Uint8Array): ValidationResult {
  const errors: string[] = [];
  const warnings: string[] = [];

  let pkg: QpkPackage;
  try {
    // Desktop profile: every checksum, full index sweep.
    pkg = readPackage(bytes);
  } catch (error) {
    return {
      ok: false,
      errors: [error instanceof QpkError ? error.message : String(error)],
      warnings,
    };
  }

  // Device profile: what the firmware actually does at open. A package that
  // passes the strict check but fails this one would install and then be
  // rejected on the device, which is the worst possible time to find out.
  try {
    readPackage(bytes, { verifyChecksums: false, deepIndexCheck: false });
  } catch (error) {
    errors.push(
      `the device parser would reject this package: ${
        error instanceof QpkError ? error.message : String(error)
      }`,
    );
  }

  // Rule 17 (qpk-format.md 11): LAYOUT_DATA line records ascend by
  // (page, line_id) and therefore by first_word_index. Checked HERE and not
  // on the device deliberately -- verifying it costs a full sweep of every
  // line record, which is exactly the open-time scan section 1.1 forbids, so
  // the device binary-searches on the invariant's word and this is the last
  // place that can prove it holds. A violated invariant would not crash the
  // device (getLine is bounds-checked either way); it would silently render
  // the wrong line, which is worse.
  if (pkg.section(SectionId.LayoutData)) {
    try {
      const layout = pkg.getLayoutHeader();
      let previousKey = -1;
      let previousWord = -1;
      for (let i = 0; i < layout.lineCountTotal; i++) {
        const line = pkg.getLine(i);
        const key = line.page * 0x10000 + line.lineId;
        if (key <= previousKey) {
          errors.push(
            `LAYOUT_DATA line ${i} is out of order: (page ${line.page}, line ` +
              `${line.lineId}) does not ascend. A renderer binary-searches these.`,
          );
          break;
        }
        if (line.firstWordIndex < previousWord) {
          errors.push(
            `LAYOUT_DATA line ${i} starts at word ${line.firstWordIndex}, ` +
              `before line ${i - 1}'s ${previousWord}. Word spans must ascend.`,
          );
          break;
        }
        previousKey = key;
        previousWord = line.firstWordIndex;
      }
      if (layout.lineCountTotal > 0 && pkg.recordCount(SectionId.WordIndex) > 0) {
        const last = pkg.getLine(layout.lineCountTotal - 1);
        const words = pkg.recordCount(SectionId.WordIndex);
        if (last.firstWordIndex >= words) {
          errors.push(
            `LAYOUT_DATA's last line starts at word ${last.firstWordIndex}, ` +
              `past the ${words} words in WORD_INDEX.`,
          );
        }
      }
    } catch (error) {
      errors.push(
        `LAYOUT_DATA could not be walked: ${
          error instanceof QpkError ? error.message : String(error)
        }`,
      );
    }
  }

  // COVER is optional, but one that is present has to be well formed: the
  // device silently draws a placeholder for a bad one, which would look like
  // the cover simply went missing.
  const cover = pkg.section(SectionId.Cover);
  if (cover) {
    try {
      decodeCover(bytes.subarray(cover.offset, cover.offset + cover.length));
    } catch (error) {
      errors.push(`COVER is not a valid cover picture: ${error instanceof Error ? error.message : String(error)}`);
    }
  }

  // Page pictures: both sections or neither, and every page decodes to exactly
  // one bitmap. The device only finds a bad page when someone turns to it.
  const imageIndex = pkg.section(SectionId.PageImageIndex);
  const imageData = pkg.section(SectionId.PageImageData);
  if (imageIndex || imageData) {
    if (!imageIndex || !imageData) {
      errors.push('PAGE_IMAGE_INDEX and PAGE_IMAGE_DATA must be present together');
    } else {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
      for (let i = 0; i < imageIndex.count; i++) {
        const record = decodePageImageRecord(view, imageIndex.offset + i * 16);
        if (record.offset + record.length > imageData.length) {
          errors.push(`page picture ${i + 1} lies outside PAGE_IMAGE_DATA`);
          break;
        }
        const start = imageData.offset + record.offset;
        try {
          unpackBits(bytes.subarray(start, start + record.length), pageImageBytes(record.width, record.height));
        } catch (error) {
          errors.push(
            `page picture ${i + 1} does not decode: ${error instanceof Error ? error.message : String(error)}`,
          );
          break;
        }
      }
    }
  }

  if (!pkg.metadata(MetadataKey.Title)) {
    warnings.push('no title in metadata: the library browser will show the content id');
  }
  if (pkg.header.packageType === PackageType.Quran && !pkg.metadata(MetadataKey.Source)) {
    warnings.push('a Quran package with no metadata.source: the text provenance is unrecorded');
  }

  return {
    ok: errors.length === 0,
    errors,
    warnings,
    summary: summarise(pkg),
  };
}

export function summarise(pkg: QpkPackage): PackageSummary {
  const metadata: Record<string, string> = {};
  for (const [key, value] of pkg.allMetadata()) {
    metadata[MetadataKey[key] ?? String(key)] = value;
  }

  return {
    // Upper case so it matches ConversionReport.contentType exactly; a UI
    // comparing the two should not have to know about the casing.
    type: packageTypeName(pkg.header.packageType).toUpperCase(),
    packageSize: pkg.header.packageSize,
    contentVersion: pkg.header.contentVersion,
    contentId: [...pkg.header.contentId].map((b) => b.toString(16).padStart(2, '0')).join(''),
    metadata,
    sections: pkg.sections.map((section) => ({
      id: section.id,
      name: sectionName(section.id),
      length: section.length,
      records: section.count,
    })),
    pageCount: pkg.recordCount(SectionId.PageIndex),
    chapterCount:
      pkg.recordCount(SectionId.ChapterIndex) + pkg.recordCount(SectionId.SurahIndex),
    ayahCount: pkg.recordCount(SectionId.AyahIndex),
    wordCount: pkg.recordCount(SectionId.WordIndex),
    hasWordLayout: (pkg.header.flags & FLAG_HAS_WORD_LAYOUT) !== 0,
  };
}
