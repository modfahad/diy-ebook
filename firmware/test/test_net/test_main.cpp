// Host unit tests for the transport layer: the JSON codec, the pairing-token
// check, library_index.bin, the resumable upload / atomic install state
// machine, and the BLE provisioning state machine.
//
// This is where development rule 8 -- "test power loss during package
// installation" -- finally comes due. It was deferred twice, correctly,
// because there was nothing to install. There is now, and the test cuts power
// at every single write of a full upload-and-install and asserts the invariant
// each time: a partially transferred package is NEVER visible in the library
// index, and whatever is left on disk is either resumable or safely removable.
//
// Not covered here, because it needs hardware: the HTTP socket handling, the
// BLE GATT wiring, and the Wi-Fi radio itself. See docs/development.md.

#include <ctype.h>
#include <unity.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fake_storage.h"
#include "net/auth.h"
#include "net/json.h"
#include "net/library_index.h"
#include "net/photo_store.h"
#include "net/protocol.h"
#include "net/time_zone.h"
#include "net/provisioning_state.h"
#include "net/reading_progress.h"
#include "net/reset_log.h"
#include "net/bookmarks.h"
#include "net/base64.h"
#include "net/upload_manager.h"
#include "qpk/memory_file.h"
#include "qpk/qpk_reader.h"
#include "test_qpk/qpk_test_package.h"
#include "transcript_fixture.h"

using faketest::FakeStorage;
using faketest::PowerLoss;

namespace {

int kErr(net::Error e) { return static_cast<int>(e); }

const char* kContentId = "0102030405060708090a0b0c0d0e0f10";

std::vector<uint8_t> MiniPackage() {
  qpktest::MiniQuran mini = qpktest::BuildMiniQuran();
  return mini.file;
}

/** The content id the mini package actually carries, as hex. */
std::string MiniPackageId() {
  const std::vector<uint8_t> bytes = MiniPackage();
  char hex[33];
  net::ContentIdToHex(bytes.data() + 24, hex, sizeof(hex));
  return std::string(hex);
}

net::UploadBegin BeginFor(const std::vector<uint8_t>& package,
                          const std::string& id) {
  net::UploadBegin request;
  snprintf(request.content_id_hex, sizeof(request.content_id_hex), "%s",
           id.c_str());
  request.content_version = qpk::Read32(package.data() + 40);
  request.type = qpk::Read16(package.data() + 8);
  request.size = package.size();
  request.payload_crc32 = qpk::Read32(package.data() + 52);
  snprintf(request.title, sizeof(request.title), "Mini Test Package");
  return request;
}

/** Uploads `package` in `chunk` byte pieces and finishes. Returns the verdict. */
net::Error UploadAll(net::UploadManager* manager, const std::vector<uint8_t>& package,
                     const std::string& id, uint32_t chunk,
                     net::LibraryEntry* installed = nullptr) {
  uint64_t received = 0;
  net::Error error = manager->beginUpload(BeginFor(package, id), &received);
  if (error != net::Error::kOk) return error;

  while (received < package.size()) {
    const uint32_t take = static_cast<uint32_t>(
        package.size() - received < chunk ? package.size() - received : chunk);
    uint64_t expected = 0;
    error = manager->writeChunk(id.c_str(), received, package.data() + received,
                                take, &received, &expected);
    if (error != net::Error::kOk) return error;
  }
  return manager->finish(id.c_str(), 1700000000u, installed);
}

}  // namespace

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

void test_json_reads_a_flat_object() {
  const char* body =
      "{\"contentId\":\"abc\",\"size\":123456,\"ok\":true,\"version\":3}";
  net::JsonReader reader(body);
  TEST_ASSERT_TRUE(reader.valid());

  char text[32];
  TEST_ASSERT_TRUE(reader.getString("contentId", text, sizeof(text)));
  TEST_ASSERT_EQUAL_STRING("abc", text);

  uint64_t size = 0;
  TEST_ASSERT_TRUE(reader.getUint64("size", &size));
  TEST_ASSERT_EQUAL_UINT32(123456u, static_cast<uint32_t>(size));

  bool flag = false;
  TEST_ASSERT_TRUE(reader.getBool("ok", &flag));
  TEST_ASSERT_TRUE(flag);

  uint32_t version = 0;
  TEST_ASSERT_TRUE(reader.getUint32("version", &version));
  TEST_ASSERT_EQUAL_UINT32(3u, version);

  TEST_ASSERT_FALSE(reader.has("missing"));
}

void test_json_handles_escapes_and_whitespace() {
  const char* body = "{ \"a\" : \"line\\nbreak \\\"q\\\" \\u00e9\" }";
  net::JsonReader reader(body);
  char text[32];
  TEST_ASSERT_TRUE(reader.getString("a", text, sizeof(text)));
  // \u00e9 is two UTF-8 bytes.
  TEST_ASSERT_EQUAL_STRING("line\nbreak \"q\" \xc3\xa9", text);
}

void test_json_rejects_rather_than_guesses() {
  // A malformed body must be a 400, never a silently defaulted field.
  net::JsonReader not_object("[1,2,3]");
  TEST_ASSERT_FALSE(not_object.valid());

  net::JsonReader reader("{\"n\":-5,\"f\":1.5,\"s\":\"x\"}");
  uint64_t value = 0;
  TEST_ASSERT_FALSE(reader.getUint64("n", &value));  // negative
  TEST_ASSERT_FALSE(reader.getUint64("f", &value));  // not an integer
  TEST_ASSERT_FALSE(reader.getUint64("s", &value));  // not a number

  char text[4];
  net::JsonReader long_value("{\"s\":\"much too long\"}");
  TEST_ASSERT_FALSE(long_value.getString("s", text, sizeof(text)));
}

void test_json_skips_nested_values_without_desyncing() {
  const char* body = "{\"a\":{\"x\":1},\"b\":[1,2,{\"y\":2}],\"c\":7}";
  net::JsonReader reader(body);
  uint32_t value = 0;
  TEST_ASSERT_TRUE(reader.getUint32("c", &value));
  TEST_ASSERT_EQUAL_UINT32(7u, value);
}

void test_json_writer_escapes_and_reports_overflow() {
  char buffer[128];
  net::JsonWriter writer(buffer, sizeof(buffer));
  writer.beginObject();
  writer.keyString("q", "say \"hi\"\n");
  writer.keyUint("n", 42);
  writer.keyBool("b", false);
  writer.endObject();
  TEST_ASSERT_TRUE(writer.ok());
  TEST_ASSERT_EQUAL_STRING("{\"q\":\"say \\\"hi\\\"\\n\",\"n\":42,\"b\":false}",
                           buffer);

  char tiny[8];
  net::JsonWriter small(tiny, sizeof(tiny));
  small.beginObject();
  small.keyString("key", "a value that does not fit");
  small.endObject();
  // A truncated response must never be sent as if it were complete.
  TEST_ASSERT_FALSE(small.ok());
}

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

void test_bearer_token_check() {
  const char* token = "s3cret-token";
  const uint32_t length = 12;
  TEST_ASSERT_TRUE(net::CheckBearer("Bearer s3cret-token", token, length));
  TEST_ASSERT_FALSE(net::CheckBearer("Bearer s3cret-toke", token, length));
  TEST_ASSERT_FALSE(net::CheckBearer("Bearer s3cret-tokenX", token, length));
  TEST_ASSERT_FALSE(net::CheckBearer("Basic s3cret-token", token, length));
  TEST_ASSERT_FALSE(net::CheckBearer(nullptr, token, length));
  // An unpaired device must not serve a request just because no token is set.
  TEST_ASSERT_FALSE(net::CheckBearer("Bearer anything", "", 0));
  TEST_ASSERT_FALSE(net::CheckBearer("Bearer anything", nullptr, 0));
}

void test_constant_time_compare_is_value_correct() {
  TEST_ASSERT_TRUE(net::ConstantTimeEquals("abcd", "abcd", 4));
  TEST_ASSERT_FALSE(net::ConstantTimeEquals("abcd", "abce", 4));
  TEST_ASSERT_FALSE(net::ConstantTimeEquals("abcd", "zbcd", 4));
}

// ---------------------------------------------------------------------------
// library_index.bin
// ---------------------------------------------------------------------------

void test_content_id_hex_round_trip() {
  uint8_t id[16];
  TEST_ASSERT_TRUE(net::HexToContentId(kContentId, id));
  TEST_ASSERT_EQUAL_UINT8(0x01, id[0]);
  TEST_ASSERT_EQUAL_UINT8(0x10, id[15]);

  char hex[33];
  TEST_ASSERT_TRUE(net::ContentIdToHex(id, hex, sizeof(hex)));
  TEST_ASSERT_EQUAL_STRING(kContentId, hex);

  TEST_ASSERT_FALSE(net::HexToContentId("nothex", id));
  TEST_ASSERT_FALSE(net::HexToContentId("0102030405060708090a0b0c0d0e0f1", id));
}

