// qpk_reader.h -- read-only, validating QPK1 parser.
//
// Design rules, all of them consequences of the product spec:
//
//   * Never scan. Every lookup resolves through fixed-size index records at a
//     computed offset (docs/qpk-format.md section 10).
//   * Never trust the file. Every validation rule in section 11 is checked and
//     reported as a distinct Error, so a failure says which rule broke.
//   * No dynamic allocation. Only the section table is cached (at most
//     kMaxSections * sizeof(SectionEntry)); records are read on demand into
//     caller-provided storage.
//   * No Arduino. The parser reads through hal::IFile, so the same code runs
//     against an SD card on the device and against a memory buffer in the
//     host tests.

#pragma once

#include <stdint.h>

#include "hal/storage.h"
#include "qpk/qpk_format.h"

namespace qpk {

enum class Error : uint8_t {
  kOk = 0,
  kNotOpen,             // no package is open
  kIoError,             // the underlying file read failed or came up short
  kTooSmall,            // file is smaller than a header
  kBadMagic,            // rule 2
  kUnsupportedVersion,  // rule 3
  kBadHeaderSize,       // rule 4
  kHeaderChecksum,      // rule 5
  kSizeMismatch,        // rule 6: package_size != actual file size
  kBadSectionTable,     // rule 7
  kSectionMisaligned,   // rule 8
  kSectionOutOfRange,   // rule 8
  kSectionOverlap,      // rule 9
  kSectionOrder,        // rule 10: duplicate or non-ascending ids
  kSectionSizeMismatch, // rule 11
  kMissingSection,      // rule 12
  kSectionChecksum,     // rule 13
  kIndexInconsistent,   // rule 14
  kFontSectionsRequired, // rule 15: kFlagShapedTextData needs FONT_METADATA + ASSETS
  kLayoutSectionRequired, // rule 16: kFlagHasWordLayout needs LAYOUT_DATA
  kNoSuchSection,       // the package simply does not carry that section
  kOutOfRange,          // the caller asked for a record that does not exist
};

const char* ErrorText(Error error);

// PackBits (docs/qpk-format.md 9d): true only if `src` decodes to exactly
// `dst_len` bytes without running past either buffer.
bool UnpackBits(const uint8_t* src, uint32_t src_len, uint8_t* dst, uint32_t dst_len);

// The COVER picture's kCoverPixelBytes straight from a package file, without
// Reader::open()'s checksum sweep: a shelf of covers would otherwise verify
// every book's indexes (seconds for a big picture book) just to draw a
// thumbnail. Reads the header and section table only as far as COVER, then
// checks the cover's own header as Reader::readCover() does.
// kNoSuchSection when the package has no cover.
Error ReadCoverFromFile(hal::IFile* file, uint8_t* dst, uint32_t capacity);

class Reader {
 public:
  // Parses and validates. On failure nothing is left open and the error names
  // the rule that failed. `file` must outlive the Reader; the Reader does not
  // take ownership and does not close it.
  Error open(hal::IFile* file);
  // The same, but with rule 13's fixed-index checksum sweep left to the
  // caller when `verify_index_checksums` is false: the device remembers
  // packages that already passed it (net::VerifiedPackages), because for a
  // big book the sweep is seconds of card reads on every open. Every other
  // rule is still checked. The caller then runs verifyIndexChecksums() itself
  // for a package it has not seen pass.
  Error open(hal::IFile* file, bool verify_index_checksums);
  void close();

  // Rule 13's eager half on its own: every fixed-size index section's CRC.
  Error verifyIndexChecksums() const;

  // Up to `count` whole records of `id` from record `first`, straight into
  // `dst`: one read instead of one per record. Stops at the section's end and
  // at what `capacity` holds. Returns the records copied; 0 when none can be.
  uint32_t readRecordRange(SectionId id, uint32_t first, uint32_t count, uint8_t* dst,
                           uint32_t capacity) const;

  bool isOpen() const { return file_ != nullptr; }

  const Header& header() const { return header_; }
  PackageType type() const { return header_.package_type; }
  uint8_t sectionCount() const { return section_count_; }

  const SectionEntry* section(SectionId id) const;
  const SectionEntry* sectionAt(uint8_t index) const;
  bool hasSection(SectionId id) const { return section(id) != nullptr; }

  // The COVER picture's kCoverPixelBytes of 2bpp pixels, into `dst`, after
  // checking its header (docs/qpk-format.md 9c). kNoSuchSection when the
  // package has no cover, kSectionSizeMismatch when the section is not a
  // well-formed one, kSectionOutOfRange when it cannot be read.
  Error readCover(uint8_t* dst, uint32_t capacity) const;

