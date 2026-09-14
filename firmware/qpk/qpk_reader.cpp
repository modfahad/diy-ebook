#include "qpk/qpk_reader.h"

#include <string.h>

#include "qpk/byte_order.h"
#include "qpk/crc32.h"

namespace qpk {
namespace {

// Scratch for streaming a CRC over a section. It used to be 256 bytes on the
// stack, which made opening a 913-page picture book (a multi-megabyte
// WORD_INDEX) thousands of tiny SD reads -- about 4 s per open. It is a
// static buffer instead, so it can be big without touching the main task's
// few-kilobyte stack; readers are only ever used from that one task.
constexpr uint32_t kCrcChunkBytes = 8192;
uint8_t g_crc_chunk[kCrcChunkBytes];

void DecodeSurah(const uint8_t* b, SurahRecord* out) {
  out->surah_id = Read16(b + 0);
  out->ayah_count = Read16(b + 2);
  out->first_ayah_index = Read32(b + 4);
  out->first_page = Read16(b + 8);
  out->last_page = Read16(b + 10);
  out->first_juz = Read16(b + 12);
  out->revelation_place = Read8(b + 14);
  out->has_bismillah = Read8(b + 15);
  out->name_offset = Read32(b + 16);
  out->name_length = Read16(b + 20);
}

void DecodeAyah(const uint8_t* b, AyahRecord* out) {
  out->surah_id = Read16(b + 0);
  out->ayah_number = Read16(b + 2);
  out->page = Read16(b + 4);
  out->line = Read16(b + 6);
  out->word_count = Read16(b + 8);
  out->flags = Read16(b + 10);
  out->text_offset = Read32(b + 12);
  out->text_length = Read32(b + 16);
  out->first_word_index = Read32(b + 20);
}

void DecodeWord(const uint8_t* b, WordRecord* out) {
  out->text_offset = Read32(b + 0);
  out->text_length = Read16(b + 4);
  out->x = Read16(b + 6);
  out->y = Read16(b + 8);
  out->width = Read16(b + 10);
  out->height = Read16(b + 12);
  out->line_id = Read16(b + 14);
}

void DecodePage(const uint8_t* b, PageRecord* out) {
  out->page_number = Read16(b + 0);
  out->ayah_count = Read16(b + 2);
  out->first_ayah_index = Read32(b + 4);
  out->first_word_index = Read32(b + 8);
  out->line_count = Read8(b + 12);
  out->juz = Read8(b + 13);
  out->flags = Read16(b + 14);
}

void DecodeRange(const uint8_t* b, RangeRecord* out) {
  out->id = Read16(b + 0);
  out->first_page = Read16(b + 2);
  out->first_ayah_index = Read32(b + 4);
}

void DecodeSajdah(const uint8_t* b, SajdahRecord* out) {
  out->ayah_index = Read32(b + 0);
  out->page = Read16(b + 4);
  out->kind = Read8(b + 6);
}

void DecodeTranslation(const uint8_t* b, TranslationRecord* out) {
  out->text_offset = Read32(b + 0);
  out->text_length = Read32(b + 4);
}

void DecodeChapter(const uint8_t* b, ChapterRecord* out) {
  out->chapter_number = Read32(b + 0);
  out->first_page = Read32(b + 4);
  out->first_section_index = Read32(b + 8);
  out->title_offset = Read32(b + 12);
  out->title_length = Read16(b + 16);
  out->depth = Read16(b + 18);
  out->parent_index = Read32(b + 20);
}

void DecodeBookSection(const uint8_t* b, BookSectionRecord* out) {
  out->first_page = Read32(b + 0);
  out->title_offset = Read32(b + 4);
  out->title_length = Read16(b + 8);
  out->depth = Read16(b + 10);
  out->first_text_index = Read32(b + 12);
}

void DecodeTextRecord(const uint8_t* b, TextRecord* out) {
  out->text_offset = Read32(b + 0);
  out->text_length = Read32(b + 4);
  out->page_number = Read16(b + 8);
  out->flags = Read16(b + 10);
}

void DecodeLayoutHeader(const uint8_t* b, LayoutHeader* out) {
  out->layout_version = Read16(b + 0);
  out->page_width = Read16(b + 2);
  out->page_height = Read16(b + 4);
  out->line_count_total = Read16(b + 6);
  out->line_records_offset = Read32(b + 8);
}

void DecodeLine(const uint8_t* b, LineRecord* out) {
  out->page = Read16(b + 0);
  out->line_id = Read16(b + 2);
  out->y = Read16(b + 4);
  out->height = Read16(b + 6);
  out->first_word_index = Read32(b + 8);
}

void DecodeGlyph(const uint8_t* b, GlyphRecord* out) {
  out->glyph_id = Read16(b + 0);
  out->bitmap_offset = Read32(b + 2);
  out->width = Read8(b + 6);
  out->height = Read8(b + 7);
  out->x_advance = static_cast<int8_t>(Read8(b + 8));
  out->x_offset = static_cast<int8_t>(Read8(b + 9));
  out->y_offset = static_cast<int8_t>(Read8(b + 10));
  out->reserved = Read8(b + 11);
}

// Every bounds check in this file is written in subtraction form.
// `offset + length <= limit` overflows on a hostile file; this cannot.
bool WithinBounds(uint64_t offset, uint64_t length, uint64_t limit) {
  return offset <= limit && length <= limit - offset;
}

}  // namespace

const char* ErrorText(Error error) {
  switch (error) {
    case Error::kOk:                  return "ok";
    case Error::kNotOpen:             return "no package open";
    case Error::kIoError:             return "read failed or short read";
    case Error::kTooSmall:            return "file smaller than a header";
    case Error::kBadMagic:            return "bad magic (not a QPK1 file)";
    case Error::kUnsupportedVersion:  return "unsupported format version";
    case Error::kBadHeaderSize:       return "bad header_size";
    case Error::kHeaderChecksum:      return "header checksum mismatch";
    case Error::kSizeMismatch:        return "package_size != file size";
    case Error::kBadSectionTable:     return "bad section table";
    case Error::kSectionMisaligned:   return "section offset not 4-byte aligned";
    case Error::kSectionOutOfRange:   return "section runs past end of file";
    case Error::kSectionOverlap:      return "sections overlap";
    case Error::kSectionOrder:        return "section ids not unique/ascending";
    case Error::kSectionSizeMismatch: return "section length != count * record_size";
    case Error::kMissingSection:      return "required section missing";
    case Error::kSectionChecksum:     return "section checksum mismatch";
    case Error::kIndexInconsistent:   return "index records inconsistent";
    case Error::kFontSectionsRequired:
      return "kFlagShapedTextData set without FONT_METADATA + ASSETS";
    case Error::kLayoutSectionRequired:
      return "kFlagHasWordLayout set without LAYOUT_DATA";
    case Error::kNoSuchSection:       return "package has no such section";
    case Error::kOutOfRange:          return "record index out of range";
    default:                          return "unknown error";
  }
}

// --- open / close -----------------------------------------------------------

Error Reader::open(hal::IFile* file) {
  close();
  if (file == nullptr || !file->valid()) return Error::kIoError;
  file_ = file;

  Error error = readHeader(file->size());
  if (error == Error::kOk) error = readSectionTable();
  if (error == Error::kOk) error = validateSectionTable();
  if (error == Error::kOk) error = requireSectionsForType();
  if (error == Error::kOk) error = requireFontSectionsIfShaped();
  if (error == Error::kOk) error = requireLayoutIfHasWordLayout();
  if (error == Error::kOk) error = verifyFixedIndexChecksums();
  if (error == Error::kOk) error = checkIndexConsistency();

  if (error != Error::kOk) close();
  return error;
}

void Reader::close() {
  file_ = nullptr;
  header_ = Header();
  section_count_ = 0;
  glyph_index_ = nullptr;
  glyph_index_count_ = 0;
}

// --- header -----------------------------------------------------------------

Error Reader::readHeader(uint64_t file_size) {
  // Rule 1.
  if (file_size < kHeaderSize) return Error::kTooSmall;

  uint8_t b[kHeaderSize];
  if (!file_->readExact(0, b, kHeaderSize)) return Error::kIoError;

  // Rule 2.
  if (b[0] != kMagic0 || b[1] != kMagic1 || b[2] != kMagic2 ||
      b[3] != kMagic3) {
    return Error::kBadMagic;
  }

  // Rule 3.
  header_.format_version = Read16(b + 4);
  if (header_.format_version != kFormatVersion) {
    return Error::kUnsupportedVersion;
  }

  // Rule 4.
  header_.header_size = Read16(b + 6);
  if (header_.header_size < kHeaderSize || header_.header_size > file_size) {
    return Error::kBadHeaderSize;
  }

  // Rule 5: the CRC covers bytes [0, 60).
  header_.header_crc32 = Read32(b + 60);
  if (Crc32(b, 60) != header_.header_crc32) return Error::kHeaderChecksum;

  header_.package_type = static_cast<PackageType>(Read16(b + 8));
  header_.section_count = Read16(b + 10);
  header_.flags = Read32(b + 12);
  header_.package_size = Read64(b + 16);
  memcpy(header_.content_id, b + 24, kContentIdBytes);
  header_.content_version = Read32(b + 40);
  header_.section_table_offset = Read32(b + 44);
  header_.section_table_length = Read32(b + 48);
  header_.payload_crc32 = Read32(b + 52);

  // Rule 6.
  if (header_.package_size != file_size) return Error::kSizeMismatch;
  return Error::kOk;
}

// --- section table ----------------------------------------------------------

Error Reader::readSectionTable() {
  const uint16_t count = header_.section_count;
  if (count == 0 || count > kMaxSections) return Error::kBadSectionTable;

  // Rule 7.
  if (static_cast<uint32_t>(count) * kSectionEntrySize !=
      header_.section_table_length) {
    return Error::kBadSectionTable;
  }
  if (header_.section_table_offset < header_.header_size) {
    return Error::kBadSectionTable;
  }
  if (!WithinBounds(header_.section_table_offset, header_.section_table_length,
                    header_.package_size)) {
    return Error::kBadSectionTable;
  }

  uint8_t b[kSectionEntrySize];
  for (uint16_t i = 0; i < count; ++i) {
    const uint64_t offset = static_cast<uint64_t>(header_.section_table_offset) +
                            static_cast<uint64_t>(i) * kSectionEntrySize;
    if (!file_->readExact(offset, b, kSectionEntrySize)) {
      return Error::kIoError;
    }
    SectionEntry& e = sections_[i];
    e.id = static_cast<SectionId>(Read16(b + 0));
    e.version = Read16(b + 2);
    e.offset = Read64(b + 4);
    e.length = Read64(b + 12);
    e.count = Read32(b + 20);
    e.crc32 = Read32(b + 24);
    e.record_size = Read16(b + 28);
    e.flags = Read16(b + 30);
  }
  section_count_ = static_cast<uint8_t>(count);
  return Error::kOk;
}

Error Reader::validateSectionTable() {
  const uint64_t table_end = static_cast<uint64_t>(header_.section_table_offset) +
                             header_.section_table_length;

  for (uint8_t i = 0; i < section_count_; ++i) {
    const SectionEntry& e = sections_[i];

    // Rule 10.
    if (i > 0 &&
        static_cast<uint16_t>(e.id) <=
            static_cast<uint16_t>(sections_[i - 1].id)) {
      return Error::kSectionOrder;
    }

    // Rule 8.
    if ((e.offset & 3u) != 0) return Error::kSectionMisaligned;
    if (e.offset < table_end) return Error::kSectionOutOfRange;
    if (!WithinBounds(e.offset, e.length, header_.package_size)) {
      return Error::kSectionOutOfRange;
    }

    // Rule 11. A known fixed-size section must also use the stride this
    // version of the format defines, so a writer cannot quietly change it.
    const uint16_t expected = RecordSizeFor(e.id);
    if (expected != 0 && e.record_size != expected) {
      return Error::kSectionSizeMismatch;
    }
    if (e.record_size != 0) {
      const uint64_t needed =
          static_cast<uint64_t>(e.count) * static_cast<uint64_t>(e.record_size);
      if (needed != e.length) return Error::kSectionSizeMismatch;
    }
  }

  // Rule 9. Entries are ordered by id, not by offset, so this cannot be a
  // single sweep. n <= 32, so the pairwise check is free.
  for (uint8_t i = 0; i < section_count_; ++i) {
    if (sections_[i].length == 0) continue;
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < section_count_; ++j) {
      if (sections_[j].length == 0) continue;
      const uint64_t a0 = sections_[i].offset;
      const uint64_t a1 = a0 + sections_[i].length;
      const uint64_t b0 = sections_[j].offset;
      const uint64_t b1 = b0 + sections_[j].length;
      if (a0 < b1 && b0 < a1) return Error::kSectionOverlap;
    }
  }
  return Error::kOk;
}