void test_utf8_truncation_never_splits_a_codepoint() {
  // "aé" -- the accented character is two bytes.
  const char* source = "a\xc3\xa9";
  char out[3];
  net::CopyUtf8Truncated(source, 3, out, sizeof(out));
  // Only "a" fits: the two-byte sequence must not be cut in half.
  TEST_ASSERT_EQUAL_STRING("a", out);

  char roomy[8];
  net::CopyUtf8Truncated(source, 3, roomy, sizeof(roomy));
  TEST_ASSERT_EQUAL_STRING("a\xc3\xa9", roomy);
}

void test_library_index_round_trips() {
  FakeStorage storage;
  net::LibraryIndex index;

  net::LibraryEntry entry;
  TEST_ASSERT_TRUE(net::HexToContentId(kContentId, entry.content_id));
  entry.content_version = 7;
  entry.package_size = 4096;
  entry.payload_crc32 = 0xDEADBEEF;
  entry.installed_at = 1700000000u;
  entry.type = 1;  // QURAN
  snprintf(entry.title, sizeof(entry.title), "A Mushaf");
  snprintf(entry.author, sizeof(entry.author), "A Publisher");
  snprintf(entry.language, sizeof(entry.language), "ar");
  TEST_ASSERT_TRUE(index.upsert(entry));
  TEST_ASSERT_TRUE(index.save(&storage));

  net::LibraryIndex reloaded;
  TEST_ASSERT_TRUE(reloaded.load(&storage));
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.count());
  const net::LibraryEntry* found = reloaded.find(entry.content_id);
  TEST_ASSERT_NOT_NULL(found);
  TEST_ASSERT_EQUAL_UINT32(7u, found->content_version);
  TEST_ASSERT_EQUAL_STRING("A Mushaf", found->title);
  TEST_ASSERT_EQUAL_STRING("ar", found->language);

  // The location is derived, never stored: there is exactly one right answer.
  char location[56];
  TEST_ASSERT_TRUE(net::BuildLocation(*found, location, sizeof(location)));
  TEST_ASSERT_EQUAL_STRING("QURAN/0102030405060708090a0b0c0d0e0f10.qpk", location);
}

void test_library_index_rejects_a_corrupt_file() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::LibraryEntry entry;
  net::HexToContentId(kContentId, entry.content_id);
  entry.type = 1;
  index.upsert(entry);
  index.save(&storage);

  std::vector<uint8_t> bytes = *storage.peek("/LIBRARY/library_index.bin");
  bytes[40] ^= 0xFF;  // inside the first entry
  storage.put("/LIBRARY/library_index.bin", bytes);

  net::LibraryIndex reloaded;
  TEST_ASSERT_FALSE(reloaded.load(&storage));
  TEST_ASSERT_EQUAL_UINT16(0, reloaded.count());
}

void test_library_index_rebuilds_from_the_packages_on_disk() {
  // A corrupt index must not brick the library: it is re-derivable.
  FakeStorage storage;
  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  storage.put("/LIBRARY/QURAN/" + id + ".qpk", package);
  storage.put("/LIBRARY/QURAN/not-a-package.txt", {1, 2, 3});

  net::LibraryIndex index;
  TEST_ASSERT_TRUE(index.rebuild(&storage));
  TEST_ASSERT_EQUAL_UINT16(1, index.count());
  TEST_ASSERT_EQUAL_STRING("Mini Test Package", index.at(0)->title);
  TEST_ASSERT_EQUAL_UINT32(3u, index.at(0)->content_version);
}

void test_library_index_upsert_replaces_and_remove_works() {
  net::LibraryIndex index;
  net::LibraryEntry entry;
  net::HexToContentId(kContentId, entry.content_id);
  entry.content_version = 1;
  index.upsert(entry);
  entry.content_version = 2;
  index.upsert(entry);
  TEST_ASSERT_EQUAL_UINT16(1, index.count());
  TEST_ASSERT_EQUAL_UINT32(2u, index.at(0)->content_version);
  TEST_ASSERT_TRUE(index.remove(entry.content_id));
  TEST_ASSERT_EQUAL_UINT16(0, index.count());
  TEST_ASSERT_FALSE(index.remove(entry.content_id));
}

// ---------------------------------------------------------------------------
// progress.bin (net::ReadingProgress) -- docs/pending.md's reading-position
// persistence, keyed on content_id like LibraryEntry is.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// resets.log / alive.bin (net::reset_log) -- why the device restarted.
// ---------------------------------------------------------------------------

std::string FileText(FakeStorage& storage, const char* path) {
  const auto* bytes = storage.peek(path);
  return bytes == nullptr ? std::string() : std::string(bytes->begin(), bytes->end());
}

// ---------------------------------------------------------------------------
// bookmarks.bin (net::Bookmarks)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// base64 chunk bodies (net::DecodeBase64)
// ---------------------------------------------------------------------------

std::string EncodeBase64ForTest(const std::vector<uint8_t>& bytes) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t n = static_cast<uint32_t>(bytes[i]) << 16;
    if (i + 1 < bytes.size()) n |= static_cast<uint32_t>(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size()) n |= bytes[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? kAlphabet[n & 63] : '=';
  }
  return out;
}

void test_base64_decodes_text_and_every_byte_value() {
  uint8_t out[64];
  uint32_t n = 0;
  TEST_ASSERT_TRUE(net::DecodeBase64("SGVsbG8=", 8, out, sizeof(out), &n));
  TEST_ASSERT_EQUAL_UINT32(5u, n);
  TEST_ASSERT_EQUAL_MEMORY("Hello", out, 5);
  TEST_ASSERT_TRUE(net::DecodeBase64("", 0, out, sizeof(out), &n));
  TEST_ASSERT_EQUAL_UINT32(0u, n);

  // A binary chunk: zeros included, which is the whole point of base64 here.
  std::vector<uint8_t> bytes(1000);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 7);
  const std::string text = EncodeBase64ForTest(bytes);
  std::vector<uint8_t> decoded(bytes.size());
  TEST_ASSERT_TRUE(net::DecodeBase64(text.c_str(), static_cast<uint32_t>(text.size()), decoded.data(),
                                     static_cast<uint32_t>(decoded.size()), &n));
  TEST_ASSERT_EQUAL_UINT32(1000u, n);
  TEST_ASSERT_EQUAL_MEMORY(bytes.data(), decoded.data(), bytes.size());
}

void test_base64_rejects_bad_characters_overflow_and_data_after_padding() {
  uint8_t out[4];
  uint32_t n = 0;
  TEST_ASSERT_FALSE(net::DecodeBase64("SGV*bG8=", 8, out, sizeof(out), &n));
  TEST_ASSERT_FALSE(net::DecodeBase64("SGVsbG8=", 8, out, sizeof(out), &n));  // 5 bytes into 4
  TEST_ASSERT_FALSE(net::DecodeBase64("SG=VsbG8", 8, out, sizeof(out), &n));
}

net::Bookmark PageMark(uint8_t id, uint32_t page) {
  net::Bookmark mark;
  mark.content_id[0] = id;
  mark.kind = net::BookmarkKind::kPageBook;
  mark.a = page;
  return mark;
}

void test_bookmarks_round_trip_newest_first() {
  FakeStorage storage;
  net::Bookmarks marks;
  marks.add(PageMark(1, 10));
  net::Bookmark quran;
  quran.content_id[0] = 2;
  quran.kind = net::BookmarkKind::kQuran;
  quran.a = 18;
  quran.b = 2140;
  quran.c = 3;
  quran.created_unix = 1789390000u;
  marks.add(quran);
  TEST_ASSERT_TRUE(marks.save(&storage));
  TEST_ASSERT_FALSE(storage.hasFile("/USER/bookmarks.tmp"));

  net::Bookmarks loaded;
  loaded.load(&storage);
  TEST_ASSERT_EQUAL_UINT16(2, loaded.count());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::BookmarkKind::kQuran),
                        static_cast<int>(loaded.at(0)->kind));
  TEST_ASSERT_EQUAL_UINT32(18u, loaded.at(0)->a);
  TEST_ASSERT_EQUAL_UINT32(2140u, loaded.at(0)->b);
  TEST_ASSERT_EQUAL_UINT32(3u, loaded.at(0)->c);
  TEST_ASSERT_EQUAL_UINT32(1789390000u, loaded.at(0)->created_unix);
  TEST_ASSERT_EQUAL_UINT32(10u, loaded.at(1)->a);
  TEST_ASSERT_NULL(loaded.at(2));
}

