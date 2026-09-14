#include "drivers/ble_provisioning.h"

#include <string.h>

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <WiFi.h>

#include "app/app_config.h"
#include "drivers/serial_log.h"
#include "drivers/device_identity.h"
#include "net/ble_uuids.h"

namespace drivers {
namespace {

void SetStringValue(BLECharacteristic* characteristic, const char* value) {
  characteristic->setValue(reinterpret_cast<uint8_t*>(const_cast<char*>(value)),
                            strlen(value));
}

}  // namespace

// Each of these just copies the written value into `state_` via one setter.
// The write is rejected client-side implicitly: net::ProvisioningState's
// setters never crash on bad input, they just record last_error() for the
// next status read.

class BleProvisioning::SsidCallback : public BLECharacteristicCallbacks {
 public:
  explicit SsidCallback(BleProvisioning* owner) : owner_(owner) {}
  void onWrite(BLECharacteristic* c) override {
    owner_->state_->setSsid(c->getValue().c_str());
    owner_->publishStatus();
  }

 private:
  BleProvisioning* owner_;
};

class BleProvisioning::PassphraseCallback : public BLECharacteristicCallbacks {
 public:
  explicit PassphraseCallback(BleProvisioning* owner) : owner_(owner) {}
  void onWrite(BLECharacteristic* c) override {
    owner_->state_->setPassphrase(c->getValue().c_str());
    owner_->publishStatus();
  }

 private:
  BleProvisioning* owner_;
};

class BleProvisioning::DeviceNameCallback : public BLECharacteristicCallbacks {
 public:
  explicit DeviceNameCallback(BleProvisioning* owner) : owner_(owner) {}
  void onWrite(BLECharacteristic* c) override {
    owner_->state_->setDeviceName(c->getValue().c_str());
    owner_->publishStatus();
  }

 private:
  BleProvisioning* owner_;
};

class BleProvisioning::PairingTokenCallback : public BLECharacteristicCallbacks {
 public:
  explicit PairingTokenCallback(BleProvisioning* owner) : owner_(owner) {}
  void onWrite(BLECharacteristic* c) override {
    owner_->state_->setPairingToken(c->getValue().c_str());
    owner_->publishStatus();
  }

 private:
  BleProvisioning* owner_;
};

// Value is ignored -- only the write event matters. The actual connect
// attempt happens in poll(), on the loop() task, not here on the Bluedroid
// task: WiFi.begin() plus the association wait is not something to do inside
// a GATT callback.
class BleProvisioning::CommitCallback : public BLECharacteristicCallbacks {
 public:
  explicit CommitCallback(BleProvisioning* owner) : owner_(owner) {}
  void onWrite(BLECharacteristic* /*c*/) override {
    owner_->commit_requested_ = true;
  }

 private:
  BleProvisioning* owner_;
};

// BLESecurityCallbacks has no default-implemented methods -- all five are
// pure virtual, so all five need a body even though this device's pairing
// mode (ESP_IO_CAP_NONE, "Just Works" -- no display to show a passkey on, no
// keypad to enter one) never actually drives a passkey exchange. Only
// onSecurityRequest() and onAuthenticationComplete() are ever meaningfully
// invoked for this IO capability; the rest exist to satisfy the interface.
class BleProvisioning::SecurityCallbacks : public BLESecurityCallbacks {
 public:
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t /*pass_key*/) override {}
  bool onConfirmPIN(uint32_t /*pin*/) override { return true; }
  // Always accept: an unpaired device has nothing to lose by encrypting, and
  // the sensitive characteristics themselves require an encrypted link (see
  // ESP_GATT_PERM_*_ENCRYPTED below) regardless of this answer.
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    drivers::Logf("[ble] pairing %s\n", cmpl.success ? "ok" : "failed");
  }
};

