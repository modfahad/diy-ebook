#include "drivers/http_server.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <Update.h>
#include <WebServer.h>
#include <esp_ota_ops.h>

#include "app/app_config.h"
#include "drivers/device_identity.h"
#include "drivers/serial_log.h"
#include "net/base64.h"
#include "net/auth.h"
#include "net/json.h"
#include "net/protocol.h"
#include "net/time_zone.h"

namespace drivers {
namespace {

constexpr uint16_t kStatusInternalError = 500;

// One receive buffer for both chunk endpoints (packages and photos): too large
// for the loop task's stack, and only one request is ever served at a time.
uint8_t g_chunk_buf[net::kMaxChunkBytes];

// WebServer::client() returns a copy of the connection, and on the real board
// (core 2.0.14) a chunk body read through that copy lost whatever arrived in
// the same TCP segment as the headers: a chunked PUT sent in one piece lost
// its size line, a small one lost its whole body, and a body sent 300 ms after
// its headers read perfectly. Every fetch-based upload sends them together,
// which is why package and photo uploads failed on hardware. Reading through
// the server's own connection object -- _currentClient, the one
// _parseRequest() read the headers from -- sees those buffered bytes.
class ApiWebServer : public WebServer {
 public:
  explicit ApiWebServer(int port) : WebServer(port) {}
  WiFiClient& currentClient() { return _currentClient; }
};

}  // namespace

// --- request-body binary safety ---------------------------------------------
//
// The Arduino WebServer buffers any request that carries a Content-Length
// header into a heap `String` via `arg("plain")` -- and that String is built
// with `String(const char*)`, which stops at the first 0x00 byte. A QPK chunk
// is arbitrary binary data and WILL contain zero bytes, so `arg("plain")`
// would silently truncate uploads. `_parseRequest` only reads a body at all
// when Content-Length is present (see Parsing.cpp): a request with no
// Content-Length and no multipart Content-Type leaves its body completely
// unread on the socket, at which point our own handler can read it directly,
// byte-exact, with no String involved anywhere.
//
// So: the desktop sends the chunk PUT with `Transfer-Encoding: chunked` and
// no Content-Length, and this function is the chunked-transfer-coding
// decoder that reads it straight into a caller-owned buffer.
net::Error HttpServer::ReadChunkedBody(WiFiClient& client, uint8_t* out,
                                       uint32_t capacity, uint32_t* length_out) {
  *length_out = 0;
  // Seconds, not milliseconds -- see app::kChunkedBodyReadTimeoutSec's
  // comment. This used to read client.setTimeout(5000), a ~83-minute
  // timeout instead of the intended 5 seconds.
  client.setTimeout(app::kChunkedBodyReadTimeoutSec);
  // Bytes that came in with the headers. Non-zero here is the normal case for
  // a fetch client; it was the case that failed before ApiWebServer.
  Logf("[transfer] ReadChunkedBody: %d bytes already buffered", client.available());

  // docs/pending.md's upload chunk-PUT investigation: "which of
  // HandleUploadChunk's three kBadRequest exits fires -- the missing offset
  // header, the strtoull parse, or ReadChunkedBody. One Serial.printf of the
  // first line ReadChunkedBody actually reads would settle it." This covers
  // that bucket's own three ways to fail, each tagged so serial output alone
  // identifies which one fired without guessing -- not just the first line,
  // since a malformed size line and a short/failed body read look identical
  // from HandleUploadChunk's side (both surface as plain kBadRequest) but
  // need different fixes.
  bool first_line = true;
  while (true) {
    String line = client.readStringUntil('\n');
    if (first_line) {
      Logf("[transfer] ReadChunkedBody first line: \"%s\"", line.c_str());
      first_line = false;
    }
    line.trim();
    const int semicolon = line.indexOf(';');  // chunk extensions, unused here
    if (semicolon >= 0) line = line.substring(0, semicolon);
    line.trim();
    if (line.length() == 0) {
      LogLine("[transfer] ReadChunkedBody: empty/malformed size line "
              "(client hang-up or bad framing)");
      return net::Error::kBadRequest;
    }

    char* end = nullptr;
    const long chunk_size = strtol(line.c_str(), &end, 16);
    if (end == line.c_str() || chunk_size < 0) {
      Logf("[transfer] ReadChunkedBody: size line \"%s\" is not a valid hex "
           "chunk size", line.c_str());
      return net::Error::kBadRequest;
    }

    if (chunk_size == 0) {
      // The trailer section (we expect none) ends at the first bare CRLF,
      // which reads back as "\r" (length 1) once readStringUntil('\n') has
      // eaten the '\n'.
      String trailer;
      do {
        trailer = client.readStringUntil('\n');
      } while (trailer.length() > 1);
      return net::Error::kOk;
    }

    if (*length_out + static_cast<uint32_t>(chunk_size) > capacity) {
      return net::Error::kChunkTooLarge;
    }

    uint32_t got = 0;
    while (got < static_cast<uint32_t>(chunk_size)) {
      const int n = client.readBytes(
          reinterpret_cast<char*>(out + *length_out + got), chunk_size - got);
      if (n <= 0) {
        Logf("[transfer] ReadChunkedBody: short body read (got %u of %ld bytes, "
             "readBytes returned %d)", static_cast<unsigned>(got), chunk_size, n);
        return net::Error::kBadRequest;
      }
      got += static_cast<uint32_t>(n);
    }
    *length_out += static_cast<uint32_t>(chunk_size);
    client.readStringUntil('\n');  // the CRLF that follows every chunk
  }
}

// --- lifecycle ---------------------------------------------------------------

void HttpServer::begin(hal::IStorage* storage, hal::IPower* power,
                       net::LibraryIndex* index, net::UploadManager* uploads,
                       const net::ProvisioningState* provisioning,
                       const char* default_device_name, hal::IClock* clock,
                       net::PhotoStore* photos) {
  storage_ = storage;
  power_ = power;
  index_ = index;
  uploads_ = uploads;
  provisioning_ = provisioning;
  default_device_name_ = default_device_name;
  clock_ = clock;
  photos_ = photos;

  server_ = new ApiWebServer(net::kDefaultPort);

  const char* collected_headers[] = {net::kHeaderAuthorization, net::kHeaderOffset,
                                     net::kHeaderBody};
  server_->collectHeaders(collected_headers, 3);

  server_->on(net::kPathDeviceInfo, HTTP_GET, [this]() { HandleDeviceInfo(); });
  server_->on(net::kPathDeviceStatus, HTTP_GET, [this]() { HandleDeviceStatus(); });
  server_->on(net::kPathDeviceTime, HTTP_POST, [this]() { HandleDeviceTime(); });
  server_->on(net::kPathLibrary, HTTP_GET, [this]() { HandleLibraryList(); });
  server_->on(net::kPathUploadBegin, HTTP_POST, [this]() { HandleUploadBegin(); });
  server_->on(net::kPathBackup, HTTP_POST, [this]() { HandleBackup(); });
  server_->on(net::kPathRestore, HTTP_POST, [this]() { HandleRestore(); });
  server_->on(net::kPathPhotos, HTTP_GET, [this]() { HandlePhotoList(); });
  server_->on(net::kPathFirmwareBegin, HTTP_POST, [this]() { HandleFirmwareBegin(); });
  server_->on(net::kPathFirmwareChunk, HTTP_PUT, [this]() { HandleFirmwareChunk(); });
  server_->on(net::kPathFirmwareFinish, HTTP_POST, [this]() { HandleFirmwareFinish(); });
  server_->on(net::kPathFirmwareAbort, HTTP_POST, [this]() { HandleFirmwareAbort(); });
  server_->onNotFound([this]() { HandleDynamic(); });

  server_->begin();
  // development.md 10.6: the baseline to compare each chunk's logged
  // free_heap against.
  Logf("[transfer] http server up, free_heap=%u", ESP.getFreeHeap());
}

void HttpServer::end() {
  if (server_ == nullptr) return;
  server_->stop();
  delete server_;
  server_ = nullptr;
}

void HttpServer::poll() {
  if (server_ == nullptr) return;
  server_->handleClient();
}

bool HttpServer::takePhotosChanged() {
  const bool changed = photos_changed_;
  photos_changed_ = false;
  return changed;
}

bool HttpServer::takeTimeZoneChanged() {
  const bool changed = time_zone_changed_;
  time_zone_changed_ = false;
  return changed;
}

// --- shared helpers ------------------------------------------------------------

bool HttpServer::RequireAuth() {
  const String header = server_->header(net::kHeaderAuthorization);
  const char* token = provisioning_->pairingToken();
  if (!net::CheckBearer(header.c_str(), token, strlen(token))) {
    server_->send(net::kStatusUnauthorized, net::kContentTypeJson,
                  "{\"error\":\"UNAUTHORIZED\"}");
    return false;
  }
  return true;
}

void HttpServer::SendError(net::Error error, uint64_t expected_offset) {
  char buf[128];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyString("error", net::ErrorCodeName(error));
  if (error == net::Error::kOffsetMismatch) {
    writer.keyUint("expectedOffset", expected_offset);
  }
  writer.endObject();
  server_->send(net::ErrorStatus(error), net::kContentTypeJson,
               writer.ok() ? writer.c_str() : "{\"error\":\"BAD_REQUEST\"}");
}

void HttpServer::SendJson(uint16_t status, const char* body, bool ok) {
  if (!ok) {
    // JsonWriter overflowed -- see JsonWriter::ok(). A truncated body must
    // never go out as if it were complete.
    server_->send(kStatusInternalError, net::kContentTypeJson,
                  "{\"error\":\"STORAGE_ERROR\"}");
    return;
  }
  server_->send(status, net::kContentTypeJson, body);
}

bool HttpServer::ReadOffsetHeader(const char* what, uint64_t* offset) {
  const String offset_header = server_->header(net::kHeaderOffset);
  if (offset_header.length() == 0) {
    Logf("[transfer] %s: X-Qr-Offset header missing", what);
    SendError(net::Error::kBadRequest);
    return false;
  }
  char* offset_end = nullptr;
  *offset = strtoull(offset_header.c_str(), &offset_end, 10);
  if (offset_end == offset_header.c_str()) {
    Logf("[transfer] %s: X-Qr-Offset \"%s\" is not a valid number", what,
         offset_header.c_str());
    SendError(net::Error::kBadRequest);
    return false;
  }
  return true;
}

// --- fixed-path handlers -------------------------------------------------------

void HttpServer::HandleDeviceInfo() {
  char device_id[kDeviceIdChars + 1];
  DeviceId(device_id, sizeof(device_id));
  const char* name = provisioning_->deviceName()[0] != 0
                          ? provisioning_->deviceName()
                          : default_device_name_;

  char buf[384];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyUint("protocolVersion", net::kProtocolVersion);
  writer.keyString("deviceId", device_id);
  writer.keyString("name", name);
  writer.keyString("model", "CrowPanel ESP32-S3 + GDEY075T7 7.5in");
  writer.keyString("firmwareVersion", app::kFirmwareVersion);
  writer.keyUint("maxChunkBytes", net::kMaxChunkBytes);
  writer.keyBool("paired", provisioning_->pairingToken()[0] != 0);
  if (reset_reason_[0] != 0) writer.keyString("resetReason", reset_reason_);
  writer.keyUint("bootCount", boot_count_);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandleDeviceStatus() {
  if (!RequireAuth()) return;

  const hal::StorageInfo info = storage_->info();
  char buf[320];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.beginObject("storage");
  writer.keyBool("mounted", info.mounted);
  writer.keyUint("capacityBytes", info.capacity_bytes);
  writer.keyUint("usedBytes", info.used_bytes);
  writer.keyUint("freeBytes", storage_->freeBytes());
  writer.endObject();
  writer.beginObject("battery");
  writer.keyBool("available", power_->batteryAvailable());
  if (power_->batteryAvailable()) {
    writer.keyUint("millivolts", power_->batteryMillivolts());
  }
  writer.endObject();
  writer.keyUint("uptimeSeconds", millis() / 1000);
  // Reachable at all only while serving HTTP, i.e. only in transfer mode.
  writer.keyBool("transferMode", true);
  writer.keyUint("openSessions", uploads_->openSessionCount());
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

// POST /api/device/time {"tz": "<POSIX TZ>"}: stored on the card so it
// survives a reboot, and applied to the clock immediately.
void HttpServer::HandleDeviceTime() {
  if (!RequireAuth()) return;
  if (clock_ == nullptr) {
    SendError(net::Error::kNotFound);
    return;
  }

  const String body = server_->arg("plain");
  net::JsonReader reader(body.c_str(), body.length());
  char tz[net::kTimeZoneMaxChars + 1] = {0};
  if (!reader.valid() || !reader.getString("tz", tz, sizeof(tz)) ||
      !net::ValidTimeZone(tz)) {
    Logf("[time] rejected time zone request: %s", body.c_str());
    SendError(net::Error::kBadRequest);
    return;
  }
  if (!storage_->writeAll(app::kFileTimeZone, tz, strlen(tz))) {
    SendError(net::Error::kStorageError);
    return;
  }
  clock_->setTimeZone(tz);
  time_zone_changed_ = true;
  Logf("[time] time zone set to \"%s\"", tz);

  char buf[160];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyString("tz", clock_->timeZone());
  writer.keyBool("synced", clock_->synced());
  writer.keyUint("nowUnix", clock_->nowUnix());
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

namespace {

void WriteLibraryItem(net::JsonWriter* writer, const net::LibraryEntry& entry) {
  char id_hex[net::kContentIdHexChars + 1];
  net::ContentIdToHex(entry.content_id, id_hex, sizeof(id_hex));
  char location[64];
  net::BuildLocation(entry, location, sizeof(location));

  writer->keyString("contentId", id_hex);
  writer->keyString("title", entry.title);
  writer->keyString("author", entry.author);
  writer->keyString("type", net::PackageTypeName(entry.type));
  writer->keyString("language", entry.language);
  writer->keyUint("contentVersion", entry.content_version);
  writer->keyUint("packageSize", entry.package_size);
  writer->keyString("location", location);
  writer->keyUint("installedAt", entry.installed_at);
}

}  // namespace

void HttpServer::HandleLibraryList() {
  if (!RequireAuth()) return;

  const uint16_t count = index_->count();
  const uint32_t capacity = 64 + static_cast<uint32_t>(count) * 400u;
  char* buf = new char[capacity];
  net::JsonWriter writer(buf, capacity);
  writer.beginObject();
  writer.beginArray("items");
  for (uint16_t i = 0; i < count; ++i) {
    const net::LibraryEntry* entry = index_->at(i);
    if (entry == nullptr) continue;
    writer.beginArrayElement();
    writer.beginObject();
    WriteLibraryItem(&writer, *entry);
    writer.endObject();
  }
  writer.endArray();
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
  delete[] buf;
}

void HttpServer::HandleUploadBegin() {
  if (!RequireAuth()) return;

  const String body = server_->arg("plain");
  net::JsonReader reader(body.c_str(), body.length());
  if (!reader.valid()) {
    SendError(net::Error::kBadRequest);
    return;
  }

  net::UploadBegin req;
  char type_name[16] = {0};
  uint32_t version = 0;
  uint64_t size = 0;
  uint32_t crc = 0;
  if (!reader.getString("contentId", req.content_id_hex, sizeof(req.content_id_hex)) ||
      !reader.getUint32("contentVersion", &version) ||
      !reader.getString("type", type_name, sizeof(type_name)) ||
      !reader.getUint64("size", &size) ||
      !reader.getUint32("payloadCrc32", &crc)) {
    SendError(net::Error::kBadRequest);
    return;
  }
  req.content_version = version;
  req.size = size;
  req.payload_crc32 = crc;
  req.type = net::PackageTypeFromName(type_name);
  if (req.type == 0) {
    SendError(net::Error::kBadRequest);
    return;
  }
  if (reader.has("title")) {
    reader.getString("title", req.title, sizeof(req.title));
  }

  // Best-effort, same rule as installed_at: no synced clock means no
  // deadline is recorded and none is enforced.
  const uint32_t now_unix =
      (clock_ != nullptr && clock_->synced()) ? clock_->nowUnix() : 0;

  uint64_t received = 0;
  uint32_t expires_at = 0;
  const net::Error err =
      uploads_->beginUpload(req, &received, now_unix, &expires_at);
  if (err != net::Error::kOk) {
    SendError(err);
    return;
  }

  char buf[256];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyString("sessionId", req.content_id_hex);
  writer.keyUint("receivedBytes", received);
  writer.keyUint("maxChunkBytes", net::kMaxChunkBytes);
  // What is actually left, not the constant: on a resume the session is
  // already part-way through its life, and a client told "900" every time
  // cannot schedule around a deadline it is not being given. Falls back to
  // the constant when there is no clock, which is what it always reported.
  const uint32_t remaining =
      (expires_at != 0 && now_unix != 0)
          ? (expires_at > now_unix ? expires_at - now_unix : 0)
          : net::kSessionExpirySeconds;
  writer.keyUint("expiresInSeconds", remaining);
  writer.endObject();
  SendJson(received > 0 ? net::kStatusOk : net::kStatusCreated, buf, writer.ok());
}

void HttpServer::HandleBackup() {
  if (!RequireAuth()) return;
  // No backup/restore logic exists in net:: yet -- packing /USER and /DEVICE
  // into a transferable blob is real, untested design work of its own, not
  // something to invent as a side effect of wiring up what already exists.
  server_->send(501, net::kContentTypeJson,
               "{\"error\":\"NOT_IMPLEMENTED\",\"detail\":\"backup/restore has no packing format yet\"}");
}

void HttpServer::HandleRestore() {
  if (!RequireAuth()) return;
  server_->send(501, net::kContentTypeJson,
               "{\"error\":\"NOT_IMPLEMENTED\",\"detail\":\"backup/restore has no packing format yet\"}");
}

void HttpServer::HandlePhotoList() {
  if (!RequireAuth()) return;
  if (photos_ == nullptr) {
    SendError(net::Error::kNotFound);
    return;
  }

  net::PhotoName* names = new net::PhotoName[app::kMaxPhotos];
  const uint16_t count = photos_->list(names, app::kMaxPhotos);
  const uint32_t capacity = 96 + static_cast<uint32_t>(count) * 72u;
  char* buf = new char[capacity];
  net::JsonWriter writer(buf, capacity);
  writer.beginObject();
  writer.keyUint("width", net::kPhotoWidth);
  writer.keyUint("height", net::kPhotoHeight);
  writer.beginArray("items");
  for (uint16_t i = 0; i < count; ++i) {
    writer.beginArrayElement();
    writer.beginObject();
    writer.keyString("name", names[i].name);
    writer.keyUint("bytes", net::kPhotoFileBytes);
    writer.endObject();
  }
  writer.endArray();
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
  delete[] buf;
  delete[] names;
}

// --- dynamic-path dispatch -------------------------------------------------

void HttpServer::HandleDynamic() {
  if (!RequireAuth()) return;

  const String uri = server_->uri();
  const HTTPMethod method = server_->method();

  if (uri.startsWith(net::kPathUploadPrefix)) {
    const String remainder = uri.substring(strlen(net::kPathUploadPrefix));
    if (remainder.endsWith(net::kUploadChunkSuffix)) {
      const String session =
          remainder.substring(0, remainder.length() - strlen(net::kUploadChunkSuffix));
      if (method == HTTP_PUT) {
        HandleUploadChunk(session.c_str());
        return;
      }
    } else if (remainder.endsWith(net::kUploadFinishSuffix)) {
      const String session = remainder.substring(
          0, remainder.length() - strlen(net::kUploadFinishSuffix));
      if (method == HTTP_POST) {
        HandleUploadFinish(session.c_str());
        return;
      }
    } else if (method == HTTP_DELETE) {
      HandleUploadAbort(remainder.c_str());
      return;
    }
    server_->send(net::kStatusNotFound, net::kContentTypeJson,
                 "{\"error\":\"NOT_FOUND\"}");
    return;
  }

  if (uri.startsWith(net::kPathPhotoPrefix)) {
    const String remainder = uri.substring(strlen(net::kPathPhotoPrefix));
    if (remainder.endsWith(net::kPhotoChunkSuffix) && method == HTTP_PUT) {
      const String name =
          remainder.substring(0, remainder.length() - strlen(net::kPhotoChunkSuffix));
      HandlePhotoChunk(name.c_str());
      return;
    }
    if (remainder.endsWith(net::kPhotoFinishSuffix) && method == HTTP_POST) {
      const String name =
          remainder.substring(0, remainder.length() - strlen(net::kPhotoFinishSuffix));
      HandlePhotoFinish(name.c_str());
      return;
    }
    if (method == HTTP_DELETE && remainder.indexOf('/') < 0) {
      HandlePhotoDelete(remainder.c_str());
      return;
    }
    server_->send(net::kStatusNotFound, net::kContentTypeJson,
                 "{\"error\":\"NOT_FOUND\"}");
    return;
  }

  if (uri.startsWith(net::kPathLibraryItemPrefix)) {
    const String id = uri.substring(strlen(net::kPathLibraryItemPrefix));
    if (method == HTTP_GET) {
      HandleLibraryItem(id.c_str(), /*delete_it=*/false);
      return;
    }
    if (method == HTTP_DELETE) {
      HandleLibraryItem(id.c_str(), /*delete_it=*/true);
      return;
    }
  }

  server_->send(net::kStatusNotFound, net::kContentTypeJson, "{\"error\":\"NOT_FOUND\"}");
}

void HttpServer::HandleLibraryItem(const char* content_id_hex, bool delete_it) {
  uint8_t id[16];
  if (!net::HexToContentId(content_id_hex, id)) {
    SendError(net::Error::kBadRequest);
    return;
  }
  const net::LibraryEntry* entry = index_->find(id);
  if (entry == nullptr) {
    SendError(net::Error::kNotFound);
    return;
  }

  if (delete_it) {
    char path[64];
    net::BuildPackagePath(*entry, path, sizeof(path));
    storage_->remove(path);
    index_->remove(id);
    index_->save(storage_);
    server_->send(net::kStatusOk, net::kContentTypeJson, "{}");
    return;
  }

  char buf[320];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  WriteLibraryItem(&writer, *entry);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandleUploadChunk(const char* session_id) {
  uint64_t offset = 0;
  if (!ReadOffsetHeader("chunk PUT", &offset)) return;

  uint32_t length = 0;
  const net::Error read_err = ReadChunkBody(&length);
  if (read_err != net::Error::kOk) {
    SendError(read_err);
    return;
  }

  uint64_t received = 0;
  uint64_t expected = 0;
  const uint32_t chunk_now =
      (clock_ != nullptr && clock_->synced()) ? clock_->nowUnix() : 0;
  const net::Error err = uploads_->writeChunk(session_id, offset, g_chunk_buf, length,
                                              &received, &expected, chunk_now);
  if (err != net::Error::kOk) {
    Logf("[transfer] chunk PUT offset=%llu length=%u failed: %s (device has %llu bytes)",
         static_cast<unsigned long long>(offset), static_cast<unsigned>(length),
         net::ErrorCodeName(err), static_cast<unsigned long long>(expected));
    SendError(err, expected);
    return;
  }

  // development.md 10.6: nothing has measured real heap headroom during a
  // transfer yet. Logging it here, per chunk, means the first hardware
  // bring-up session answers that by reading the monitor, not by writing
  // new instrumentation under time pressure.
  Logf("[transfer] chunk received=%llu free_heap=%u",
       static_cast<unsigned long long>(received), ESP.getFreeHeap());

  char buf[64];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyUint("receivedBytes", received);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandleUploadFinish(const char* session_id) {
  // 0 means "clock unset", exactly what LibraryEntry::installed_at
  // documents -- that's also what an unsynced or absent clock_ gives us
  // here, so no special-casing needed beyond the synced() check itself.
  const uint32_t now_unix =
      (clock_ != nullptr && clock_->synced()) ? clock_->nowUnix() : 0;
  net::LibraryEntry installed;
  const net::Error err = uploads_->finish(session_id, now_unix, &installed);
  if (err != net::Error::kOk) {
    Logf("[transfer] finish %.32s failed: %s", session_id, net::ErrorCodeName(err));
    SendError(err);
    return;
  }
  Logf("[transfer] installed %.32s (%s)", session_id, installed.title);

  char id_hex[net::kContentIdHexChars + 1];
  net::ContentIdToHex(installed.content_id, id_hex, sizeof(id_hex));
  char location[64];
  net::BuildLocation(installed, location, sizeof(location));

  char buf[192];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyString("contentId", id_hex);
  writer.keyBool("installed", true);
  writer.keyString("location", location);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandleUploadAbort(const char* session_id) {
  const net::Error err = uploads_->abort(session_id);
  if (err != net::Error::kOk) {
    SendError(err);
    return;
  }
  server_->send(net::kStatusOk, net::kContentTypeJson, "{}");
}

// --- firmware over Wi-Fi ---------------------------------------------------------
//
// The image is written into the OTA slot the device is not running from
// (Update, from the Arduino core). Nothing about the running firmware changes
// until finish has checked the MD5 and switched the boot slot; a failed or
// abandoned update leaves the device booting what it booted before, and USB
// flashing stays the way back.

// A chunk's bytes into g_chunk_buf, whichever way they came: the streamed
// chunked body desktop clients send, or -- from a client that cannot stream a
// request body, like the Android app's fetch -- base64 text marked
// X-Qr-Body: base64, which WebServer has already read whole into
// arg("plain") (text is safe there; a binary body would stop at a zero byte).
net::Error HttpServer::ReadChunkBody(uint32_t* length) {
  *length = 0;
  if (server_->header(net::kHeaderBody).equalsIgnoreCase(net::kBodyEncodingBase64)) {
    const String text = server_->arg("plain");
    // Four characters per three bytes, less up to two for padding: anything
    // longer than that cannot fit, and says so the way a streamed body would.
    if (text.length() / 4 * 3 > sizeof(g_chunk_buf) + 2) return net::Error::kChunkTooLarge;
    if (!net::DecodeBase64(text.c_str(), text.length(), g_chunk_buf, sizeof(g_chunk_buf),
                           length)) {
      Logf("[transfer] base64 chunk of %u characters did not decode",
           static_cast<unsigned>(text.length()));
      return net::Error::kBadRequest;
    }
    return net::Error::kOk;
  }
  WiFiClient& client = static_cast<ApiWebServer*>(server_)->currentClient();
  return ReadChunkedBody(client, g_chunk_buf, sizeof(g_chunk_buf), length);
}

void HttpServer::HandleFirmwareBegin() {
  if (!RequireAuth()) return;
  const String body = server_->arg("plain");
  net::JsonReader reader(body.c_str(), body.length());
  uint32_t size = 0;
  char md5[33] = {0};
  if (!reader.valid() || !reader.getUint32("size", &size) ||
      !reader.getString("md5", md5, sizeof(md5)) || strlen(md5) != 32 || size == 0) {
    SendError(net::Error::kBadRequest);
    return;
  }
  if (Update.isRunning()) Update.abort();  // a new begin replaces an unfinished update
  firmware_size_ = 0;
  if (!Update.begin(size, U_FLASH)) {
    Logf("[firmware] begin for %u bytes refused: %s", static_cast<unsigned>(size),
         Update.errorString());
    SendError(Update.getError() == UPDATE_ERROR_SIZE ? net::Error::kNoSpace
                                                     : net::Error::kStorageError);
    return;
  }
  Update.setMD5(md5);
  firmware_size_ = size;
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  Logf("[firmware] update started: %u bytes into %s", static_cast<unsigned>(size),
       slot != nullptr ? slot->label : "?");

  char buf[128];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyUint("receivedBytes", 0);
  writer.keyUint("maxChunkBytes", net::kMaxChunkBytes);
  writer.keyUint("slotBytes", slot != nullptr ? slot->size : 0);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandleFirmwareChunk() {
  if (!RequireAuth()) return;
  if (firmware_size_ == 0 || !Update.isRunning()) {
    SendError(net::Error::kNoSession);
    return;
  }
  uint64_t offset = 0;
  if (!ReadOffsetHeader("firmware chunk PUT", &offset)) return;

  uint32_t length = 0;
  const net::Error read_err = ReadChunkBody(&length);
  if (read_err != net::Error::kOk) {
    SendError(read_err);
    return;
  }

  const uint64_t have = Update.progress();
  if (offset + length > have) {
    // Update only appends, so any other offset is a desync: tell the client
    // where to resume, exactly as package uploads do.
    if (offset != have) {
      SendError(net::Error::kOffsetMismatch, have);
      return;
    }
    if (have + length > firmware_size_) {
      SendError(net::Error::kSizeMismatch);
      return;
    }
    if (Update.write(g_chunk_buf, length) != length) {
      Logf("[firmware] write at %llu failed: %s", static_cast<unsigned long long>(have),
           Update.errorString());
      Update.abort();
      firmware_size_ = 0;
      SendError(net::Error::kStorageError);
      return;
    }
  }  // else: a retried chunk that is already written

  char buf[64];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyUint("receivedBytes", Update.progress());
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandleFirmwareFinish() {
  if (!RequireAuth()) return;
  if (firmware_size_ == 0 || !Update.isRunning()) {
    SendError(net::Error::kNoSession);
    return;
  }
  if (Update.progress() != firmware_size_) {
    SendError(net::Error::kSizeMismatch);
    return;
  }
  // end() checks the MD5 given at begin, then makes the new slot the boot one.
  if (!Update.end()) {
    Logf("[firmware] install refused: %s", Update.errorString());
    firmware_size_ = 0;
    SendError(net::Error::kVerifyFailed);
    return;
  }
  firmware_size_ = 0;
  LogLine("[firmware] image verified and set to boot");
  SendJson(net::kStatusOk, "{\"updated\":true,\"restarting\":true}", true);
  restart_requested_ = true;  // main.cpp restarts once this reply is out
}

void HttpServer::HandleFirmwareAbort() {
  if (!RequireAuth()) return;
  const bool had_update = firmware_size_ != 0 || Update.isRunning();
  if (Update.isRunning()) Update.abort();
  firmware_size_ = 0;
  if (!had_update) {
    SendError(net::Error::kNoSession);
    return;
  }
  LogLine("[firmware] update abandoned");
  server_->send(net::kStatusOk, net::kContentTypeJson, "{}");
}

void HttpServer::HandlePhotoChunk(const char* name) {
  if (photos_ == nullptr) {
    SendError(net::Error::kNotFound);
    return;
  }
  uint64_t offset = 0;
  if (!ReadOffsetHeader("photo chunk PUT", &offset)) return;

  uint32_t length = 0;
  const net::Error read_err = ReadChunkBody(&length);
  if (read_err != net::Error::kOk) {
    SendError(read_err);
    return;
  }

  uint64_t received = 0;
  uint64_t expected = 0;
  const net::Error err =
      photos_->writeChunk(name, offset, g_chunk_buf, length, &received, &expected);
  if (err != net::Error::kOk) {
    Logf("[photos] chunk for \"%s\" at %llu refused: %s", name,
         static_cast<unsigned long long>(offset), net::ErrorCodeName(err));
    SendError(err, expected);
    return;
  }

  char buf[64];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyUint("receivedBytes", received);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandlePhotoFinish(const char* name) {
  if (photos_ == nullptr) {
    SendError(net::Error::kNotFound);
    return;
  }
  const net::Error err = photos_->finish(name);
  if (err != net::Error::kOk) {
    Logf("[photos] finish \"%s\" refused: %s", name, net::ErrorCodeName(err));
    SendError(err);
    return;
  }
  photos_changed_ = true;
  Logf("[photos] installed \"%s\"", name);

  char buf[96];
  net::JsonWriter writer(buf, sizeof(buf));
  writer.beginObject();
  writer.keyString("name", name);
  writer.keyBool("installed", true);
  writer.endObject();
  SendJson(net::kStatusOk, buf, writer.ok());
}

void HttpServer::HandlePhotoDelete(const char* name) {
  if (photos_ == nullptr) {
    SendError(net::Error::kNotFound);
    return;
  }
  const net::Error err = photos_->remove(name);
  if (err != net::Error::kOk) {
    SendError(err);
    return;
  }
  photos_changed_ = true;
  Logf("[photos] deleted \"%s\"", name);
  server_->send(net::kStatusOk, net::kContentTypeJson, "{}");
}

}  // namespace drivers