void test_bookmarks_same_place_moves_to_front_and_full_list_drops_the_oldest() {
  net::Bookmarks marks;
  marks.add(PageMark(1, 1));
  marks.add(PageMark(1, 2));
  marks.add(PageMark(1, 1));  // again: moved, not duplicated
  TEST_ASSERT_EQUAL_UINT16(2, marks.count());
  TEST_ASSERT_EQUAL_UINT32(1u, marks.at(0)->a);
  TEST_ASSERT_TRUE(marks.contains(PageMark(1, 2)));
  TEST_ASSERT_FALSE(marks.contains(PageMark(2, 2)));

  for (uint32_t page = 100; page < 100 + net::kMaxBookmarks; ++page) marks.add(PageMark(3, page));
  TEST_ASSERT_EQUAL_UINT16(net::kMaxBookmarks, marks.count());
  TEST_ASSERT_EQUAL_UINT32(100u + net::kMaxBookmarks - 1, marks.at(0)->a);
  TEST_ASSERT_FALSE(marks.contains(PageMark(1, 1)));  // the oldest went

  TEST_ASSERT_TRUE(marks.removeAt(0));
  TEST_ASSERT_EQUAL_UINT16(net::kMaxBookmarks - 1, marks.count());
  TEST_ASSERT_FALSE(marks.removeAt(net::kMaxBookmarks));
}

void test_bookmarks_damaged_file_loads_as_empty() {
  FakeStorage storage;
  net::Bookmarks marks;
  marks.add(PageMark(1, 5));
  TEST_ASSERT_TRUE(marks.save(&storage));
  std::vector<uint8_t> bytes = *storage.peek("/USER/bookmarks.bin");
  bytes[bytes.size() - 1] ^= 0xFF;  // inside a record: the CRC must catch it
  storage.put("/USER/bookmarks.bin", bytes);
  net::Bookmarks loaded;
  loaded.add(PageMark(9, 9));
  loaded.load(&storage);
  TEST_ASSERT_EQUAL_UINT16(0, loaded.count());
}

void test_reset_log_appends_one_line_per_boot() {
  FakeStorage storage;
  TEST_ASSERT_TRUE(net::AppendResetRecord(&storage, 7, "brownout", 1789390000u));
  TEST_ASSERT_TRUE(net::AppendResetRecord(&storage, 8, "power-on", 0));
  TEST_ASSERT_EQUAL_STRING(
      "boot=7 reset=brownout last_alive=1789390000\nboot=8 reset=power-on last_alive=0\n",
      FileText(storage, "/DEVICE/resets.log").c_str());
}

void test_reset_log_stays_bounded_and_keeps_the_newest_whole_lines() {
  FakeStorage storage;
  for (uint32_t i = 0; i < 1000; ++i) {
    TEST_ASSERT_TRUE(net::AppendResetRecord(&storage, i, "panic", i));
  }
  const std::string text = FileText(storage, "/DEVICE/resets.log");
  TEST_ASSERT_TRUE(text.size() <= net::kResetLogMaxBytes + 96);
  TEST_ASSERT_TRUE(text.find("boot=999 reset=panic last_alive=999\n") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("boot=", text.substr(0, 5).c_str());  // starts on a whole line
  TEST_ASSERT_FALSE(storage.hasFile("/DEVICE/resets.tmp"));
}

void test_alive_time_round_trips_and_is_zero_when_absent_or_damaged() {
  FakeStorage storage;
  TEST_ASSERT_EQUAL_UINT32(0u, net::LoadAliveTime(&storage));
  TEST_ASSERT_TRUE(net::SaveAliveTime(&storage, 1789391234u));
  TEST_ASSERT_EQUAL_UINT32(1789391234u, net::LoadAliveTime(&storage));

  std::vector<uint8_t> bytes = *storage.peek("/DEVICE/alive.bin");
  bytes[0] = 'X';
  storage.put("/DEVICE/alive.bin", bytes);
  TEST_ASSERT_EQUAL_UINT32(0u, net::LoadAliveTime(&storage));
}

void test_page_progress_round_trips_without_touching_progress_bin() {
  FakeStorage storage;
  net::PageProgress progress;
  net::HexToContentId(kContentId, progress.content_id);
  progress.page = 487;
  TEST_ASSERT_TRUE(net::SavePageProgress(&storage, progress));
  TEST_ASSERT_TRUE(storage.hasFile("/USER/pages.bin"));
  TEST_ASSERT_FALSE(storage.hasFile("/USER/pages.tmp"));
  TEST_ASSERT_FALSE(storage.hasFile("/USER/progress.bin"));

  net::PageProgress reloaded;
  TEST_ASSERT_TRUE(net::LoadPageProgress(&storage, &reloaded));
  TEST_ASSERT_EQUAL_UINT32(487u, reloaded.page);
  TEST_ASSERT_EQUAL_MEMORY(progress.content_id, reloaded.content_id, 16);

  // A Quran position saved alongside does not disturb it.
  net::ReadingProgress quran;
  quran.surah_id = 36;
  TEST_ASSERT_TRUE(net::SaveReadingProgress(&storage, quran));
  TEST_ASSERT_TRUE(net::LoadPageProgress(&storage, &reloaded));
  TEST_ASSERT_EQUAL_UINT32(487u, reloaded.page);
}

void test_page_progress_absent_or_corrupt_is_rejected() {
  FakeStorage storage;
  net::PageProgress out;
  out.page = 9;
  TEST_ASSERT_FALSE(net::LoadPageProgress(&storage, &out));
  TEST_ASSERT_EQUAL_UINT32(0u, out.page);

  net::PageProgress progress;
  progress.page = 12;
  TEST_ASSERT_TRUE(net::SavePageProgress(&storage, progress));
  std::vector<uint8_t> bytes = *storage.peek("/USER/pages.bin");
  bytes[8] ^= 0xFF;  // inside the page field; the CRC must catch it
  storage.put("/USER/pages.bin", bytes);
  TEST_ASSERT_FALSE(net::LoadPageProgress(&storage, &out));
}

void test_reading_progress_round_trips() {
  FakeStorage storage;
  net::ReadingProgress progress;
  net::HexToContentId(kContentId, progress.content_id);
  progress.surah_id = 18;
  progress.first_ayah_index = 12345;
  progress.screen_number = 7;

  TEST_ASSERT_TRUE(net::SaveReadingProgress(&storage, progress));
  TEST_ASSERT_TRUE(storage.hasFile("/USER/progress.bin"));
  TEST_ASSERT_FALSE(storage.hasFile("/USER/progress.tmp"));  // renamed away

  net::ReadingProgress reloaded;
  TEST_ASSERT_TRUE(net::LoadReadingProgress(&storage, &reloaded));
  TEST_ASSERT_EQUAL_UINT16(18, reloaded.surah_id);
  TEST_ASSERT_EQUAL_UINT32(12345u, reloaded.first_ayah_index);
  TEST_ASSERT_EQUAL_UINT16(7, reloaded.screen_number);
  TEST_ASSERT_EQUAL_MEMORY(progress.content_id, reloaded.content_id, 16);
}

void test_reading_progress_second_save_overwrites_the_first() {
  // One record only (see reading_progress.h): opening a different package
  // overwrites where the last one left off, it does not accumulate a history.
  FakeStorage storage;
  net::ReadingProgress first;
  net::HexToContentId(kContentId, first.content_id);
  first.surah_id = 2;
  TEST_ASSERT_TRUE(net::SaveReadingProgress(&storage, first));

  net::ReadingProgress second;
  second.content_id[0] = 0xAA;
  second.surah_id = 114;
  TEST_ASSERT_TRUE(net::SaveReadingProgress(&storage, second));

  net::ReadingProgress reloaded;
  TEST_ASSERT_TRUE(net::LoadReadingProgress(&storage, &reloaded));
  TEST_ASSERT_EQUAL_UINT16(114, reloaded.surah_id);
  TEST_ASSERT_EQUAL_UINT8(0xAA, reloaded.content_id[0]);
}

void test_reading_progress_absent_file_is_not_an_error() {
  FakeStorage storage;
  net::ReadingProgress out;
  out.surah_id = 99;  // must be reset, not left dangling
  TEST_ASSERT_FALSE(net::LoadReadingProgress(&storage, &out));
  TEST_ASSERT_EQUAL_UINT16(0, out.surah_id);
}

void test_reading_progress_rejects_a_corrupt_record() {
  FakeStorage storage;
  net::ReadingProgress progress;
  progress.surah_id = 5;
  net::SaveReadingProgress(&storage, progress);

  std::vector<uint8_t> bytes = *storage.peek("/USER/progress.bin");
  bytes[6] ^= 0xFF;  // inside surah_id, before the trailing crc32
  storage.put("/USER/progress.bin", bytes);

  net::ReadingProgress reloaded;
  TEST_ASSERT_FALSE(net::LoadReadingProgress(&storage, &reloaded));
}

void test_reading_progress_rejects_a_future_format_version() {
  FakeStorage storage;
  net::ReadingProgress progress;
  net::SaveReadingProgress(&storage, progress);

  std::vector<uint8_t> bytes = *storage.peek("/USER/progress.bin");
  bytes[4] = 0xFF;  // format_version -- rejected before the crc is even checked
  storage.put("/USER/progress.bin", bytes);

  net::ReadingProgress reloaded;
  TEST_ASSERT_FALSE(net::LoadReadingProgress(&storage, &reloaded));
}

// ---------------------------------------------------------------------------
// Resumable upload
// ---------------------------------------------------------------------------

void test_upload_installs_a_package() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  net::LibraryEntry installed;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(UploadAll(&manager, package, id, 256, &installed)));

  TEST_ASSERT_TRUE(storage.hasFile("/LIBRARY/QURAN/" + id + ".qpk"));
  TEST_ASSERT_FALSE(storage.hasFile("/DEVICE/uploads/" + id + ".part"));
  TEST_ASSERT_FALSE(storage.hasFile("/DEVICE/uploads/" + id + ".meta"));
  TEST_ASSERT_EQUAL_UINT16(1, index.count());
  TEST_ASSERT_EQUAL_STRING("Mini Test Package", index.at(0)->title);
  TEST_ASSERT_EQUAL_UINT32(1700000000u, index.at(0)->installed_at);

  net::LibraryIndex reloaded;
  TEST_ASSERT_TRUE(reloaded.load(&storage));
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.count());
}

