// provisioning_state.h -- the pure half of BLE first-boot provisioning.
//
// The GATT plumbing cannot be tested without hardware. This can: it is the
// state machine that accumulates the credentials, decides when a commit is
// legal, and produces the status the desktop reads back.
//
// Two rules it exists to enforce:
//   * commit-then-verify-then-persist. Credentials are only written to NVS
//     after the Wi-Fi connection they describe has actually worked. Persisting
//     first would leave a device that boots, fails to associate, and has no
//     way back except a factory reset.
//   * the password is never readable. Not through status, not through the
//     serialiser, not through a getter. There is deliberately no accessor that
//     returns it; only the driver's connect path sees it.

#pragma once

#include <stdint.h>

#include "net/protocol.h"

namespace net {

// IEEE 802.11 limits. Rejecting out-of-range input here means the radio layer
// never has to.
constexpr uint8_t kSsidMaxBytes = 32;
constexpr uint8_t kPassphraseMinBytes = 8;
constexpr uint8_t kPassphraseMaxBytes = 63;
constexpr uint8_t kDeviceNameMaxBytes = 31;

enum class ProvisioningStatus : uint8_t {
  kIdle = 0,        // nothing received yet
  kReceiving,       // some fields set, not yet committable
  kReady,           // enough to attempt a connection
  kConnecting,      // commit issued, radio trying
  kProvisioned,     // connected and persisted
  kFailed,          // the last attempt failed; fields are kept for a retry
};

enum class ProvisioningError : uint8_t {
  kOk = 0,
  kSsidTooLong,
  kSsidEmpty,
  kPassphraseTooShort,
  kPassphraseTooLong,
  kNameTooLong,
  kTokenTooLong,
  kNotReady,
  kConnectFailed,
  kPersistFailed,
};

const char* ProvisioningStatusName(ProvisioningStatus status);
const char* ProvisioningErrorName(ProvisioningError error);

class ProvisioningState {
 public:
  void reset();

  ProvisioningError setSsid(const char* ssid);
  /** An empty passphrase means an open network, which is allowed. */
  ProvisioningError setPassphrase(const char* passphrase);
  ProvisioningError setDeviceName(const char* name);
  /** The pairing token the desktop will present over HTTP. */
  ProvisioningError setPairingToken(const char* token);

  /** True once an SSID is present; the passphrase may legitimately be empty. */
  bool ready() const;

  /** Moves to kConnecting. Fails if not ready. */
  ProvisioningError beginCommit();

  /** Records the outcome of the radio's attempt. */
  void commitSucceeded();
  void commitFailed();
  void persistFailed();

  ProvisioningStatus status() const { return status_; }
  ProvisioningError lastError() const { return last_error_; }

  const char* ssid() const { return ssid_; }
  const char* deviceName() const { return device_name_; }
  const char* pairingToken() const { return pairing_token_; }
  bool hasPassphrase() const { return passphrase_[0] != 0; }

  /**
   * The ONLY way the passphrase leaves this object. Not a getter: a named,
   * awkward call that shows up in review, used by the Wi-Fi driver and nowhere
   * else.
   */
  const char* passphraseForRadioUseOnly() const { return passphrase_; }

  /**
   * Writes the status document the desktop polls. It must never contain the
   * passphrase; there is a test that asserts exactly that.
   */
  uint32_t writeStatusJson(char* out, uint32_t capacity) const;

 private:
  char ssid_[kSsidMaxBytes + 1] = {0};
  char passphrase_[kPassphraseMaxBytes + 1] = {0};
  char device_name_[kDeviceNameMaxBytes + 1] = {0};
  char pairing_token_[kTokenMaxChars + 1] = {0};
  ProvisioningStatus status_ = ProvisioningStatus::kIdle;
  ProvisioningError last_error_ = ProvisioningError::kOk;
};

}  // namespace net