void BleProvisioning::begin(net::ProvisioningState* state,
                            const char* device_name) {
  state_ = state;
  commit_requested_ = false;
  connecting_ = false;

  BLEDevice::init(device_name);

  // Encrypt the link before any credential-bearing characteristic accepts a
  // read or write. ESP_IO_CAP_NONE ("Just Works") is the only honest choice
  // here: this device has no display to show a passkey on and no keypad to
  // enter one, so anything claiming MITM protection would be theater. This
  // still protects against passive eavesdropping, which the total absence
  // of any security configuration (the state before this) did not.
  static SecurityCallbacks security_callbacks;
  BLEDevice::setSecurityCallbacks(&security_callbacks);
  BLESecurity security;
  security.setAuthenticationMode(ESP_LE_AUTH_REQ_SC_ONLY);
  security.setCapability(ESP_IO_CAP_NONE);
  security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security.setKeySize();

  server_ = BLEDevice::createServer();
  BLEService* service = server_->createService(net::kBleServiceUuid);

  // Not encrypted, deliberately: a central needs to read this to recognise
  // the device *before* pairing (see docs/provisioning.md), the same reason
  // GET /api/device/info is the one HTTP endpoint that skips auth.
  BLECharacteristic* identity = service->createCharacteristic(
      net::kBleCharDeviceIdentityUuid, BLECharacteristic::PROPERTY_READ);
  char device_id[kDeviceIdChars + 1] = {0};
  DeviceId(device_id, sizeof(device_id));
  SetStringValue(identity, device_id);

  BLECharacteristic* ssid_char = service->createCharacteristic(
      net::kBleCharSsidUuid, BLECharacteristic::PROPERTY_WRITE);
  ssid_char->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  ssid_char->setCallbacks(new SsidCallback(this));

  BLECharacteristic* passphrase_char = service->createCharacteristic(
      net::kBleCharPassphraseUuid, BLECharacteristic::PROPERTY_WRITE);
  passphrase_char->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  passphrase_char->setCallbacks(new PassphraseCallback(this));

  BLECharacteristic* name_char = service->createCharacteristic(
      net::kBleCharDeviceNameUuid, BLECharacteristic::PROPERTY_WRITE);
  name_char->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  name_char->setCallbacks(new DeviceNameCallback(this));

  BLECharacteristic* token_char = service->createCharacteristic(
      net::kBleCharPairingTokenUuid, BLECharacteristic::PROPERTY_WRITE);
  token_char->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  token_char->setCallbacks(new PairingTokenCallback(this));

  BLECharacteristic* commit_char = service->createCharacteristic(
      net::kBleCharCommitUuid, BLECharacteristic::PROPERTY_WRITE);
  commit_char->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);
  commit_char->setCallbacks(new CommitCallback(this));

  status_char_ = service->createCharacteristic(
      net::kBleCharStatusUuid,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  status_char_->setAccessPermissions(ESP_GATT_PERM_READ_ENCRYPTED);
  status_char_->addDescriptor(new BLE2902());

  service->start();
  publishStatus();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(net::kBleServiceUuid);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  active_ = true;
}

void BleProvisioning::end() {
  if (!active_) return;
  BLEDevice::deinit(/*release_memory=*/true);
  server_ = nullptr;
  status_char_ = nullptr;
  active_ = false;
}

void BleProvisioning::poll() {
  if (!active_) return;

  if (commit_requested_ && !connecting_) {
    commit_requested_ = false;
    handleCommitRequested();
  }

  if (connecting_) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting_ = false;
      finishConnecting(/*succeeded=*/true);
    } else if (millis() - connect_started_ms_ > app::kWifiConnectTimeoutMs) {
      connecting_ = false;
      WiFi.disconnect(/*wifioff=*/true);
      finishConnecting(/*succeeded=*/false);
    }
  }
}

void BleProvisioning::handleCommitRequested() {
  if (state_->beginCommit() != net::ProvisioningError::kOk) {
    publishStatus();  // not ready -- last_error() explains why
    return;
  }
  publishStatus();  // now "connecting"

  if (state_->hasPassphrase()) {
    WiFi.begin(state_->ssid(), state_->passphraseForRadioUseOnly());
  } else {
    WiFi.begin(state_->ssid());
  }
  connecting_ = true;
  connect_started_ms_ = millis();
}

void BleProvisioning::finishConnecting(bool succeeded) {
  if (!succeeded) {
    state_->commitFailed();
    publishStatus();
    return;
  }

  // Commit-then-verify-then-persist: only reachable here once WiFi.status()
  // has actually reported an association.
  state_->commitSucceeded();
  if (!credentials_.save(*state_)) {
    state_->persistFailed();
  }
  publishStatus();
}

void BleProvisioning::publishStatus() {
  if (status_char_ == nullptr) return;
  char json[192];
  const uint32_t length = state_->writeStatusJson(json, sizeof(json));
  if (length == 0) return;  // truncated -- see JsonWriter::ok()
  status_char_->setValue(reinterpret_cast<uint8_t*>(json), length);
  status_char_->notify();
}

}  // namespace drivers