void test_upload_resumes_from_the_size_on_disk() {
  FakeStorage storage;
  net::LibraryIndex index;
  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();

  {
    net::UploadManager manager;
    manager.begin(&storage, &index);
    uint64_t received = 0;
    TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                          kErr(manager.beginUpload(BeginFor(package, id), &received)));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(received));
    uint64_t expected = 0;
    manager.writeChunk(id.c_str(), 0, package.data(), 300, &received, &expected);
    TEST_ASSERT_EQUAL_UINT32(300u, static_cast<uint32_t>(received));
  }

  // Reboot: brand new objects, same disk. Nothing was kept in RAM.
  net::LibraryIndex fresh_index;
  net::UploadManager fresh;
  fresh.begin(&storage, &fresh_index);
  uint64_t received = 0;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(fresh.beginUpload(BeginFor(package, id), &received)));
  TEST_ASSERT_EQUAL_UINT32(300u, static_cast<uint32_t>(received));

  while (received < package.size()) {
    const uint32_t take =
        static_cast<uint32_t>(package.size() - received < 512 ? package.size() - received : 512);
    uint64_t expected = 0;
    TEST_ASSERT_EQUAL_INT(
        kErr(net::Error::kOk),
        kErr(fresh.writeChunk(id.c_str(), received, package.data() + received, take,
                              &received, &expected)));
  }
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(fresh.finish(id.c_str(), 0, nullptr)));
  TEST_ASSERT_TRUE(storage.hasFile("/LIBRARY/QURAN/" + id + ".qpk"));
}

void test_repeated_chunk_is_idempotent() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  manager.beginUpload(BeginFor(package, id), &received);

  uint64_t expected = 0;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(manager.writeChunk(id.c_str(), 0, package.data(), 128,
                                                &received, &expected)));
  TEST_ASSERT_EQUAL_UINT32(128u, static_cast<uint32_t>(received));

  // The response was lost and the client retried the same chunk. This must not
  // append it twice; a lost response is the common case, not the rare one.
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(manager.writeChunk(id.c_str(), 0, package.data(), 128,
                                                &received, &expected)));
  TEST_ASSERT_EQUAL_UINT32(128u, static_cast<uint32_t>(received));
}

void test_offset_mismatch_reports_where_to_resume() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  manager.beginUpload(BeginFor(package, id), &received);
  uint64_t expected = 0;
  manager.writeChunk(id.c_str(), 0, package.data(), 128, &received, &expected);

  // A gap: the client thinks it is further along than the device is.
  TEST_ASSERT_EQUAL_INT(
      kErr(net::Error::kOffsetMismatch),
      kErr(manager.writeChunk(id.c_str(), 500, package.data() + 500, 64, &received,
                              &expected)));
  // The 409 carries the real offset so the client re-syncs in one round trip.
  TEST_ASSERT_EQUAL_UINT32(128u, static_cast<uint32_t>(expected));
}

// A QPK package is arbitrary binary data and will contain 0x00 bytes as a
// matter of course. This is the one thing a transport that buffers a chunk
// into a C string (strlen-terminated) instead of a length-prefixed buffer
// would get wrong while still reporting success -- see http_server.cpp's
// ReadChunkedBody for the transport-side half of this invariant, which
// cannot be exercised on the host. This test pins the UploadManager half:
// writeChunk() must be byte-exact, not "exact up to the first NUL".
void test_write_chunk_preserves_embedded_nul_bytes() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const uint8_t chunk[] = {0x41, 0x00, 0x42, 0x00, 0x00, 0x43, 0x00, 0x44};
  net::UploadBegin begin;
  snprintf(begin.content_id_hex, sizeof(begin.content_id_hex), "%s", kContentId);
  begin.content_version = 1;
  begin.type = 1;
  begin.size = sizeof(chunk);
  begin.payload_crc32 = 0;

  uint64_t received = 0;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(manager.beginUpload(begin, &received)));

  uint64_t expected = 0;
  TEST_ASSERT_EQUAL_INT(
      kErr(net::Error::kOk),
      kErr(manager.writeChunk(kContentId, 0, chunk, sizeof(chunk), &received,
                              &expected)));
  TEST_ASSERT_EQUAL_UINT64(sizeof(chunk), received);

  // Read the .part file back directly: a length-losing bug would still
  // report the right *count* above, since received_out comes from the file
  // size on disk, not from re-reading what was just written.
  uint8_t roundtrip[sizeof(chunk)] = {0};
  const std::string part_path =
      std::string("/DEVICE/uploads/") + kContentId + ".part";
  const int32_t n = storage.read(part_path.c_str(), 0, roundtrip, sizeof(roundtrip));
  TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(sizeof(chunk)), n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(chunk, roundtrip, sizeof(chunk));
}

void test_chunk_limits_are_enforced() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  manager.beginUpload(BeginFor(package, id), &received);

  std::vector<uint8_t> huge(net::kMaxChunkBytes + 1, 0);
  uint64_t expected = 0;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kChunkTooLarge),
                        kErr(manager.writeChunk(id.c_str(), 0, huge.data(),
                                                static_cast<uint32_t>(huge.size()),
                                                &received, &expected)));

  // Writing past the declared size is a size mismatch, not an append.
  TEST_ASSERT_EQUAL_INT(
      kErr(net::Error::kSizeMismatch),
      kErr(manager.writeChunk(id.c_str(), package.size() - 4, package.data(), 64,
                              &received, &expected)));
}

void test_chunk_without_a_session_is_not_found() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);
  uint64_t received = 0;
  uint64_t expected = 0;
  uint8_t byte = 0;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kNoSession),
                        kErr(manager.writeChunk(kContentId, 0, &byte, 1, &received,
                                                &expected)));
}

void test_finish_rejects_a_short_upload() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  manager.beginUpload(BeginFor(package, id), &received);
  uint64_t expected = 0;
  manager.writeChunk(id.c_str(), 0, package.data(), 128, &received, &expected);

  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kSizeMismatch),
                        kErr(manager.finish(id.c_str(), 0, nullptr)));
  // A short upload is still resumable: it was not thrown away.
  TEST_ASSERT_TRUE(storage.hasFile("/DEVICE/uploads/" + id + ".part"));
}

void test_finish_rejects_a_corrupted_payload() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  const net::UploadBegin request = BeginFor(package, id);
  // Damage a byte in the payload AFTER computing the declared checksum: this
  // is the transfer-corruption case, and only the finish-time verify sees it.
  package[400] ^= 0xFF;

  uint64_t received = 0;
  manager.beginUpload(request, &received);
  uint64_t expected = 0;
  while (received < package.size()) {
    const uint32_t take =
        static_cast<uint32_t>(package.size() - received < 512 ? package.size() - received : 512);
    manager.writeChunk(id.c_str(), received, package.data() + received, take,
                       &received, &expected);
  }

  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kVerifyFailed),
                        kErr(manager.finish(id.c_str(), 0, nullptr)));
  // A package that does not verify is not worth resuming: it is gone.
  TEST_ASSERT_FALSE(storage.hasFile("/DEVICE/uploads/" + id + ".part"));
  TEST_ASSERT_FALSE(storage.hasFile("/DEVICE/uploads/" + id + ".meta"));
  TEST_ASSERT_EQUAL_UINT16(0, index.count());
}

void test_begin_discards_a_part_from_a_different_package() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  manager.beginUpload(BeginFor(package, id), &received);
  uint64_t expected = 0;
  manager.writeChunk(id.c_str(), 0, package.data(), 256, &received, &expected);

  // Same content id, different version: the bytes on disk are not a prefix of
  // this package, so they must be thrown away rather than resumed.
  net::UploadBegin other = BeginFor(package, id);
  other.content_version += 1;
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(manager.beginUpload(other, &received)));
  TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(received));
}

void test_begin_refuses_when_there_is_no_room() {
  FakeStorage storage;
  storage.setFreeBytes(16);
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  uint64_t received = 0;
  TEST_ASSERT_EQUAL_INT(
      kErr(net::Error::kNoSpace),
      kErr(manager.beginUpload(BeginFor(MiniPackage(), MiniPackageId()), &received)));
}