Error Reader::requireSectionsForType() const {
  // Rule 12. An unrecognised package_type is NOT rejected: unknown things are
  // skipped, not fatal (see docs/qpk-format.md 5.1). Such a package still has
  // to carry METADATA, and its accessors will report kNoSuchSection.
  static const SectionId kQuranRequired[] = {
      SectionId::kMetadata, SectionId::kSurahIndex, SectionId::kPageIndex,
      SectionId::kAyahIndex, SectionId::kTextData};
  static const SectionId kBookRequired[] = {
      SectionId::kMetadata, SectionId::kChapterIndex, SectionId::kPageIndex,
      SectionId::kTextIndex, SectionId::kTextData};
  static const SectionId kTranslationRequired[] = {
      SectionId::kMetadata, SectionId::kTranslationIndex,
      SectionId::kTranslationData};
  static const SectionId kMinimalRequired[] = {SectionId::kMetadata};

  const SectionId* required = kMinimalRequired;
  uint8_t n = 1;
  switch (header_.package_type) {
    case PackageType::kQuran:
      required = kQuranRequired;
      n = 5;
      break;
    case PackageType::kBook:
      required = kBookRequired;
      n = 5;
      break;
    case PackageType::kTranslation:
    case PackageType::kTafsir:
      required = kTranslationRequired;
      n = 3;
      break;
    default:
      break;
  }
  for (uint8_t i = 0; i < n; ++i) {
    if (section(required[i]) == nullptr) return Error::kMissingSection;
  }
  return Error::kOk;
}

