#include "qpk/qpk_format.h"

namespace qpk {

uint16_t RecordSizeFor(SectionId id) {
  switch (id) {
    case SectionId::kSurahIndex:       return kSurahRecordSize;
    case SectionId::kJuzIndex:
    case SectionId::kHizbIndex:
    case SectionId::kRubIndex:         return kRangeRecordSize;
    case SectionId::kPageIndex:        return kPageRecordSize;
    case SectionId::kAyahIndex:        return kAyahRecordSize;
    case SectionId::kWordIndex:        return kWordRecordSize;
    case SectionId::kSajdahIndex:      return kSajdahRecordSize;
    case SectionId::kTranslationIndex: return kTranslationRecordSize;
    case SectionId::kChapterIndex:     return kChapterRecordSize;
    case SectionId::kSectionIndex:     return kBookSectionRecordSize;
    case SectionId::kTextIndex:        return kTextRecordSize;
    case SectionId::kFontMetadata:     return kGlyphRecordSize;
    case SectionId::kPageImageIndex:   return kPageImageRecordSize;
    default:                           return 0;  // blob or variable (incl. ASSETS)
  }
}

bool IsFixedIndexSection(SectionId id) { return RecordSizeFor(id) != 0; }

const char* PackageTypeName(PackageType type) {
  switch (type) {
    case PackageType::kQuran:       return "QURAN";
    case PackageType::kBook:        return "BOOK";
    case PackageType::kTranslation: return "TRANSLATION";
    case PackageType::kTafsir:      return "TAFSIR";
    default:                        return "UNKNOWN";
  }
}

const char* SectionName(SectionId id) {
  switch (id) {
    case SectionId::kMetadata:         return "METADATA";
    case SectionId::kSurahIndex:       return "SURAH_INDEX";
    case SectionId::kJuzIndex:         return "JUZ_INDEX";
    case SectionId::kHizbIndex:        return "HIZB_INDEX";
    case SectionId::kRubIndex:         return "RUB_INDEX";
    case SectionId::kPageIndex:        return "PAGE_INDEX";
    case SectionId::kAyahIndex:        return "AYAH_INDEX";
    case SectionId::kWordIndex:        return "WORD_INDEX";
    case SectionId::kSajdahIndex:      return "SAJDAH_INDEX";
    case SectionId::kTextData:         return "TEXT_DATA";
    case SectionId::kLayoutData:       return "LAYOUT_DATA";
    case SectionId::kTranslationIndex: return "TRANSLATION_INDEX";
    case SectionId::kTranslationData:  return "TRANSLATION_DATA";
    case SectionId::kFontMetadata:     return "FONT_METADATA";
    case SectionId::kAssets:           return "ASSETS";
    case SectionId::kChapterIndex:     return "CHAPTER_INDEX";
    case SectionId::kSectionIndex:     return "SECTION_INDEX";
    case SectionId::kTextIndex:        return "TEXT_INDEX";
    default:                           return "UNKNOWN";
  }
}

}  // namespace qpk
