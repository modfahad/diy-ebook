// Driving the two TypeScript libraries from the app.
//
// `desktop/converter` and `desktop/device-client` are Node libraries and stay
// that way (see desktop/app-bridge/src/commands.mjs for why porting them to
// the webview is rework, not assembly). So every operation the UI needs that
// is not plain filesystem work runs as one short-lived `node` process:
//
//   Rust  --(request JSON on stdin)-->  desktop/app-bridge/run.mjs
//         <--(JSON lines on stdout)---
//
// One process per operation. No sidecar to supervise, no port to allocate, no
// state to leak between operations, and a wedged conversion cannot poison the
// next one. The cost is a ~100ms Node start per call, which is nothing next to
// parsing a PDF or pushing megabytes over the device's Wi-Fi.
//
// Progress lines are forwarded to the webview as Tauri events rather than
// buffered, because an upload is minutes long and a progress bar that only
// moves at the end is not a progress bar.

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter};

/// What a failed bridge call looks like on the JavaScript side.
///
/// `code` is the machine-readable half -- the bridge's own vocabulary
/// (`LIBRARY_NOT_BUILT`, `BAD_REQUEST`, `VALIDATION_FAILED`) plus the device
/// protocol's codes (`UNAUTHORIZED`, `NOT_IN_TRANSFER_MODE`, ...) passed
/// straight through from `DeviceError`. The UI switches on it; `message` is
/// for the human and is never parsed.
#[derive(Debug, Serialize)]
pub struct BridgeFailure {
    pub code: String,
    pub message: String,
    /// Anything the child wrote to stderr. pdfjs's font warnings land here,
    /// and so does a Node crash that never produced a JSON line at all.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub stderr: Option<String>,
}

impl BridgeFailure {
    fn new(code: &str, message: impl Into<String>) -> Self {
        Self { code: code.to_string(), message: message.into(), stderr: None }
    }
}

/// One line of the child's stdout.
#[derive(Debug, PartialEq)]
enum BridgeEvent {
    Progress(Value),
    Result(Value),
    Error { code: String, message: String },
    /// A line that is not JSON, or JSON without a known `type`. Kept rather
    /// than dropped: a library printing to stdout is a bug worth surfacing,
    /// and silently ignoring it would make it invisible.
    Unparsed(String),
}

fn parse_line(line: &str) -> BridgeEvent {
    let value: Value = match serde_json::from_str(line) {
        Ok(value) => value,
        Err(_) => return BridgeEvent::Unparsed(line.to_string()),
    };
    match value.get("type").and_then(Value::as_str) {
        Some("progress") => BridgeEvent::Progress(value),
        Some("result") => BridgeEvent::Result(value.get("value").cloned().unwrap_or(Value::Null)),
        Some("error") => BridgeEvent::Error {
            code: value
                .get("code")
                .and_then(Value::as_str)
                .unwrap_or("ERROR")
                .to_string(),
            message: value
                .get("message")
                .and_then(Value::as_str)
                .unwrap_or("the bridge failed without a message")
                .to_string(),
        },
        _ => BridgeEvent::Unparsed(line.to_string()),
    }
}

/// Reads the child's stdout to the end, forwarding progress as it arrives.
///
/// Split out from the process plumbing so the protocol itself is testable
/// without spawning anything.
fn collect_events<R: BufRead>(
    reader: R,
    on_progress: &mut dyn FnMut(Value),
) -> Result<Value, BridgeFailure> {
    let mut outcome: Option<Result<Value, BridgeFailure>> = None;
    let mut noise: Vec<String> = Vec::new();

    for line in reader.lines() {
        let line = line.map_err(|e| BridgeFailure::new("IO_ERROR", format!("reading the bridge's output failed: {e}")))?;
        if line.trim().is_empty() {
            continue;
        }
        match parse_line(&line) {
            BridgeEvent::Progress(event) => on_progress(event),
            // First outcome wins. A well-behaved bridge emits exactly one, but
            // a later line must never overwrite a failure with a success.
            BridgeEvent::Result(value) => {
                if outcome.is_none() {
                    outcome = Some(Ok(value));
                }
            }
            BridgeEvent::Error { code, message } => {
                if outcome.is_none() {
                    outcome = Some(Err(BridgeFailure::new(&code, message)));
                }
            }
            BridgeEvent::Unparsed(text) => noise.push(text),
        }
    }

    match outcome {
        Some(result) => result,
        None => Err(BridgeFailure {
            code: "NO_RESULT".to_string(),
            message: "the bridge exited without producing a result".to_string(),
            stderr: if noise.is_empty() { None } else { Some(noise.join("\n")) },
        }),
    }
}

