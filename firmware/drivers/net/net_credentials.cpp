#include "drivers/net_credentials.h"

#include <Preferences.h>
#include <string.h>

#include "net/protocol.h"

namespace drivers {
namespace {

constexpr const char* kNamespace = "netcred";
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPass = "pass";
constexpr const char* kKeyName = "name";
constexpr const char* kKeyToken = "token";

// Preferences::putString returns the number of bytes written, which is 0 both
// on failure and for a legitimately empty string (an open network's
// passphrase) -- so success is "wrote exactly strlen(value)", not "wrote > 0".
bool PutStringExact(Preferences* prefs, const char* key, const char* value) {
  return prefs->putString(key, value) == strlen(value);
}

}  // namespace

bool NetCredentials::load(net::ProvisioningState* state) const {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/true)) return false;

  char ssid[net::kSsidMaxBytes + 1] = {0};
  const bool has_ssid =
      prefs.getString(kKeySsid, ssid, sizeof(ssid)) > 0;
  if (!has_ssid) {
    prefs.end();
    return false;
  }

  char pass[net::kPassphraseMaxBytes + 1] = {0};
  char name[net::kDeviceNameMaxBytes + 1] = {0};
  char token[net::kTokenMaxChars + 1] = {0};
  prefs.getString(kKeyPass, pass, sizeof(pass));
  prefs.getString(kKeyName, name, sizeof(name));
  prefs.getString(kKeyToken, token, sizeof(token));
  prefs.end();

  state->setSsid(ssid);
  state->setPassphrase(pass);
  state->setDeviceName(name);
  state->setPairingToken(token);
  return true;
}

bool NetCredentials::save(const net::ProvisioningState& state) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;

  bool ok = true;
  ok = PutStringExact(&prefs, kKeySsid, state.ssid()) && ok;
  ok = PutStringExact(&prefs, kKeyPass, state.passphraseForRadioUseOnly()) && ok;
  ok = PutStringExact(&prefs, kKeyName, state.deviceName()) && ok;
  ok = PutStringExact(&prefs, kKeyToken, state.pairingToken()) && ok;
  prefs.end();
  return ok;
}

bool NetCredentials::clear() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, /*readOnly=*/false)) return false;
  const bool ok = prefs.clear();
  prefs.end();
  return ok;
}

}  // namespace drivers
