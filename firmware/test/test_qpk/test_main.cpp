// Host unit tests for the QPK1 parser.
//
// Structure: build a valid synthetic package, then damage exactly one thing
// and assert the parser reports exactly the rule that broke. Every validation
// rule in docs/qpk-format.md section 11 has a case here, plus the direct-access
// paths from section 10.

#include <unity.h>

#include <stdio.h>

#include <string>
#include <vector>

#include "qpk/crc32.h"
#include "qpk/memory_file.h"
#include "qpk/qpk_reader.h"
#include "qpk_test_package.h"

// Generated and committed by packages/qpk-format/scripts/build-fixture.ts.
// Included unconditionally on purpose: making it optional would let the
// cross-language coverage disappear silently.
#include "golden_fixture.h"

#include <deque>
#include <string>

#include "qpk/translation_text.h"

using qpktest::Bytes;

namespace {

int kErr(qpk::Error e) { return static_cast<int>(e); }

// Opens a package held in memory and returns the parser's verdict.
//
// The bytes are copied somewhere that outlives the call. Callers pass
// builder.build() straight in, and a MemoryFile pointing into that temporary
// read freed memory the moment a test looked past open(): zeros decoded as
// "no such glyph" and a zero line count. That was the three long-standing
// "Expected 0 Was 21" / "Expected 21 Was 0" failures (fixed 2026-09-14).
qpk::Error OpenBytes(const Bytes& file, qpk::Reader* reader,
                     qpk::MemoryFile* backing) {
  static std::deque<Bytes> kept;  // deque: element addresses never move
  kept.push_back(file);
  backing->reset(kept.back().data(), kept.back().size());
  return reader->open(backing);
}

qpk::Error OpenVerdict(const Bytes& file) {
  qpk::MemoryFile memory;
  qpk::Reader reader;
  return OpenBytes(file, &reader, &memory);
}

std::string TextOf(const qpk::Reader& reader, uint32_t offset,
                   uint32_t length) {
  char buffer[128];
  const int32_t n = reader.readText(offset, length, buffer, sizeof(buffer));
  if (n < 0) return "<error>";
  return std::string(buffer, static_cast<size_t>(n));
}

// --- page pictures (docs/qpk-format.md 9d) ------------------------------------

void PushLe(Bytes* out, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void test_unpack_bits_decodes_runs_and_literals_exactly() {
  // A 3-byte literal, then 0xAB repeated 5 times, then a 1-byte literal.
  const uint8_t packed[] = {2, 1, 2, 3, 252, 0xAB, 0, 9};
  uint8_t out[9] = {0};
  TEST_ASSERT_TRUE(qpk::UnpackBits(packed, sizeof(packed), out, sizeof(out)));
  const uint8_t expected[] = {1, 2, 3, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 9};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));

  uint8_t short_out[8];
  TEST_ASSERT_FALSE(qpk::UnpackBits(packed, sizeof(packed), short_out, sizeof(short_out)));
  uint8_t long_out[10];
  TEST_ASSERT_FALSE(qpk::UnpackBits(packed, sizeof(packed), long_out, sizeof(long_out)));
  const uint8_t truncated[] = {4, 1, 2};
  uint8_t any[5];
  TEST_ASSERT_FALSE(qpk::UnpackBits(truncated, sizeof(truncated), any, sizeof(any)));
}

void test_page_pictures_read_back_page_by_page() {
  // Two 16x2 pages (4 bytes each): all white, then all black.
  Bytes data;
  const uint8_t white[] = {255, 0};         // 0 repeated 2 bytes... per row
  const uint8_t black[] = {253, 0xFF};      // 0xFF repeated 4 bytes
  data.insert(data.end(), white, white + 2);
  data.insert(data.end(), white, white + 2);  // second row of zeros
  data.insert(data.end(), black, black + 2);

  Bytes index;
  PushLe(&index, 0, 4);  // page 1: offset 0, 4 bytes, 16 x 2
  PushLe(&index, 4, 4);
  PushLe(&index, 16, 2);
  PushLe(&index, 2, 2);
  PushLe(&index, 0, 4);
  PushLe(&index, 4, 4);  // page 2: offset 4, 2 bytes, 16 x 2
  PushLe(&index, 2, 4);
  PushLe(&index, 16, 2);
  PushLe(&index, 2, 2);
  PushLe(&index, 0, 4);

  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Pictures");
  builder.addSection(qpk::SectionId::kPageImageIndex, index, 2);
  builder.addSection(qpk::SectionId::kPageImageData, data, 0);
  const Bytes file = builder.build();

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory)));
  TEST_ASSERT_EQUAL_UINT32(2, reader.pageImageCount());

  uint8_t scratch[16];
  uint8_t page[4];
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(reader.readPageImage(0, scratch, sizeof(scratch), page, sizeof(page))));
  const uint8_t zeros[4] = {0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, page, 4);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(reader.readPageImage(1, scratch, sizeof(scratch), page, sizeof(page))));
  const uint8_t ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(ones, page, 4);

  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.readPageImage(2, scratch, sizeof(scratch), page, sizeof(page))));
  uint8_t small[3];
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionSizeMismatch),
                        kErr(reader.readPageImage(1, scratch, sizeof(scratch), small, sizeof(small))));
}

void test_a_package_without_page_pictures_reports_none() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Text only");
  const Bytes file = builder.build();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory)));
  TEST_ASSERT_EQUAL_UINT32(0, reader.pageImageCount());
  qpk::PageImageRecord record;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection), kErr(reader.getPageImage(0, &record)));
}

// --- covers without opening (qpk::ReadCoverFromFile) --------------------------

Bytes CoverSection(char first_magic_byte) {
  Bytes cover = {static_cast<uint8_t>(first_magic_byte), 'C', 'V', '1'};
  qpktest::Append16(&cover, qpk::kCoverWidth);
  qpktest::Append16(&cover, qpk::kCoverHeight);
  cover.push_back(2);
  for (int i = 0; i < 7; ++i) cover.push_back(0);
  for (uint32_t i = 0; i < qpk::kCoverPixelBytes; ++i) cover.push_back(static_cast<uint8_t>(i));
  return cover;
}

void test_read_cover_from_file_matches_reader_read_cover() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Covered");
  builder.addSection(qpk::SectionId::kCover, CoverSection('Q'), 0);
  const Bytes file = builder.build();

  qpk::MemoryFile memory;
  memory.reset(file.data(), file.size());
  std::vector<uint8_t> quick(qpk::kCoverPixelBytes);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(qpk::ReadCoverFromFile(&memory, quick.data(),
                                                    static_cast<uint32_t>(quick.size()))));
  TEST_ASSERT_EQUAL_UINT8(5, quick[5]);

  qpk::MemoryFile memory2;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory2)));
  std::vector<uint8_t> opened(qpk::kCoverPixelBytes);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(reader.readCover(opened.data(), static_cast<uint32_t>(opened.size()))));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(opened.data(), quick.data(), qpk::kCoverPixelBytes);
}