/// Where the repository is, at runtime.
///
/// The bridge is a script in the checkout, not a bundled resource, so the app
/// has to find the checkout. This is a development-time arrangement and is
/// documented as one (docs/pending.md): shipping an installer means bundling
/// Node and the built libraries as Tauri resources, which is separate work.
///
/// Order: an explicit override first, then upward from the executable (the
/// dev binary lives in `desktop/src-tauri/target/debug`), then upward from the
/// working directory (`npm run tauri dev` starts in `desktop/`).
pub fn find_repo_root() -> Result<PathBuf, BridgeFailure> {
    if let Ok(explicit) = std::env::var("QURAN_DEVICE_REPO") {
        let path = PathBuf::from(explicit);
        if is_repo_root(&path) {
            return Ok(path);
        }
        return Err(BridgeFailure::new(
            "REPO_NOT_FOUND",
            format!(
                "QURAN_DEVICE_REPO points at {}, which has no desktop/app-bridge/run.mjs",
                path.display()
            ),
        ));
    }

    // The checkout this binary was built from comes last: an installed copy
    // (/Applications/Quran Device.app) is not under the checkout, and a Finder
    // launch's working directory is "/", so only this finds it there.
    let starts = [
        std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)),
        std::env::current_dir().ok(),
        Some(PathBuf::from(env!("CARGO_MANIFEST_DIR"))),
    ];
    for start in starts.into_iter().flatten() {
        if let Some(root) = walk_up(&start) {
            return Ok(root);
        }
    }

    Err(BridgeFailure::new(
        "REPO_NOT_FOUND",
        "could not find the project checkout (looked for desktop/app-bridge/run.mjs above the \
         executable and the working directory). Set QURAN_DEVICE_REPO to the checkout path.",
    ))
}

fn is_repo_root(path: &Path) -> bool {
    path.join("desktop").join("app-bridge").join("run.mjs").is_file()
}

fn walk_up(start: &Path) -> Option<PathBuf> {
    let mut current = Some(start);
    while let Some(directory) = current {
        if is_repo_root(directory) {
            return Some(directory.to_path_buf());
        }
        current = directory.parent();
    }
    None
}

fn bridge_script(root: &Path) -> PathBuf {
    root.join("desktop").join("app-bridge").join("run.mjs")
}

/// How one bridge run is started: the Node to run, the script, and where.
struct BridgeLaunch {
    node: PathBuf,
    script: PathBuf,
    cwd: PathBuf,
    /// The app's own bundled bridge rather than a checkout's run.mjs.
    bundled: bool,
    repo_root: Option<PathBuf>,
}

/// The bridge an installed app carries (desktop/scripts/prepare-standalone.mjs):
/// Node as a Tauri sidecar beside the executable, and bridge.mjs among the
/// app's resources -- Contents/Resources on macOS, beside the executable on
/// Windows.
fn bundled_launch() -> Option<BridgeLaunch> {
    let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
    let node = exe_dir.join(if cfg!(windows) { "node.exe" } else { "node" });
    if !node.is_file() {
        return None;
    }
    let script = [
        exe_dir.join("..").join("Resources").join("bridge").join("bridge.mjs"),
        exe_dir.join("bridge").join("bridge.mjs"),
    ]
    .into_iter()
    .find(|candidate| candidate.is_file())?;
    let cwd = script.parent()?.to_path_buf();
    Some(BridgeLaunch { node, script, cwd, bundled: true, repo_root: None })
}

fn checkout_launch() -> Result<BridgeLaunch, BridgeFailure> {
    let root = find_repo_root()?;
    Ok(BridgeLaunch {
        node: PathBuf::from(node_command()),
        script: bridge_script(&root),
        cwd: root.clone(),
        bundled: false,
        repo_root: Some(root),
    })
}

/// Development builds (and QURAN_DEVICE_REPO) prefer the checkout, so edits
/// to the bridge take effect without re-bundling; a release build prefers the
/// bundle it shipped with. Either falls back to the other.
fn locate_bridge() -> Result<BridgeLaunch, BridgeFailure> {
    let prefer_checkout = cfg!(debug_assertions) || std::env::var("QURAN_DEVICE_REPO").is_ok();
    if prefer_checkout {
        return checkout_launch().or_else(|failure| bundled_launch().ok_or(failure));
    }
    match bundled_launch() {
        Some(launch) => Ok(launch),
        None => checkout_launch(),
    }
}