  // Page pictures (docs/qpk-format.md 9d). pageImageCount() is 0 for a
  // package without them. getPageImage() reads record `index` and checks it
  // lies inside PAGE_IMAGE_DATA; readPageImage() also reads the compressed
  // bytes into `scratch` and decodes them into `dst`, which must hold
  // ceil(width / 8) * height bytes. kSectionSizeMismatch when a buffer is too
  // small or the data does not decode to exactly one page.
  uint32_t pageImageCount() const;
  Error getPageImage(uint32_t index, PageImageRecord* out) const;
  Error readPageImage(uint32_t index, uint8_t* scratch, uint32_t scratch_capacity,
                      uint8_t* dst, uint32_t dst_capacity) const;
  uint32_t recordCount(SectionId id) const;

  // --- Quran access (docs/qpk-format.md section 10) ----------------------

  Error getSurah(uint16_t surah_id, SurahRecord* out) const;
  Error getAyah(uint16_t surah_id, uint16_t ayah_number, AyahRecord* out) const;
  Error getAyahByIndex(uint32_t index, AyahRecord* out) const;
  Error getWord(uint16_t surah_id, uint16_t ayah_number, uint16_t word_id,
                WordRecord* out) const;
  Error getWordByIndex(uint32_t index, WordRecord* out) const;
  Error getPage(uint16_t page_number, PageRecord* out) const;
  Error getJuz(uint16_t id, RangeRecord* out) const;
  Error getHizb(uint16_t id, RangeRecord* out) const;
  Error getRub(uint16_t id, RangeRecord* out) const;
  Error getSajdahByIndex(uint32_t index, SajdahRecord* out) const;

  // --- generic book access ------------------------------------------------

  Error getChapter(uint32_t index, ChapterRecord* out) const;
  Error getBookSection(uint32_t index, BookSectionRecord* out) const;
  Error getTextRecord(uint32_t index, TextRecord* out) const;

  // --- translation / tafsir ------------------------------------------------

  Error getTranslation(uint32_t ayah_index, TranslationRecord* out) const;

  // --- payload -------------------------------------------------------------

  // Copies text out of TEXT_DATA (or TRANSLATION_DATA). Returns the number of
  // bytes copied, or -1 on error. Never writes more than `capacity`, and the
  // result is NOT NUL-terminated -- QPK strings carry an explicit length.
  int32_t readText(uint32_t text_offset, uint32_t length, void* dst,
                   uint32_t capacity) const;
  int32_t readTranslationText(uint32_t text_offset, uint32_t length, void* dst,
                              uint32_t capacity) const;

  // Copies the value of a metadata key. Returns false if the key is absent.
  // `out_length` receives the full value length even when it was truncated to
  // `capacity`.
  bool metadata(MetadataKey key, void* dst, uint32_t capacity,
                uint32_t* out_length) const;

  /**
   * The atlas glyph ids that draw end-of-ayah markers, from
   * kAyahMarkerGlyphs ("<firstGlyphId>:<count>", docs/qpk-format.md 9b).
   *
   * The marker for ayah number n is glyph id `*first_glyph_id + (n - 1)`,
   * valid for n in 1..*count. Arithmetic, not a lookup table: the shaper
   * mints these contiguously and last, so no per-ayah field is needed and
   * AYAH_INDEX is untouched.
   *
   * False when the key is absent, malformed, or names an empty range --
   * every one of which the caller handles the same way, by drawing whatever
   * fallback it has. Not an Error, because "this package has no markers" is
   * an ordinary answer, not a failure.
   */
  bool ayahMarkerGlyphs(uint16_t* first_glyph_id, uint16_t* count) const;

  // --- layout (docs/qpk-format.md 9) ---------------------------------------

  Error getLayoutHeader(LayoutHeader* out) const;

  // Direct index, like getWordByIndex -- NOT resolved from a page number.
  // There is no page -> first-line-index field in the wire format yet (see
  // qpk_format.h's LayoutHeader comment), so finding a page's lines means
  // walking records 0..line_count_total-1 and testing `.page`.
  Error getLine(uint32_t index, LineRecord* out) const;

