// net_credentials.h -- NVS-backed persistence for provisioned Wi-Fi
// credentials and the pairing token.
//
// Deliberately dumb: this class only moves bytes between NVS and a
// net::ProvisioningState. The rule that matters -- persist only after the
// radio has proven the credentials work -- lives in the caller (see
// net/provisioning_state.h), not here. save() will happily write whatever
// state you hand it.

#pragma once

#include "net/provisioning_state.h"

namespace drivers {

class NetCredentials {
 public:
  // Reads the saved SSID/passphrase/device name/pairing token, if any, into
  // `state` via its setters. Returns false and leaves `state` untouched if
  // nothing has been saved yet (first boot, or never provisioned).
  bool load(net::ProvisioningState* state) const;

  // Persists the current SSID/passphrase/device name/pairing token from
  // `state`. Call only after the connection they describe has actually
  // succeeded -- see the commit-then-verify-then-persist rule in
  // provisioning_state.h.
  bool save(const net::ProvisioningState& state);

  // Erases everything saved -- the factory-reset / re-provisioning path.
  // Idempotent: clearing an already-empty namespace is not an error.
  bool clear();
};

}  // namespace drivers
