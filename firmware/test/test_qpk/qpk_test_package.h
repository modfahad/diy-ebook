// qpk_test_package.h -- host-only helpers for building QPK1 packages in
// memory, so every corruption case can be expressed as "make a valid package,
// flip these bytes, expect this error".
//
// Host tests only: this uses std::vector and std::string, which firmware code
// must not. It is never compiled for the device (test_ignore = * in the
// crowpanel env).
//
// IMPORTANT: all text here is obviously-synthetic placeholder content.
// No Quranic text is fabricated anywhere in this repository; real mushaf text
// requires a verified source and belongs to the converter milestone.

#pragma once

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "qpk/byte_order.h"
#include "qpk/crc32.h"
#include "qpk/qpk_format.h"

namespace qpktest {

using Bytes = std::vector<uint8_t>;

inline void Pad4(Bytes* out) {
  while ((out->size() & 3u) != 0) out->push_back(0);
}

inline void Append16(Bytes* out, uint16_t v) {
  uint8_t b[2];
  qpk::Write16(b, v);
  out->insert(out->end(), b, b + 2);
}

inline void Append32(Bytes* out, uint32_t v) {
  uint8_t b[4];
  qpk::Write32(b, v);
  out->insert(out->end(), b, b + 4);
}

// Accumulates strings into a TEXT_DATA blob and hands back (offset, length).
class TextPool {
 public:
  struct Ref {
    uint32_t offset = 0;
    uint32_t length = 0;
  };

  Ref add(const std::string& text) {
    Ref ref;
    ref.offset = static_cast<uint32_t>(bytes_.size());
    ref.length = static_cast<uint32_t>(text.size());
    bytes_.insert(bytes_.end(), text.begin(), text.end());
    return ref;
  }

  const Bytes& bytes() const { return bytes_; }

 private:
  Bytes bytes_;
};

// Builds a syntactically valid QPK1 file: correct offsets, alignment, section
// ordering and every checksum. Tests then damage the result on purpose.
class PackageBuilder {
 public:
  void setType(qpk::PackageType type) { type_ = type; }
  void setFlags(uint32_t flags) { flags_ = flags; }
  void setContentVersion(uint32_t v) { content_version_ = v; }
  void setContentId(uint8_t seed) {
    for (int i = 0; i < qpk::kContentIdBytes; ++i) {
      content_id_[i] = static_cast<uint8_t>(seed + i);
    }
  }

  // `count` is the record count for fixed-size sections, 0 for blobs.
  void addSection(qpk::SectionId id, const Bytes& payload, uint32_t count) {
    Section s;
    s.id = id;
    s.payload = payload;
    s.count = count;
    s.record_size = qpk::RecordSizeFor(id);
    sections_[static_cast<uint16_t>(id)] = s;
  }

  void addMetadata(qpk::MetadataKey key, const std::string& value) {
    metadata_[static_cast<uint16_t>(key)] = value;
  }

  bool hasSection(qpk::SectionId id) const {
    return sections_.count(static_cast<uint16_t>(id)) != 0;
  }

  void removeSection(qpk::SectionId id) {
    sections_.erase(static_cast<uint16_t>(id));
  }