void test_read_cover_from_file_reports_missing_and_malformed_covers() {
  std::vector<uint8_t> pixels(qpk::kCoverPixelBytes);

  qpktest::PackageBuilder plain;
  plain.setType(qpk::PackageType::kUnknown);
  plain.addMetadata(qpk::MetadataKey::kTitle, "No cover");
  const Bytes plain_file = plain.build();
  qpk::MemoryFile memory;
  memory.reset(plain_file.data(), plain_file.size());
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(qpk::ReadCoverFromFile(&memory, pixels.data(),
                                                    static_cast<uint32_t>(pixels.size()))));

  qpktest::PackageBuilder broken;
  broken.setType(qpk::PackageType::kUnknown);
  broken.addMetadata(qpk::MetadataKey::kTitle, "Bad cover");
  broken.addSection(qpk::SectionId::kCover, CoverSection('X'), 0);
  const Bytes broken_file = broken.build();
  memory.reset(broken_file.data(), broken_file.size());
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionSizeMismatch),
                        kErr(qpk::ReadCoverFromFile(&memory, pixels.data(),
                                                    static_cast<uint32_t>(pixels.size()))));
}

// --- translation text (qpk::AppendTranslationVerses) ----------------------------

void test_translation_verses_are_numbered_folded_and_bounded() {
  const std::string first = "In the name of Allah\xE2\x80\x94the Merciful";  // an em dash
  const std::string second = "\xE2\x80\x9CPraise\xE2\x80\x9D";               // curly quotes
  Bytes data(first.begin(), first.end());
  data.insert(data.end(), second.begin(), second.end());
  Bytes index;
  PushLe(&index, 0, 4);
  PushLe(&index, static_cast<uint32_t>(first.size()), 4);
  PushLe(&index, static_cast<uint32_t>(first.size()), 4);
  PushLe(&index, static_cast<uint32_t>(second.size()), 4);

  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kTranslation);
  builder.addMetadata(qpk::MetadataKey::kTitle, "A test translation");
  builder.addSection(qpk::SectionId::kTranslationIndex, index, 2);
  builder.addSection(qpk::SectionId::kTranslationData, data, 0);
  const Bytes file = builder.build();

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory)));

  char out[256];
  uint32_t n = qpk::AppendTranslationVerses(reader, 0, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("1. In the name of Allah--the Merciful\n\n2. \"Praise\"\n\n",
                           std::string(out, n).c_str());

  // Numbering is within the run, so a surah starting mid-translation begins at 1.
  n = qpk::AppendTranslationVerses(reader, 1, 1, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("1. \"Praise\"\n\n", std::string(out, n).c_str());

  char tiny[10];
  n = qpk::AppendTranslationVerses(reader, 0, 2, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_UINT32(10u, n);  // filled to the brim, not past it
  TEST_ASSERT_EQUAL_MEMORY("1. In the ", tiny, 10);

  TEST_ASSERT_EQUAL_UINT32(0u, qpk::AppendTranslationVerses(reader, 5, 2, out, sizeof(out)));
}

// A translation of `verses` ayahs whose texts are "Verse <n>." end to end.
Bytes BuildNumberedTranslation(uint32_t verses) {
  Bytes data;
  Bytes index;
  for (uint32_t i = 0; i < verses; ++i) {
    const std::string text = "Verse " + std::to_string(i + 1) + ".";
    PushLe(&index, static_cast<uint32_t>(data.size()), 4);
    PushLe(&index, static_cast<uint32_t>(text.size()), 4);
    data.insert(data.end(), text.begin(), text.end());
  }
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kTranslation);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Numbered");
  builder.addSection(qpk::SectionId::kTranslationIndex, index, verses);
  builder.addSection(qpk::SectionId::kTranslationData, data, 0);
  return builder.build();
}

void test_translation_verses_read_in_one_go_match_the_verse_by_verse_text() {
  const Bytes file = BuildNumberedTranslation(700);  // more than one 512-record batch
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory)));

  static char slow[16384];
  static char fast[16384];
  static char scratch[16384];
  const uint32_t slow_n = qpk::AppendTranslationVerses(reader, 3, 690, slow, sizeof(slow));
  const uint32_t fast_n =
      qpk::AppendTranslationVerses(reader, 3, 690, fast, sizeof(fast), scratch, sizeof(scratch));
  TEST_ASSERT_TRUE(slow_n > 0);
  TEST_ASSERT_EQUAL_UINT32(slow_n, fast_n);
  TEST_ASSERT_EQUAL_MEMORY(slow, fast, slow_n);
  TEST_ASSERT_EQUAL_STRING("1. Verse 4.\n\n2. Verse 5.", std::string(fast, 24).c_str());

  // A scratch too small for the span: the verse-by-verse path, same text.
  char tiny_scratch[8];
  const uint32_t fallback_n = qpk::AppendTranslationVerses(reader, 3, 690, fast, sizeof(fast),
                                                           tiny_scratch, sizeof(tiny_scratch));
  TEST_ASSERT_EQUAL_UINT32(slow_n, fallback_n);
  TEST_ASSERT_EQUAL_MEMORY(slow, fast, slow_n);

  // Past the end of the index: only what exists.
  const uint32_t tail_n =
      qpk::AppendTranslationVerses(reader, 698, 10, fast, sizeof(fast), scratch, sizeof(scratch));
  TEST_ASSERT_EQUAL_STRING("1. Verse 699.\n\n2. Verse 700.\n\n", std::string(fast, tail_n).c_str());
}

void test_read_record_range_stops_at_the_section_end_and_the_buffer() {
  const Bytes file = BuildNumberedTranslation(10);
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory)));

  uint8_t records[64];
  TEST_ASSERT_EQUAL_UINT32(4u, reader.readRecordRange(qpk::SectionId::kTranslationIndex, 0, 4,
                                                      records, sizeof(records)));
  qpk::TranslationRecord second;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getTranslation(1, &second)));
  TEST_ASSERT_EQUAL_MEMORY(records + 8, file.data() + reader.section(qpk::SectionId::kTranslationIndex)->offset + 8, 8);
  TEST_ASSERT_EQUAL_UINT32(3u, reader.readRecordRange(qpk::SectionId::kTranslationIndex, 7, 9,
                                                      records, sizeof(records)));
  TEST_ASSERT_EQUAL_UINT32(2u, reader.readRecordRange(qpk::SectionId::kTranslationIndex, 0, 9,
                                                      records, 20));  // 20 bytes: two records
  TEST_ASSERT_EQUAL_UINT32(0u, reader.readRecordRange(qpk::SectionId::kTranslationIndex, 10, 1,
                                                      records, sizeof(records)));
  TEST_ASSERT_EQUAL_UINT32(0u, reader.readRecordRange(qpk::SectionId::kSurahIndex, 0, 1,
                                                      records, sizeof(records)));
}

void test_open_can_leave_the_index_checksums_to_the_caller() {
  Bytes file = BuildNumberedTranslation(10);
  uint64_t index_offset = 0;
  {
    qpk::MemoryFile memory;
    qpk::Reader reader;
    TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenBytes(file, &reader, &memory)));
    index_offset = reader.section(qpk::SectionId::kTranslationIndex)->offset;
  }
  file[static_cast<size_t>(index_offset) + 4] ^= 0x01;  // one index byte, still in bounds

  qpk::MemoryFile memory;
  memory.reset(file.data(), file.size());
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionChecksum), kErr(reader.open(&memory)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.open(&memory, false)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionChecksum), kErr(reader.verifyIndexChecksums()));
  reader.close();
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNotOpen), kErr(reader.verifyIndexChecksums()));
}

}  // namespace

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

void test_crc32_check_value() {
  // The canonical CRC-32/ISO-HDLC check value. The TypeScript writer in
  // packages/qpk-format is tested against the same constant, which is what
  // makes the two implementations comparable at all.
  const char* input = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, qpk::Crc32(input, 9));
}

void test_crc32_is_streamable() {
  const char* input = "123456789";
  uint32_t crc = qpk::kCrc32Init;
  crc = qpk::Crc32Update(crc, input, 4);
  crc = qpk::Crc32Update(crc, input + 4, 5);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, qpk::Crc32Finish(crc));
}

