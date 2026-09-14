# BLE provisioning

**Status: implemented — Phase 1, Milestone 4. Never run on hardware.**

First-boot provisioning and recovery only. After provisioning, BLE stays down
and everything happens over Wi-Fi (spec §3, development rule 18).

`provisioning_state.cpp` is the **pure, host-tested** state machine: it
accumulates the credentials, decides when a commit is legal, and produces the
status document the desktop reads back. The GATT front end that drives it is
`firmware/drivers/net/ble_provisioning.cpp` (Arduino-dependent, compiled only
for the device), and the UUIDs are in `firmware/include/net/ble_uuids.h`.

Two rules the state machine exists to enforce:

- **Commit, then verify, then persist.** Credentials reach NVS only after the
  Wi-Fi connection they describe has actually worked. Persisting first would
  leave a device that boots, fails to associate, and has no way back short of
  reflashing.
- **The passphrase is never readable.** Not through status, not through the
  serialiser, not through a getter — the only way out is
  `passphraseForRadioUseOnly()`, a deliberately awkward name that shows up in
  review. There is a test asserting the status document never contains it.

BLE and Wi-Fi are never up at once: the stack is deinitialised before a
transfer session brings Wi-Fi up.

Wire spec: [docs/provisioning.md](../../docs/provisioning.md).
