# Wi-Fi transport

**Status: implemented — Phase 1, Milestone 4. Never run on hardware.**

The parts here are the **pure, host-tested** half of the transport. The
Arduino-dependent glue lives in `firmware/drivers/net/` so that this directory
can be compiled into the host test build; `run_host_tests.py` globs
`wifi/*.cpp` non-recursively for exactly that reason.

| File | Role |
|---|---|
| `protocol.cpp` | the C++ mirror of `packages/protocol` — paths, codes, type names |
| `json.cpp` | a flat-object JSON reader/writer, no allocation |
| `upload_manager.cpp` | resumable upload and atomic install |

## The rule `upload_manager.cpp` exists to enforce

**Every byte of session state lives on disk, never in RAM.**

The device can reset mid-transfer. If the received-byte count lived in a
session struct, a reset would silently restart from zero — or worse, resume at
a stale offset and corrupt the `.part`. So the session id *is* the content id,
the resume point is the `.part` file's size read fresh each time, and what was
declared lives in a `.meta` sidecar. A reset is therefore transparent: the
desktop re-issues "begin", gets the true byte count back, and carries on.

In-progress transfers live under `/DEVICE/uploads`, never in `/LIBRARY`, so
nothing scanning the library tree can see a partial package.

Tested in `firmware/test/test_net/`, including power loss at every write of a
full upload-and-install.

Wire spec: [docs/protocol.md](../../docs/protocol.md).