// ---------------------------------------------------------------------------
// A valid package parses, and every direct-access path resolves
// ---------------------------------------------------------------------------

void test_valid_package_opens() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenVerdict(mini.file)));
}

void test_header_fields_round_trip() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));

  const qpk::Header& h = reader.header();
  TEST_ASSERT_EQUAL_UINT16(qpk::kFormatVersion, h.format_version);
  TEST_ASSERT_EQUAL_UINT16(qpk::kHeaderSize, h.header_size);
  TEST_ASSERT_TRUE(h.package_type == qpk::PackageType::kQuran);
  TEST_ASSERT_EQUAL_UINT32(3, h.content_version);
  TEST_ASSERT_EQUAL_UINT32(mini.file.size(), h.package_size);
  TEST_ASSERT_TRUE((h.flags & qpk::kFlagHasWordLayout) != 0);
  TEST_ASSERT_TRUE((h.flags & qpk::kFlagRightToLeft) != 0);
  TEST_ASSERT_EQUAL_UINT8(0x10, h.content_id[0]);
  TEST_ASSERT_EQUAL_UINT8(0x1F, h.content_id[15]);
}

void test_get_surah_resolves_directly() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  qpk::SurahRecord s;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getSurah(2, &s)));
  TEST_ASSERT_EQUAL_UINT16(2, s.surah_id);
  TEST_ASSERT_EQUAL_UINT16(2, s.ayah_count);
  TEST_ASSERT_EQUAL_UINT32(3, s.first_ayah_index);
  TEST_ASSERT_EQUAL_UINT16(2, s.first_page);
  TEST_ASSERT_EQUAL_STRING("PLACEHOLDER-SURAH-TWO",
                           TextOf(reader, s.name_offset, s.name_length).c_str());
}

void test_get_ayah_maps_surah_and_number() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  qpk::AyahRecord a;
  // Surah 2, ayah 2 is the 5th ayah overall: first_ayah_index 3 + (2-1).
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getAyah(2, 2, &a)));
  TEST_ASSERT_EQUAL_UINT16(2, a.surah_id);
  TEST_ASSERT_EQUAL_UINT16(2, a.ayah_number);
  TEST_ASSERT_EQUAL_UINT16(2, a.page);
  TEST_ASSERT_EQUAL_UINT16(2, a.word_count);
  TEST_ASSERT_EQUAL_UINT32(10, a.first_word_index);
  TEST_ASSERT_EQUAL_STRING("PLACEHOLDER-AYAH-2-2",
                           TextOf(reader, a.text_offset, a.text_length).c_str());
}

void test_get_word_resolves_layout() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  // This is the Phase 3 path: an AI response {surah, ayah, word_id} has to
  // land on pixels with no search.
  qpk::WordRecord w;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(reader.getWord(2, 2, 1, &w)));
  TEST_ASSERT_EQUAL_UINT16(mini.word_x[11], w.x);
  TEST_ASSERT_EQUAL_UINT16(400, w.width);  // > 255: the u16 promotion matters
  TEST_ASSERT_EQUAL_STRING("WORD11",
                           TextOf(reader, w.text_offset, w.text_length).c_str());
}

void test_get_page_and_juz() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  qpk::PageRecord p;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getPage(2, &p)));
  TEST_ASSERT_EQUAL_UINT16(2, p.page_number);
  TEST_ASSERT_EQUAL_UINT32(3, p.first_ayah_index);
  TEST_ASSERT_EQUAL_UINT32(7, p.first_word_index);

  qpk::RangeRecord j;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getJuz(1, &j)));
  TEST_ASSERT_EQUAL_UINT16(1, j.id);
  TEST_ASSERT_EQUAL_UINT16(1, j.first_page);
}

void test_metadata_lookup() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  char buffer[64];
  uint32_t length = 0;
  TEST_ASSERT_TRUE(reader.metadata(qpk::MetadataKey::kTitle, buffer,
                                   sizeof(buffer), &length));
  TEST_ASSERT_EQUAL_UINT32(17, length);
  TEST_ASSERT_EQUAL_INT(0, memcmp(buffer, "Mini Test Package", 17));

  // A key that comes after another in the section proves the 4-byte padding
  // walk is right.
  TEST_ASSERT_TRUE(reader.metadata(qpk::MetadataKey::kScript, buffer,
                                   sizeof(buffer), &length));
  TEST_ASSERT_EQUAL_UINT32(11, length);
  TEST_ASSERT_EQUAL_INT(0, memcmp(buffer, "placeholder", 11));

  TEST_ASSERT_FALSE(reader.metadata(qpk::MetadataKey::kAuthor, buffer,
                                    sizeof(buffer), &length));
}

void test_read_text_truncates_to_capacity() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  char small[4];
  const int32_t n = reader.readText(mini.surah_name[0].offset,
                                    mini.surah_name[0].length, small,
                                    sizeof(small));
  TEST_ASSERT_EQUAL_INT32(4, n);
  TEST_ASSERT_EQUAL_INT(0, memcmp(small, "PLAC", 4));
}

void test_read_text_rejects_out_of_range() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  char buffer[16];
  const uint64_t text_length =
      qpktest::SectionLength(&mini.file, qpk::SectionId::kTextData);
  TEST_ASSERT_EQUAL_INT32(
      -1, reader.readText(static_cast<uint32_t>(text_length), 1, buffer,
                          sizeof(buffer)));
  // The classic overflow: offset + length wraps, so the check must be in
  // subtraction form.
  TEST_ASSERT_EQUAL_INT32(-1, reader.readText(0xFFFFFFF0u, 0x20u, buffer,
                                              sizeof(buffer)));
}

void test_out_of_range_lookups() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  qpk::SurahRecord s;
  qpk::AyahRecord a;
  qpk::WordRecord w;
  qpk::PageRecord p;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getSurah(0, &s)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getSurah(3, &s)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getAyah(1, 0, &a)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getAyah(1, 4, &a)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getWord(1, 1, 2, &w)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getPage(3, &p)));
}

void test_absent_sections_report_no_such_section() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);

  // The mini package carries no hizb, rub, sajdah or translation sections.
  qpk::RangeRecord r;
  qpk::TranslationRecord t;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(reader.getHizb(1, &r)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(reader.getRub(1, &r)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(reader.getTranslation(0, &t)));
  TEST_ASSERT_FALSE(reader.hasSection(qpk::SectionId::kSajdahIndex));
  TEST_ASSERT_TRUE(reader.hasSection(qpk::SectionId::kWordIndex));
  TEST_ASSERT_EQUAL_UINT32(qpktest::MiniQuran::kWordCount,
                           reader.recordCount(qpk::SectionId::kWordIndex));
}

void test_closed_reader_reports_not_open() {
  qpk::Reader reader;
  qpk::SurahRecord s;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNotOpen), kErr(reader.getSurah(1, &s)));
  TEST_ASSERT_FALSE(reader.isOpen());
}

// ---------------------------------------------------------------------------
// Corruption matrix -- one case per validation rule
// ---------------------------------------------------------------------------

void test_rule1_file_shorter_than_header() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  Bytes truncated(mini.file.begin(), mini.file.begin() + 32);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kTooSmall),
                        kErr(OpenVerdict(truncated)));
}