  Bytes build() const {
    // METADATA is emitted from the key/value map so tests never hand-encode
    // its padding rules.
    std::map<uint16_t, Section> all = sections_;
    if (!metadata_.empty()) {
      Bytes payload;
      for (const auto& kv : metadata_) {
        Append16(&payload, kv.first);
        Append16(&payload, static_cast<uint16_t>(kv.second.size()));
        payload.insert(payload.end(), kv.second.begin(), kv.second.end());
        Pad4(&payload);
      }
      Section s;
      s.id = qpk::SectionId::kMetadata;
      s.payload = payload;
      s.count = static_cast<uint32_t>(metadata_.size());
      s.record_size = 0;
      all[static_cast<uint16_t>(qpk::SectionId::kMetadata)] = s;
    }

    const uint32_t section_count = static_cast<uint32_t>(all.size());
    const uint32_t table_offset = qpk::kHeaderSize;
    const uint32_t table_length = section_count * qpk::kSectionEntrySize;

    Bytes file(qpk::kHeaderSize + table_length, 0);
    while ((file.size() & 3u) != 0) file.push_back(0);

    struct Placed {
      qpk::SectionId id;
      uint16_t record_size;
      uint64_t offset;
      uint64_t length;
      uint32_t count;
      uint32_t crc;
    };
    std::vector<Placed> placed;

    // std::map iterates in ascending key order, which is exactly the section
    // ordering the format requires.
    for (const auto& kv : all) {
      const Section& s = kv.second;
      Placed p;
      p.id = s.id;
      p.record_size = s.record_size;
      p.offset = file.size();
      p.length = s.payload.size();
      p.count = s.count;
      p.crc = qpk::Crc32(s.payload.data(), s.payload.size());
      file.insert(file.end(), s.payload.begin(), s.payload.end());
      Pad4(&file);
      placed.push_back(p);
    }

    // Section table.
    for (size_t i = 0; i < placed.size(); ++i) {
      uint8_t* e = file.data() + table_offset + i * qpk::kSectionEntrySize;
      qpk::Write16(e + 0, static_cast<uint16_t>(placed[i].id));
      qpk::Write16(e + 2, 1);
      qpk::Write64(e + 4, placed[i].offset);
      qpk::Write64(e + 12, placed[i].length);
      qpk::Write32(e + 20, placed[i].count);
      qpk::Write32(e + 24, placed[i].crc);
      qpk::Write16(e + 28, placed[i].record_size);
      qpk::Write16(e + 30, 0);
    }

    // Header.
    uint8_t* h = file.data();
    h[0] = qpk::kMagic0;
    h[1] = qpk::kMagic1;
    h[2] = qpk::kMagic2;
    h[3] = qpk::kMagic3;
    qpk::Write16(h + 4, qpk::kFormatVersion);
    qpk::Write16(h + 6, qpk::kHeaderSize);
    qpk::Write16(h + 8, static_cast<uint16_t>(type_));
    qpk::Write16(h + 10, static_cast<uint16_t>(section_count));
    qpk::Write32(h + 12, flags_);
    qpk::Write64(h + 16, static_cast<uint64_t>(file.size()));
    for (int i = 0; i < qpk::kContentIdBytes; ++i) h[24 + i] = content_id_[i];
    qpk::Write32(h + 40, content_version_);
    qpk::Write32(h + 44, table_offset);
    qpk::Write32(h + 48, table_length);
    qpk::Write32(h + 52, qpk::Crc32(file.data() + qpk::kHeaderSize,
                                    file.size() - qpk::kHeaderSize));
    qpk::Write32(h + 56, 0);
    qpk::Write32(h + 60, qpk::Crc32(file.data(), 60));
    return file;
  }

 private:
  struct Section {
    qpk::SectionId id = qpk::SectionId::kNone;
    Bytes payload;
    uint32_t count = 0;
    uint16_t record_size = 0;
  };