Error Reader::requireFontSectionsIfShaped() const {
  // Rule 15. A package that claims its WORD_INDEX text spans are shaped glyph
  // runs (see kFlagShapedTextData in qpk_format.h) has to actually carry the
  // atlas and bitmaps those runs reference -- otherwise a reader would only
  // discover the missing sections the first time it tried to render a word.
  if ((header_.flags & kFlagShapedTextData) == 0) return Error::kOk;
  if (section(SectionId::kFontMetadata) == nullptr) {
    return Error::kFontSectionsRequired;
  }
  if (section(SectionId::kAssets) == nullptr) {
    return Error::kFontSectionsRequired;
  }
  return Error::kOk;
}

Error Reader::requireLayoutIfHasWordLayout() const {
  // Rule 16. kFlagHasWordLayout's own documented meaning (qpk_format.h) is
  // "WORD_INDEX + LAYOUT_DATA present" -- a package claiming word-level
  // layout has to actually carry the line geometry, not just word boxes.
  if ((header_.flags & kFlagHasWordLayout) == 0) return Error::kOk;
  if (section(SectionId::kLayoutData) == nullptr) {
    return Error::kLayoutSectionRequired;
  }
  return Error::kOk;
}

// --- checksums --------------------------------------------------------------

Error Reader::crcRange(uint64_t offset, uint64_t length, uint32_t* out) const {
  uint32_t crc = kCrc32Init;
  uint8_t* const chunk = g_crc_chunk;
  uint64_t done = 0;
  while (done < length) {
    const uint64_t remaining = length - done;
    const uint32_t n = remaining < kCrcChunkBytes
                           ? static_cast<uint32_t>(remaining)
                           : kCrcChunkBytes;
    if (!file_->readExact(offset + done, chunk, n)) return Error::kIoError;
    crc = Crc32Update(crc, chunk, n);
    done += n;
  }
  *out = Crc32Finish(crc);
  return Error::kOk;
}

Error Reader::verifyFixedIndexChecksums() const {
  // Rule 13, eager half: only the fixed-size index sections. Blobs
  // (TEXT_DATA and friends) are deliberately left to verifyPayload() at
  // install time -- reading TEXT_DATA here would be the full scan the design
  // exists to avoid.
  for (uint8_t i = 0; i < section_count_; ++i) {
    const SectionEntry& e = sections_[i];
    if (!IsFixedIndexSection(e.id)) continue;
    uint32_t actual = 0;
    const Error error = crcRange(e.offset, e.length, &actual);
    if (error != Error::kOk) return error;
    if (actual != e.crc32) return Error::kSectionChecksum;
  }
  return Error::kOk;
}

