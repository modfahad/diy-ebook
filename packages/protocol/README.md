# @quran-device/protocol

**Status: implemented — Phase 1, Milestone 4.**

The single source of truth for the desktop ↔ device wire protocol: paths,
header names, status codes, the error-code vocabulary and every request and
response shape. The wire spec itself is [docs/protocol.md](../../docs/protocol.md).

The firmware mirrors this file in C++ (`firmware/include/net/protocol.h`).
The two mirrors are kept honest by a generated fixture, the same trick the QPK
golden fixture plays for the package format:

```bash
npm run fixture --prefix packages/protocol
```

writes both

| Path | Consumer |
|---|---|
| `fixtures/protocol-constants.json` | this package's own tests |
| `firmware/test/test_net/transcript_fixture.h` | the firmware `test_net` suite |

and the firmware test compiles that header and asserts `protocol.h` against it.
Regenerate and commit both after any change here; a diff means the C++ and
TypeScript sides have drifted.

**What the fixture does not cover:** JSON response *shapes* (`DeviceInfo`,
`DeviceStatus`, …). Checking those needs the firmware's response building
pulled out of `http_server.cpp` into pure functions first, which has not been
done. See [development.md §10](../../docs/development.md).

## Consumers

- [`desktop/device-client`](../../desktop/device-client/README.md) — the
  desktop implementation of this protocol.
- [`tools/device-cli`](../../tools/device-cli/README.md) — a CLI over it.
- `firmware/drivers/net/http_server.cpp` — the device side.

## Design constraints this encodes

- **Uploads are resumable** (spec §17): a Quran package will not always
  transfer in one request, and the resume point is the device's disk.
- **Installation is atomic** (§17): a partial upload never becomes an active
  library item.
- **No unauthenticated destructive endpoints** (§19). `GET /api/device/info`
  is the only exempt route — discovery has to work before pairing — and it
  exposes nothing sensitive. The Wi-Fi password is not readable through any
  route.
- **Wi-Fi is on-demand** (§6, §7). The device serves this API only while the
  user has put it in transfer mode.