  qpk::PackageType type_ = qpk::PackageType::kQuran;
  uint32_t flags_ = 0;
  uint32_t content_version_ = 1;
  uint8_t content_id_[qpk::kContentIdBytes] = {0};
  std::map<uint16_t, Section> sections_;
  std::map<uint16_t, std::string> metadata_;
};

// --- post-build surgery -----------------------------------------------------

inline void FixHeaderCrc(Bytes* file) {
  qpk::Write32(file->data() + 60, qpk::Crc32(file->data(), 60));
}

// Locates a section-table entry by id. Returns nullptr if absent.
inline uint8_t* FindTableEntry(Bytes* file, qpk::SectionId id) {
  const uint32_t table_offset = qpk::Read32(file->data() + 44);
  const uint16_t count = qpk::Read16(file->data() + 10);
  for (uint16_t i = 0; i < count; ++i) {
    uint8_t* e = file->data() + table_offset + i * qpk::kSectionEntrySize;
    if (qpk::Read16(e) == static_cast<uint16_t>(id)) return e;
  }
  return nullptr;
}

inline uint64_t SectionOffset(Bytes* file, qpk::SectionId id) {
  uint8_t* e = FindTableEntry(file, id);
  return e == nullptr ? 0 : qpk::Read64(e + 4);
}

inline uint64_t SectionLength(Bytes* file, qpk::SectionId id) {
  uint8_t* e = FindTableEntry(file, id);
  return e == nullptr ? 0 : qpk::Read64(e + 12);
}

// Recomputes one section's CRC after its bytes were edited on purpose.
inline void FixSectionCrc(Bytes* file, qpk::SectionId id) {
  uint8_t* e = FindTableEntry(file, id);
  if (e == nullptr) return;
  const uint64_t offset = qpk::Read64(e + 4);
  const uint64_t length = qpk::Read64(e + 12);
  qpk::Write32(e + 24, qpk::Crc32(file->data() + offset,
                                  static_cast<size_t>(length)));
}

inline void FixPayloadCrc(Bytes* file) {
  qpk::Write32(file->data() + 52,
               qpk::Crc32(file->data() + qpk::kHeaderSize,
                          file->size() - qpk::kHeaderSize));
  FixHeaderCrc(file);
}

// --- the shared fixture -----------------------------------------------------

// A tiny, entirely synthetic "Quran-shaped" package:
//   2 surahs, 5 ayahs, 12 words, 2 pages, 1 juz.
// The numbers are chosen so that every direct-access path in section 10 of
// the format doc has something non-trivial to resolve.
struct MiniQuran {
  Bytes file;

  static constexpr uint16_t kSurahCount = 2;
  static constexpr uint32_t kAyahCount = 5;
  static constexpr uint32_t kWordCount = 12;
  static constexpr uint16_t kPageCount = 2;