Error Reader::verifySection(SectionId id) const {
  if (file_ == nullptr) return Error::kNotOpen;
  const SectionEntry* e = section(id);
  if (e == nullptr) return Error::kNoSuchSection;
  uint32_t actual = 0;
  const Error error = crcRange(e->offset, e->length, &actual);
  if (error != Error::kOk) return error;
  return actual == e->crc32 ? Error::kOk : Error::kSectionChecksum;
}

Error Reader::verifyPayload() const {
  if (file_ == nullptr) return Error::kNotOpen;
  uint32_t actual = 0;
  const Error error = crcRange(header_.header_size,
                               header_.package_size - header_.header_size,
                               &actual);
  if (error != Error::kOk) return error;
  return actual == header_.payload_crc32 ? Error::kOk : Error::kSectionChecksum;
}

// --- index consistency ------------------------------------------------------

Error Reader::checkIndexConsistency() const {
  const SectionEntry* surahs = section(SectionId::kSurahIndex);
  const SectionEntry* ayahs = section(SectionId::kAyahIndex);
  const SectionEntry* words = section(SectionId::kWordIndex);
  const SectionEntry* pages = section(SectionId::kPageIndex);
  const SectionEntry* text = section(SectionId::kTextData);

  uint8_t buffer[kMaxRecordSize];

  // Rule 14, first clause. SURAH_INDEX is at most 114 records, and every
  // Quran lookup starts here, so it is checked in full rather than sampled.
  if (surahs != nullptr && ayahs != nullptr) {
    for (uint32_t i = 0; i < surahs->count; ++i) {
      const Error error = readRecordBytes(*surahs, i, buffer);
      if (error != Error::kOk) return error;
      SurahRecord s;
      DecodeSurah(buffer, &s);
      if (!WithinBounds(s.first_ayah_index, s.ayah_count, ayahs->count)) {
        return Error::kIndexInconsistent;
      }
      if (text != nullptr &&
          !WithinBounds(s.name_offset, s.name_length, text->length)) {
        return Error::kIndexInconsistent;
      }
    }
  }

  // Remaining clauses are sampled at the edges: AYAH_INDEX alone is ~150 KB
  // for a full mushaf, and every accessor re-checks the record it actually
  // reads, so a full sweep here would buy little for a lot of SD traffic.
  if (ayahs != nullptr && ayahs->count > 0) {
    const uint32_t probes[2] = {0, ayahs->count - 1};
    for (uint32_t p = 0; p < 2; ++p) {
      const Error error = readRecordBytes(*ayahs, probes[p], buffer);
      if (error != Error::kOk) return error;
      AyahRecord a;
      DecodeAyah(buffer, &a);
      if (text != nullptr &&
          !WithinBounds(a.text_offset, a.text_length, text->length)) {
        return Error::kIndexInconsistent;
      }
      if (words != nullptr &&
          !WithinBounds(a.first_word_index, a.word_count, words->count)) {
        return Error::kIndexInconsistent;
      }
    }
  }

  // METADATA is walked in full: the section is small, and validating it here
  // is what lets metadata()'s `false` mean "absent" rather than "absent or
  // malformed". docs/qpk-format.md section 6.
  const SectionEntry* metadata_section = section(SectionId::kMetadata);
  if (metadata_section != nullptr) {
    uint64_t position = 0;
    while (position + 4 <= metadata_section->length) {
      uint8_t head[4];
      if (!file_->readExact(metadata_section->offset + position, head, 4)) {
        return Error::kIoError;
      }
      const uint16_t value_length = Read16(head + 2);
      const uint64_t value_position = position + 4;
      if (!WithinBounds(value_position, value_length,
                        metadata_section->length)) {
        return Error::kIndexInconsistent;
      }
      const uint64_t next = (value_position + value_length + 3u) & ~3ull;
      if (next <= position) return Error::kIndexInconsistent;
      position = next;
    }
    // Fewer than 4 trailing bytes are permitted padding.
  }

  // PAGE_INDEX must be numbered 1..count so that getPage() can index it
  // directly.
  if (pages != nullptr && pages->count > 0) {
    PageRecord first;
    PageRecord last;
    Error error = readRecordBytes(*pages, 0, buffer);
    if (error != Error::kOk) return error;
    DecodePage(buffer, &first);
    error = readRecordBytes(*pages, pages->count - 1, buffer);
    if (error != Error::kOk) return error;
    DecodePage(buffer, &last);
    if (first.page_number != 1 || last.page_number != pages->count) {
      return Error::kIndexInconsistent;
    }
  }
  return Error::kOk;
}

// --- section lookup ---------------------------------------------------------

const SectionEntry* Reader::section(SectionId id) const {
  for (uint8_t i = 0; i < section_count_; ++i) {
    if (sections_[i].id == id) return &sections_[i];
  }
  return nullptr;
}

const SectionEntry* Reader::sectionAt(uint8_t index) const {
  if (index >= section_count_) return nullptr;
  return &sections_[index];
}

uint32_t Reader::recordCount(SectionId id) const {
  const SectionEntry* e = section(id);
  return e == nullptr ? 0u : e->count;
}

// --- record reads -----------------------------------------------------------

Error Reader::readRecordBytes(const SectionEntry& entry, uint32_t index,
                              uint8_t* buffer) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (entry.record_size == 0) return Error::kNoSuchSection;
  // `buffer` is kMaxRecordSize at every call site and `record_size` comes off
  // the file. validateSectionTable() pins the stride for every known section,
  // but that guard lives in another function and sectionAt() hands out unknown
  // entries publicly -- so check it here, next to the buffer it protects.
  if (entry.record_size > kMaxRecordSize) return Error::kSectionSizeMismatch;
  if (index >= entry.count) return Error::kOutOfRange;
  const uint64_t offset =
      entry.offset + static_cast<uint64_t>(index) * entry.record_size;
  if (!file_->readExact(offset, buffer, entry.record_size)) {
    return Error::kIoError;
  }
  return Error::kOk;
}