void test_abort_and_sweep() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  manager.beginUpload(BeginFor(package, id), &received);
  TEST_ASSERT_EQUAL_UINT16(1, manager.openSessionCount());

  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk), kErr(manager.abort(id.c_str())));
  TEST_ASSERT_EQUAL_UINT16(0, manager.openSessionCount());
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kNoSession), kErr(manager.abort(id.c_str())));

  // A .part with no .meta can never be finished, so a boot sweep reclaims it.
  storage.put("/DEVICE/uploads/" + id + ".part", {1, 2, 3});
  TEST_ASSERT_EQUAL_UINT16(1, manager.sweepOrphans());
  TEST_ASSERT_FALSE(storage.hasFile("/DEVICE/uploads/" + id + ".part"));
}

void test_sweep_keeps_a_live_session() {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);

  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();
  uint64_t received = 0;
  uint64_t expected = 0;
  manager.beginUpload(BeginFor(package, id), &received);
  manager.writeChunk(id.c_str(), 0, package.data(), 64, &received, &expected);

  // Surviving a reset is the whole point: a session with a .meta stays.
  TEST_ASSERT_EQUAL_UINT16(0, manager.sweepOrphans());
  TEST_ASSERT_TRUE(storage.hasFile("/DEVICE/uploads/" + id + ".part"));
}

// ---------------------------------------------------------------------------
// Development rule 8: power loss during installation
// ---------------------------------------------------------------------------

namespace {

/**
 * The invariant, checked after every simulated power cut:
 *
 *   AFTER BOOT RECOVERY, every entry in library_index.bin names a package file
 *   that exists on disk and parses -- so a partially transferred package is
 *   never visible as installed content.
 *
 * "After boot recovery" rather than "at every instant", deliberately.
 * Replacing an installed package is remove-then-rename, and FAT offers no way
 * to make those one operation: power lost between them leaves the index naming
 * a file that is gone. That window cannot be closed, so it is repaired instead
 * -- LibraryIndex::prune() at boot is what makes the statement above true.
 * Claiming "never" would be claiming something the filesystem cannot give.
 */
void BootRecover(FakeStorage& storage, net::LibraryIndex* index,
                 net::UploadManager* manager) {
  if (!index->load(&storage)) index->rebuild(&storage);
  index->prune(&storage);
  manager->begin(&storage, index);
  manager->sweepOrphans();
}

/** Mutating operations one clean install of `package` costs. */
uint32_t WritesForACleanInstall(const std::vector<uint8_t>& package,
                                const std::string& id) {
  FakeStorage storage;
  net::LibraryIndex index;
  net::UploadManager manager;
  manager.begin(&storage, &index);
  storage.resetWriteCount();
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(UploadAll(&manager, package, id, 512)));
  return storage.writeCount();
}

void AssertNoPartialPackageIsVisible(FakeStorage& storage) {
  net::LibraryIndex index;
  net::UploadManager manager;
  BootRecover(storage, &index, &manager);
  for (uint16_t i = 0; i < index.count(); ++i) {
    char path[96];
    TEST_ASSERT_TRUE(net::BuildPackagePath(*index.at(i), path, sizeof(path)));
    TEST_ASSERT_TRUE_MESSAGE(storage.hasFile(path),
                             "index names a package that is not on disk");
    const std::vector<uint8_t>* bytes = storage.peek(path);
    qpk::MemoryFile file(bytes->data(), bytes->size());
    qpk::Reader reader;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, static_cast<int>(reader.open(&file)),
                                  "index names a package that does not parse");
    reader.close();
  }
}

}  // namespace

void test_power_loss_at_every_write_never_exposes_a_partial_package() {
  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();

  // Establish how many mutating operations a clean install takes, then cut
  // power before each one in turn.
  const uint32_t total_writes = WritesForACleanInstall(package, id);
  TEST_ASSERT_TRUE(total_writes > 4);

  for (uint32_t cut = 0; cut < total_writes; ++cut) {
    FakeStorage storage;
    {
      net::LibraryIndex index;
      net::UploadManager manager;
      manager.begin(&storage, &index);
      storage.cutPowerAfter(static_cast<int32_t>(cut));
      try {
        UploadAll(&manager, package, id, 512);
      } catch (const PowerLoss&) {
        // The device stopped here. Everything below is the next boot.
      }
    }
    storage.restorePower();

    AssertNoPartialPackageIsVisible(storage);

    // Recovery: a fresh boot re-runs the transfer and must reach a good state.
    net::LibraryIndex index;
    net::UploadManager manager;
    BootRecover(storage, &index, &manager);

    const net::Error retry = UploadAll(&manager, package, id, 512);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kErr(net::Error::kOk), kErr(retry),
                                  "recovery upload failed");
    TEST_ASSERT_TRUE(storage.hasFile("/LIBRARY/QURAN/" + id + ".qpk"));
    AssertNoPartialPackageIsVisible(storage);
  }
}

void test_power_loss_while_replacing_an_installed_package() {
  // The upgrade path is the one with a real window: finish() removes the
  // installed package and then renames the new one in, and FAT cannot make
  // that atomic. The fresh-install matrix above never reaches that branch,
  // because nothing is installed yet, so it gets its own sweep.
  const std::vector<uint8_t> v1 = MiniPackage();
  const std::string id = MiniPackageId();

  // v2: same content id, a later content_version, checksums repaired.
  std::vector<uint8_t> v2 = MiniPackage();
  qpk::Write32(v2.data() + 40, qpk::Read32(v2.data() + 40) + 1);
  qpk::Write32(v2.data() + 52,
               qpk::Crc32(v2.data() + qpk::kHeaderSize,
                          v2.size() - qpk::kHeaderSize));
  qpk::Write32(v2.data() + 60, qpk::Crc32(v2.data(), 60));

  const uint32_t total_writes = WritesForACleanInstall(v2, id);

  for (uint32_t cut = 0; cut < total_writes; ++cut) {
    FakeStorage storage;
    {
      net::LibraryIndex index;
      net::UploadManager manager;
      manager.begin(&storage, &index);
      TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                            kErr(UploadAll(&manager, v1, id, 512)));
    }

    {
      net::LibraryIndex index;
      net::UploadManager manager;
      BootRecover(storage, &index, &manager);
      storage.cutPowerAfter(static_cast<int32_t>(cut));
      try {
        UploadAll(&manager, v2, id, 512);
      } catch (const PowerLoss&) {
      }
    }
    storage.restorePower();

    // Boot recovery must leave the index honest: either v1 is still installed,
    // or the entry for the package deleted mid-swap has been pruned.
    AssertNoPartialPackageIsVisible(storage);

    net::LibraryIndex index;
    net::UploadManager manager;
    BootRecover(storage, &index, &manager);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kErr(net::Error::kOk),
                                  kErr(UploadAll(&manager, v2, id, 512)),
                                  "upgrade did not recover");
    AssertNoPartialPackageIsVisible(storage);
    TEST_ASSERT_EQUAL_UINT16(1, index.count());
    TEST_ASSERT_EQUAL_UINT32(qpk::Read32(v2.data() + 40),
                             index.at(0)->content_version);
  }
}

void test_prune_drops_an_entry_whose_package_vanished() {
  FakeStorage storage;
  const std::vector<uint8_t> package = MiniPackage();
  const std::string id = MiniPackageId();

  {
    net::LibraryIndex index;
    net::UploadManager manager;
    manager.begin(&storage, &index);
    TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                          kErr(UploadAll(&manager, package, id, 512)));
    TEST_ASSERT_EQUAL_UINT16(1, index.count());

    // Exactly the aftermath of losing power between remove and rename.
    storage.remove(("/LIBRARY/QURAN/" + id + ".qpk").c_str());
    TEST_ASSERT_EQUAL_UINT16(1, index.prune(&storage));
    TEST_ASSERT_EQUAL_UINT16(0, index.count());
  }

  // Present but unreadable is dropped too: the browser would offer it and the
  // reader would fail to open it.
  net::LibraryIndex second;
  net::UploadManager again;
  again.begin(&storage, &second);
  TEST_ASSERT_EQUAL_INT(kErr(net::Error::kOk),
                        kErr(UploadAll(&again, package, id, 512)));
  std::vector<uint8_t> corrupt = package;
  corrupt[5] = static_cast<uint8_t>(corrupt[5] ^ 0xFF);  // format version
  storage.put("/LIBRARY/QURAN/" + id + ".qpk", corrupt);
  TEST_ASSERT_EQUAL_UINT16(1, second.prune(&storage));
  TEST_ASSERT_EQUAL_UINT16(0, second.count());
}

