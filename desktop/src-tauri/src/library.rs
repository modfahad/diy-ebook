// Library browser backend: scans a directory for .qpk files and reads just
// enough of each (header + METADATA section) to list it.
//
// Deliberately not a full QPK parser -- no CRC verification, no section
// records beyond METADATA decoded. That is validation's job
// (tools/quran-validator, desktop/converter's validatePackage), not
// browsing's. A package that fails to parse here is reported as an error row
// rather than skipped, so a corrupt file on the card is visible, not silent.
//
// Field offsets mirror docs/qpk-format.md sections 4, 5 and 6 exactly --
// this is an independent Rust reader of the same wire format the TypeScript
// reader (packages/qpk-format) and the firmware reader (firmware/qpk) already
// implement, kept in sync with the spec rather than with either of them.

use serde::Serialize;
use std::fs;
use std::path::Path;

const MAGIC: [u8; 4] = [0x51, 0x50, 0x4b, 0x31]; // "QPK1"
const HEADER_SIZE: usize = 64;
const SECTION_ENTRY_SIZE: usize = 32;

const SECTION_METADATA: u16 = 1;
const SECTION_SURAH_INDEX: u16 = 2;
const SECTION_AYAH_INDEX: u16 = 7;
const SECTION_CHAPTER_INDEX: u16 = 16;
const SECTION_COVER: u16 = 19;

// docs/qpk-format.md 9c: "QCV1" header (16 bytes) + 108x144 pixels at 2 bpp.
const COVER_MAGIC: [u8; 4] = [0x51, 0x43, 0x56, 0x31];
const COVER_BYTES: usize = 16 + 27 * 144;

const META_TITLE: u16 = 1;
const META_AUTHOR: u16 = 2;
const META_LANGUAGE: u16 = 3;

#[derive(Serialize)]
pub struct PackageSummary {
    file: String,
    /// Full path, so the UI can hand a package straight to the Device tab's
    /// uploader without the user retyping it. `file` stays the display name.
    path: String,
    #[serde(rename = "packageType")]
    package_type: String,
    #[serde(rename = "packageSize")]
    package_size: u64,
    #[serde(rename = "contentVersion")]
    content_version: u32,
    #[serde(rename = "contentId")]
    content_id: String,
    title: Option<String>,
    author: Option<String>,
    language: Option<String>,
    #[serde(rename = "chapterCount")]
    chapter_count: u32,
    #[serde(rename = "ayahCount")]
    ayah_count: Option<u32>,
    /// The COVER section's whole payload when the package carries a
    /// well-formed one -- header included, so the UI decodes it with the same
    /// function the converter validates with.
    cover: Option<Vec<u8>>,
    error: Option<String>,
}

fn package_type_name(value: u16) -> &'static str {
    match value {
        1 => "QURAN",
        2 => "BOOK",
        3 => "TRANSLATION",
        4 => "TAFSIR",
        _ => "UNKNOWN",
    }
}

fn read_u16(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

fn read_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

pub fn summarise_file(path: &Path) -> PackageSummary {
    let file_name = path
        .file_name()
        .map(|n| n.to_string_lossy().to_string())
        .unwrap_or_else(|| path.to_string_lossy().to_string());

    let full_path = path.to_string_lossy().to_string();

    let bytes = match fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            return PackageSummary {
                file: file_name,
                path: full_path,
                package_type: "UNKNOWN".to_string(),
                package_size: 0,
                content_version: 0,
                content_id: String::new(),
                title: None,
                author: None,
                language: None,
                chapter_count: 0,
                ayah_count: None,
                cover: None,
                error: Some(format!("could not read file: {e}")),
            };
        }
    };

    match parse(&bytes) {
        Ok(summary) => PackageSummary { file: file_name, path: full_path, ..summary },
        Err(e) => PackageSummary {
            file: file_name,
            path: full_path,
            package_type: "UNKNOWN".to_string(),
            package_size: bytes.len() as u64,
            content_version: 0,
            content_id: String::new(),
            title: None,
            author: None,
            language: None,
            chapter_count: 0,
            ayah_count: None,
            cover: None,
            error: Some(e),
        },
    }
}