Error Reader::getSurah(uint16_t surah_id, SurahRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kSurahIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  if (surah_id == 0 || surah_id > e->count) return Error::kOutOfRange;

  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, surah_id - 1u, buffer);
  if (error != Error::kOk) return error;
  DecodeSurah(buffer, out);
  // SURAH_INDEX[id-1] must actually be surah `id`; anything else means the
  // direct-access contract is broken.
  if (out->surah_id != surah_id) return Error::kIndexInconsistent;
  return Error::kOk;
}

Error Reader::getAyahByIndex(uint32_t index, AyahRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kAyahIndex);
  if (e == nullptr) return Error::kNoSuchSection;

  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, index, buffer);
  if (error != Error::kOk) return error;
  DecodeAyah(buffer, out);
  return Error::kOk;
}

Error Reader::getAyah(uint16_t surah_id, uint16_t ayah_number,
                      AyahRecord* out) const {
  SurahRecord surah;
  const Error error = getSurah(surah_id, &surah);
  if (error != Error::kOk) return error;
  if (ayah_number == 0 || ayah_number > surah.ayah_count) {
    return Error::kOutOfRange;
  }
  const uint32_t index = surah.first_ayah_index + (ayah_number - 1u);
  const Error read_error = getAyahByIndex(index, out);
  if (read_error != Error::kOk) return read_error;
  if (out->surah_id != surah_id || out->ayah_number != ayah_number) {
    return Error::kIndexInconsistent;
  }
  return Error::kOk;
}

Error Reader::getWordByIndex(uint32_t index, WordRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kWordIndex);
  if (e == nullptr) return Error::kNoSuchSection;

  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, index, buffer);
  if (error != Error::kOk) return error;
  DecodeWord(buffer, out);
  return Error::kOk;
}

Error Reader::getWord(uint16_t surah_id, uint16_t ayah_number,
                      uint16_t word_id, WordRecord* out) const {
  AyahRecord ayah;
  const Error error = getAyah(surah_id, ayah_number, &ayah);
  if (error != Error::kOk) return error;
  if (word_id >= ayah.word_count) return Error::kOutOfRange;
  return getWordByIndex(ayah.first_word_index + word_id, out);
}

Error Reader::getPage(uint16_t page_number, PageRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kPageIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  if (page_number == 0 || page_number > e->count) return Error::kOutOfRange;

  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, page_number - 1u, buffer);
  if (error != Error::kOk) return error;
  DecodePage(buffer, out);
  if (out->page_number != page_number) return Error::kIndexInconsistent;
  return Error::kOk;
}

// Juz, hizb and rub share a record layout and an "id is 1-based, record N is
// at index N-1" access rule.
Error Reader::getRange(SectionId section_id, uint16_t id,
                       RangeRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(section_id);
  if (e == nullptr) return Error::kNoSuchSection;
  if (id == 0 || id > e->count) return Error::kOutOfRange;

  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, id - 1u, buffer);
  if (error != Error::kOk) return error;
  DecodeRange(buffer, out);
  if (out->id != id) return Error::kIndexInconsistent;
  return Error::kOk;
}

Error Reader::getJuz(uint16_t id, RangeRecord* out) const {
  return getRange(SectionId::kJuzIndex, id, out);
}

Error Reader::getHizb(uint16_t id, RangeRecord* out) const {
  return getRange(SectionId::kHizbIndex, id, out);
}

Error Reader::getRub(uint16_t id, RangeRecord* out) const {
  return getRange(SectionId::kRubIndex, id, out);
}

Error Reader::getSajdahByIndex(uint32_t index, SajdahRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kSajdahIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, index, buffer);
  if (error != Error::kOk) return error;
  DecodeSajdah(buffer, out);
  return Error::kOk;
}

Error Reader::getChapter(uint32_t index, ChapterRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kChapterIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, index, buffer);
  if (error != Error::kOk) return error;
  DecodeChapter(buffer, out);
  return Error::kOk;
}

Error Reader::getBookSection(uint32_t index, BookSectionRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kSectionIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, index, buffer);
  if (error != Error::kOk) return error;
  DecodeBookSection(buffer, out);
  return Error::kOk;
}

Error Reader::getTextRecord(uint32_t index, TextRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kTextIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, index, buffer);
  if (error != Error::kOk) return error;
  DecodeTextRecord(buffer, out);
  return Error::kOk;
}

Error Reader::getTranslation(uint32_t ayah_index,
                             TranslationRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kTranslationIndex);
  if (e == nullptr) return Error::kNoSuchSection;
  uint8_t buffer[kMaxRecordSize];
  const Error error = readRecordBytes(*e, ayah_index, buffer);
  if (error != Error::kOk) return error;
  DecodeTranslation(buffer, out);
  return Error::kOk;
}

// --- payload ----------------------------------------------------------------

int32_t Reader::readBlob(SectionId section_id, uint32_t offset, uint32_t length,
                         void* dst, uint32_t capacity) const {
  if (file_ == nullptr || dst == nullptr) return -1;
  const SectionEntry* e = section(section_id);
  if (e == nullptr) return -1;
  if (!WithinBounds(offset, length, e->length)) return -1;

  const uint32_t n = length < capacity ? length : capacity;
  if (n == 0) return 0;
  if (!file_->readExact(e->offset + offset, dst, n)) return -1;
  return static_cast<int32_t>(n);
}

