// Preview: read one page back out of a finished package.
//
// This deliberately goes through the QPK reader rather than the in-memory
// ParsedDocument. Previewing the source would show what the converter meant;
// previewing the package shows what the device will actually get, which is
// the only preview worth having before an install.

import {
  PackageType,
  SectionId,
  readPackage,
  type QpkPackage,
} from '@quran-device/qpk-format';

export interface PreviewWord {
  text: string;
  x: number;
  y: number;
  width: number;
  height: number;
  lineId: number;
}

export interface PagePreview {
  pageNumber: number;
  pageCount: number;
  /** Chapter title or surah name covering this page, when one is known. */
  heading?: string;
  text: string;
  words: PreviewWord[];
}

export function previewPage(bytes: Uint8Array, pageNumber: number): PagePreview {
  const pkg = readPackage(bytes);
  const pageCount = pkg.recordCount(SectionId.PageIndex);
  if (pageNumber < 1 || pageNumber > pageCount) {
    throw new Error(`page ${pageNumber} is out of range (1..${pageCount})`);
  }

  return pkg.header.packageType === PackageType.Quran
    ? previewQuranPage(pkg, pageNumber, pageCount)
    : previewBookPage(pkg, pageNumber, pageCount);
}

function previewBookPage(pkg: QpkPackage, pageNumber: number, pageCount: number): PagePreview {
  const record = pkg.getTextRecord(pageNumber - 1);
  const page = pkg.getPage(pageNumber);

  let heading: string | undefined;
  for (let i = 0; i < pkg.recordCount(SectionId.ChapterIndex); i++) {
    const chapter = pkg.getChapter(i);
    if (chapter.firstPage <= pageNumber) {
      heading = pkg.text(chapter.titleOffset, chapter.titleLength);
    }
  }

  const words: PreviewWord[] = [];
  if (pkg.section(SectionId.WordIndex)) {
    const total = pkg.recordCount(SectionId.WordIndex);
    const end =
      pageNumber < pageCount ? pkg.getPage(pageNumber + 1).firstWordIndex : total;
    for (let i = page.firstWordIndex; i < end && i < total; i++) {
      words.push(toPreviewWord(pkg, i));
    }
  }

  return {
    pageNumber,
    pageCount,
    ...(heading ? { heading } : {}),
    text: pkg.text(record.textOffset, record.textLength),
    words,
  };
}

function previewQuranPage(pkg: QpkPackage, pageNumber: number, pageCount: number): PagePreview {
  const page = pkg.getPage(pageNumber);
  const lines: string[] = [];
  let heading: string | undefined;

  for (let i = 0; i < page.ayahCount; i++) {
    const ayah = pkg.getAyahByIndex(page.firstAyahIndex + i);
    if (heading === undefined) {
      const surah = pkg.getSurah(ayah.surahId);
      heading = pkg.text(surah.nameOffset, surah.nameLength);
    }
    lines.push(
      `${ayah.surahId}:${ayah.ayahNumber}  ${pkg.text(ayah.textOffset, ayah.textLength)}`,
    );
  }

  const words: PreviewWord[] = [];
  if (pkg.section(SectionId.WordIndex)) {
    const total = pkg.recordCount(SectionId.WordIndex);
    const end =
      pageNumber < pageCount ? pkg.getPage(pageNumber + 1).firstWordIndex : total;
    for (let i = page.firstWordIndex; i < end && i < total; i++) {
      words.push(toPreviewWord(pkg, i));
    }
  }

  return {
    pageNumber,
    pageCount,
    ...(heading ? { heading } : {}),
    text: lines.join('\n'),
    words,
  };
}

function toPreviewWord(pkg: QpkPackage, index: number): PreviewWord {
  const word = pkg.getWordByIndex(index);
  return {
    text: pkg.text(word.textOffset, word.textLength),
    x: word.x,
    y: word.y,
    width: word.width,
    height: word.height,
    lineId: word.lineId,
  };
}
