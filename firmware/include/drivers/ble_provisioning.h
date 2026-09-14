// ble_provisioning.h -- GATT front end for net::ProvisioningState.
//
// See net/ble_uuids.h for the characteristic UUIDs and docs/provisioning.md
// for the wire spec. Kept Arduino/BLE-stack types out of this header (opaque
// pointers only) so nothing that merely wants to call begin()/poll()/end()
// has to pull in BLEDevice.h.
//
// Threading note: BLE characteristic callbacks run on the Bluedroid host
// task, not the Arduino loop() task. Callbacks only touch `state_` (whose
// setters are simple, self-contained field writes) and flip
// `commit_requested_`; the actual Wi-Fi connect attempt -- the part with
// timing and retries -- happens in poll(), called from loop(), matching the
// single-threaded model the rest of this firmware uses.

#pragma once

#include <stdint.h>

#include "drivers/net_credentials.h"
#include "net/provisioning_state.h"

class BLEServer;
class BLECharacteristic;

namespace drivers {

class BleProvisioning {
 public:
  // Starts advertising `device_name` and serves `state` over GATT. `state`
  // must outlive this object or the next end(), whichever comes first.
  void begin(net::ProvisioningState* state, const char* device_name);

  // Stops advertising and frees the BLE stack's memory (BLEDevice::deinit).
  // Call before bringing up Wi-Fi -- see architecture.md's power model: the
  // product flow never needs both radios at once, just briefly overlapping
  // during commit.
  void end();

  // Advances a pending commit: on a commit request, calls
  // net::ProvisioningState::beginCommit(), attempts WiFi.begin(), and on
  // success or timeout reports the outcome back into the state machine and
  // (on success) persists the credentials via NetCredentials. No-op if no
  // commit is pending or in flight. Call every loop() iteration while
  // active().
  void poll();

  bool active() const { return active_; }

 private:
  void handleCommitRequested();
  void finishConnecting(bool succeeded);
  void publishStatus();

  net::ProvisioningState* state_ = nullptr;
  NetCredentials credentials_;
  BLEServer* server_ = nullptr;
  BLECharacteristic* status_char_ = nullptr;

  bool active_ = false;
  volatile bool commit_requested_ = false;
  bool connecting_ = false;
  uint32_t connect_started_ms_ = 0;

  class SsidCallback;
  class PassphraseCallback;
  class DeviceNameCallback;
  class PairingTokenCallback;
  class CommitCallback;
  class SecurityCallbacks;
};

}  // namespace drivers
