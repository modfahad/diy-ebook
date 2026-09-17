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
use std::path::{Path, PathBuf};

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

/// Content id and version from the header alone -- enough to tell whether two
/// files are the same package without reading either one whole.
fn header_identity(path: &Path) -> Option<(String, u32)> {
    use std::io::Read;
    let mut header = [0u8; HEADER_SIZE];
    fs::File::open(path).ok()?.read_exact(&mut header).ok()?;
    if header[0..4] != MAGIC {
        return None;
    }
    let content_id: String = header[24..40].iter().map(|b| format!("{b:02x}")).collect();
    Some((content_id, read_u32(&header, 40)))
}

/// The library subfolder a package type belongs in, as the card lays it out.
fn type_folder(package_type: &str) -> &'static str {
    match package_type {
        "QURAN" => "QURAN",
        "TRANSLATION" => "TRANSLATIONS",
        "TAFSIR" => "TAFSIR",
        _ => "BOOKS",
    }
}

/// `stem` made safe as a file name, much as mobile/src/packageStore.ts does
/// it -- though letters in any script are kept, so an Arabic title stays one.
fn safe_stem(stem: &str) -> String {
    let mut out = String::with_capacity(stem.len());
    let mut in_run = false;
    for c in stem.chars() {
        if c.is_alphanumeric() || c == '_' || c == '.' || c == '-' {
            out.push(c);
            in_run = false;
        } else if !in_run {
            out.push('_');
            in_run = true;
        }
    }
    // No leading dot: that would be a hidden file.
    let trimmed = out.trim_matches(|c: char| c == '_' || c == '.');
    if trimmed.is_empty() { "package".to_string() } else { trimmed.to_string() }
}

/// `folder/stem.qpk`, or `stem-2.qpk` and so on -- never a file that exists.
fn free_path(folder: &Path, stem: &str) -> PathBuf {
    let stem = safe_stem(stem);
    let mut candidate = folder.join(format!("{stem}.qpk"));
    let mut n = 2;
    while candidate.exists() {
        candidate = folder.join(format!("{stem}-{n}.qpk"));
        n += 1;
    }
    candidate
}

fn library_folder(dir: &str, package_type: &str) -> Result<PathBuf, String> {
    let root = Path::new(dir);
    if !root.is_dir() {
        return Err(format!("{dir} is not a directory"));
    }
    let folder = root.join(type_folder(package_type));
    fs::create_dir_all(&folder).map_err(|e| format!("could not create {}: {e}", folder.display()))?;
    Ok(folder)
}

/// Where a package about to be converted should be written: a free name in the
/// library folder for its type. The converter writes it; this only picks.
#[tauri::command]
pub fn library_output_path(dir: String, package_type: String, stem: String) -> Result<String, String> {
    let folder = library_folder(&dir, &package_type)?;
    Ok(free_path(&folder, &stem).to_string_lossy().to_string())
}

#[derive(Serialize)]
pub struct ImportResult {
    /// Where the package now is in the library.
    path: String,
    /// False when an identical package (same content id and version) was
    /// already there, so nothing was copied.
    copied: bool,
}

/// Copies an existing `.qpk` into the library folder for its type. The header
/// is read first, so a file that is not a package is refused rather than
/// copied, and a package already in the library is not copied a second time.
#[tauri::command]
pub fn import_package(src: String, dir: String) -> Result<ImportResult, String> {
    let source = Path::new(&src);
    let bytes = fs::read(source).map_err(|e| format!("could not read {src}: {e}"))?;
    let summary = parse(&bytes)?;

    let identity = (summary.content_id.clone(), summary.content_version);
    let mut existing = Vec::new();
    collect_qpk_files(Path::new(&dir), 0, &mut existing)?;
    for path in existing {
        if header_identity(&path).as_ref() == Some(&identity) {
            return Ok(ImportResult { path: path.to_string_lossy().to_string(), copied: false });
        }
    }

    let folder = library_folder(&dir, &summary.package_type)?;
    let stem = source.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
    let target = free_path(&folder, &stem);
    fs::write(&target, &bytes).map_err(|e| format!("could not write {}: {e}", target.display()))?;
    Ok(ImportResult { path: target.to_string_lossy().to_string(), copied: true })
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

    fn empty_library(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("qd-library-test-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn importing_files_a_package_under_its_type_and_skips_a_second_copy() {
        let dir = empty_library("import");
        let lib = dir.to_string_lossy().to_string();
        let quran = examples_dir().join("al-fatihah.qpk").to_string_lossy().to_string();
        let translation = examples_dir().join("al-fatihah-en.qpk").to_string_lossy().to_string();

        let first = import_package(quran.clone(), lib.clone()).unwrap();
        assert!(first.copied);
        assert_eq!(PathBuf::from(&first.path), dir.join("QURAN").join("al-fatihah.qpk"));

        let again = import_package(quran, lib.clone()).unwrap();
        assert!(!again.copied, "the same package must not be copied twice");
        assert_eq!(again.path, first.path);

        let other = import_package(translation, lib.clone()).unwrap();
        assert_eq!(PathBuf::from(&other.path), dir.join("TRANSLATIONS").join("al-fatihah-en.qpk"));

        assert_eq!(scan_library(lib).unwrap().len(), 2);
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn importing_a_file_that_is_not_a_package_is_refused() {
        let dir = empty_library("refuse");
        let text = examples_dir().join("../for-bushra.txt").to_string_lossy().to_string();
        assert!(import_package(text, dir.to_string_lossy().to_string()).is_err());
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn output_paths_never_name_an_existing_file() {
        let dir = empty_library("output");
        let lib = dir.to_string_lossy().to_string();
        let first = library_output_path(lib.clone(), "BOOK".into(), "My Book: Vol 1".into()).unwrap();
        assert_eq!(PathBuf::from(&first), dir.join("BOOKS").join("My_Book_Vol_1.qpk"));
        fs::write(&first, b"x").unwrap();
        let second = library_output_path(lib, "BOOK".into(), "My Book: Vol 1".into()).unwrap();
        assert_eq!(PathBuf::from(&second), dir.join("BOOKS").join("My_Book_Vol_1-2.qpk"));
        let _ = fs::remove_dir_all(&dir);
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
