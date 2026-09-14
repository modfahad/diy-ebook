# Tauri Rust backend

**Status: implemented — Milestone 5.**

Two jobs, and a deliberate line between them.

| Module | What it does |
|---|---|
| `library.rs` | scans a directory for `.qpk` files and reads each one's header and METADATA — an independent Rust reader of the same QPK1 wire format the TypeScript and C++ readers implement, written against `docs/qpk-format.md` rather than against either of them |
| `bridge.rs` | runs `desktop/app-bridge/run.mjs` as a one-shot subprocess and streams its progress lines to the webview as Tauri events |

Three commands: `scan_library`, `bridge_call`, `bridge_environment`.

`bridge_call` is one generic entry point rather than one command per
operation. The request shapes belong to the bridge and the response shapes to
the TypeScript libraries, so re-declaring both in Rust would be a third copy of
types that already exist on both ends of the pipe — and a copy that could
drift. The typing lives in `src/bridge.ts`, next to the code that consumes it.

No new crates were added for this: no HTTP client, no dialog plugin. The device
protocol is spoken by `desktop/device-client`, over the bridge.

## Tests

```bash
cargo test --lib --manifest-path desktop/src-tauri/Cargo.toml
```

13 tests. `library.rs` is checked against real packages built by the converter
(`../converter/examples/surahs`), not synthetic fixtures. `bridge.rs` covers
the JSON-line protocol on canned output, the checkout walk-up, and — when Node
is on PATH — a real spawned round trip through `run.mjs`, skipped with a note
otherwise, the same convention `firmware/scripts/run_host_tests.py` uses for a
missing host compiler.

`--lib` rather than plain `cargo test`: the `main.rs` test harness binary
contains no tests, and on this machine Windows Application Control
intermittently refuses to execute a freshly built test binary
(`os error 4551`) until it has finished evaluating it — re-running the command
succeeds. Skipping the empty harness avoids the noise; if the `--lib` binary
itself is blocked on the first run, run it again.
