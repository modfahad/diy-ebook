// What a conversion tells you before you install it.
//
// The spec requires validation and preview *before* installing to the device.
// This is the machine-readable half of that: everything a UI or a CLI needs to
// decide whether the conversion is good enough, including the things that went
// approximately rather than exactly right.

export interface ConversionReport {
  contentType: 'QURAN' | 'BOOK';
  packageSize: number;
  pageCount: number;
  wordCount: number;
  /** Chapters/surahs. */
  chapterCount: number;
  sectionCount: number;

  /** The coordinate space word boxes are expressed in. */
  layoutSpace: { width: number; height: number };

  /**
   * Fraction of word boxes that were apportioned across a multi-word text run
   * rather than measured. High values mean word-level highlighting will be
   * approximate.
   */
  estimatedWordFraction: number;

  /** Word boxes whose coordinates exceeded the u16 field and were clamped. */
  clampedCoordinates: number;

  warnings: string[];
}

export function emptyReport(contentType: 'QURAN' | 'BOOK'): ConversionReport {
  return {
    contentType,
    packageSize: 0,
    pageCount: 0,
    wordCount: 0,
    chapterCount: 0,
    sectionCount: 0,
    layoutSpace: { width: 0, height: 0 },
    estimatedWordFraction: 0,
    clampedCoordinates: 0,
    warnings: [],
  };
}

export interface ConversionResult {
  bytes: Uint8Array;
  report: ConversionReport;
}

/** u16 fields in WORD_INDEX. Anything larger is clamped and counted. */
export const MAX_COORDINATE = 0xffff;

export function clampCoordinate(value: number, counter: { clamped: number }): number {
  const rounded = Math.max(0, Math.round(value));
  if (rounded > MAX_COORDINATE) {
    counter.clamped++;
    return MAX_COORDINATE;
  }
  return rounded;
}