void test_rule2_bad_magic() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  mini.file[0] = 'X';
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kBadMagic),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule3_unsupported_version() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::Write16(mini.file.data() + 4, 2);
  qpktest::FixHeaderCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kUnsupportedVersion),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule4_bad_header_size() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::Write16(mini.file.data() + 6, 32);  // smaller than the fixed header
  qpktest::FixHeaderCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kBadHeaderSize),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule5_header_checksum() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  mini.file[30] ^= 0xFF;  // inside content_id, deliberately not recomputing
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kHeaderChecksum),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule6_package_size_mismatch() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::Write64(mini.file.data() + 16, mini.file.size() + 4);
  qpktest::FixHeaderCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSizeMismatch),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule6_trailing_bytes_appended() {
  // The other half of rule 6: a file that grew (an interrupted append, a
  // resumed upload written twice) must not parse.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  mini.file.push_back(0);
  mini.file.push_back(0);
  mini.file.push_back(0);
  mini.file.push_back(0);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSizeMismatch),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule7_section_table_length_inconsistent() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint16_t count = qpk::Read16(mini.file.data() + 10);
  qpk::Write16(mini.file.data() + 10, static_cast<uint16_t>(count + 1));
  qpktest::FixHeaderCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kBadSectionTable),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule7_section_table_overlaps_header() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::Write32(mini.file.data() + 44, 16);  // inside the header
  qpktest::FixHeaderCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kBadSectionTable),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule8_section_misaligned() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* e = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kWordIndex);
  TEST_ASSERT_NOT_NULL(e);
  qpk::Write64(e + 4, qpk::Read64(e + 4) + 1);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionMisaligned),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule8_section_past_end_of_file() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* e = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kTextData);
  TEST_ASSERT_NOT_NULL(e);
  qpk::Write64(e + 12, mini.file.size());  // length now runs off the end
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionOutOfRange),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule8_offset_length_overflow_is_not_accepted() {
  // offset + length wraps to a small number; only a subtraction-form bounds
  // check rejects this.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* e = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kTextData);
  TEST_ASSERT_NOT_NULL(e);
  const uint64_t offset = qpk::Read64(e + 4);
  qpk::Write64(e + 12, 0xFFFFFFFFFFFFFFFFull - offset + 8ull);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionOutOfRange),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule9_sections_overlap() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* ayah = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kAyahIndex);
  uint8_t* page = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kPageIndex);
  TEST_ASSERT_NOT_NULL(ayah);
  TEST_ASSERT_NOT_NULL(page);
  // Point PAGE_INDEX at AYAH_INDEX's bytes: still aligned, still in range,
  // still the right length -- only the overlap rule catches it.
  qpk::Write64(page + 4, qpk::Read64(ayah + 4));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionOverlap),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule10_section_ids_not_ascending() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* ayah = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kAyahIndex);
  TEST_ASSERT_NOT_NULL(ayah);
  // Duplicate an earlier id, breaking strict ascent.
  qpk::Write16(ayah, static_cast<uint16_t>(qpk::SectionId::kSurahIndex));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionOrder),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule11_length_does_not_match_record_count() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* e = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kAyahIndex);
  TEST_ASSERT_NOT_NULL(e);
  qpk::Write32(e + 20, qpk::Read32(e + 20) + 1);  // count no longer fits
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionSizeMismatch),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule11_wrong_record_stride_is_rejected() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* e = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kWordIndex);
  TEST_ASSERT_NOT_NULL(e);
  // A writer that used 8-byte word records would still produce a consistent
  // count*size, so the stride itself has to be pinned to the format version.
  qpk::Write16(e + 28, 8);
  qpk::Write32(e + 20, static_cast<uint32_t>(qpk::Read64(e + 12) / 8));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionSizeMismatch),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule11_record_count_multiplication_cannot_overflow() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  uint8_t* e = qpktest::FindTableEntry(&mini.file, qpk::SectionId::kWordIndex);
  TEST_ASSERT_NOT_NULL(e);
  // count * 16 overflows 32 bits and lands back on the real length if the
  // product is computed in 32-bit arithmetic.
  const uint64_t length = qpk::Read64(e + 12);
  qpk::Write32(e + 20, static_cast<uint32_t>((0x100000000ull + length) / 16));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionSizeMismatch),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule12_required_section_missing() {
  // A Quran package without TEXT_DATA is not a Quran package.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));

  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kQuran);
  builder.addMetadata(qpk::MetadataKey::kTitle, "No text data");
  Bytes surahs(qpk::kSurahRecordSize, 0);
  qpk::Write16(surahs.data(), 1);
  builder.addSection(qpk::SectionId::kSurahIndex, surahs, 1);
  Bytes pages(qpk::kPageRecordSize, 0);
  qpk::Write16(pages.data(), 1);
  builder.addSection(qpk::SectionId::kPageIndex, pages, 1);
  builder.addSection(qpk::SectionId::kAyahIndex,
                     Bytes(qpk::kAyahRecordSize, 0), 1);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kMissingSection),
                        kErr(OpenVerdict(builder.build())));
}

void test_rule13_index_section_checksum_is_checked_at_open() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kWordIndex);
  mini.file[static_cast<size_t>(offset) + 7] ^= 0x01;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionChecksum),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule13_blob_checksum_is_deferred_not_ignored() {
  // TEXT_DATA is not checksummed at open -- doing so would read the whole
  // package, which is exactly the full scan the format exists to avoid. It is
  // still detectable on demand, and by the install-time payload check.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kTextData);
  mini.file[static_cast<size_t>(offset) + 2] ^= 0x40;

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionChecksum),
                        kErr(reader.verifySection(qpk::SectionId::kTextData)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kSectionChecksum),
                        kErr(reader.verifyPayload()));
}

void test_payload_checksum_passes_on_an_intact_package() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  OpenBytes(mini.file, &reader, &memory);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.verifyPayload()));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(reader.verifySection(qpk::SectionId::kTextData)));
}

void test_rule14_surah_points_past_the_ayah_index() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kSurahIndex);
  // Surah 1's first_ayah_index -> way past the end of AYAH_INDEX.
  qpk::Write32(mini.file.data() + offset + 4, 100000);
  qpktest::FixSectionCrc(&mini.file, qpk::SectionId::kSurahIndex);
  qpktest::FixPayloadCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kIndexInconsistent),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule14_ayah_text_runs_past_text_data() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kAyahIndex);
  // First ayah record: text_length at +16.
  qpk::Write32(mini.file.data() + offset + 16, 0xFFFF);
  qpktest::FixSectionCrc(&mini.file, qpk::SectionId::kAyahIndex);
  qpktest::FixPayloadCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kIndexInconsistent),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule14_metadata_record_overruns_its_section() {
  // Without a structural check at open, a corrupt METADATA section would make
  // every key read as "absent" instead of "damaged".
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kMetadata);
  // First record: value_length at +2.
  qpk::Write16(mini.file.data() + offset + 2, 0xFFFF);
  qpktest::FixSectionCrc(&mini.file, qpk::SectionId::kMetadata);
  qpktest::FixPayloadCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kIndexInconsistent),
                        kErr(OpenVerdict(mini.file)));
}

void test_rule14_pages_must_be_numbered_from_one() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kPageIndex);
  qpk::Write16(mini.file.data() + offset, 7);  // first page claims to be 7
  qpktest::FixSectionCrc(&mini.file, qpk::SectionId::kPageIndex);
  qpktest::FixPayloadCrc(&mini.file);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kIndexInconsistent),
                        kErr(OpenVerdict(mini.file)));
}

