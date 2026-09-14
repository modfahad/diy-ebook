// ble_uuids.h -- the GATT service/characteristic UUIDs for first-boot
// provisioning. See docs/provisioning.md for the wire spec these serve.
//
// Custom 128-bit UUIDs, randomly generated once for this product and then
// fixed forever (changing one breaks pairing with every device already in
// the field). Same base, last two hex digits of the first group vary by
// characteristic -- the same convention Nordic's UART service uses.

#pragma once

namespace net {

constexpr const char* kBleServiceUuid =
    "0000d6d6-6502-43f7-82c9-cd76e08e5339";

// Read-only. A stable, chip-derived id (see DeviceInfo.deviceId in
// packages/protocol) -- how the desktop recognises a device it has already
// paired with, before it has Wi-Fi to ask the HTTP API.
constexpr const char* kBleCharDeviceIdentityUuid =
    "0001d6d6-6502-43f7-82c9-cd76e08e5339";

// Write-only. Plain UTF-8, not JSON: see net::ProvisioningState::setSsid.
constexpr const char* kBleCharSsidUuid =
    "0002d6d6-6502-43f7-82c9-cd76e08e5339";

// Write-only, and never readable back -- see
// net::ProvisioningState::passphraseForRadioUseOnly.
constexpr const char* kBleCharPassphraseUuid =
    "0003d6d6-6502-43f7-82c9-cd76e08e5339";

constexpr const char* kBleCharDeviceNameUuid =
    "0004d6d6-6502-43f7-82c9-cd76e08e5339";

// Write-only. The token the desktop will later present as a Bearer token
// over HTTP; see net::auth.h.
constexpr const char* kBleCharPairingTokenUuid =
    "0005d6d6-6502-43f7-82c9-cd76e08e5339";

// Write-only, value ignored. Any write triggers
// net::ProvisioningState::beginCommit() and a connection attempt.
constexpr const char* kBleCharCommitUuid =
    "0006d6d6-6502-43f7-82c9-cd76e08e5339";

// Read + notify. Value is exactly
// net::ProvisioningState::writeStatusJson() -- never hand-rolled, so the "no
// passphrase in the status document" guarantee holds here too.
constexpr const char* kBleCharStatusUuid =
    "0007d6d6-6502-43f7-82c9-cd76e08e5339";

}  // namespace net