Error Reader::readCover(uint8_t* dst, uint32_t capacity) const {
  const SectionEntry* e = section(SectionId::kCover);
  if (e == nullptr) return Error::kNoSuchSection;
  if (e->length != kCoverBytes || dst == nullptr || capacity < kCoverPixelBytes) {
    return Error::kSectionSizeMismatch;
  }
  uint8_t header[kCoverHeaderBytes];
  if (readBlob(SectionId::kCover, 0, kCoverHeaderBytes, header, sizeof(header)) !=
      static_cast<int32_t>(kCoverHeaderBytes)) {
    return Error::kSectionOutOfRange;
  }
  const uint16_t width = static_cast<uint16_t>(header[4] | (header[5] << 8));
  const uint16_t height = static_cast<uint16_t>(header[6] | (header[7] << 8));
  if (header[0] != 'Q' || header[1] != 'C' || header[2] != 'V' || header[3] != '1' ||
      width != kCoverWidth || height != kCoverHeight || header[8] != 2) {
    return Error::kSectionSizeMismatch;
  }
  if (readBlob(SectionId::kCover, kCoverHeaderBytes, kCoverPixelBytes, dst, capacity) !=
      static_cast<int32_t>(kCoverPixelBytes)) {
    return Error::kSectionOutOfRange;
  }
  return Error::kOk;
}

Error ReadCoverFromFile(hal::IFile* file, uint8_t* dst, uint32_t capacity) {
  if (file == nullptr || !file->valid() || dst == nullptr || capacity < kCoverPixelBytes) {
    return Error::kOutOfRange;
  }
  uint8_t header[kHeaderSize];
  if (!file->readExact(0, header, kHeaderSize)) return Error::kIoError;
  if (header[0] != kMagic0 || header[1] != kMagic1 || header[2] != kMagic2 ||
      header[3] != kMagic3) {
    return Error::kBadMagic;
  }
  const uint16_t count = Read16(header + 10);
  const uint32_t table = Read32(header + 44);
  if (count == 0 || count > kMaxSections) return Error::kBadSectionTable;

  const uint64_t size = file->size();
  uint8_t entry[kSectionEntrySize];
  for (uint16_t i = 0; i < count; ++i) {
    const uint64_t at = static_cast<uint64_t>(table) + static_cast<uint64_t>(i) * kSectionEntrySize;
    if (!file->readExact(at, entry, kSectionEntrySize)) return Error::kIoError;
    if (Read16(entry) != static_cast<uint16_t>(SectionId::kCover)) continue;

    const uint64_t offset = Read64(entry + 4);
    const uint64_t length = Read64(entry + 12);
    if (length != kCoverBytes || !WithinBounds(offset, length, size)) {
      return Error::kSectionSizeMismatch;
    }
    uint8_t cover[kCoverHeaderBytes];
    if (!file->readExact(offset, cover, kCoverHeaderBytes)) return Error::kIoError;
    if (cover[0] != 'Q' || cover[1] != 'C' || cover[2] != 'V' || cover[3] != '1' ||
        Read16(cover + 4) != kCoverWidth || Read16(cover + 6) != kCoverHeight || cover[8] != 2) {
      return Error::kSectionSizeMismatch;
    }
    if (!file->readExact(offset + kCoverHeaderBytes, dst, kCoverPixelBytes)) {
      return Error::kIoError;
    }
    return Error::kOk;
  }
  return Error::kNoSuchSection;
}

bool UnpackBits(const uint8_t* src, uint32_t src_len, uint8_t* dst, uint32_t dst_len) {
  if ((src == nullptr && src_len > 0) || (dst == nullptr && dst_len > 0)) return false;
  uint32_t i = 0;
  uint32_t o = 0;
  while (i < src_len) {
    const uint8_t n = src[i++];
    if (n < 128) {
      const uint32_t count = static_cast<uint32_t>(n) + 1;
      if (count > src_len - i || count > dst_len - o) return false;
      for (uint32_t k = 0; k < count; ++k) dst[o + k] = src[i + k];
      i += count;
      o += count;
    } else if (n > 128) {
      const uint32_t count = 257u - n;
      if (i >= src_len || count > dst_len - o) return false;
      const uint8_t value = src[i++];
      for (uint32_t k = 0; k < count; ++k) dst[o + k] = value;
      o += count;
    }
  }
  return o == dst_len;
}

uint32_t Reader::pageImageCount() const {
  const SectionEntry* index_section = section(SectionId::kPageImageIndex);
  if (index_section == nullptr || section(SectionId::kPageImageData) == nullptr) return 0;
  return index_section->count;
}

Error Reader::getPageImage(uint32_t index, PageImageRecord* out) const {
  if (out == nullptr || file_ == nullptr) return Error::kOutOfRange;
  const SectionEntry* index_section = section(SectionId::kPageImageIndex);
  const SectionEntry* data_section = section(SectionId::kPageImageData);
  if (index_section == nullptr || data_section == nullptr) return Error::kNoSuchSection;
  if (index >= index_section->count) return Error::kOutOfRange;

  uint8_t raw[kPageImageRecordSize];
  if (!file_->readExact(index_section->offset +
                            static_cast<uint64_t>(index) * kPageImageRecordSize,
                        raw, sizeof(raw))) {
    return Error::kSectionOutOfRange;
  }
  out->offset = Read32(raw);
  out->length = Read32(raw + 4);
  out->width = Read16(raw + 8);
  out->height = Read16(raw + 10);
  if (!WithinBounds(out->offset, out->length, data_section->length)) {
    return Error::kSectionOutOfRange;
  }
  return Error::kOk;
}