void test_surah_index_slot_must_hold_that_surah() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  const uint64_t offset =
      qpktest::SectionOffset(&mini.file, qpk::SectionId::kSurahIndex);
  qpk::Write16(mini.file.data() + offset, 9);  // slot 0 claims to be surah 9
  qpktest::FixSectionCrc(&mini.file, qpk::SectionId::kSurahIndex);
  qpktest::FixPayloadCrc(&mini.file);

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));
  qpk::SurahRecord s;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kIndexInconsistent),
                        kErr(reader.getSurah(1, &s)));
}

// ---------------------------------------------------------------------------
// Font / glyph atlas (rule 15, docs/qpk-format.md 9a)
// ---------------------------------------------------------------------------

namespace {

void AppendGlyph(Bytes* out, uint16_t glyph_id, uint32_t bitmap_offset,
                 uint8_t width, uint8_t height, int8_t x_advance,
                 int8_t x_offset, int8_t y_offset) {
  qpktest::Append16(out, glyph_id);
  qpktest::Append32(out, bitmap_offset);
  out->push_back(width);
  out->push_back(height);
  out->push_back(static_cast<uint8_t>(x_advance));
  out->push_back(static_cast<uint8_t>(x_offset));
  out->push_back(static_cast<uint8_t>(y_offset));
  out->push_back(0);  // reserved
}

}  // namespace

void test_rule15_shaped_text_data_requires_font_sections() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.setFlags(qpk::kFlagShapedTextData);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Shaped, no font sections");
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kFontSectionsRequired),
                        kErr(OpenVerdict(builder.build())));
}

void test_rule15_shaped_text_data_requires_both_font_sections() {
  // FONT_METADATA alone is not enough -- ASSETS (the bitmaps it points into)
  // has to be there too.
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.setFlags(qpk::kFlagShapedTextData);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Font index, no bitmaps");
  Bytes glyphs;
  AppendGlyph(&glyphs, /*glyph_id=*/1, /*bitmap_offset=*/0, 8, 8, 8, 0, 0);
  builder.addSection(qpk::SectionId::kFontMetadata, glyphs, 1);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kFontSectionsRequired),
                        kErr(OpenVerdict(builder.build())));
}

void test_rule15_shaped_text_data_opens_with_both_sections_present() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.setFlags(qpk::kFlagShapedTextData);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Font index and bitmaps");
  Bytes glyphs;
  AppendGlyph(&glyphs, /*glyph_id=*/1, /*bitmap_offset=*/0, 8, 8, 8, 0, 0);
  builder.addSection(qpk::SectionId::kFontMetadata, glyphs, 1);
  builder.addSection(qpk::SectionId::kAssets, Bytes(8, 0xAA), 0);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(OpenVerdict(builder.build())));
}

void test_get_glyph_finds_by_id_not_by_table_position() {
  // Glyph ids are assigned by the desktop shaper and need not be sorted or
  // dense -- unlike SURAH_INDEX, this cannot be direct-indexed.
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Glyph lookup");
  Bytes glyphs;
  AppendGlyph(&glyphs, /*glyph_id=*/500, /*bitmap_offset=*/100, 10, 12, 9, 1, -3);
  AppendGlyph(&glyphs, /*glyph_id=*/10, /*bitmap_offset=*/0, 6, 8, 5, -1, 2);
  builder.addSection(qpk::SectionId::kFontMetadata, glyphs, 2);
  builder.addSection(qpk::SectionId::kAssets, Bytes(200, 0), 0);

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(builder.build(), &reader, &memory)));

  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getGlyph(10, &g)));
  TEST_ASSERT_EQUAL_UINT16(10, g.glyph_id);
  TEST_ASSERT_EQUAL_UINT32(0, g.bitmap_offset);
  TEST_ASSERT_EQUAL_UINT8(6, g.width);
  TEST_ASSERT_EQUAL_UINT8(8, g.height);
  TEST_ASSERT_EQUAL_INT8(5, g.x_advance);
  TEST_ASSERT_EQUAL_INT8(-1, g.x_offset);
  TEST_ASSERT_EQUAL_INT8(2, g.y_offset);

  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getGlyph(500, &g)));
  TEST_ASSERT_EQUAL_UINT32(100, g.bitmap_offset);
  TEST_ASSERT_EQUAL_INT8(-3, g.y_offset);
}

void test_get_glyph_reports_out_of_range_and_no_such_section() {
  qpktest::PackageBuilder with_font;
  with_font.setType(qpk::PackageType::kUnknown);
  with_font.addMetadata(qpk::MetadataKey::kTitle, "One glyph");
  Bytes glyphs;
  AppendGlyph(&glyphs, /*glyph_id=*/1, 0, 4, 4, 4, 0, 0);
  with_font.addSection(qpk::SectionId::kFontMetadata, glyphs, 1);
  with_font.addSection(qpk::SectionId::kAssets, Bytes(4, 0), 0);

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(with_font.build(), &reader, &memory)));
  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getGlyph(9999, &g)));

  qpktest::PackageBuilder without_font;
  without_font.setType(qpk::PackageType::kUnknown);
  without_font.addMetadata(qpk::MetadataKey::kTitle, "No font at all");
  qpk::MemoryFile memory2;
  qpk::Reader reader2;
  TEST_ASSERT_EQUAL_INT(
      kErr(qpk::Error::kOk),
      kErr(OpenBytes(without_font.build(), &reader2, &memory2)));
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(reader2.getGlyph(1, &g)));
}

void test_read_asset_returns_the_bitmap_bytes() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Bitmap blob");
  Bytes glyphs;
  AppendGlyph(&glyphs, /*glyph_id=*/1, /*bitmap_offset=*/2, 8, 1, 8, 0, 0);
  builder.addSection(qpk::SectionId::kFontMetadata, glyphs, 1);
  // A tiny "bitmap": 2 unrelated bytes, then the 1-row/8-col glyph itself.
  const Bytes assets = {0x00, 0x00, 0xB4};  // 0b10110100
  builder.addSection(qpk::SectionId::kAssets, assets, 0);

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(builder.build(), &reader, &memory)));

  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getGlyph(1, &g)));
  uint8_t bitmap[4] = {0};
  const int32_t n = reader.readAsset(g.bitmap_offset, 1, bitmap, sizeof(bitmap));
  TEST_ASSERT_EQUAL_INT32(1, n);
  TEST_ASSERT_EQUAL_UINT8(0xB4, bitmap[0]);
}

// --- glyph index (Reader::buildGlyphIndex) ---------------------------------

namespace {

// Five glyphs out of id order, with id 10 twice. The duplicates differ only
// in width, so a lookup shows which of the two it resolved to. Every bitmap
// offset is 0: the index only changes how records are found, not what they
// point at.
Bytes GlyphTablePackage(bool with_font) {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Glyph index");
  if (with_font) {
    Bytes glyphs;
    AppendGlyph(&glyphs, /*glyph_id=*/500, 0, 10, 1, 9, 1, -3);
    AppendGlyph(&glyphs, /*glyph_id=*/10, 0, 6, 1, 5, -1, 2);
    AppendGlyph(&glyphs, /*glyph_id=*/77, 0, 7, 1, 7, 0, 0);
    AppendGlyph(&glyphs, /*glyph_id=*/10, 0, 11, 1, 5, -1, 2);
    AppendGlyph(&glyphs, /*glyph_id=*/3, 0, 3, 1, 4, 0, -1);
    builder.addSection(qpk::SectionId::kFontMetadata, glyphs, 5);
    builder.addSection(qpk::SectionId::kAssets, Bytes(16, 0), 0);
  }
  return builder.build();
}

}  // namespace