/// Runs one request to completion.
///
/// `on_progress` is called on this thread as each progress line arrives.
pub fn call(request: &Value, on_progress: &mut dyn FnMut(Value)) -> Result<Value, BridgeFailure> {
    let launch = locate_bridge()?;

    let mut child = Command::new(&launch.node)
        .arg(&launch.script)
        .arg("-") // read the request from stdin: no argv quoting to get wrong
        .current_dir(&launch.cwd)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| {
            if e.kind() == std::io::ErrorKind::NotFound {
                BridgeFailure::new(
                    "NODE_NOT_FOUND",
                    "could not run `node`. The desktop app drives the converter and device \
                     client through Node, so Node 20 or newer must be on PATH.",
                )
            } else {
                BridgeFailure::new("SPAWN_FAILED", format!("could not start the bridge: {e}"))
            }
        })?;

    // stderr is drained on its own thread. pdfjs is chatty, and a full stderr
    // pipe would block the child mid-conversion while we sit reading stdout.
    let mut stderr_reader = child.stderr.take().map(|stderr| {
        std::thread::spawn(move || {
            let mut text = String::new();
            let mut reader = BufReader::new(stderr);
            let mut line = String::new();
            while reader.read_line(&mut line).unwrap_or(0) > 0 {
                text.push_str(&line);
                line.clear();
            }
            text
        })
    });

    if let Some(mut stdin) = child.stdin.take() {
        let body = serde_json::to_vec(request)
            .map_err(|e| BridgeFailure::new("BAD_REQUEST", format!("could not encode the request: {e}")))?;
        // A closed stdin means the child died early; the real error is in the
        // output we are about to read, so this write's failure is not fatal.
        let _ = stdin.write_all(&body);
        drop(stdin);
    }

    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| BridgeFailure::new("SPAWN_FAILED", "the bridge produced no stdout"))?;
    let outcome = collect_events(BufReader::new(stdout), on_progress);

    let _ = child.wait();
    let stderr = stderr_reader
        .take()
        .and_then(|handle| handle.join().ok())
        .filter(|text| !text.trim().is_empty());

    match outcome {
        Ok(value) => Ok(value),
        Err(mut failure) => {
            if failure.stderr.is_none() {
                failure.stderr = stderr;
            }
            Err(failure)
        }
    }
}

fn node_command() -> String {
    if let Ok(explicit) = std::env::var("QURAN_DEVICE_NODE") {
        return explicit;
    }
    let on_path = std::env::var("PATH")
        .map(|path| path.split(':').any(|dir| Path::new(dir).join("node").is_file()))
        .unwrap_or(false);
    if on_path {
        return "node".to_string();
    }
    // Opened from Finder, the app gets launchd's minimal PATH, which leaves out
    // wherever a login shell would have found Node. Try the usual places.
    let home = std::env::var("HOME").unwrap_or_default();
    [
        format!("{home}/.local/node/bin/node"),
        "/opt/homebrew/bin/node".to_string(),
        "/usr/local/bin/node".to_string(),
    ]
    .into_iter()
    .find(|candidate| Path::new(candidate).is_file())
    .unwrap_or_else(|| "node".to_string())
}

// --- Tauri commands ----------------------------------------------------------

/// The single entry point the UI calls.
///
/// One generic command rather than one per operation: the request shapes are
/// the bridge's (`desktop/app-bridge/src/commands.mjs`) and the response
/// shapes are the libraries' own, so re-declaring both in Rust would be a
/// third copy of types that already exist in TypeScript on both ends of this
/// pipe -- and a copy that could drift. Typing lives in `src/bridge.ts`, next
/// to the code that consumes it.
///
/// `progress_event` names the Tauri event progress lines are emitted on. The
/// caller supplies a unique name per operation so two concurrent calls (an
/// upload and a conversion, say) do not land in each other's handlers.
#[tauri::command(async)]
pub fn bridge_call(
    app: AppHandle,
    request: Value,
    progress_event: Option<String>,
) -> Result<Value, BridgeFailure> {
    let mut forward = |event: Value| {
        if let Some(name) = progress_event.as_deref() {
            let _ = app.emit(name, event);
        }
    };
    call(&request, &mut forward)
}

/// What the app found, before any operation is attempted.
///
/// The Dashboard shows this so "Node is not installed" and "the converter is
/// not built" are explained up front rather than surfacing as a mysterious
/// failure on the user's first conversion.
#[derive(Serialize)]
pub struct BridgeEnvironment {
    #[serde(rename = "repoRoot")]
    repo_root: Option<String>,
    #[serde(rename = "bridgeScript")]
    bridge_script: Option<String>,
    /// Whether the checkout was located. Deliberately *not* "ready to run":
    /// whether each library is built is a separate question, answered by the
    /// `ping` command, and the Dashboard reports the two separately.
    found: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    problem: Option<String>,
}

