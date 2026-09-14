// The Quran import schema.
//
// ARCHITECTURAL DECISION, recorded here and in docs/conversion.md:
// the Quran pipeline does NOT parse arbitrary PDFs. Section 12 of the product
// spec warns against assuming a PDF contains clean machine-readable Quran
// text, and rule 17 makes correctness here non-negotiable in a way heuristics
// cannot satisfy. Extracting Quranic text with layout heuristics risks
// silently corrupting scripture, and no amount of validation downstream can
// recover from that.
//
// So: Quran packages are built from a *structured, verified* source in the
// schema below, which the publisher is responsible for. Word coordinates are
// an optional enrichment layer applied on top, keyed by surah:ayah:word, and
// applied only after the text validates.
//
// No Quran data is bundled with this repository.

export const QURAN_SCHEMA_VERSION = 1;

export type RevelationPlace = 'meccan' | 'medinan';
export type SajdahKind = 'recommended' | 'obligatory';

export interface QuranSourceMetadata {
  title: string;
  /** e.g. "uthmani", "indopak". Recorded so the renderer can pick a font. */
  script?: string;
  language?: string;
  publisher?: string;
  /** Provenance of the text. Strongly recommended. */
  source?: string;
}

export interface QuranSourceSurah {
  id: number;
  name: string;
  revelationPlace: RevelationPlace;
  hasBismillah?: boolean;
}

export interface QuranSourceAyah {
  surah: number;
  ayah: number;
  /** 1-based mushaf page. */
  page: number;
  /** 1-based line on that page, if the source knows it. */
  line?: number;
  text: string;
  /**
   * Word-by-word split of `text`. Required for Hifz word hiding and for
   * word-level AI feedback; optional otherwise.
   */
  words?: string[];
}

export interface QuranSourcePage {
  page: number;
  lineCount?: number;
  juz?: number;
}

/** Juz, hizb and rub all start at a given ayah. */
export interface QuranSourceRange {
  id: number;
  surah: number;
  ayah: number;
}

export interface QuranSourceSajdah {
  surah: number;
  ayah: number;
  kind?: SajdahKind;
}

export interface QuranSourceWordBox {
  surah: number;
  ayah: number;
  /** 0-based index within the ayah. */
  word: number;
  x: number;
  y: number;
  width: number;
  height: number;
  line?: number;
  /**
   * The page this word is on, when it differs from its ayah's.
   *
   * Not redundant: a real mushaf routinely breaks an ayah across a page, so
   * the words of one ayah can carry two page numbers. Defaults to the ayah's
   * page, which is all the placeholder one-page-per-surah data ever needed --
   * and is exactly why the LAYOUT_DATA line aggregation used to key on the
   * ayah's page and was never wrong until real data arrived.
   */
  page?: number;
}

export interface QuranSourceLayout {
  /** The coordinate space the boxes are in. */
  pageWidth: number;
  pageHeight: number;
  words: QuranSourceWordBox[];
}

export interface QuranSource {
  schemaVersion: number;
  metadata: QuranSourceMetadata;
  surahs: QuranSourceSurah[];
  ayahs: QuranSourceAyah[];
  pages?: QuranSourcePage[];
  juz?: QuranSourceRange[];
  hizb?: QuranSourceRange[];
  rub?: QuranSourceRange[];
  sajdah?: QuranSourceSajdah[];
  layout?: QuranSourceLayout;
}

export interface SourceValidation {
  errors: string[];
  warnings: string[];
}

export function isValid(result: SourceValidation): boolean {
  return result.errors.length === 0;
}

/**
 * Structural validation of a Quran source.
 *
 * This checks the things the package format then relies on for direct access:
 * contiguous surah ids, contiguous ayah numbers within each surah, contiguous
 * page numbers from 1, and layout references that point at ayahs and words
 * that exist. It does NOT and cannot check that the text itself is correct --
 * that is the publisher's responsibility, and `metadata.source` exists so the
 * provenance travels with the package.
 */