void test_glyph_index_returns_exactly_what_the_scan_returns() {
  const Bytes file = GlyphTablePackage(true);
  qpk::MemoryFile scan_memory;
  qpk::Reader scan;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &scan, &scan_memory)));
  qpk::MemoryFile indexed_memory;
  qpk::Reader indexed;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &indexed, &indexed_memory)));

  qpk::GlyphRecord storage[8];
  TEST_ASSERT_EQUAL_UINT32(5, indexed.buildGlyphIndex(storage, 8));
  TEST_ASSERT_TRUE(indexed.glyphIndexed());
  TEST_ASSERT_FALSE(scan.glyphIndexed());

  const uint16_t ids[] = {0, 3, 9, 10, 11, 77, 500, 501, 65535};
  for (uint16_t id : ids) {
    qpk::GlyphRecord a;
    qpk::GlyphRecord b;
    const qpk::Error ea = scan.getGlyph(id, &a);
    const qpk::Error eb = indexed.getGlyph(id, &b);
    TEST_ASSERT_EQUAL_INT(kErr(ea), kErr(eb));
    if (ea != qpk::Error::kOk) continue;
    TEST_ASSERT_EQUAL_UINT16(a.glyph_id, b.glyph_id);
    TEST_ASSERT_EQUAL_UINT32(a.bitmap_offset, b.bitmap_offset);
    TEST_ASSERT_EQUAL_UINT8(a.width, b.width);
    TEST_ASSERT_EQUAL_UINT8(a.height, b.height);
    TEST_ASSERT_EQUAL_INT8(a.x_advance, b.x_advance);
    TEST_ASSERT_EQUAL_INT8(a.x_offset, b.x_offset);
    TEST_ASSERT_EQUAL_INT8(a.y_offset, b.y_offset);
  }

  // The duplicated id resolves to its first record in table order (width 6),
  // not the later one (width 11).
  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(indexed.getGlyph(10, &g)));
  TEST_ASSERT_EQUAL_UINT8(6, g.width);
}

void test_glyph_index_too_small_keeps_the_scan() {
  const Bytes file = GlyphTablePackage(true);
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &reader, &memory)));

  qpk::GlyphRecord storage[4];
  TEST_ASSERT_EQUAL_UINT32(0, reader.buildGlyphIndex(storage, 4));
  TEST_ASSERT_FALSE(reader.glyphIndexed());

  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getGlyph(77, &g)));
  TEST_ASSERT_EQUAL_UINT8(7, g.width);
}

void test_glyph_index_is_dropped_by_close_and_reopen() {
  const Bytes file = GlyphTablePackage(true);
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &reader, &memory)));
  qpk::GlyphRecord storage[8];
  TEST_ASSERT_EQUAL_UINT32(5, reader.buildGlyphIndex(storage, 8));

  reader.close();
  TEST_ASSERT_FALSE(reader.glyphIndexed());
  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNotOpen), kErr(reader.getGlyph(3, &g)));

  // Reopening must not revive an index built for the previous file.
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &reader, &memory)));
  TEST_ASSERT_FALSE(reader.glyphIndexed());
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getGlyph(500, &g)));
  TEST_ASSERT_EQUAL_UINT8(10, g.width);
}

void test_glyph_index_needs_font_metadata() {
  const Bytes file = GlyphTablePackage(false);
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &reader, &memory)));
  qpk::GlyphRecord storage[8];
  TEST_ASSERT_EQUAL_UINT32(0, reader.buildGlyphIndex(storage, 8));
  TEST_ASSERT_FALSE(reader.glyphIndexed());
  qpk::GlyphRecord g;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(reader.getGlyph(1, &g)));
}

// ---------------------------------------------------------------------------
// Layout (rule 16, docs/qpk-format.md 9)
// ---------------------------------------------------------------------------

// --- end-of-ayah markers (docs/qpk-format.md 9b) ---------------------------

namespace {

// Builds the smallest package that can carry a kAyahMarkerGlyphs value, so
// the parser can be exercised on strings a real writer would never emit.
Bytes PackageWithMarkerMetadata(const char* value) {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Marker metadata");
  if (value != nullptr) {
    builder.addMetadata(qpk::MetadataKey::kAyahMarkerGlyphs, value);
  }
  return builder.build();
}

bool ParseMarkerMetadata(const char* value, uint16_t* first, uint16_t* count) {
  const Bytes bytes = PackageWithMarkerMetadata(value);
  qpk::MemoryFile memory;
  qpk::Reader reader;
  if (OpenBytes(bytes, &reader, &memory) != qpk::Error::kOk) return false;
  return reader.ayahMarkerGlyphs(first, count);
}

}  // namespace

void test_ayah_marker_glyphs_reads_the_range_from_metadata() {
  uint16_t first = 0;
  uint16_t count = 0;
  TEST_ASSERT_TRUE(ParseMarkerMetadata("1508:286", &first, &count));
  TEST_ASSERT_EQUAL_UINT16(1508, first);
  TEST_ASSERT_EQUAL_UINT16(286, count);
}

void test_ayah_marker_glyphs_is_absent_without_the_key() {
  // The whole point of putting this in METADATA: a package built before
  // markers existed is not broken by them, it just has no answer here.
  uint16_t first = 0xFFFF;
  uint16_t count = 0xFFFF;
  TEST_ASSERT_FALSE(ParseMarkerMetadata(nullptr, &first, &count));
}

void test_ayah_marker_glyphs_rejects_malformed_values() {
  // Every one of these is a rejection rather than a partially-filled pair --
  // a half-parsed range would have the reader asking getGlyph() for ids the
  // writer never minted, which is a wrong glyph on screen, not an error.
  const char* kBad[] = {
      "",           // empty
      "1508",       // no count
      "1508:",      // count missing after the separator
      ":286",       // first missing before the separator
      "1508:286:1", // a third field
      "1508 286",   // wrong separator
      "1508:286x",  // trailing junk
      "abc:def",    // not decimal
      "1508:0",     // a range covering no ayah at all
      "70000:1",    // first id past u16
      "65535:2",    // last id would wrap past u16
  };
  for (uint32_t i = 0; i < sizeof(kBad) / sizeof(kBad[0]); ++i) {
    uint16_t first = 0;
    uint16_t count = 0;
    if (ParseMarkerMetadata(kBad[i], &first, &count)) {
      char message[64];
      snprintf(message, sizeof(message), "accepted malformed value \"%s\"",
               kBad[i]);
      TEST_FAIL_MESSAGE(message);
    }
  }
}

void test_ayah_marker_glyphs_resolve_to_real_atlas_glyphs() {
  // The contract the device relies on: marker for ayah n is
  // first_glyph_id + (n - 1), and every id in the range is really in
  // FONT_METADATA. Checked against the fixture's own three markers.
  qpktest::MiniShapedQuran mini = qpktest::BuildMiniShapedQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));

  uint16_t first = 0;
  uint16_t count = 0;
  TEST_ASSERT_TRUE(reader.ayahMarkerGlyphs(&first, &count));
  TEST_ASSERT_EQUAL_UINT16(qpktest::MiniShapedQuran::kMarkerGlyphIdBase, first);
  TEST_ASSERT_EQUAL_UINT16(qpktest::MiniShapedQuran::kMarkerCount, count);

  for (uint16_t ayah_number = 1; ayah_number <= count; ++ayah_number) {
    qpk::GlyphRecord glyph;
    const uint16_t glyph_id = static_cast<uint16_t>(first + ayah_number - 1);
    TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                          kErr(reader.getGlyph(glyph_id, &glyph)));
    TEST_ASSERT_EQUAL_UINT8(qpktest::MiniShapedQuran::kMarkerWidth, glyph.width);
    TEST_ASSERT_EQUAL_UINT8(qpktest::MiniShapedQuran::kMarkerHeight,
                            glyph.height);
    TEST_ASSERT_EQUAL_UINT32(
        mini.marker_bitmap_offset[ayah_number - 1], glyph.bitmap_offset);
  }

  // And the marker ids are genuinely distinct from the word glyphs -- a
  // marker that happened to alias a word glyph would render as Arabic text
  // and still pass every check above.
  qpk::GlyphRecord word_glyph;
  TEST_ASSERT_EQUAL_INT(
      kErr(qpk::Error::kOk),
      kErr(reader.getGlyph(qpktest::MiniShapedQuran::kGlyphIdBase, &word_glyph)));
  TEST_ASSERT_TRUE(word_glyph.width != qpktest::MiniShapedQuran::kMarkerWidth);
}