void test_a_half_written_index_is_never_loaded() {
  // The index is written to a .tmp and renamed; the rename is the commit. Cut
  // power during the write and the previous index must still be the one that
  // loads.
  FakeStorage storage;
  net::LibraryIndex index;
  net::LibraryEntry entry;
  net::HexToContentId(kContentId, entry.content_id);
  entry.type = 1;
  snprintf(entry.title, sizeof(entry.title), "First");
  index.upsert(entry);
  TEST_ASSERT_TRUE(index.save(&storage));

  net::LibraryEntry second = entry;
  second.content_id[0] = 0xAA;
  snprintf(second.title, sizeof(second.title), "Second");
  index.upsert(second);

  storage.cutPowerAfter(1);  // die partway through the rewrite
  try {
    index.save(&storage);
  } catch (const PowerLoss&) {
  }
  storage.restorePower();

  net::LibraryIndex reloaded;
  TEST_ASSERT_TRUE(reloaded.load(&storage));
  TEST_ASSERT_EQUAL_UINT16(1, reloaded.count());
  TEST_ASSERT_EQUAL_STRING("First", reloaded.at(0)->title);
}

// ---------------------------------------------------------------------------
// BLE provisioning state
// ---------------------------------------------------------------------------

void test_provisioning_validates_credentials() {
  net::ProvisioningState state;
  TEST_ASSERT_TRUE(state.status() == net::ProvisioningStatus::kIdle);

  TEST_ASSERT_TRUE(state.setSsid("") == net::ProvisioningError::kSsidEmpty);
  std::string too_long(33, 'x');
  TEST_ASSERT_TRUE(state.setSsid(too_long.c_str()) ==
                   net::ProvisioningError::kSsidTooLong);

  TEST_ASSERT_TRUE(state.setSsid("home-network") == net::ProvisioningError::kOk);
  TEST_ASSERT_TRUE(state.ready());
  TEST_ASSERT_TRUE(state.status() == net::ProvisioningStatus::kReady);

  TEST_ASSERT_TRUE(state.setPassphrase("short") ==
                   net::ProvisioningError::kPassphraseTooShort);
  std::string long_pass(64, 'y');
  TEST_ASSERT_TRUE(state.setPassphrase(long_pass.c_str()) ==
                   net::ProvisioningError::kPassphraseTooLong);
  TEST_ASSERT_TRUE(state.setPassphrase("goodpassword") ==
                   net::ProvisioningError::kOk);
  // An open network is legal and has no passphrase.
  TEST_ASSERT_TRUE(state.setPassphrase("") == net::ProvisioningError::kOk);
  TEST_ASSERT_FALSE(state.hasPassphrase());
}

void test_provisioning_commits_only_when_ready() {
  net::ProvisioningState state;
  TEST_ASSERT_TRUE(state.beginCommit() == net::ProvisioningError::kNotReady);

  state.setSsid("net");
  state.setPassphrase("goodpassword");
  TEST_ASSERT_TRUE(state.beginCommit() == net::ProvisioningError::kOk);
  TEST_ASSERT_TRUE(state.status() == net::ProvisioningStatus::kConnecting);

  state.commitFailed();
  TEST_ASSERT_TRUE(state.status() == net::ProvisioningStatus::kFailed);
  // The fields survive a failure so the user can retry with a fixed password.
  TEST_ASSERT_EQUAL_STRING("net", state.ssid());
  TEST_ASSERT_TRUE(state.ready());

  state.beginCommit();
  state.commitSucceeded();
  TEST_ASSERT_TRUE(state.status() == net::ProvisioningStatus::kProvisioned);
}

void test_provisioning_status_never_leaks_the_passphrase() {
  net::ProvisioningState state;
  state.setSsid("home-network");
  state.setPassphrase("SuperSecret123");
  state.setDeviceName("Reader");
  state.setPairingToken("token-abc");

  char json[256];
  const uint32_t length = state.writeStatusJson(json, sizeof(json));
  TEST_ASSERT_TRUE(length > 0);
  TEST_ASSERT_NULL_MESSAGE(strstr(json, "SuperSecret123"),
                           "the passphrase must never appear in status output");
  TEST_ASSERT_NOT_NULL(strstr(json, "home-network"));
  TEST_ASSERT_NOT_NULL(strstr(json, "\"hasPassphrase\":true"));
}

// ---------------------------------------------------------------------------
// Protocol mirror
// ---------------------------------------------------------------------------

void test_error_codes_map_to_the_documented_statuses() {
  TEST_ASSERT_EQUAL_UINT16(409, net::ErrorStatus(net::Error::kOffsetMismatch));
  TEST_ASSERT_EQUAL_UINT16(413, net::ErrorStatus(net::Error::kChunkTooLarge));
  TEST_ASSERT_EQUAL_UINT16(422, net::ErrorStatus(net::Error::kVerifyFailed));
  TEST_ASSERT_EQUAL_UINT16(507, net::ErrorStatus(net::Error::kNoSpace));
  TEST_ASSERT_EQUAL_UINT16(401, net::ErrorStatus(net::Error::kUnauthorized));
  TEST_ASSERT_EQUAL_STRING("OFFSET_MISMATCH",
                           net::ErrorCodeName(net::Error::kOffsetMismatch));
}

void test_package_type_names_match_the_library_layout() {
  TEST_ASSERT_EQUAL_UINT16(1, net::PackageTypeFromName("QURAN"));
  TEST_ASSERT_EQUAL_UINT16(2, net::PackageTypeFromName("BOOK"));
  TEST_ASSERT_EQUAL_UINT16(0, net::PackageTypeFromName("NONSENSE"));
  TEST_ASSERT_EQUAL_STRING("QURAN", net::LibraryDirFor(1));
  TEST_ASSERT_EQUAL_STRING("BOOKS", net::LibraryDirFor(2));
  TEST_ASSERT_EQUAL_STRING("TRANSLATIONS", net::LibraryDirFor(3));
  TEST_ASSERT_NULL(net::LibraryDirFor(99));
}

// ---------------------------------------------------------------------------
// Transcript fixture: protocol.h vs. packages/protocol/src/index.ts
//
// transcript_fixture.h is generated FROM index.ts (see
// packages/protocol/scripts/build-transcript.ts) and committed. These tests
// check the OTHER mirror -- protocol.h, hand-maintained -- against it. A
// failure here means one side of the mirror was edited without the other:
// either fix protocol.h, or edit index.ts and run
// `npm run fixture --prefix packages/protocol` to regenerate this header.
// ---------------------------------------------------------------------------

bool EqualsIgnoreCase(const char* a, const char* b) {
  while (*a != 0 && *b != 0) {
    if (tolower(static_cast<unsigned char>(*a)) !=
        tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == *b;  // both must land on the NUL together
}

void test_transcript_wire_basics_match_protocol_h() {
  TEST_ASSERT_EQUAL_UINT16(transcript::kProtocolVersion, net::kProtocolVersion);
  TEST_ASSERT_EQUAL_UINT16(transcript::kDefaultPort, net::kDefaultPort);
  TEST_ASSERT_EQUAL_STRING(transcript::kMdnsService, net::kMdnsService);
}

void test_transcript_headers_match_protocol_h_case_insensitively() {
  // HTTP header names are case-insensitive on the wire, and both real
  // consumers already compare this way (WebServer::header() via
  // equalsIgnoreCase; index.ts's own constants are lowercase by convention).
  // A differing NAME is drift; a differing case is not.
  TEST_ASSERT_TRUE(EqualsIgnoreCase(transcript::kHeaderAuthorization,
                                    net::kHeaderAuthorization));
  TEST_ASSERT_TRUE(EqualsIgnoreCase(transcript::kHeaderContentType,
                                    net::kHeaderContentType));
  TEST_ASSERT_TRUE(EqualsIgnoreCase(transcript::kHeaderProtocolVersion,
                                    net::kHeaderProtocolVersion));
  TEST_ASSERT_TRUE(EqualsIgnoreCase(transcript::kHeaderOffset, net::kHeaderOffset));
  TEST_ASSERT_TRUE(EqualsIgnoreCase(transcript::kHeaderBody, net::kHeaderBody));
  TEST_ASSERT_EQUAL_STRING(transcript::kBodyEncodingBase64, net::kBodyEncodingBase64);
  TEST_ASSERT_EQUAL_STRING(transcript::kContentTypeJson, net::kContentTypeJson);
  TEST_ASSERT_EQUAL_STRING(transcript::kContentTypeOctet, net::kContentTypeOctet);
}

void test_transcript_paths_match_protocol_h() {
  TEST_ASSERT_EQUAL_STRING(transcript::kPathDeviceInfo, net::kPathDeviceInfo);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathDeviceStatus, net::kPathDeviceStatus);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathLibrary, net::kPathLibrary);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathLibraryItemPrefix,
                           net::kPathLibraryItemPrefix);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathUploadBegin, net::kPathUploadBegin);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathUploadPrefix, net::kPathUploadPrefix);
  TEST_ASSERT_EQUAL_STRING(transcript::kUploadChunkSuffix, net::kUploadChunkSuffix);
  TEST_ASSERT_EQUAL_STRING(transcript::kUploadFinishSuffix, net::kUploadFinishSuffix);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathBackup, net::kPathBackup);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathRestore, net::kPathRestore);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathPhotos, net::kPathPhotos);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathPhotoPrefix, net::kPathPhotoPrefix);
  TEST_ASSERT_EQUAL_STRING(transcript::kPhotoChunkSuffix, net::kPhotoChunkSuffix);
  TEST_ASSERT_EQUAL_STRING(transcript::kPhotoFinishSuffix, net::kPhotoFinishSuffix);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathDeviceTime, net::kPathDeviceTime);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathFirmwareBegin, net::kPathFirmwareBegin);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathFirmwareChunk, net::kPathFirmwareChunk);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathFirmwareFinish, net::kPathFirmwareFinish);
  TEST_ASSERT_EQUAL_STRING(transcript::kPathFirmwareAbort, net::kPathFirmwareAbort);
}

