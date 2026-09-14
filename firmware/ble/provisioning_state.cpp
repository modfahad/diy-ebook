#include "net/provisioning_state.h"

#include <string.h>

#include "net/json.h"

namespace net {
namespace {

uint32_t Length(const char* text) {
  if (text == nullptr) return 0;
  return static_cast<uint32_t>(strlen(text));
}

void CopyBounded(char* out, uint32_t capacity, const char* src) {
  uint32_t i = 0;
  if (src != nullptr) {
    while (src[i] != 0 && i + 1 < capacity) {
      out[i] = src[i];
      ++i;
    }
  }
  out[i] = 0;
}

}  // namespace

const char* ProvisioningStatusName(ProvisioningStatus status) {
  switch (status) {
    case ProvisioningStatus::kIdle:        return "idle";
    case ProvisioningStatus::kReceiving:   return "receiving";
    case ProvisioningStatus::kReady:       return "ready";
    case ProvisioningStatus::kConnecting:  return "connecting";
    case ProvisioningStatus::kProvisioned: return "provisioned";
    case ProvisioningStatus::kFailed:      return "failed";
    default:                               return "unknown";
  }
}

const char* ProvisioningErrorName(ProvisioningError error) {
  switch (error) {
    case ProvisioningError::kOk:                 return "OK";
    case ProvisioningError::kSsidTooLong:        return "SSID_TOO_LONG";
    case ProvisioningError::kSsidEmpty:          return "SSID_EMPTY";
    case ProvisioningError::kPassphraseTooShort: return "PASSPHRASE_TOO_SHORT";
    case ProvisioningError::kPassphraseTooLong:  return "PASSPHRASE_TOO_LONG";
    case ProvisioningError::kNameTooLong:        return "NAME_TOO_LONG";
    case ProvisioningError::kTokenTooLong:       return "TOKEN_TOO_LONG";
    case ProvisioningError::kNotReady:           return "NOT_READY";
    case ProvisioningError::kConnectFailed:      return "CONNECT_FAILED";
    case ProvisioningError::kPersistFailed:      return "PERSIST_FAILED";
    default:                                     return "UNKNOWN";
  }
}

void ProvisioningState::reset() {
  memset(ssid_, 0, sizeof(ssid_));
  memset(passphrase_, 0, sizeof(passphrase_));
  memset(device_name_, 0, sizeof(device_name_));
  memset(pairing_token_, 0, sizeof(pairing_token_));
  status_ = ProvisioningStatus::kIdle;
  last_error_ = ProvisioningError::kOk;
}

ProvisioningError ProvisioningState::setSsid(const char* ssid) {
  const uint32_t length = Length(ssid);
  if (length == 0) {
    last_error_ = ProvisioningError::kSsidEmpty;
    return last_error_;
  }
  if (length > kSsidMaxBytes) {
    last_error_ = ProvisioningError::kSsidTooLong;
    return last_error_;
  }
  CopyBounded(ssid_, sizeof(ssid_), ssid);
  status_ = ready() ? ProvisioningStatus::kReady : ProvisioningStatus::kReceiving;
  last_error_ = ProvisioningError::kOk;
  return last_error_;
}

ProvisioningError ProvisioningState::setPassphrase(const char* passphrase) {
  const uint32_t length = Length(passphrase);
  // Empty is legal: an open network has no passphrase. Anything between 1 and
  // 7 bytes cannot be a WPA2 passphrase, so it is a typo, not a network.
  if (length > 0 && length < kPassphraseMinBytes) {
    last_error_ = ProvisioningError::kPassphraseTooShort;
    return last_error_;
  }
  if (length > kPassphraseMaxBytes) {
    last_error_ = ProvisioningError::kPassphraseTooLong;
    return last_error_;
  }
  CopyBounded(passphrase_, sizeof(passphrase_), passphrase);
  status_ = ready() ? ProvisioningStatus::kReady : ProvisioningStatus::kReceiving;
  last_error_ = ProvisioningError::kOk;
  return last_error_;
}

ProvisioningError ProvisioningState::setDeviceName(const char* name) {
  if (Length(name) > kDeviceNameMaxBytes) {
    last_error_ = ProvisioningError::kNameTooLong;
    return last_error_;
  }
  CopyBounded(device_name_, sizeof(device_name_), name);
  if (status_ == ProvisioningStatus::kIdle) {
    status_ = ProvisioningStatus::kReceiving;
  }
  last_error_ = ProvisioningError::kOk;
  return last_error_;
}

ProvisioningError ProvisioningState::setPairingToken(const char* token) {
  if (Length(token) > kTokenMaxChars) {
    last_error_ = ProvisioningError::kTokenTooLong;
    return last_error_;
  }
  CopyBounded(pairing_token_, sizeof(pairing_token_), token);
  if (status_ == ProvisioningStatus::kIdle) {
    status_ = ProvisioningStatus::kReceiving;
  }
  last_error_ = ProvisioningError::kOk;
  return last_error_;
}

bool ProvisioningState::ready() const { return ssid_[0] != 0; }

ProvisioningError ProvisioningState::beginCommit() {
  if (!ready()) {
    last_error_ = ProvisioningError::kNotReady;
    return last_error_;
  }
  status_ = ProvisioningStatus::kConnecting;
  last_error_ = ProvisioningError::kOk;
  return last_error_;
}

void ProvisioningState::commitSucceeded() {
  status_ = ProvisioningStatus::kProvisioned;
  last_error_ = ProvisioningError::kOk;
}

void ProvisioningState::commitFailed() {
  // The fields are kept: the user is standing there and will want to retry
  // with a corrected password, not re-enter everything.
  status_ = ProvisioningStatus::kFailed;
  last_error_ = ProvisioningError::kConnectFailed;
}

void ProvisioningState::persistFailed() {
  status_ = ProvisioningStatus::kFailed;
  last_error_ = ProvisioningError::kPersistFailed;
}

uint32_t ProvisioningState::writeStatusJson(char* out, uint32_t capacity) const {
  JsonWriter writer(out, capacity);
  writer.beginObject();
  writer.keyString("status", ProvisioningStatusName(status_));
  writer.keyString("error", ProvisioningErrorName(last_error_));
  writer.keyString("ssid", ssid_);
  writer.keyString("deviceName", device_name_);
  // Whether a passphrase was supplied is useful; the passphrase is not.
  writer.keyBool("hasPassphrase", hasPassphrase());
  writer.keyBool("hasPairingToken", pairing_token_[0] != 0);
  writer.keyBool("ready", ready());
  writer.endObject();
  return writer.ok() ? writer.length() : 0;
}

}  // namespace net