void test_rule16_word_layout_requires_layout_section() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.setFlags(qpk::kFlagHasWordLayout);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Word layout, no LAYOUT_DATA");
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kLayoutSectionRequired),
                        kErr(OpenVerdict(builder.build())));
}

void test_get_layout_header_and_line_round_trip() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));

  qpk::LayoutHeader header;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getLayoutHeader(&header)));
  TEST_ASSERT_EQUAL_UINT16(1, header.layout_version);
  TEST_ASSERT_EQUAL_UINT16(1000, header.page_width);
  TEST_ASSERT_EQUAL_UINT16(1400, header.page_height);
  TEST_ASSERT_EQUAL_UINT16(2, header.line_count_total);

  qpk::LineRecord line0;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getLine(0, &line0)));
  TEST_ASSERT_EQUAL_UINT16(1, line0.page);
  TEST_ASSERT_EQUAL_UINT16(1, line0.line_id);
  TEST_ASSERT_EQUAL_UINT16(100, line0.y);
  TEST_ASSERT_EQUAL_UINT16(48, line0.height);
  TEST_ASSERT_EQUAL_UINT32(0, line0.first_word_index);

  qpk::LineRecord line1;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getLine(1, &line1)));
  TEST_ASSERT_EQUAL_UINT16(2, line1.page);
  TEST_ASSERT_EQUAL_UINT32(7, line1.first_word_index);

  qpk::LineRecord out_of_range;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getLine(2, &out_of_range)));
}

void test_find_line_by_word_index_binary_searches_the_records() {
  // The access path a line-driven renderer needs: reading position (a word
  // index) -> the line it sits on. MiniQuran's layout has two lines, starting
  // at words 0 and 7, over 12 words.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));

  struct Case { uint32_t word; uint32_t expect_line; uint16_t expect_page; };
  const Case kCases[] = {
      {0, 0, 1},   // first word of the first line
      {3, 0, 1},   // mid-span: belongs to the line that STARTS at or before it
      {6, 0, 1},   // last word before the second line begins
      {7, 1, 2},   // exactly the second line's first word
      {11, 1, 2},  // last word in the package
      {99, 1, 2},  // past the end: still the last line that starts before it
  };
  for (uint32_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    uint32_t line_index = 0xFFFFFFFF;
    qpk::LineRecord line;
    TEST_ASSERT_EQUAL_INT(
        kErr(qpk::Error::kOk),
        kErr(reader.findLineByWordIndex(kCases[i].word, &line_index, &line)));
    TEST_ASSERT_EQUAL_UINT32(kCases[i].expect_line, line_index);
    TEST_ASSERT_EQUAL_UINT16(kCases[i].expect_page, line.page);
  }
}

void test_find_line_by_word_index_reports_a_package_with_no_layout() {
  // The shaped fixture carries no LAYOUT_DATA -- the renderer must be able to
  // ask and be told no, because that is what selects the fill-and-wrap path.
  qpktest::MiniShapedQuran mini = qpktest::BuildMiniShapedQuran();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(mini.file, &reader, &memory)));

  uint32_t line_index = 0;
  qpk::LineRecord line;
  TEST_ASSERT_EQUAL_INT(
      kErr(qpk::Error::kNoSuchSection),
      kErr(reader.findLineByWordIndex(0, &line_index, &line)));
}

void test_get_layout_header_reports_no_such_section() {
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "No layout at all");

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(builder.build(), &reader, &memory)));
  qpk::LayoutHeader header;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kNoSuchSection),
                        kErr(reader.getLayoutHeader(&header)));
}

void test_layout_header_claiming_too_many_lines_is_rejected() {
  // line_count_total says 5000 lines; the payload only has room for 1. A
  // malformed header must fail at getLayoutHeader(), not surface as 5000
  // individual getLine() failures downstream.
  Bytes layout;
  qpktest::Append16(&layout, 1);                       // layout_version
  qpktest::Append16(&layout, 800);                      // page_width
  qpktest::Append16(&layout, 1200);                     // page_height
  qpktest::Append16(&layout, 5000);                     // line_count_total (bogus)
  qpktest::Append32(&layout, qpk::kLayoutHeaderSize);   // line_records_offset
  qpktest::Append16(&layout, 1);                        // one real line record
  qpktest::Append16(&layout, 1);
  qpktest::Append16(&layout, 0);
  qpktest::Append16(&layout, 10);
  qpktest::Append32(&layout, 0);

  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kUnknown);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Bad layout header");
  builder.addSection(qpk::SectionId::kLayoutData, layout, 0);

  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(builder.build(), &reader, &memory)));
  qpk::LayoutHeader header;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOutOfRange),
                        kErr(reader.getLayoutHeader(&header)));
}

// ---------------------------------------------------------------------------
// Forward compatibility
// ---------------------------------------------------------------------------

void test_unknown_section_ids_are_skipped_not_rejected() {
  // A package written by a future version carries a section this build has
  // never heard of. It must open, and everything known must still work.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();

  // Rebuild the same content plus one unknown section. Id 999 sorts after
  // every known id, so the ascending-order rule still holds.
  qpktest::PackageBuilder builder;
  builder.setType(qpk::PackageType::kQuran);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Future package");

  // Copy the real sections straight out of the mini package's bytes.
  const qpk::SectionId kCopied[] = {
      qpk::SectionId::kSurahIndex, qpk::SectionId::kAyahIndex,
      qpk::SectionId::kWordIndex,  qpk::SectionId::kPageIndex,
      qpk::SectionId::kTextData};
  const uint32_t kCounts[] = {qpktest::MiniQuran::kSurahCount,
                              qpktest::MiniQuran::kAyahCount,
                              qpktest::MiniQuran::kWordCount,
                              qpktest::MiniQuran::kPageCount, 0};
  for (size_t i = 0; i < 5; ++i) {
    const uint64_t offset = qpktest::SectionOffset(&mini.file, kCopied[i]);
    const uint64_t length = qpktest::SectionLength(&mini.file, kCopied[i]);
    Bytes payload(mini.file.begin() + offset,
                  mini.file.begin() + offset + length);
    builder.addSection(kCopied[i], payload, kCounts[i]);
  }
  const Bytes future_payload = {0xDE, 0xAD, 0xBE, 0xEF};
  builder.addSection(static_cast<qpk::SectionId>(999), future_payload, 0);

  const Bytes file = builder.build();
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &reader, &memory)));

  // The unknown section is carried, not interpreted...
  TEST_ASSERT_TRUE(reader.hasSection(static_cast<qpk::SectionId>(999)));
  // ...and the known access paths are unaffected.
  qpk::AyahRecord a;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getAyah(1, 2, &a)));
  TEST_ASSERT_EQUAL_UINT16(3, a.word_count);
}