void test_transcript_status_codes_match_protocol_h() {
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusOk, net::kStatusOk);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusCreated, net::kStatusCreated);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusBadRequest, net::kStatusBadRequest);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusUnauthorized, net::kStatusUnauthorized);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusNotFound, net::kStatusNotFound);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusConflict, net::kStatusConflict);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusPayloadTooLarge,
                           net::kStatusPayloadTooLarge);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusUnprocessable, net::kStatusUnprocessable);
  TEST_ASSERT_EQUAL_UINT16(transcript::kStatusInsufficientStorage,
                           net::kStatusInsufficientStorage);
}

void test_transcript_error_codes_and_statuses_match_protocol_h() {
  // Every net::Error that maps to a wire error code (kOk does not -- 200
  // needs no ErrorCode). Listed explicitly, not derived, so an error added to
  // one side and forgotten on the other shows up as a count mismatch below.
  const net::Error kAllErrors[] = {
      net::Error::kBadRequest,     net::Error::kUnauthorized,
      net::Error::kNotFound,       net::Error::kOffsetMismatch,
      net::Error::kChunkTooLarge,  net::Error::kSizeMismatch,
      net::Error::kVerifyFailed,   net::Error::kPackageRejected,
      net::Error::kNoSpace,        net::Error::kNoSession,
      net::Error::kStorageError,   net::Error::kNotInTransferMode,
  };
  const uint32_t kCount = sizeof(kAllErrors) / sizeof(kAllErrors[0]);
  TEST_ASSERT_EQUAL_UINT32(transcript::kErrorCount, kCount);

  for (uint32_t i = 0; i < kCount; ++i) {
    const char* name = net::ErrorCodeName(kAllErrors[i]);
    const uint16_t status = net::ErrorStatus(kAllErrors[i]);

    bool found = false;
    for (uint32_t j = 0; j < transcript::kErrorCount; ++j) {
      if (strcmp(transcript::kErrors[j].code, name) == 0) {
        found = true;
        TEST_ASSERT_EQUAL_UINT16(transcript::kErrors[j].status, status);
        break;
      }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, name);
  }
}

// ---------------------------------------------------------------------------

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// PhotoStore -- the home screen's photos -- and time zones
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> MakePhotoFile(uint8_t fill) {
  std::vector<uint8_t> file(net::kPhotoFileBytes, fill);
  net::WritePhotoHeader(file.data());
  return file;
}

// Uploads a whole file the way the desktop does: chunks of `chunk` bytes, each
// at the offset the device last reported, then finish().
net::Error SendPhoto(net::PhotoStore* store, const char* name,
                     const std::vector<uint8_t>& file, uint32_t chunk) {
  uint64_t offset = 0;
  while (offset < file.size()) {
    const uint32_t n = static_cast<uint32_t>(
        std::min<uint64_t>(chunk, file.size() - offset));
    uint64_t received = 0;
    uint64_t expected = 0;
    const net::Error err = store->writeChunk(name, offset, file.data() + offset, n,
                                             &received, &expected);
    if (err != net::Error::kOk) return err;
    offset = received;
  }
  return store->finish(name);
}

}  // namespace

void test_photo_names_and_header() {
  TEST_ASSERT_TRUE(net::ValidPhotoName("sunset-01"));
  TEST_ASSERT_TRUE(net::ValidPhotoName("a_b"));
  TEST_ASSERT_TRUE(net::ValidPhotoName(std::string(32, 'a').c_str()));
  TEST_ASSERT_FALSE(net::ValidPhotoName(std::string(33, 'a').c_str()));
  TEST_ASSERT_FALSE(net::ValidPhotoName(""));
  TEST_ASSERT_FALSE(net::ValidPhotoName(nullptr));
  TEST_ASSERT_FALSE(net::ValidPhotoName("Upper"));
  TEST_ASSERT_FALSE(net::ValidPhotoName("a b"));
  TEST_ASSERT_FALSE(net::ValidPhotoName("../x"));

  uint8_t header[net::kPhotoHeaderBytes];
  net::WritePhotoHeader(header);
  TEST_ASSERT_TRUE(net::ParsePhotoHeader(header));
  TEST_ASSERT_EQUAL_UINT32(net::kPhotoHeaderBytes + 48000u, net::kPhotoFileBytes);

  uint8_t bad[net::kPhotoHeaderBytes];
  memcpy(bad, header, sizeof(bad));
  bad[0] = 'X';
  TEST_ASSERT_FALSE(net::ParsePhotoHeader(bad));
  memcpy(bad, header, sizeof(bad));
  bad[8] = 4;  // 4 bpp
  TEST_ASSERT_FALSE(net::ParsePhotoHeader(bad));
  memcpy(bad, header, sizeof(bad));
  bad[4] = 0x20;  // width 800
  bad[5] = 0x03;
  TEST_ASSERT_FALSE(net::ParsePhotoHeader(bad));
  memcpy(bad, header, sizeof(bad));
  bad[15] = 1;  // reserved
  TEST_ASSERT_FALSE(net::ParsePhotoHeader(bad));
}

void test_photo_upload_round_trip_lists_and_loads() {
  faketest::FakeStorage storage;
  net::PhotoStore store;
  store.begin(&storage);

  const std::vector<uint8_t> b = MakePhotoFile(0x5A);
  const std::vector<uint8_t> a = MakePhotoFile(0x11);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(SendPhoto(&store, "b-photo", b, net::kMaxChunkBytes)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(SendPhoto(&store, "a-photo", a, 5000)));
  TEST_ASSERT_TRUE(storage.hasFile("/PHOTOS/a-photo.g4"));
  TEST_ASSERT_FALSE(storage.hasFile("/PHOTOS/a-photo.part"));

  net::PhotoName names[8];
  TEST_ASSERT_EQUAL_UINT16(2, store.list(names, 8));
  TEST_ASSERT_EQUAL_STRING("a-photo", names[0].name);
  TEST_ASSERT_EQUAL_STRING("b-photo", names[1].name);

  std::vector<uint8_t> pixels(net::kPhotoPixelBytes, 0);
  TEST_ASSERT_TRUE(store.load("a-photo", pixels.data(), static_cast<uint32_t>(pixels.size())));
  TEST_ASSERT_TRUE(std::equal(pixels.begin(), pixels.end(),
                              a.begin() + net::kPhotoHeaderBytes));
  TEST_ASSERT_FALSE(store.load("missing", pixels.data(), static_cast<uint32_t>(pixels.size())));

  // A second upload of the same name replaces the photo.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(SendPhoto(&store, "a-photo", b, net::kMaxChunkBytes)));
  TEST_ASSERT_TRUE(store.load("a-photo", pixels.data(), static_cast<uint32_t>(pixels.size())));
  TEST_ASSERT_EQUAL_UINT8(0x5A, pixels[0]);
  TEST_ASSERT_EQUAL_UINT16(2, store.list(names, 8));
}

void test_photo_chunk_offset_mismatch_names_the_resume_point() {
  faketest::FakeStorage storage;
  net::PhotoStore store;
  store.begin(&storage);
  const std::vector<uint8_t> file = MakePhotoFile(0x22);

  uint64_t received = 0;
  uint64_t expected = 0;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(store.writeChunk("p", 0, file.data(), 1000,
                                                          &received, &expected)));
  TEST_ASSERT_EQUAL_UINT64(1000, received);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOffsetMismatch),
                        static_cast<int>(store.writeChunk("p", 5000, file.data() + 5000, 10,
                                                          &received, &expected)));
  TEST_ASSERT_EQUAL_UINT64(1000, expected);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(store.writeChunk("p", expected, file.data() + 1000,
                                                          500, &received, &expected)));
  TEST_ASSERT_EQUAL_UINT64(1500, received);
}