  // Expected values the tests assert against.
  TextPool::Ref surah_name[kSurahCount];
  TextPool::Ref ayah_text[kAyahCount];
  TextPool::Ref word_text[kWordCount];
  uint16_t word_x[kWordCount];
};

inline MiniQuran BuildMiniQuran() {
  MiniQuran out;
  TextPool text;

  // Surah names (placeholder, not real names).
  out.surah_name[0] = text.add("PLACEHOLDER-SURAH-ONE");
  out.surah_name[1] = text.add("PLACEHOLDER-SURAH-TWO");

  // 3 ayahs in surah 1, 2 in surah 2. Word counts: 2,3,2,3,2 = 12.
  const uint16_t words_per_ayah[MiniQuran::kAyahCount] = {2, 3, 2, 3, 2};
  const uint16_t ayah_surah[MiniQuran::kAyahCount] = {1, 1, 1, 2, 2};
  const uint16_t ayah_number[MiniQuran::kAyahCount] = {1, 2, 3, 1, 2};
  const uint16_t ayah_page[MiniQuran::kAyahCount] = {1, 1, 1, 2, 2};

  for (uint32_t i = 0; i < MiniQuran::kAyahCount; ++i) {
    out.ayah_text[i] =
        text.add("PLACEHOLDER-AYAH-" + std::to_string(ayah_surah[i]) + "-" +
                 std::to_string(ayah_number[i]));
  }
  for (uint32_t w = 0; w < MiniQuran::kWordCount; ++w) {
    out.word_text[w] = text.add("WORD" + std::to_string(w));
    out.word_x[w] = static_cast<uint16_t>(40 + w * 60);
  }

  // SURAH_INDEX
  Bytes surahs;
  {
    const uint16_t ayah_counts[MiniQuran::kSurahCount] = {3, 2};
    const uint32_t first_ayah[MiniQuran::kSurahCount] = {0, 3};
    const uint16_t first_page[MiniQuran::kSurahCount] = {1, 2};
    const uint16_t last_page[MiniQuran::kSurahCount] = {1, 2};
    for (uint16_t s = 0; s < MiniQuran::kSurahCount; ++s) {
      Append16(&surahs, static_cast<uint16_t>(s + 1));
      Append16(&surahs, ayah_counts[s]);
      Append32(&surahs, first_ayah[s]);
      Append16(&surahs, first_page[s]);
      Append16(&surahs, last_page[s]);
      Append16(&surahs, 1);                       // first_juz
      surahs.push_back(s == 0 ? 0 : 1);           // revelation_place
      surahs.push_back(1);                        // has_bismillah
      Append32(&surahs, out.surah_name[s].offset);
      Append16(&surahs, static_cast<uint16_t>(out.surah_name[s].length));
      Append16(&surahs, 0);                       // reserved
    }
  }

  // AYAH_INDEX
  Bytes ayahs;
  {
    uint32_t first_word = 0;
    for (uint32_t i = 0; i < MiniQuran::kAyahCount; ++i) {
      Append16(&ayahs, ayah_surah[i]);
      Append16(&ayahs, ayah_number[i]);
      Append16(&ayahs, ayah_page[i]);
      Append16(&ayahs, 1);  // line
      Append16(&ayahs, words_per_ayah[i]);
      Append16(&ayahs, i == 0 ? qpk::kAyahFlagStartsJuz : 0);
      Append32(&ayahs, out.ayah_text[i].offset);
      Append32(&ayahs, out.ayah_text[i].length);
      Append32(&ayahs, first_word);
      first_word += words_per_ayah[i];
    }
  }

  // WORD_INDEX
  Bytes words;
  {
    for (uint32_t w = 0; w < MiniQuran::kWordCount; ++w) {
      Append32(&words, out.word_text[w].offset);
      Append16(&words, static_cast<uint16_t>(out.word_text[w].length));
      Append16(&words, out.word_x[w]);
      Append16(&words, 100);   // y
      Append16(&words, 400);   // width -- deliberately > 255 to exercise u16
      Append16(&words, 48);    // height
      Append16(&words, 1);     // line_id
    }
  }

  // PAGE_INDEX
  Bytes pages;
  {
    const uint16_t ayah_counts[MiniQuran::kPageCount] = {3, 2};
    const uint32_t first_ayah[MiniQuran::kPageCount] = {0, 3};
    const uint32_t first_word[MiniQuran::kPageCount] = {0, 7};
    for (uint16_t p = 0; p < MiniQuran::kPageCount; ++p) {
      Append16(&pages, static_cast<uint16_t>(p + 1));
      Append16(&pages, ayah_counts[p]);
      Append32(&pages, first_ayah[p]);
      Append32(&pages, first_word[p]);
      pages.push_back(15);  // line_count
      pages.push_back(1);   // juz
      Append16(&pages, 0);  // flags
    }
  }

  // JUZ_INDEX
  Bytes juz;
  Append16(&juz, 1);
  Append16(&juz, 1);
  Append32(&juz, 0);

  // LAYOUT_DATA. Every word in this fixture sits on line 1 (see the words
  // loop above), one line per page, so there is exactly one line record per
  // page, y/height matching the words' own fixed y=100/height=48. Must stay
  // byte-identical to mini-quran.ts's buildMiniQuran().
  Bytes layout;
  Append16(&layout, 1);     // layout_version
  Append16(&layout, 1000);  // page_width
  Append16(&layout, 1400);  // page_height
  Append16(&layout, 2);     // line_count_total
  Append32(&layout, qpk::kLayoutHeaderSize);  // line_records_offset
  Append16(&layout, 1);     // line 0: page
  Append16(&layout, 1);     // line_id
  Append16(&layout, 100);   // y
  Append16(&layout, 48);    // height
  Append32(&layout, 0);     // first_word_index
  Append16(&layout, 2);     // line 1: page
  Append16(&layout, 1);     // line_id
  Append16(&layout, 100);   // y
  Append16(&layout, 48);    // height
  Append32(&layout, 7);     // first_word_index

  PackageBuilder builder;
  builder.setType(qpk::PackageType::kQuran);
  builder.setFlags(qpk::kFlagHasWordLayout | qpk::kFlagRightToLeft);
  builder.setContentVersion(3);
  builder.setContentId(0x10);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Mini Test Package");
  builder.addMetadata(qpk::MetadataKey::kLanguage, "ar");
  builder.addMetadata(qpk::MetadataKey::kScript, "placeholder");
  builder.addSection(qpk::SectionId::kSurahIndex, surahs,
                     MiniQuran::kSurahCount);
  builder.addSection(qpk::SectionId::kAyahIndex, ayahs, MiniQuran::kAyahCount);
  builder.addSection(qpk::SectionId::kWordIndex, words, MiniQuran::kWordCount);
  builder.addSection(qpk::SectionId::kPageIndex, pages, MiniQuran::kPageCount);
  builder.addSection(qpk::SectionId::kJuzIndex, juz, 1);
  builder.addSection(qpk::SectionId::kTextData, text.bytes(), 0);
  builder.addSection(qpk::SectionId::kLayoutData, layout, 0);

  out.file = builder.build();
  return out;
}

// A shaped variant of MiniQuran, for exercising ui::QuranScreen's actual
// glyph-blitting path -- something no fixture here could do before, because
// FONT_METADATA/ASSETS/a shaped WORD_INDEX never existed for anything but
// the real, desktop-generated Al-Fatihah atlas (which needs HarfBuzz+
// FreeType, not available in a host test). Same surah/ayah/page/juz shape
// as MiniQuran (2 surahs, 5 ayahs, 12 words, 2 pages) so the surah-boundary
// paging QuranScreen actually does gets exercised too, not just single-word
// rendering.
//
// The "glyphs" are 3 small synthetic bitmaps -- obviously not real Arabic,
// consistent with this file's rule that no Quranic text is fabricated here
// -- cycled across the 12 words so every word has a real, blittable run.
// Sized separately from MiniShapedQuran::kMarkerCount because a static
// constexpr member cannot size an array declared in the same struct.
constexpr uint16_t MiniShapedQuranMarkerSlots = 3;

struct MiniShapedQuran {
  Bytes file;