Error Reader::readPageImage(uint32_t index, uint8_t* scratch, uint32_t scratch_capacity,
                            uint8_t* dst, uint32_t dst_capacity) const {
  PageImageRecord record;
  const Error err = getPageImage(index, &record);
  if (err != Error::kOk) return err;

  const uint32_t bytes = ((static_cast<uint32_t>(record.width) + 7u) / 8u) * record.height;
  if (scratch == nullptr || dst == nullptr || bytes == 0 || bytes > dst_capacity ||
      record.length > scratch_capacity) {
    return Error::kSectionSizeMismatch;
  }
  if (readBlob(SectionId::kPageImageData, record.offset, record.length, scratch,
               scratch_capacity) != static_cast<int32_t>(record.length)) {
    return Error::kSectionOutOfRange;
  }
  if (!UnpackBits(scratch, record.length, dst, bytes)) return Error::kSectionSizeMismatch;
  return Error::kOk;
}

int32_t Reader::readText(uint32_t text_offset, uint32_t length, void* dst,
                         uint32_t capacity) const {
  return readBlob(SectionId::kTextData, text_offset, length, dst, capacity);
}

int32_t Reader::readTranslationText(uint32_t text_offset, uint32_t length,
                                    void* dst, uint32_t capacity) const {
  return readBlob(SectionId::kTranslationData, text_offset, length, dst,
                  capacity);
}

bool Reader::metadata(MetadataKey key, void* dst, uint32_t capacity,
                      uint32_t* out_length) const {
  if (file_ == nullptr) return false;
  const SectionEntry* e = section(SectionId::kMetadata);
  if (e == nullptr) return false;

  uint64_t position = 0;
  while (position + 4 <= e->length) {
    uint8_t head[4];
    if (!file_->readExact(e->offset + position, head, 4)) return false;
    const uint16_t record_key = Read16(head);
    const uint16_t value_length = Read16(head + 2);
    const uint64_t value_position = position + 4;

    // A record that would run past the section end makes the section
    // malformed; stop rather than return a truncated value.
    if (!WithinBounds(value_position, value_length, e->length)) return false;

    if (record_key == static_cast<uint16_t>(key)) {
      if (out_length != nullptr) *out_length = value_length;
      if (dst == nullptr || capacity == 0) return true;
      const uint32_t n = value_length < capacity ? value_length : capacity;
      if (n != 0 && !file_->readExact(e->offset + value_position, dst, n)) {
        return false;
      }
      return true;
    }

    // Next record starts at the next 4-byte boundary.
    const uint64_t next = (value_position + value_length + 3u) & ~3ull;
    if (next <= position) return false;  // no forward progress: malformed
    position = next;
  }
  return false;
}

Error Reader::getLayoutHeader(LayoutHeader* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kLayoutData);
  if (e == nullptr) return Error::kNoSuchSection;
  if (e->length < kLayoutHeaderSize) return Error::kOutOfRange;

  uint8_t b[kLayoutHeaderSize];
  if (!file_->readExact(e->offset, b, kLayoutHeaderSize)) return Error::kIoError;
  DecodeLayoutHeader(b, out);

  // The line records the header claims must actually fit -- checked here,
  // once, rather than leaving every getLine() caller to discover a bad
  // line_count_total one failed read at a time (rule 11 applies the same
  // discipline to fixed-size sections' own count * record_size).
  const uint64_t needed = static_cast<uint64_t>(out->line_count_total) * kLineRecordSize;
  if (!WithinBounds(out->line_records_offset, needed, e->length)) {
    return Error::kOutOfRange;
  }
  return Error::kOk;
}

Error Reader::getLine(uint32_t index, LineRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kLayoutData);
  if (e == nullptr) return Error::kNoSuchSection;

  LayoutHeader layout;
  const Error error = getLayoutHeader(&layout);
  if (error != Error::kOk) return error;
  if (index >= layout.line_count_total) return Error::kOutOfRange;

  // getLayoutHeader() already proved line_records_offset + line_count_total *
  // kLineRecordSize <= e->length, so this read cannot run past the section.
  const uint64_t at = e->offset +
                      static_cast<uint64_t>(layout.line_records_offset) +
                      static_cast<uint64_t>(index) * kLineRecordSize;
  uint8_t b[kLineRecordSize];
  if (!file_->readExact(at, b, kLineRecordSize)) return Error::kIoError;
  DecodeLine(b, out);
  return Error::kOk;
}

bool Reader::ayahMarkerGlyphs(uint16_t* first_glyph_id,
                              uint16_t* count) const {
  if (first_glyph_id == nullptr || count == nullptr) return false;

  // Longest legitimate value is "65535:65535" -- 11 bytes. A longer one is
  // malformed, and the truncation check below catches it rather than parsing
  // a prefix of it into a plausible-looking pair.
  char raw[16];
  uint32_t length = 0;
  if (!metadata(MetadataKey::kAyahMarkerGlyphs, raw, sizeof(raw), &length)) {
    return false;
  }
  if (length == 0 || length > sizeof(raw)) return false;

  // Hand-parsed rather than sscanf'd: two unsigned decimals separated by one
  // ':' is the whole grammar, and this way a trailing byte or a missing field
  // is a rejection instead of a partially-filled pair.
  uint32_t value[2] = {0, 0};
  uint8_t field = 0;
  bool digits_seen = false;
  for (uint32_t i = 0; i < length; ++i) {
    const char c = raw[i];
    if (c >= '0' && c <= '9') {
      value[field] = value[field] * 10u + static_cast<uint32_t>(c - '0');
      if (value[field] > 0xffffu) return false;
      digits_seen = true;
      continue;
    }
    if (c == ':' && field == 0 && digits_seen) {
      field = 1;
      digits_seen = false;
      continue;
    }
    return false;
  }
  if (field != 1 || !digits_seen) return false;
  if (value[1] == 0) return false;  // a range covering no ayah is no range

  // The last id must still be addressable. A range that wraps past 65535
  // would have getGlyph() searching for ids the writer never minted.
  if (value[0] + value[1] - 1u > 0xffffu) return false;

  *first_glyph_id = static_cast<uint16_t>(value[0]);
  *count = static_cast<uint16_t>(value[1]);
  return true;
}