// ---------------------------------------------------------------------------
// Cross-language: a package written by the TypeScript writer
// ---------------------------------------------------------------------------

void test_golden_package_from_typescript_writer_parses() {
  Bytes file(kGoldenPackage, kGoldenPackage + kGoldenPackageSize);
  qpk::MemoryFile memory;
  qpk::Reader reader;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(OpenBytes(file, &reader, &memory)));

  TEST_ASSERT_TRUE(reader.type() == qpk::PackageType::kQuran);
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.verifyPayload()));

  qpk::AyahRecord a;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk), kErr(reader.getAyah(2, 2, &a)));
  TEST_ASSERT_EQUAL_UINT16(2, a.word_count);
  TEST_ASSERT_EQUAL_STRING("PLACEHOLDER-AYAH-2-2",
                           TextOf(reader, a.text_offset, a.text_length).c_str());

  qpk::WordRecord w;
  TEST_ASSERT_EQUAL_INT(kErr(qpk::Error::kOk),
                        kErr(reader.getWord(2, 2, 1, &w)));
  TEST_ASSERT_EQUAL_UINT16(400, w.width);
  TEST_ASSERT_EQUAL_STRING("WORD11",
                           TextOf(reader, w.text_offset, w.text_length).c_str());

  char buffer[64];
  uint32_t length = 0;
  TEST_ASSERT_TRUE(reader.metadata(qpk::MetadataKey::kTitle, buffer,
                                   sizeof(buffer), &length));
  TEST_ASSERT_EQUAL_UINT32(17, length);
  TEST_ASSERT_EQUAL_INT(0, memcmp(buffer, "Mini Test Package", 17));
}

void test_golden_package_is_byte_identical_to_the_cpp_builder() {
  // The strongest form of the cross-language contract: given the same logical
  // content, both writers must emit the same bytes.
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kGoldenPackageSize),
                           static_cast<uint32_t>(mini.file.size()));
  TEST_ASSERT_EQUAL_INT(0, memcmp(mini.file.data(), kGoldenPackage,
                                  kGoldenPackageSize));
}

// ---------------------------------------------------------------------------

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_crc32_check_value);
  RUN_TEST(test_crc32_is_streamable);

  RUN_TEST(test_valid_package_opens);
  RUN_TEST(test_header_fields_round_trip);
  RUN_TEST(test_get_surah_resolves_directly);
  RUN_TEST(test_get_ayah_maps_surah_and_number);
  RUN_TEST(test_get_word_resolves_layout);
  RUN_TEST(test_get_page_and_juz);
  RUN_TEST(test_metadata_lookup);
  RUN_TEST(test_read_text_truncates_to_capacity);
  RUN_TEST(test_read_text_rejects_out_of_range);
  RUN_TEST(test_out_of_range_lookups);
  RUN_TEST(test_absent_sections_report_no_such_section);
  RUN_TEST(test_closed_reader_reports_not_open);

  RUN_TEST(test_rule1_file_shorter_than_header);
  RUN_TEST(test_rule2_bad_magic);
  RUN_TEST(test_rule3_unsupported_version);
  RUN_TEST(test_rule4_bad_header_size);
  RUN_TEST(test_rule5_header_checksum);
  RUN_TEST(test_rule6_package_size_mismatch);
  RUN_TEST(test_rule6_trailing_bytes_appended);
  RUN_TEST(test_rule7_section_table_length_inconsistent);
  RUN_TEST(test_rule7_section_table_overlaps_header);
  RUN_TEST(test_rule8_section_misaligned);
  RUN_TEST(test_rule8_section_past_end_of_file);
  RUN_TEST(test_rule8_offset_length_overflow_is_not_accepted);
  RUN_TEST(test_rule9_sections_overlap);
  RUN_TEST(test_rule10_section_ids_not_ascending);
  RUN_TEST(test_rule11_length_does_not_match_record_count);
  RUN_TEST(test_rule11_wrong_record_stride_is_rejected);
  RUN_TEST(test_rule11_record_count_multiplication_cannot_overflow);
  RUN_TEST(test_rule12_required_section_missing);
  RUN_TEST(test_rule13_index_section_checksum_is_checked_at_open);
  RUN_TEST(test_rule13_blob_checksum_is_deferred_not_ignored);
  RUN_TEST(test_payload_checksum_passes_on_an_intact_package);
  RUN_TEST(test_rule14_surah_points_past_the_ayah_index);
  RUN_TEST(test_rule14_ayah_text_runs_past_text_data);
  RUN_TEST(test_rule14_metadata_record_overruns_its_section);
  RUN_TEST(test_rule14_pages_must_be_numbered_from_one);
  RUN_TEST(test_surah_index_slot_must_hold_that_surah);

  RUN_TEST(test_rule15_shaped_text_data_requires_font_sections);
  RUN_TEST(test_rule15_shaped_text_data_requires_both_font_sections);
  RUN_TEST(test_rule15_shaped_text_data_opens_with_both_sections_present);
  RUN_TEST(test_get_glyph_finds_by_id_not_by_table_position);
  RUN_TEST(test_get_glyph_reports_out_of_range_and_no_such_section);
  RUN_TEST(test_read_asset_returns_the_bitmap_bytes);
  RUN_TEST(test_glyph_index_returns_exactly_what_the_scan_returns);
  RUN_TEST(test_glyph_index_too_small_keeps_the_scan);
  RUN_TEST(test_glyph_index_is_dropped_by_close_and_reopen);
  RUN_TEST(test_glyph_index_needs_font_metadata);
  RUN_TEST(test_ayah_marker_glyphs_reads_the_range_from_metadata);
  RUN_TEST(test_ayah_marker_glyphs_is_absent_without_the_key);
  RUN_TEST(test_ayah_marker_glyphs_rejects_malformed_values);
  RUN_TEST(test_ayah_marker_glyphs_resolve_to_real_atlas_glyphs);
  RUN_TEST(test_rule16_word_layout_requires_layout_section);
  RUN_TEST(test_get_layout_header_and_line_round_trip);
  RUN_TEST(test_get_layout_header_reports_no_such_section);
  RUN_TEST(test_find_line_by_word_index_binary_searches_the_records);
  RUN_TEST(test_find_line_by_word_index_reports_a_package_with_no_layout);
  RUN_TEST(test_layout_header_claiming_too_many_lines_is_rejected);

  RUN_TEST(test_unknown_section_ids_are_skipped_not_rejected);

  RUN_TEST(test_golden_package_from_typescript_writer_parses);
  RUN_TEST(test_golden_package_is_byte_identical_to_the_cpp_builder);
  RUN_TEST(test_unpack_bits_decodes_runs_and_literals_exactly);
  RUN_TEST(test_page_pictures_read_back_page_by_page);
  RUN_TEST(test_a_package_without_page_pictures_reports_none);
  RUN_TEST(test_read_cover_from_file_matches_reader_read_cover);
  RUN_TEST(test_read_cover_from_file_reports_missing_and_malformed_covers);
  RUN_TEST(test_translation_verses_are_numbered_folded_and_bounded);
  RUN_TEST(test_translation_verses_read_in_one_go_match_the_verse_by_verse_text);
  RUN_TEST(test_read_record_range_stops_at_the_section_end_and_the_buffer);
  RUN_TEST(test_open_can_leave_the_index_checksums_to_the_caller);

  return UNITY_END();
}