  static constexpr uint16_t kGlyphCount = 3;
  static constexpr uint8_t kGlyphWidth = 4;
  static constexpr uint8_t kGlyphHeight = 6;
  static constexpr uint16_t kGlyphIdBase = 501;  // 501, 502, 503

  // End-of-ayah markers (docs/qpk-format.md 9b): one glyph per ayah NUMBER,
  // minted contiguously so the device resolves them by arithmetic. This
  // fixture's ayah numbers run 1..3, so three markers cover it exactly -- and
  // the base is deliberately NOT adjacent to kGlyphIdBase, so a test cannot
  // pass by accidentally landing on a word glyph.
  static constexpr uint16_t kMarkerCount = 3;
  static constexpr uint8_t kMarkerWidth = 8;
  static constexpr uint8_t kMarkerHeight = 6;
  static constexpr uint8_t kMarkerXAdvance = 9;
  static constexpr uint16_t kMarkerGlyphIdBase = 601;  // 601, 602, 603

  // The optional LAYOUT_DATA: 4 lines of 3 words over MiniQuran's 12, split
  // across 2 pages so page-crossing is exercised, not just line-crossing.
  static constexpr uint16_t kLayoutLineCount = 4;
  static constexpr uint32_t kWordsPerLine = 3;

  // Expected values the tests assert against.
  uint32_t word_glyph_offset[MiniQuran::kWordCount];  // into ASSETS
  uint16_t word_glyph_count[MiniQuran::kWordCount];
  uint32_t marker_bitmap_offset[MiniShapedQuranMarkerSlots];  // into ASSETS
};

// `ayah_marker_metadata` is the kAyahMarkerGlyphs value, or nullptr to omit
// the key entirely. The atlas itself is unchanged either way, so a caller can
// produce three distinct shapes from one fixture: markers that resolve (the
// default), a package built before markers existed (nullptr), and one that
// claims a range its atlas does not hold ("900:3") -- the two cases
// ui::QuranScreen's Latin "(n)" fallback exists for.
// `with_layout` adds a LAYOUT_DATA section describing four mushaf lines over
// the fixture's 12 words (3 each). That is what selects ui::QuranScreen's
// layout path over fill-and-wrap, so a caller can render the same content
// both ways and compare -- which is the only way to test that the two paths
// disagree about line BREAKS and agree about everything else.
inline MiniShapedQuran BuildMiniShapedQuran(
    const char* ayah_marker_metadata = "601:3", bool with_layout = false) {
  MiniShapedQuran out;
  TextPool text;

  // --- surah / ayah / page / juz: identical shape to BuildMiniQuran -------
  TextPool::Ref surah_name[MiniQuran::kSurahCount];
  surah_name[0] = text.add("PLACEHOLDER-SURAH-ONE");
  surah_name[1] = text.add("PLACEHOLDER-SURAH-TWO");

  const uint16_t words_per_ayah[MiniQuran::kAyahCount] = {2, 3, 2, 3, 2};
  const uint16_t ayah_surah[MiniQuran::kAyahCount] = {1, 1, 1, 2, 2};
  const uint16_t ayah_number[MiniQuran::kAyahCount] = {1, 2, 3, 1, 2};
  const uint16_t ayah_page[MiniQuran::kAyahCount] = {1, 1, 1, 2, 2};

  Bytes surahs;
  {
    const uint16_t ayah_counts[MiniQuran::kSurahCount] = {3, 2};
    const uint32_t first_ayah[MiniQuran::kSurahCount] = {0, 3};
    const uint16_t first_page[MiniQuran::kSurahCount] = {1, 2};
    const uint16_t last_page[MiniQuran::kSurahCount] = {1, 2};
    for (uint16_t s = 0; s < MiniQuran::kSurahCount; ++s) {
      Append16(&surahs, static_cast<uint16_t>(s + 1));
      Append16(&surahs, ayah_counts[s]);
      Append32(&surahs, first_ayah[s]);
      Append16(&surahs, first_page[s]);
      Append16(&surahs, last_page[s]);
      Append16(&surahs, 1);
      surahs.push_back(s == 0 ? 0 : 1);
      surahs.push_back(1);
      Append32(&surahs, surah_name[s].offset);
      Append16(&surahs, static_cast<uint16_t>(surah_name[s].length));
      Append16(&surahs, 0);
    }
  }

  Bytes ayahs;
  {
    uint32_t first_word = 0;
    for (uint32_t i = 0; i < MiniQuran::kAyahCount; ++i) {
      Append16(&ayahs, ayah_surah[i]);
      Append16(&ayahs, ayah_number[i]);
      Append16(&ayahs, ayah_page[i]);
      Append16(&ayahs, 1);
      Append16(&ayahs, words_per_ayah[i]);
      Append16(&ayahs, i == 0 ? qpk::kAyahFlagStartsJuz : 0);
      Append32(&ayahs, 0);  // text_offset -- unused by a shaped reader
      Append32(&ayahs, 0);  // text_length
      Append32(&ayahs, first_word);
      first_word += words_per_ayah[i];
    }
  }

  Bytes pages;
  {
    const uint16_t ayah_counts[MiniQuran::kPageCount] = {3, 2};
    const uint32_t first_ayah[MiniQuran::kPageCount] = {0, 3};
    const uint32_t first_word[MiniQuran::kPageCount] = {0, 7};
    for (uint16_t p = 0; p < MiniQuran::kPageCount; ++p) {
      Append16(&pages, static_cast<uint16_t>(p + 1));
      Append16(&pages, ayah_counts[p]);
      Append32(&pages, first_ayah[p]);
      Append32(&pages, first_word[p]);
      pages.push_back(15);
      pages.push_back(1);
      Append16(&pages, 0);
    }
  }

  Bytes juz;
  Append16(&juz, 1);
  Append16(&juz, 1);
  Append32(&juz, 0);

  // --- glyph atlas: 3 synthetic bitmaps, distinct so a misrouted read is
  // visible rather than silently reading a different-but-plausible glyph --
  const uint8_t kGlyphBitmaps[MiniShapedQuran::kGlyphCount]
                             [MiniShapedQuran::kGlyphHeight] = {
      {0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0},  // solid block
      {0xA0, 0x50, 0xA0, 0x50, 0xA0, 0x50},  // checkerboard
      {0xF0, 0x90, 0x90, 0x90, 0x90, 0xF0},  // hollow box
  };

  Bytes assets;
  uint32_t bitmap_offset[MiniShapedQuran::kGlyphCount];
  for (uint16_t g = 0; g < MiniShapedQuran::kGlyphCount; ++g) {
    bitmap_offset[g] = static_cast<uint32_t>(assets.size());
    assets.insert(assets.end(), kGlyphBitmaps[g],
                 kGlyphBitmaps[g] + MiniShapedQuran::kGlyphHeight);
  }

  // Each word gets a 1-3 glyph run, cycling through the 3 synthetic glyphs
  // deterministically so tests can predict exactly what a given word draws.
  Bytes words;
  {
    for (uint32_t w = 0; w < MiniQuran::kWordCount; ++w) {
      const uint16_t run_len = static_cast<uint16_t>((w % 3) + 1);
      out.word_glyph_offset[w] = static_cast<uint32_t>(assets.size());
      out.word_glyph_count[w] = run_len;
      for (uint16_t g = 0; g < run_len; ++g) {
        Append16(&assets, static_cast<uint16_t>(
                              MiniShapedQuran::kGlyphIdBase + ((w + g) % 3)));
      }

      Append32(&words, out.word_glyph_offset[w]);           // text_offset
      Append16(&words, static_cast<uint16_t>(run_len * 2));  // text_length
      Append16(&words, 0);   // x -- unused, no LAYOUT_DATA in this fixture
      Append16(&words, 0);   // y
      Append16(&words, 0);   // width
      Append16(&words, 0);   // height
      Append16(&words, 1);   // line_id
    }
  }

  // Marker bitmaps, appended after the word runs. Distinct from the three
  // word glyphs above and from each other, so a marker drawn for the wrong
  // ayah number is visible as different ink rather than plausible ink.
  const uint8_t kMarkerBitmaps[MiniShapedQuranMarkerSlots]
                              [MiniShapedQuran::kMarkerHeight] = {
      {0xFF, 0x81, 0x81, 0x81, 0x81, 0xFF},  // marker for ayah 1: ring
      {0xFF, 0xFF, 0x81, 0x81, 0xFF, 0xFF},  // ayah 2: thick ring
      {0x18, 0x3C, 0x7E, 0x7E, 0x3C, 0x18},  // ayah 3: diamond
  };
  for (uint16_t m = 0; m < MiniShapedQuranMarkerSlots; ++m) {
    out.marker_bitmap_offset[m] = static_cast<uint32_t>(assets.size());
    assets.insert(assets.end(), kMarkerBitmaps[m],
                  kMarkerBitmaps[m] + MiniShapedQuran::kMarkerHeight);
  }

  Bytes font_metadata;
  for (uint16_t g = 0; g < MiniShapedQuran::kGlyphCount; ++g) {
    Append16(&font_metadata,
            static_cast<uint16_t>(MiniShapedQuran::kGlyphIdBase + g));
    Append32(&font_metadata, bitmap_offset[g]);
    // Copied to locals before push_back(): passing a static constexpr
    // member straight to push_back(const uint8_t&) ODR-uses it, which needs
    // an out-of-class definition this header (included by multiple TUs)
    // cannot cleanly provide in C++11 -- confirmed by a real link error.
    const uint8_t glyph_width = MiniShapedQuran::kGlyphWidth;
    const uint8_t glyph_height = MiniShapedQuran::kGlyphHeight;
    font_metadata.push_back(glyph_width);
    font_metadata.push_back(glyph_height);
    font_metadata.push_back(static_cast<uint8_t>(6));   // x_advance
    font_metadata.push_back(static_cast<uint8_t>(0));   // x_offset
    font_metadata.push_back(static_cast<uint8_t>(
        static_cast<int8_t>(-MiniShapedQuran::kGlyphHeight)));  // y_offset
    font_metadata.push_back(0);  // reserved
  }

  for (uint16_t m = 0; m < MiniShapedQuranMarkerSlots; ++m) {
    Append16(&font_metadata,
             static_cast<uint16_t>(MiniShapedQuran::kMarkerGlyphIdBase + m));
    Append32(&font_metadata, out.marker_bitmap_offset[m]);
    const uint8_t marker_width = MiniShapedQuran::kMarkerWidth;
    const uint8_t marker_height = MiniShapedQuran::kMarkerHeight;
    const uint8_t marker_advance = MiniShapedQuran::kMarkerXAdvance;
    font_metadata.push_back(marker_width);
    font_metadata.push_back(marker_height);
    font_metadata.push_back(marker_advance);      // x_advance
    font_metadata.push_back(static_cast<uint8_t>(0));  // x_offset
    font_metadata.push_back(static_cast<uint8_t>(
        static_cast<int8_t>(-MiniShapedQuran::kMarkerHeight)));  // y_offset
    font_metadata.push_back(0);  // reserved
  }

  PackageBuilder builder;
  builder.setType(qpk::PackageType::kQuran);
  builder.setFlags(qpk::kFlagShapedTextData | qpk::kFlagRightToLeft);
  builder.setContentVersion(1);
  builder.setContentId(0x50);
  builder.addMetadata(qpk::MetadataKey::kTitle, "Mini Shaped Test Package");
  builder.addMetadata(qpk::MetadataKey::kLanguage, "ar");
  builder.addMetadata(qpk::MetadataKey::kScript, "placeholder");
  if (ayah_marker_metadata != nullptr) {
    builder.addMetadata(qpk::MetadataKey::kAyahMarkerGlyphs,
                        ayah_marker_metadata);
  }
  builder.addSection(qpk::SectionId::kSurahIndex, surahs, MiniQuran::kSurahCount);
  builder.addSection(qpk::SectionId::kAyahIndex, ayahs, MiniQuran::kAyahCount);
  builder.addSection(qpk::SectionId::kWordIndex, words, MiniQuran::kWordCount);
  builder.addSection(qpk::SectionId::kPageIndex, pages, MiniQuran::kPageCount);
  builder.addSection(qpk::SectionId::kJuzIndex, juz, 1);
  builder.addSection(qpk::SectionId::kTextData, text.bytes(), 0);
  builder.addSection(
      qpk::SectionId::kFontMetadata, font_metadata,
      static_cast<uint32_t>(MiniShapedQuran::kGlyphCount +
                            MiniShapedQuranMarkerSlots));
  builder.addSection(qpk::SectionId::kAssets, assets, 0);

  if (with_layout) {
    Bytes layout;
    Append16(&layout, 1);     // layout_version
    Append16(&layout, 800);   // page_width
    Append16(&layout, 780);   // page_height
    Append16(&layout, MiniShapedQuran::kLayoutLineCount);
    Append32(&layout, qpk::kLayoutHeaderSize);  // line_records_offset
    for (uint16_t l = 0; l < MiniShapedQuran::kLayoutLineCount; ++l) {
      Append16(&layout, static_cast<uint16_t>(l < 2 ? 1 : 2));  // page
      Append16(&layout, static_cast<uint16_t>((l % 2) + 1));    // line_id
      Append16(&layout, static_cast<uint16_t>(100 + (l % 2) * 52));  // y
      Append16(&layout, 52);  // height
      Append32(&layout, static_cast<uint32_t>(l * MiniShapedQuran::kWordsPerLine));
    }
    builder.addSection(qpk::SectionId::kLayoutData, layout, 0);
  }

  out.file = builder.build();
  return out;
}

}  // namespace qpktest