Error Reader::findAyahByWordIndex(uint32_t word_index,
                                  uint32_t* out_ayah_index,
                                  AyahRecord* out) const {
  if (out == nullptr || out_ayah_index == nullptr) return Error::kOutOfRange;
  const uint32_t total = recordCount(SectionId::kAyahIndex);
  if (total == 0) return Error::kNoSuchSection;

  uint32_t low = 0;
  uint32_t high = total;  // exclusive
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    AyahRecord probe;
    const Error error = getAyahByIndex(mid, &probe);
    if (error != Error::kOk) return error;
    if (probe.first_word_index <= word_index) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low == 0) return Error::kOutOfRange;

  *out_ayah_index = low - 1;
  return getAyahByIndex(low - 1, out);
}

Error Reader::findLineByWordIndex(uint32_t word_index,
                                  uint32_t* out_line_index,
                                  LineRecord* out) const {
  if (out == nullptr || out_line_index == nullptr) return Error::kOutOfRange;

  LayoutHeader layout;
  const Error error = getLayoutHeader(&layout);
  if (error != Error::kOk) return error;
  if (layout.line_count_total == 0) return Error::kOutOfRange;

  // Upper bound, then step back one: finds the last record whose
  // first_word_index is <= word_index. A word belongs to the line that
  // starts at or before it, because line spans are contiguous -- there is no
  // per-line word COUNT in the record, and there does not need to be.
  uint32_t low = 0;
  uint32_t high = layout.line_count_total;  // exclusive
  while (low < high) {
    const uint32_t mid = low + (high - low) / 2;
    LineRecord probe;
    const Error probe_error = getLine(mid, &probe);
    if (probe_error != Error::kOk) return probe_error;
    if (probe.first_word_index <= word_index) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low == 0) {
    // Every line starts after this word: the layout does not cover it.
    return Error::kOutOfRange;
  }

  *out_line_index = low - 1;
  return getLine(low - 1, out);
}

Error Reader::getGlyph(uint16_t glyph_id, GlyphRecord* out) const {
  if (file_ == nullptr) return Error::kNotOpen;
  if (out == nullptr) return Error::kOutOfRange;
  const SectionEntry* e = section(SectionId::kFontMetadata);
  if (e == nullptr) return Error::kNoSuchSection;

  if (glyph_index_ != nullptr) {
    // Lower bound, so a duplicated id resolves to its first record in table
    // order -- the same one the scan below would stop at.
    uint32_t lo = 0;
    uint32_t hi = glyph_index_count_;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (glyph_index_[mid].glyph_id < glyph_id) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < glyph_index_count_ && glyph_index_[lo].glyph_id == glyph_id) {
      *out = glyph_index_[lo];
      return Error::kOk;
    }
    return Error::kOutOfRange;
  }

  // Not indexable by value like SURAH_INDEX: glyph ids are assigned by
  // whatever desktop tool did the shaping and are neither dense nor
  // guaranteed sorted.
  uint8_t buffer[kMaxRecordSize];
  for (uint32_t i = 0; i < e->count; ++i) {
    const Error error = readRecordBytes(*e, i, buffer);
    if (error != Error::kOk) return error;
    GlyphRecord candidate;
    DecodeGlyph(buffer, &candidate);
    if (candidate.glyph_id == glyph_id) {
      *out = candidate;
      return Error::kOk;
    }
  }
  return Error::kOutOfRange;
}

int32_t Reader::readAsset(uint32_t offset, uint32_t length, void* dst,
                          uint32_t capacity) const {
  return readBlob(SectionId::kAssets, offset, length, dst, capacity);
}

uint32_t Reader::buildGlyphIndex(GlyphRecord* storage, uint32_t capacity) {
  glyph_index_ = nullptr;
  glyph_index_count_ = 0;
  if (file_ == nullptr || storage == nullptr) return 0;
  const SectionEntry* e = section(SectionId::kFontMetadata);
  if (e == nullptr || e->count == 0 || e->count > capacity) return 0;
  if (e->record_size != kGlyphRecordSize) return 0;

  // The section is one contiguous run of records, so read it a chunk at a
  // time: a few dozen reads instead of one per record.
  constexpr uint32_t kChunkRecords = 64;
  uint8_t chunk[kChunkRecords * kGlyphRecordSize];
  uint32_t done = 0;
  while (done < e->count) {
    const uint32_t remaining = e->count - done;
    const uint32_t n = remaining < kChunkRecords ? remaining : kChunkRecords;
    const uint64_t offset =
        e->offset + static_cast<uint64_t>(done) * kGlyphRecordSize;
    if (!file_->readExact(offset, chunk, n * kGlyphRecordSize)) return 0;

    for (uint32_t i = 0; i < n; ++i) {
      GlyphRecord glyph;
      DecodeGlyph(chunk + i * kGlyphRecordSize, &glyph);
      // Insertion sort: stable, allocation-free, and linear on the usual
      // already-sorted atlas. Records arrive in table order, so an equal id
      // lands after the ones already placed.
      uint32_t j = done + i;
      while (j > 0 && storage[j - 1].glyph_id > glyph.glyph_id) {
        storage[j] = storage[j - 1];
        --j;
      }
      storage[j] = glyph;
    }
    done += n;
  }

  glyph_index_ = storage;
  glyph_index_count_ = e->count;
  return glyph_index_count_;
}

}  // namespace qpk
