// http_server.h -- the desktop <-> device HTTP API (docs/protocol.md).
//
// Wraps the Arduino WebServer. Kept out of this header entirely (opaque
// pointer only) so nothing that just wants begin()/poll()/end() has to pull
// in WebServer.h.
//
// NOT wired to any always-on policy: begin()/end() are plain calls. When to
// call them is main.cpp's decision -- on request in transfer mode, or all the
// time in table-clock mode (app::kTableClockMode).

#pragma once

#include <stdint.h>

#include "hal/clock.h"
#include "hal/power.h"
#include "hal/storage.h"
#include "net/library_index.h"
#include "net/photo_store.h"
#include "net/provisioning_state.h"
#include "net/upload_manager.h"

class WebServer;
class WiFiClient;

namespace drivers {

class HttpServer {
 public:
  // None of these pointers are owned; all must outlive this object or the
  // next end(), whichever comes first. `photos` may be null: the photo
  // endpoints then answer 404.
  void begin(hal::IStorage* storage, hal::IPower* power,
             net::LibraryIndex* index, net::UploadManager* uploads,
             const net::ProvisioningState* provisioning,
             const char* default_device_name, hal::IClock* clock = nullptr,
             net::PhotoStore* photos = nullptr);
  void end();

  // Services at most one pending client request. Call every loop() iteration
  // while active().
  void poll();

  bool active() const { return server_ != nullptr; }

  // Set when a photo was added, replaced or deleted / the time zone was set
  // since the last call; reading clears it. The home screen redraws on these.
  bool takePhotosChanged();
  bool takeTimeZoneChanged();

  // Reported by GET /api/device/info, so the desktop can show why the device
  // last restarted (the history itself is /DEVICE/resets.log, net::reset_log).
  void setBootInfo(const char* reset_reason, uint32_t boot_count) {
    reset_reason_ = reset_reason;
    boot_count_ = boot_count;
  }

  // True once, after a firmware update has been installed and its reply sent:
  // main.cpp then restarts into the new image.
  bool takeRestartRequested() {
    const bool requested = restart_requested_;
    restart_requested_ = false;
    return requested;
  }

 private:
  bool RequireAuth();
  void SendError(net::Error error, uint64_t expected_offset = 0);
  void SendJson(uint16_t status, const char* body, bool ok);
  // Parses X-Qr-Offset. On failure sends the error response itself.
  bool ReadOffsetHeader(const char* what, uint64_t* offset);

  void HandleDeviceInfo();
  void HandleDeviceStatus();
  void HandleDeviceTime();
  void HandleLibraryList();
  void HandleUploadBegin();
  void HandleBackup();
  void HandleRestore();
  void HandlePhotoList();
  // Dynamic paths WebServer's literal-match on() cannot express: library
  // items, upload chunk/finish/abort and photos all carry an id in the path.
  void HandleDynamic();
  void HandleLibraryItem(const char* content_id_hex, bool delete_it);
  void HandleUploadChunk(const char* session_id);
  void HandleUploadFinish(const char* session_id);
  void HandleUploadAbort(const char* session_id);
  void HandlePhotoChunk(const char* name);
  void HandlePhotoFinish(const char* name);
  void HandlePhotoDelete(const char* name);
  void HandleFirmwareBegin();
  void HandleFirmwareChunk();
  void HandleFirmwareFinish();
  void HandleFirmwareAbort();

  // A chunk PUT's bytes into the shared chunk buffer: the streamed chunked
  // body, or base64 text when X-Qr-Body: base64 (clients that cannot stream).
  net::Error ReadChunkBody(uint32_t* length);

  // Reads a Transfer-Encoding: chunked body straight off the socket into
  // `out` (capacity `capacity`), bypassing WebServer's own body parsing --
  // see the comment in http_server.cpp for why that matters for binary data.
  // Returns kOk, kChunkTooLarge (the framing was fine, the total didn't fit),
  // or kBadRequest (malformed framing, or the client gave up mid-body).
  net::Error ReadChunkedBody(WiFiClient& client, uint8_t* out, uint32_t capacity,
                             uint32_t* length_out);

  WebServer* server_ = nullptr;
  hal::IStorage* storage_ = nullptr;
  hal::IPower* power_ = nullptr;
  net::LibraryIndex* index_ = nullptr;
  net::UploadManager* uploads_ = nullptr;
  const net::ProvisioningState* provisioning_ = nullptr;
  const char* default_device_name_ = "";
  hal::IClock* clock_ = nullptr;
  const char* reset_reason_ = "";
  uint32_t boot_count_ = 0;
  bool restart_requested_ = false;
  uint32_t firmware_size_ = 0;  // of the update in progress; 0 = none
  net::PhotoStore* photos_ = nullptr;
  bool photos_changed_ = false;
  bool time_zone_changed_ = false;
};

}  // namespace drivers