  /**
   * The line whose word span contains `word_index`.
   *
   * Binary search, not a walk: line records are written in ascending
   * (page, line_id) order and therefore in ascending `first_word_index`
   * order too (docs/qpk-format.md 9, rule 17). That is what keeps this
   * inside section 1.1's "never scan" rule now that a renderer resolves a
   * reading position -- an ayah or word index -- to the line it sits on,
   * every time the reader is opened.
   *
   * `out_line_index` receives the record's index, which is the paging
   * cursor: the next screen starts some number of lines further on.
   * kOutOfRange if the layout holds no line at or before `word_index`.
   */
  Error findLineByWordIndex(uint32_t word_index, uint32_t* out_line_index,
                            LineRecord* out) const;

  /**
   * The ayah whose word span contains `word_index`.
   *
   * Same binary search, over AYAH_INDEX's `first_word_index` -- which
   * ascends for the same reason SURAH_INDEX does: words are written in ayah
   * order. A line-driven renderer walks words, not ayahs, so this is how it
   * answers "which ayah does this word end?" without a scan, and therefore
   * where to draw an end-of-ayah marker.
   */
  Error findAyahByWordIndex(uint32_t word_index, uint32_t* out_ayah_index,
                            AyahRecord* out) const;

  // --- font / glyph atlas (docs/qpk-format.md 9a) --------------------------

  // Looks up FONT_METADATA by glyph_id: entries are not required to be sorted
  // or dense, so this is a linear search -- or a binary search once
  // buildGlyphIndex() has run. kNoSuchSection if the package carries no
  // FONT_METADATA; kOutOfRange if no entry matches `glyph_id`.
  Error getGlyph(uint16_t glyph_id, GlyphRecord* out) const;

  // Reads raw bytes out of ASSETS -- a glyph's packed 1bpp bitmap
  // (GlyphRecord::bitmap_offset), or, for a kFlagShapedTextData package, a
  // word's shaped glyph-id run (WORD_INDEX's text_offset/text_length,
  // reinterpreted as text_length/2 native-endian-independent u16 glyph ids --
  // decode with qpk::Read16, do not memcpy). Same truncate-not-overflow
  // contract as readText().
  int32_t readAsset(uint32_t offset, uint32_t length, void* dst,
                    uint32_t capacity) const;

  // Copies FONT_METADATA into `storage` (caller-owned -- e.g. PSRAM on the
  // device -- so the Reader itself still allocates nothing), sorted by
  // glyph_id, and from then on getGlyph() binary-searches it instead of
  // scanning the section on the card. A Quran screen looks up every glyph two
  // or three times, and on a 1,793-glyph atlas the scan cost ~500 SD reads
  // per glyph drawn.
  //
  // The sort is stable, so for a duplicated glyph_id getGlyph() returns the
  // same record the scan would (the first in table order). Returns the number
  // of glyphs indexed, or 0 -- leaving getGlyph() on the scan -- when there is
  // no FONT_METADATA, `capacity` is too small, or the section cannot be read.
  // `storage` must outlive the index; open() and close() both drop it.
  uint32_t buildGlyphIndex(GlyphRecord* storage, uint32_t capacity);
  bool glyphIndexed() const { return glyph_index_ != nullptr; }

  // --- integrity on demand -------------------------------------------------

  // Re-reads a section and checks its CRC. Blob sections are not verified at
  // open (rule 13), so this is how a validator or the inspector checks them.
  Error verifySection(SectionId id) const;

  // Verifies payload_crc32 over the whole file after the header. This is the
  // install-time check; it reads the entire package.
  Error verifyPayload() const;

 private:
  Error readHeader(uint64_t file_size);
  Error readSectionTable();
  Error validateSectionTable();
  Error verifyFixedIndexChecksums() const;
  Error checkIndexConsistency() const;
  Error requireSectionsForType() const;
  Error requireFontSectionsIfShaped() const;
  Error requireLayoutIfHasWordLayout() const;

  Error readRecordBytes(const SectionEntry& entry, uint32_t index,
                        uint8_t* buffer) const;
  Error getRange(SectionId section_id, uint16_t id, RangeRecord* out) const;
  Error crcRange(uint64_t offset, uint64_t length, uint32_t* out) const;
  int32_t readBlob(SectionId section_id, uint32_t offset, uint32_t length,
                   void* dst, uint32_t capacity) const;

  hal::IFile* file_ = nullptr;
  Header header_;
  SectionEntry sections_[kMaxSections];
  uint8_t section_count_ = 0;

  // See buildGlyphIndex(). Not owned.
  const GlyphRecord* glyph_index_ = nullptr;
  uint32_t glyph_index_count_ = 0;
};

}  // namespace qpk
