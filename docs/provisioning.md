# First-time provisioning (BLE)

**Implemented — Milestone 4.** The state machine is
`firmware/include/net/provisioning_state.h` (pure, host-tested — see
`test_net`); the GATT front end that drives it is
`firmware/drivers/net/ble_provisioning.cpp` (Arduino BLE stack, buildable but
**never run on real hardware** — see the Milestone 1 caveat in
[development.md](development.md), which still applies to everything in this
document).

- BLE is used for **first-time provisioning and recovery only**. A device
  with no saved credentials advertises for pairing automatically at boot
  (`main.cpp`'s `BringUpNetState`); a device that already has saved
  credentials does not — see [protocol.md](protocol.md)'s "Wi-Fi is
  on-demand" note. After a successful commit, BLE is torn down
  (`BLEDevice::deinit`) and does not come back until the credentials are
  cleared. A long-press on EXIT does that (`main.cpp`'s `FactoryReset`):
  clears the `netcred` NVS namespace, resets the state machine, and
  re-enters pairing immediately, without a reboot.
- On commit the device connects, **verifies the connection**
  (`WiFi.status() == WL_CONNECTED`, with a timeout —
  `app::kWifiConnectTimeoutMs`), and only then persists the credentials via
  `drivers::NetCredentials` (NVS namespace `netcred`). A failed attempt keeps
  every field for a retry rather than making the user re-enter everything.
- Credentials are stored in NVS, never logged, and never exposed through the
  HTTP API. `ProvisioningState::passphraseForRadioUseOnly()` is the only
  accessor for the passphrase; `writeStatusJson()` — used verbatim for the
  status characteristic below — is asserted (in `test_net`) to never contain
  it.
- The desktop is expected to remember trusted devices by
  `kBleCharDeviceIdentityUuid` / `DeviceInfo.deviceId` (the same id both
  ways) — no separate BLE-side pairing-memory mechanism is implemented.

## GATT profile

UUIDs are `firmware/include/net/ble_uuids.h`. They are custom, randomly
generated once for this product, and now fixed — changing one breaks pairing
with every device already provisioned in the field.

Service: `0000d6d6-6502-43f7-82c9-cd76e08e5339`

| Characteristic | UUID (`...d6d6-6502-43f7-82c9-cd76e08e5339`) | Properties | Value |
|---|---|---|---|
| Device identity | `0001...` | Read | 12 hex chars, EFUSE-MAC-derived (`DeviceId`) — the same string HTTP calls `deviceId` |
| SSID | `0002...` | Write | UTF-8 SSID, ≤32 bytes |
| Passphrase | `0003...` | Write | UTF-8, 0 (open network) or 8–63 bytes |
| Device name | `0004...` | Write | UTF-8, ≤31 bytes |
| Pairing token | `0005...` | Write | The Bearer token the desktop will later present over HTTP, ≤64 bytes |
| Commit | `0006...` | Write | Value ignored; any write triggers `beginCommit()` + a connect attempt |
| Status | `0007...` | Read, Notify | Exactly `ProvisioningState::writeStatusJson()` — `{status, error, ssid, deviceName, hasPassphrase, hasPairingToken, ready}` |

> **Development shortcut, 2026-09-01:** the same three values can instead be
> put on the SD card as `/DEVICE/wifi.json` —
> `{"ssid":"...","passphrase":"...","token":"..."}` — which
> `main.cpp`'s `TryWifiBypassFromSdCard()` reads once at boot and then deletes.
> It exists because the Windows BLE client cannot complete the flow below, and
> it is a real security downgrade: the pairing token is the bearer credential
> for every destructive endpoint and this puts it in plaintext on removable
> media. Use BLE where you can. See [pending.md §3](pending.md).

Flow: write SSID (and optionally passphrase / device name / pairing token) →
read or wait on a Status notification to see `"ready": true` → write Commit →
watch Status for `"status"` to move `connecting` → `provisioned` or `failed`
(`error` explains which). A `failed` result keeps every field in place; the
same write-then-commit sequence can be retried without starting over.

Threading note for anyone touching `ble_provisioning.cpp`: characteristic
write callbacks run on the Bluedroid host task, not the Arduino `loop()`
task. They only ever set fields on `ProvisioningState` and flip a
`commit_requested_` flag; the actual `WiFi.begin()` call and the wait for
association happen in `BleProvisioning::poll()`, called from `loop()` —
matching the single-threaded model the rest of this firmware assumes
everywhere else.

## Link security

Every characteristic that writes or reads a credential
(SSID/passphrase/device-name/pairing-token/commit/status) requires an
encrypted link (`ESP_GATT_PERM_*_ENCRYPTED`); a write attempt before pairing
triggers the pairing handshake automatically. Device identity is the one
exception and is deliberately left unencrypted — a central has to be able to
read it to recognise the device *before* pairing, the same reason
`GET /api/device/info` is the one HTTP endpoint that skips auth.

Pairing mode is `ESP_IO_CAP_NONE` ("Just Works"), because that is simply what
this hardware is: no display to show a passkey on, no keypad to enter one.
This gets you encryption against passive eavesdropping, not protection
against an active machine-in-the-middle during the pairing window itself —
claiming MITM protection (`ESP_LE_AUTH_REQ_MITM`) with no way to verify a
passkey out-of-band would be theater, not security. `ESP_LE_AUTH_REQ_SC_ONLY`
(LE Secure Connections, no bonding) is used rather than a bonded mode: each
pairing session (first boot, or after a factory reset) is a fresh BLE
init/deinit cycle with fresh keys, and nothing ever reconnects to a
previous session's bond, so there is nothing for a stored bond to serve.

## Known gaps

- No MITM protection during pairing (see above) — an inherent limit of this
  hardware's I/O capability, not something left undone.
- Nothing here has run on physical hardware. Every claim above about GATT
  behavior is "compiles against the ESP32 Arduino BLE library and matches
  its documented API," not "observed working."