export function validateQuranSource(source: unknown): SourceValidation {
  const errors: string[] = [];
  const warnings: string[] = [];
  const fail = (message: string) => errors.push(message);

  if (typeof source !== 'object' || source === null) {
    return { errors: ['source is not an object'], warnings };
  }
  const src = source as Partial<QuranSource>;

  if (src.schemaVersion !== QURAN_SCHEMA_VERSION) {
    fail(`schemaVersion must be ${QURAN_SCHEMA_VERSION}, got ${String(src.schemaVersion)}`);
    return { errors, warnings };
  }
  if (!src.metadata || typeof src.metadata.title !== 'string' || !src.metadata.title.trim()) {
    fail('metadata.title is required');
  }
  if (!src.metadata?.source) {
    warnings.push('metadata.source is empty: the text provenance will not travel with the package');
  }

  const surahs = src.surahs ?? [];
  const ayahs = src.ayahs ?? [];
  if (surahs.length === 0) fail('surahs is empty');
  if (ayahs.length === 0) fail('ayahs is empty');
  if (errors.length > 0) return { errors, warnings };

  // Surah ids must be 1..N with no gaps: SURAH_INDEX is addressed as [id - 1].
  const sortedSurahs = [...surahs].sort((a, b) => a.id - b.id);
  sortedSurahs.forEach((surah, index) => {
    if (surah.id !== index + 1) {
      fail(`surah ids must be contiguous from 1; expected ${index + 1}, got ${surah.id}`);
    }
    if (!surah.name || !surah.name.trim()) fail(`surah ${surah.id} has no name`);
    if (surah.revelationPlace !== 'meccan' && surah.revelationPlace !== 'medinan') {
      fail(`surah ${surah.id} has an unknown revelationPlace "${String(surah.revelationPlace)}"`);
    }
  });

  // Ayah numbers must be 1..count within each surah, again for direct access.
  const bySurah = new Map<number, QuranSourceAyah[]>();
  for (const ayah of ayahs) {
    if (!bySurah.has(ayah.surah)) bySurah.set(ayah.surah, []);
    bySurah.get(ayah.surah)!.push(ayah);
  }
  for (const surah of sortedSurahs) {
    const list = bySurah.get(surah.id);
    if (!list || list.length === 0) {
      fail(`surah ${surah.id} has no ayahs`);
      continue;
    }
    const sorted = [...list].sort((a, b) => a.ayah - b.ayah);
    sorted.forEach((ayah, index) => {
      if (ayah.ayah !== index + 1) {
        fail(
          `surah ${surah.id}: ayah numbers must be contiguous from 1; ` +
            `expected ${index + 1}, got ${ayah.ayah}`,
        );
      }
      if (typeof ayah.text !== 'string' || ayah.text.length === 0) {
        fail(`ayah ${surah.id}:${ayah.ayah} has no text`);
      }
      if (!Number.isInteger(ayah.page) || ayah.page < 1) {
        fail(`ayah ${surah.id}:${ayah.ayah} has an invalid page ${String(ayah.page)}`);
      }
      if (ayah.words && ayah.words.length === 0) {
        fail(`ayah ${surah.id}:${ayah.ayah} has an empty words array`);
      }
      if (ayah.words) {
        const joined = ayah.words.join(' ').replace(/\s+/gu, ' ').trim();
        const normalised = ayah.text.replace(/\s+/gu, ' ').trim();
        if (joined !== normalised) {
          warnings.push(
            `ayah ${surah.id}:${ayah.ayah}: words do not rejoin to text; ` +
              'check the word split',
          );
        }
      }
    });
  }
  for (const surahId of bySurah.keys()) {
    if (!sortedSurahs.some((s) => s.id === surahId)) {
      fail(`ayahs reference surah ${surahId}, which is not declared`);
    }
  }

  // Pages must be 1..N with no gaps: PAGE_INDEX is addressed as [page - 1].
  const maxPage = Math.max(...ayahs.map((a) => a.page));
  const seenPages = new Set(ayahs.map((a) => a.page));
  for (let page = 1; page <= maxPage; page++) {
    if (!seenPages.has(page)) fail(`page ${page} has no ayahs; pages must be contiguous from 1`);
  }

  const ayahExists = (surah: number, ayah: number) =>
    (bySurah.get(surah) ?? []).some((a) => a.ayah === ayah);

  for (const [name, ranges] of [
    ['juz', src.juz],
    ['hizb', src.hizb],
    ['rub', src.rub],
  ] as const) {
    if (!ranges) continue;
    const sorted = [...ranges].sort((a, b) => a.id - b.id);
    sorted.forEach((range, index) => {
      if (range.id !== index + 1) {
        fail(`${name} ids must be contiguous from 1; expected ${index + 1}, got ${range.id}`);
      }
      if (!ayahExists(range.surah, range.ayah)) {
        fail(`${name} ${range.id} starts at ${range.surah}:${range.ayah}, which does not exist`);
      }
    });
  }

  for (const sajdah of src.sajdah ?? []) {
    if (!ayahExists(sajdah.surah, sajdah.ayah)) {
      fail(`sajdah at ${sajdah.surah}:${sajdah.ayah} does not exist`);
    }
  }

  if (src.layout) {
    if (!(src.layout.pageWidth > 0) || !(src.layout.pageHeight > 0)) {
      fail('layout.pageWidth and layout.pageHeight must be positive');
    }
    let missingWords = 0;
    for (const box of src.layout.words) {
      const ayah = (bySurah.get(box.surah) ?? []).find((a) => a.ayah === box.ayah);
      if (!ayah) {
        fail(`layout references ayah ${box.surah}:${box.ayah}, which does not exist`);
        continue;
      }
      if (!ayah.words) {
        missingWords++;
        continue;
      }
      if (box.word < 0 || box.word >= ayah.words.length) {
        fail(
          `layout references word ${box.word} of ${box.surah}:${box.ayah}, ` +
            `which has ${ayah.words.length} words`,
        );
      }
    }
    if (missingWords > 0) {
      fail(
        `layout gives boxes for ${missingWords} words in ayahs that have no words array; ` +
          'word coordinates need a word split to attach to',
      );
    }
  }

  return { errors, warnings };
}