fn parse(bytes: &[u8]) -> Result<PackageSummary, String> {
    if bytes.len() < HEADER_SIZE {
        return Err(format!("file is {} bytes, shorter than the 64-byte header", bytes.len()));
    }
    if bytes[0..4] != MAGIC {
        return Err("bad magic: not a QPK1 file".to_string());
    }

    let package_type = read_u16(bytes, 8);
    let section_count = read_u16(bytes, 10) as usize;
    let package_size = read_u32(bytes, 16) as u64;
    let mut content_id = String::with_capacity(32);
    for b in &bytes[24..40] {
        content_id.push_str(&format!("{:02x}", b));
    }
    let content_version = read_u32(bytes, 40);
    let section_table_offset = read_u32(bytes, 44) as usize;

    let table_end = section_table_offset
        .checked_add(section_count * SECTION_ENTRY_SIZE)
        .ok_or_else(|| "section table size overflows".to_string())?;
    if table_end > bytes.len() {
        return Err("section table runs past end of file".to_string());
    }

    struct Section {
        id: u16,
        offset: usize,
        length: usize,
        count: u32,
    }
    let mut sections = Vec::with_capacity(section_count);
    for i in 0..section_count {
        let at = section_table_offset + i * SECTION_ENTRY_SIZE;
        sections.push(Section {
            id: read_u16(bytes, at),
            offset: read_u32(bytes, at + 4) as usize,
            length: read_u32(bytes, at + 12) as usize,
            count: read_u32(bytes, at + 20),
        });
    }

    let mut title = None;
    let mut author = None;
    let mut language = None;
    if let Some(meta) = sections.iter().find(|s| s.id == SECTION_METADATA) {
        let start = meta.offset;
        let end = start
            .checked_add(meta.length)
            .filter(|&e| e <= bytes.len())
            .ok_or_else(|| "METADATA section runs past end of file".to_string())?;
        let mut at = start;
        while at + 4 <= end {
            let key = read_u16(bytes, at);
            let value_len = read_u16(bytes, at + 2) as usize;
            let value_start = at + 4;
            let value_end = value_start + value_len;
            if value_end > end {
                break; // malformed record; stop rather than read past the section
            }
            let value = String::from_utf8_lossy(&bytes[value_start..value_end]).to_string();
            match key {
                META_TITLE => title = Some(value),
                META_AUTHOR => author = Some(value),
                META_LANGUAGE => language = Some(value),
                _ => {}
            }
            let record_len = 4 + value_len;
            at += (record_len + 3) & !3; // each record is 4-byte padded
        }
    }

    let chapter_count = sections
        .iter()
        .filter(|s| s.id == SECTION_SURAH_INDEX || s.id == SECTION_CHAPTER_INDEX)
        .map(|s| s.count)
        .sum();
    let ayah_count = sections.iter().find(|s| s.id == SECTION_AYAH_INDEX).map(|s| s.count);
    let cover = sections
        .iter()
        .find(|s| s.id == SECTION_COVER && s.length == COVER_BYTES)
        .and_then(|s| s.offset.checked_add(s.length).and_then(|end| bytes.get(s.offset..end)))
        .filter(|payload| payload[0..4] == COVER_MAGIC)
        .map(|payload| payload.to_vec());

    Ok(PackageSummary {
        file: String::new(),  // filled in by summarise_file
        path: String::new(),  // filled in by summarise_file
        package_type: package_type_name(package_type).to_string(),
        package_size,
        content_version,
        content_id,
        title,
        author,
        language,
        chapter_count,
        ayah_count,
        cover,
        error: None,
    })
}

/// Bounded, non-recursive-by-default scan for `.qpk` files, one level of
/// subdirectories deep -- matches the on-device library layout
/// (LIBRARY/{QURAN,BOOKS,TRANSLATIONS,TAFSIR}/*.qpk, docs/architecture.md
/// section 6) without an unbounded directory walk.
#[tauri::command]
pub fn scan_library(dir: String) -> Result<Vec<PackageSummary>, String> {
    let root = Path::new(&dir);
    if !root.is_dir() {
        return Err(format!("{dir} is not a directory"));
    }

    let mut candidates: Vec<std::path::PathBuf> = Vec::new();
    collect_qpk_files(root, 0, &mut candidates)?;

    Ok(candidates.iter().map(|p| summarise_file(p)).collect())
}