void test_photo_offset_zero_restarts_and_oversize_is_refused() {
  faketest::FakeStorage storage;
  net::PhotoStore store;
  store.begin(&storage);
  const std::vector<uint8_t> file = MakePhotoFile(0x33);

  uint64_t received = 0;
  uint64_t expected = 0;
  store.writeChunk("p", 0, file.data(), 100, &received, &expected);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(store.writeChunk("p", 0, file.data(), 2000,
                                                          &received, &expected)));
  TEST_ASSERT_EQUAL_UINT64(2000, received);

  const std::vector<uint8_t> too_much(net::kPhotoFileBytes, 0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kSizeMismatch),
                        static_cast<int>(store.writeChunk("p", 2000, too_much.data(),
                                                          net::kPhotoFileBytes - 1999,
                                                          &received, &expected)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kBadRequest),
                        static_cast<int>(store.writeChunk("Bad Name", 0, file.data(), 10,
                                                          &received, &expected)));
}

void test_photo_finish_rejects_short_and_foreign_files() {
  faketest::FakeStorage storage;
  net::PhotoStore store;
  store.begin(&storage);
  const std::vector<uint8_t> file = MakePhotoFile(0x44);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kNoSession),
                        static_cast<int>(store.finish("never")));

  uint64_t received = 0;
  uint64_t expected = 0;
  store.writeChunk("short", 0, file.data(), 4000, &received, &expected);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kSizeMismatch),
                        static_cast<int>(store.finish("short")));
  TEST_ASSERT_TRUE(storage.hasFile("/PHOTOS/short.part"));  // still resumable

  std::vector<uint8_t> foreign = file;
  foreign[0] = 'Z';
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kVerifyFailed),
                        static_cast<int>(SendPhoto(&store, "foreign", foreign, net::kMaxChunkBytes)));
  TEST_ASSERT_FALSE(storage.hasFile("/PHOTOS/foreign.part"));
  TEST_ASSERT_FALSE(storage.hasFile("/PHOTOS/foreign.g4"));
}

void test_photo_remove() {
  faketest::FakeStorage storage;
  net::PhotoStore store;
  store.begin(&storage);
  SendPhoto(&store, "gone", MakePhotoFile(0x55), net::kMaxChunkBytes);

  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kOk),
                        static_cast<int>(store.remove("gone")));
  TEST_ASSERT_FALSE(storage.hasFile("/PHOTOS/gone.g4"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kNotFound),
                        static_cast<int>(store.remove("gone")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(net::Error::kBadRequest),
                        static_cast<int>(store.remove("../DEVICE/state")));
}

void test_photo_list_ignores_parts_and_foreign_files() {
  faketest::FakeStorage storage;
  net::PhotoStore store;
  store.begin(&storage);
  const std::vector<uint8_t> file = MakePhotoFile(0x66);
  storage.put("/PHOTOS/unfinished.part", file);
  storage.put("/PHOTOS/readme.txt", file);
  storage.put("/PHOTOS/Bad Name.g4", file);
  storage.put("/PHOTOS/short.g4", std::vector<uint8_t>(10, 0));
  storage.put("/PHOTOS/good.g4", file);

  net::PhotoName names[4];
  TEST_ASSERT_EQUAL_UINT16(1, store.list(names, 4));
  TEST_ASSERT_EQUAL_STRING("good", names[0].name);

  // Past capacity, the alphabetically first names are the ones kept.
  storage.put("/PHOTOS/a.g4", file);
  storage.put("/PHOTOS/z.g4", file);
  net::PhotoName two[2];
  TEST_ASSERT_EQUAL_UINT16(2, store.list(two, 2));
  TEST_ASSERT_EQUAL_STRING("a", two[0].name);
  TEST_ASSERT_EQUAL_STRING("good", two[1].name);
}

void test_time_zone_validation() {
  TEST_ASSERT_TRUE(net::ValidTimeZone("IST-5:30"));
  TEST_ASSERT_TRUE(net::ValidTimeZone("UTC0"));
  TEST_ASSERT_TRUE(net::ValidTimeZone("CET-1CEST,M3.5.0,M10.5.0/3"));
  TEST_ASSERT_TRUE(net::ValidTimeZone("<+0530>-5:30"));
  TEST_ASSERT_FALSE(net::ValidTimeZone(nullptr));
  TEST_ASSERT_FALSE(net::ValidTimeZone(""));
  TEST_ASSERT_FALSE(net::ValidTimeZone("5:30"));
  TEST_ASSERT_FALSE(net::ValidTimeZone("IST 5"));
  TEST_ASSERT_FALSE(net::ValidTimeZone("IST;reboot"));
  TEST_ASSERT_FALSE(net::ValidTimeZone(std::string(64, 'A').c_str()));
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_json_reads_a_flat_object);
  RUN_TEST(test_json_handles_escapes_and_whitespace);
  RUN_TEST(test_json_rejects_rather_than_guesses);
  RUN_TEST(test_json_skips_nested_values_without_desyncing);
  RUN_TEST(test_json_writer_escapes_and_reports_overflow);

  RUN_TEST(test_bearer_token_check);
  RUN_TEST(test_constant_time_compare_is_value_correct);

  RUN_TEST(test_content_id_hex_round_trip);
  RUN_TEST(test_utf8_truncation_never_splits_a_codepoint);
  RUN_TEST(test_library_index_round_trips);
  RUN_TEST(test_library_index_rejects_a_corrupt_file);
  RUN_TEST(test_library_index_rebuilds_from_the_packages_on_disk);
  RUN_TEST(test_library_index_upsert_replaces_and_remove_works);

  RUN_TEST(test_reading_progress_round_trips);
  RUN_TEST(test_reading_progress_second_save_overwrites_the_first);
  RUN_TEST(test_reading_progress_absent_file_is_not_an_error);
  RUN_TEST(test_reading_progress_rejects_a_corrupt_record);
  RUN_TEST(test_reading_progress_rejects_a_future_format_version);
  RUN_TEST(test_page_progress_round_trips_without_touching_progress_bin);
  RUN_TEST(test_page_progress_absent_or_corrupt_is_rejected);
  RUN_TEST(test_base64_decodes_text_and_every_byte_value);
  RUN_TEST(test_base64_rejects_bad_characters_overflow_and_data_after_padding);
  RUN_TEST(test_bookmarks_round_trip_newest_first);
  RUN_TEST(test_bookmarks_same_place_moves_to_front_and_full_list_drops_the_oldest);
  RUN_TEST(test_bookmarks_damaged_file_loads_as_empty);
  RUN_TEST(test_reset_log_appends_one_line_per_boot);
  RUN_TEST(test_reset_log_stays_bounded_and_keeps_the_newest_whole_lines);
  RUN_TEST(test_alive_time_round_trips_and_is_zero_when_absent_or_damaged);

  RUN_TEST(test_upload_installs_a_package);
  RUN_TEST(test_upload_resumes_from_the_size_on_disk);
  RUN_TEST(test_write_chunk_preserves_embedded_nul_bytes);
  RUN_TEST(test_repeated_chunk_is_idempotent);
  RUN_TEST(test_offset_mismatch_reports_where_to_resume);
  RUN_TEST(test_chunk_limits_are_enforced);
  RUN_TEST(test_chunk_without_a_session_is_not_found);
  RUN_TEST(test_finish_rejects_a_short_upload);
  RUN_TEST(test_finish_rejects_a_corrupted_payload);
  RUN_TEST(test_begin_discards_a_part_from_a_different_package);
  RUN_TEST(test_begin_refuses_when_there_is_no_room);
  RUN_TEST(test_abort_and_sweep);
  RUN_TEST(test_sweep_keeps_a_live_session);

  RUN_TEST(test_power_loss_at_every_write_never_exposes_a_partial_package);
  RUN_TEST(test_power_loss_while_replacing_an_installed_package);
  RUN_TEST(test_prune_drops_an_entry_whose_package_vanished);
  RUN_TEST(test_a_half_written_index_is_never_loaded);

  RUN_TEST(test_provisioning_validates_credentials);
  RUN_TEST(test_provisioning_commits_only_when_ready);
  RUN_TEST(test_provisioning_status_never_leaks_the_passphrase);

  RUN_TEST(test_error_codes_map_to_the_documented_statuses);
  RUN_TEST(test_package_type_names_match_the_library_layout);

  RUN_TEST(test_transcript_wire_basics_match_protocol_h);
  RUN_TEST(test_transcript_headers_match_protocol_h_case_insensitively);
  RUN_TEST(test_transcript_paths_match_protocol_h);

  RUN_TEST(test_photo_names_and_header);
  RUN_TEST(test_photo_upload_round_trip_lists_and_loads);
  RUN_TEST(test_photo_chunk_offset_mismatch_names_the_resume_point);
  RUN_TEST(test_photo_offset_zero_restarts_and_oversize_is_refused);
  RUN_TEST(test_photo_finish_rejects_short_and_foreign_files);
  RUN_TEST(test_photo_remove);
  RUN_TEST(test_photo_list_ignores_parts_and_foreign_files);
  RUN_TEST(test_time_zone_validation);
  RUN_TEST(test_transcript_status_codes_match_protocol_h);
  RUN_TEST(test_transcript_error_codes_and_statuses_match_protocol_h);

  return UNITY_END();
}