#[tauri::command(async)]
pub fn bridge_environment() -> BridgeEnvironment {
    match locate_bridge() {
        Ok(launch) => BridgeEnvironment {
            bridge_script: Some(launch.script.to_string_lossy().to_string()),
            // An installed app has no checkout; say where its bridge came from.
            repo_root: Some(match (&launch.repo_root, launch.bundled) {
                (Some(root), _) => root.to_string_lossy().to_string(),
                (None, _) => "bundled with the app".to_string(),
            }),
            found: true,
            problem: None,
        },
        Err(failure) => BridgeEnvironment {
            repo_root: None,
            bridge_script: None,
            found: false,
            problem: Some(failure.message),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    fn drain(stdout: &str) -> (Result<Value, BridgeFailure>, Vec<Value>) {
        let mut progress = Vec::new();
        let result = collect_events(Cursor::new(stdout), &mut |event| progress.push(event));
        (result, progress)
    }

    #[test]
    fn progress_lines_are_forwarded_and_the_result_is_returned() {
        let (result, progress) = drain(
            "{\"type\":\"progress\",\"stage\":\"parsing\"}\n\
             {\"type\":\"progress\",\"stage\":\"validating\"}\n\
             {\"type\":\"result\",\"value\":{\"written\":true}}\n",
        );
        assert_eq!(progress.len(), 2);
        assert_eq!(progress[0]["stage"], "parsing");
        assert_eq!(result.unwrap()["written"], true);
    }

    #[test]
    fn an_error_line_becomes_a_coded_failure() {
        let (result, _) = drain("{\"type\":\"error\",\"code\":\"UNAUTHORIZED\",\"message\":\"no token\"}\n");
        let failure = result.unwrap_err();
        assert_eq!(failure.code, "UNAUTHORIZED");
        assert_eq!(failure.message, "no token");
    }

    #[test]
    fn a_later_result_cannot_overwrite_an_earlier_failure() {
        let (result, _) = drain(
            "{\"type\":\"error\",\"code\":\"NO_SPACE\",\"message\":\"full\"}\n\
             {\"type\":\"result\",\"value\":1}\n",
        );
        assert_eq!(result.unwrap_err().code, "NO_SPACE");
    }

    #[test]
    fn a_child_that_says_nothing_is_a_failure_carrying_whatever_it_did_print() {
        let (result, progress) = drain("Segmentation fault\n");
        assert!(progress.is_empty());
        let failure = result.unwrap_err();
        assert_eq!(failure.code, "NO_RESULT");
        assert_eq!(failure.stderr.as_deref(), Some("Segmentation fault"));
    }

    #[test]
    fn blank_lines_are_ignored() {
        let (result, _) = drain("\n\n{\"type\":\"result\",\"value\":null}\n\n");
        assert_eq!(result.unwrap(), Value::Null);
    }

    #[test]
    fn the_repo_root_is_found_from_the_test_binarys_own_location() {
        // Proves the walk-up strategy the shipped app uses, from a real path:
        // target/debug/deps/... is four levels below the checkout.
        let root = find_repo_root().expect("the checkout should be found from the test binary");
        assert!(root.join("desktop").join("app-bridge").join("run.mjs").is_file());
    }

    // The one test that actually spawns Node. It proves the whole pipe --
    // spawn, stdin, JSON lines, exit -- rather than the parser alone. Skipped
    // rather than failed when Node is absent, following the same convention
    // firmware/scripts/run_host_tests.py uses for a missing host compiler.
    #[test]
    fn a_real_bridge_process_answers_a_ping() {
        let probe = Command::new(node_command()).arg("--version").output();
        if probe.is_err() {
            eprintln!("skipped: `node` is not on PATH");
            return;
        }

        let request = serde_json::json!({ "command": "ping" });
        let mut ignored = |_: Value| {};
        let value = call(&request, &mut ignored).expect("ping should succeed");
        assert_eq!(value["bridgeVersion"], 1);
        assert_eq!(value["libraries"]["converter"]["built"], true);
        assert_eq!(value["libraries"]["device-client"]["built"], true);
    }

    #[test]
    fn an_unknown_command_comes_back_with_the_bridges_own_code() {
        if Command::new(node_command()).arg("--version").output().is_err() {
            eprintln!("skipped: `node` is not on PATH");
            return;
        }
        let mut ignored = |_: Value| {};
        let failure = call(&serde_json::json!({ "command": "nope" }), &mut ignored).unwrap_err();
        assert_eq!(failure.code, "BAD_REQUEST");
    }
}
