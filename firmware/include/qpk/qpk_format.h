// qpk_format.h -- on-disk constants and decoded record types for the QPK1
// container. See docs/qpk-format.md, which this file must not diverge from.
//
// The structs here are DECODED forms, filled field by field from little-endian
// bytes. On-disk bytes are never cast to a struct: the section-table entry
// puts a u64 at offset 4, so no entry's `offset` field is ever 8-aligned, and
// byte-wise decoding makes alignment, struct padding and host endianness all
// non-issues -- including on the host test build.

#pragma once

#include <stdint.h>

namespace qpk {

// --- container ---------------------------------------------------------------

constexpr char kMagic0 = 'Q';
constexpr char kMagic1 = 'P';
constexpr char kMagic2 = 'K';
constexpr char kMagic3 = '1';

constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kHeaderSize = 64;
constexpr uint16_t kSectionEntrySize = 32;

// 18 section ids are defined; the cap leaves room for forward-compatible
// additions without making the cached table large.
constexpr uint8_t kMaxSections = 32;

constexpr uint16_t kContentIdBytes = 16;

enum class PackageType : uint16_t {
  kUnknown = 0,
  kQuran = 1,
  kBook = 2,
  kTranslation = 3,
  kTafsir = 4,
};

enum class SectionId : uint16_t {
  kNone = 0,
  kMetadata = 1,
  kSurahIndex = 2,
  kJuzIndex = 3,
  kHizbIndex = 4,
  kRubIndex = 5,
  kPageIndex = 6,
  kAyahIndex = 7,
  kWordIndex = 8,
  kSajdahIndex = 9,
  kTextData = 10,
  kLayoutData = 11,
  kTranslationIndex = 12,
  kTranslationData = 13,
  kFontMetadata = 14,
  kAssets = 15,
  kChapterIndex = 16,
  kSectionIndex = 17,
  kTextIndex = 18,
  kCover = 19,  // optional, any type -- docs/qpk-format.md 9c
  kPageImageIndex = 20,  // optional, Book -- docs/qpk-format.md 9d
  kPageImageData = 21,
};

// COVER: a 16-byte header ("QCV1", width u16, height u16, bpp u8 = 2, seven
// zero bytes) then kCoverWidth x kCoverHeight pixels at 2 bpp, four to a byte,
// MSB first, 0 = black .. 3 = white -- the same packing as the home screen's
// photos, so the grey refresh path draws both.
constexpr uint16_t kCoverWidth = 108;
constexpr uint16_t kCoverHeight = 144;
constexpr uint32_t kCoverHeaderBytes = 16;
constexpr uint32_t kCoverPixelBytes = (kCoverWidth / 4) * kCoverHeight;
constexpr uint32_t kCoverBytes = kCoverHeaderBytes + kCoverPixelBytes;

// PAGE_IMAGE_INDEX records (docs/qpk-format.md 9d). Each names one page
// picture in PAGE_IMAGE_DATA: a PackBits-compressed width x height bitmap,
// 1bpp, ceil(width / 8) bytes a row, MSB first, 1 = black. The reader is
// portrait, so pages are 480 wide and 800 tall.
constexpr uint16_t kPageImageRecordSize = 16;
constexpr uint16_t kPageImageWidth = 480;
constexpr uint16_t kPageImageHeight = 800;
constexpr uint32_t kPageImageBytes = ((kPageImageWidth + 7u) / 8u) * kPageImageHeight;
// PackBits' worst case: one header byte per 128 literal bytes.
constexpr uint32_t kPageImageMaxCompressedBytes = kPageImageBytes + kPageImageBytes / 128 + 1;

struct PageImageRecord {
  uint32_t offset = 0;
  uint32_t length = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

enum class MetadataKey : uint16_t {
  kTitle = 1,
  kAuthor = 2,
  kLanguage = 3,
  kPublisher = 4,
  kSource = 5,
  kBuildTimestamp = 6,
  kConverterVersion = 7,
  kScript = 8,
  // "<firstGlyphId>:<count>" -- the atlas glyph ids that draw end-of-ayah
  // markers, in a kFlagShapedTextData package. Read through
  // Reader::ayahMarkerGlyphs(), not directly. See docs/qpk-format.md 9b.
  kAlignedContentId = 9,
  kAyahMarkerGlyphs = 10,
};

// Header flags.
constexpr uint32_t kFlagHasWordLayout = 1u << 0;
constexpr uint32_t kFlagRightToLeft = 1u << 1;
constexpr uint32_t kFlagIsDelta = 1u << 2;
// WORD_INDEX records' (text_offset, text_length) point into ASSETS instead of
// TEXT_DATA, and span text_length/2 pre-shaped glyph ids (u16 LE, FONT_METADATA
// space) in left-to-right visual order, instead of raw UTF-8 bytes. Requires
// FONT_METADATA and ASSETS (rule 15) -- see docs/qpk-format.md 9a.
// AYAH_INDEX and every other section's text_offset/text_length are
// unaffected: they still mean raw UTF-8 in TEXT_DATA, always, so search,
// translation matching and TTS keep working regardless of this flag.
constexpr uint32_t kFlagShapedTextData = 1u << 3;
constexpr uint32_t kFlagKnownMask = kFlagHasWordLayout | kFlagRightToLeft |
                                    kFlagIsDelta | kFlagShapedTextData;

// --- record sizes ------------------------------------------------------------

constexpr uint16_t kSurahRecordSize = 24;
constexpr uint16_t kAyahRecordSize = 24;
constexpr uint16_t kWordRecordSize = 16;
constexpr uint16_t kPageRecordSize = 16;
constexpr uint16_t kRangeRecordSize = 8;   // juz / hizb / rub
constexpr uint16_t kSajdahRecordSize = 8;
constexpr uint16_t kTranslationRecordSize = 8;
constexpr uint16_t kChapterRecordSize = 24;
constexpr uint16_t kBookSectionRecordSize = 16;
constexpr uint16_t kTextRecordSize = 12;
constexpr uint16_t kGlyphRecordSize = 12;

// Largest fixed record, used to size stack buffers.
constexpr uint16_t kMaxRecordSize = 24;

// --- decoded records ---------------------------------------------------------

struct Header {
  uint16_t format_version = 0;
  uint16_t header_size = 0;
  PackageType package_type = PackageType::kUnknown;
  uint16_t section_count = 0;
  uint32_t flags = 0;
  uint64_t package_size = 0;
  uint8_t content_id[kContentIdBytes] = {0};
  uint32_t content_version = 0;
  uint32_t section_table_offset = 0;
  uint32_t section_table_length = 0;
  uint32_t payload_crc32 = 0;
  uint32_t header_crc32 = 0;
};

struct SectionEntry {
  SectionId id = SectionId::kNone;
  uint16_t version = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint32_t count = 0;
  uint32_t crc32 = 0;
  uint16_t record_size = 0;
  uint16_t flags = 0;
};

struct SurahRecord {
  uint16_t surah_id = 0;
  uint16_t ayah_count = 0;
  uint32_t first_ayah_index = 0;
  uint16_t first_page = 0;
  uint16_t last_page = 0;
  uint16_t first_juz = 0;
  uint8_t revelation_place = 0;  // 0 = Meccan, 1 = Medinan
  uint8_t has_bismillah = 0;
  uint32_t name_offset = 0;
  uint16_t name_length = 0;
};

// Ayah flags.
constexpr uint16_t kAyahFlagStartsJuz = 1u << 0;
constexpr uint16_t kAyahFlagSajdah = 1u << 1;

struct AyahRecord {
  uint16_t surah_id = 0;
  uint16_t ayah_number = 0;
  uint16_t page = 0;
  uint16_t line = 0;
  uint16_t word_count = 0;
  uint16_t flags = 0;
  uint32_t text_offset = 0;
  uint32_t text_length = 0;
  uint32_t first_word_index = 0;
};

struct WordRecord {
  uint32_t text_offset = 0;
  uint16_t text_length = 0;
  uint16_t x = 0;
  uint16_t y = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t line_id = 0;
};

struct PageRecord {
  uint16_t page_number = 0;
  uint16_t ayah_count = 0;
  uint32_t first_ayah_index = 0;
  uint32_t first_word_index = 0;
  uint8_t line_count = 0;
  uint8_t juz = 0;
  uint16_t flags = 0;
};

// Juz / hizb / rub share a layout.
struct RangeRecord {
  uint16_t id = 0;
  uint16_t first_page = 0;
  uint32_t first_ayah_index = 0;
};

struct SajdahRecord {
  uint32_t ayah_index = 0;
  uint16_t page = 0;
  uint8_t kind = 0;  // 0 = recommended, 1 = obligatory
};

struct TranslationRecord {
  uint32_t text_offset = 0;
  uint32_t text_length = 0;
};

struct ChapterRecord {
  uint32_t chapter_number = 0;
  uint32_t first_page = 0;
  uint32_t first_section_index = 0;
  uint32_t title_offset = 0;
  uint16_t title_length = 0;
  uint16_t depth = 0;
  uint32_t parent_index = 0;  // 0xFFFFFFFF = none
};

constexpr uint32_t kNoParent = 0xFFFFFFFFu;

struct BookSectionRecord {
  uint32_t first_page = 0;
  uint32_t title_offset = 0;
  uint16_t title_length = 0;
  uint16_t depth = 0;
  uint32_t first_text_index = 0;
};

struct TextRecord {
  uint32_t text_offset = 0;
  uint32_t text_length = 0;
  uint16_t page_number = 0;
  uint16_t flags = 0;
};

// LAYOUT_DATA (id 11) -- per-line geometry for a Quran or Book package. Blob
// section: a fixed 12-byte header (LayoutHeader) followed by
// layout_version.line_count_total 12-byte line records (LineRecord). See
// docs/qpk-format.md section 9.
//
// Line records are written in ascending (page, line_id) order -- a writer
// discipline, not a wire-format guarantee the way SURAH_INDEX/PAGE_INDEX slot
// addressing is: there is no page -> first-line-index field yet, because
// nothing reads LAYOUT_DATA at runtime to demand one. Finding a page's lines
// today means walking records the same way Reader::getGlyph() walks
// FONT_METADATA.
constexpr uint16_t kLayoutHeaderSize = 12;
constexpr uint16_t kLineRecordSize = 12;

struct LayoutHeader {
  uint16_t layout_version = 0;
  uint16_t page_width = 0;
  uint16_t page_height = 0;
  uint16_t line_count_total = 0;
  uint32_t line_records_offset = 0;  // relative to the section start
};

struct LineRecord {
  uint16_t page = 0;
  uint16_t line_id = 0;
  uint16_t y = 0;
  uint16_t height = 0;
  uint32_t first_word_index = 0;
};

// One entry in FONT_METADATA -- the glyph atlas index. `glyph_id` is arbitrary
// (assigned by whatever desktop tool did the shaping) and is what a
// kFlagShapedTextData package's WORD_INDEX-via-ASSETS glyph runs reference.
// Not necessarily sorted or dense: Reader::getGlyph() searches by value.
//
// The bitmap at `bitmap_offset` in ASSETS is `width` x `height`, 1bpp,
// MSB-first, each row padded to a whole byte -- the same convention as
// gfx::font5x7's fixed glyphs, so both can share one blit primitive. Bit 1 is
// ink (foreground); bit 0 is transparent, not "background white" -- this is
// what lets a diacritic's bitmap overlap a base letter's without clobbering
// it.
//
// x_advance/x_offset/y_offset are per-glyph-id constants, not per-occurrence:
// this format does not support context-dependent mark repositioning (the
// desktop shaper is expected to mint a distinct glyph_id per visually
// distinct base+mark combination instead). A deliberate simplification, not
// an oversight -- see docs/qpk-format.md 9a.
struct GlyphRecord {
  uint16_t glyph_id = 0;
  uint32_t bitmap_offset = 0;  // into ASSETS
  uint8_t width = 0;
  uint8_t height = 0;
  int8_t x_advance = 0;  // pen movement after drawing this glyph
  int8_t x_offset = 0;   // pen position -> bitmap left edge
  int8_t y_offset = 0;   // line baseline -> bitmap top edge
  uint8_t reserved = 0;
};

// --- helpers -----------------------------------------------------------------

// Fixed record size for a section id, or 0 for blob / variable sections.
uint16_t RecordSizeFor(SectionId id);

// True if `id` names a fixed-size index section, i.e. one whose CRC is
// verified eagerly at open (docs/qpk-format.md rule 13).
bool IsFixedIndexSection(SectionId id);

const char* PackageTypeName(PackageType type);
const char* SectionName(SectionId id);

}  // namespace qpk