fn collect_qpk_files(dir: &Path, depth: u8, out: &mut Vec<std::path::PathBuf>) -> Result<(), String> {
    let entries = fs::read_dir(dir).map_err(|e| format!("could not read {}: {e}", dir.display()))?;
    for entry in entries {
        let entry = entry.map_err(|e| format!("could not read directory entry: {e}"))?;
        let path = entry.path();
        if path.is_dir() {
            if depth < 2 {
                collect_qpk_files(&path, depth + 1, out)?;
            }
        } else if path.extension().map(|e| e.eq_ignore_ascii_case("qpk")).unwrap_or(false) {
            out.push(path);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    // Real packages built this session (desktop/converter/scripts/build-test-surahs.mjs),
    // not synthetic fixtures -- proves the Rust reader agrees with the TypeScript
    // writer on real content, not just on bytes Rust itself produced.
    fn examples_dir() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../converter/examples/surahs")
    }

    #[test]
    fn reads_a_real_quran_package() {
        let summary = summarise_file(&examples_dir().join("al-fatihah.qpk"));
        assert!(summary.error.is_none(), "unexpected error: {:?}", summary.error);
        assert_eq!(summary.package_type, "QURAN");
        assert_eq!(summary.title.as_deref(), Some("Al-Fatihah (Surah 1)"));
        assert_eq!(summary.language.as_deref(), Some("ar"));
        assert_eq!(summary.chapter_count, 1);
        assert_eq!(summary.ayah_count, Some(7));
        assert_eq!(summary.content_id.len(), 32);
    }

    #[test]
    fn reads_a_real_translation_package_with_no_chapter_or_ayah_fields() {
        let summary = summarise_file(&examples_dir().join("al-fatihah-en.qpk"));
        assert!(summary.error.is_none(), "unexpected error: {:?}", summary.error);
        assert_eq!(summary.package_type, "TRANSLATION");
        assert_eq!(summary.title.as_deref(), Some("Al-Fatihah - Saheeh International"));
        assert_eq!(summary.author.as_deref(), Some("Saheeh International"));
        // TRANSLATION packages have no SurahIndex/ChapterIndex/AyahIndex sections.
        assert_eq!(summary.chapter_count, 0);
        assert_eq!(summary.ayah_count, None);
    }

    #[test]
    fn every_row_carries_a_usable_full_path_as_well_as_a_display_name() {
        // The Library tab hands this path straight to the Device tab's
        // uploader, so it has to name a file that actually exists -- a
        // display name would not.
        let summary = summarise_file(&examples_dir().join("al-fatihah.qpk"));
        assert_eq!(summary.file, "al-fatihah.qpk");
        assert!(PathBuf::from(&summary.path).is_file(), "path was {:?}", summary.path);
        assert!(summary.path.ends_with("al-fatihah.qpk"));
    }

    #[test]
    fn a_non_qpk_file_is_reported_as_an_error_row_not_a_panic() {
        let summary = summarise_file(&examples_dir().join("../for-bushra.txt"));
        assert!(summary.error.is_some());
        assert_eq!(summary.package_type, "UNKNOWN");
    }

    #[test]
    fn scans_all_six_surah_pairs() {
        let dir = examples_dir();
        let rows = scan_library(dir.to_string_lossy().to_string()).expect("scan should succeed");
        // 6 QURAN + 6 TRANSLATION = 12 files in this flat directory.
        assert_eq!(rows.len(), 12);
        assert!(rows.iter().all(|r| r.error.is_none()));
        assert_eq!(rows.iter().filter(|r| r.package_type == "QURAN").count(), 6);
        assert_eq!(rows.iter().filter(|r| r.package_type == "TRANSLATION").count(), 6);
    }
}
