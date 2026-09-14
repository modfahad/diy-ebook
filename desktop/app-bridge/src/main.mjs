// The bridge process itself: one JSON request in, JSON lines out.
//
//   {"type":"progress", ...}     zero or more, as work happens
//   {"type":"result","value":X}  exactly one on success, then exit 0
//   {"type":"error","code":C,"message":M}   on failure, then exit 1
//
// Two entry points start it: run.mjs loads the converter and device client
// from the checkout's built dist/ folders (development), and standalone.mjs
// hands them in already bundled (the installed app, which has no checkout).

import { BridgeError, runCommand } from './commands.mjs';

/**
 * `fetch` rejecting with a bare "fetch failed" is the single most common
 * failure the Device tab will hit (device asleep, not in transfer mode, wrong
 * address) and the least informative message in the whole stack. Unwrap the
 * cause chain so the UI can show what actually went wrong.
 */
function describe(error) {
  const parts = [];
  let current = error;
  const seen = new Set();
  while (current && !seen.has(current)) {
    seen.add(current);
    const message = current instanceof Error ? current.message : String(current);
    if (message && !parts.includes(message)) parts.push(message);
    current = current instanceof Error ? current.cause : undefined;
  }
  return parts.join(': ');
}

/**
 * A device that is asleep, off its network, or simply not in transfer mode all
 * arrive here as an untyped `fetch` rejection. Naming that condition lets the
 * UI say what to check instead of showing a transport error verbatim; a
 * `DeviceError` already carries its own protocol code and keeps it.
 */
const UNREACHABLE = new Set([
  'ECONNREFUSED',
  'ENOTFOUND',
  'EHOSTUNREACH',
  'ENETUNREACH',
  'ETIMEDOUT',
  'ECONNRESET',
  'UND_ERR_CONNECT_TIMEOUT',
  'UND_ERR_HEADERS_TIMEOUT',
]);

function codeFor(error) {
  if (error?.code) return error.code;
  if (error?.name === 'AbortError') return 'DEVICE_UNREACHABLE'; // our own request timeout
  let current = error;
  const seen = new Set();
  while (current && !seen.has(current)) {
    seen.add(current);
    if (typeof current.code === 'string' && UNREACHABLE.has(current.code)) {
      return 'DEVICE_UNREACHABLE';
    }
    current = current.cause;
  }
  return 'ERROR';
}

async function readRequest() {
  const inline = process.argv[2];
  if (inline && inline !== '-') return JSON.parse(inline);

  const chunks = [];
  for await (const chunk of process.stdin) chunks.push(chunk);
  const text = Buffer.concat(chunks).toString('utf8').trim();
  if (text === '') {
    throw new BridgeError('BAD_REQUEST', 'no request on argv and nothing on stdin');
  }
  return JSON.parse(text);
}

/** Reads one request, runs it with libraries from `load`, and exits. */
export async function startBridge(load) {
  // stdout is a structured channel, not a log. Anything the libraries print --
  // pdfjs's font warnings are the usual source -- goes to stderr, where the
  // Rust side collects it as diagnostics instead of corrupting the stream.
  const write = process.stdout.write.bind(process.stdout);
  console.log = (...args) => console.error(...args);
  console.info = (...args) => console.error(...args);

  const emit = (event) => {
    write(`${JSON.stringify(event)}\n`);
  };

  // The last event, then exit -- but only once stdout has taken all of it.
  // Writes to a pipe are asynchronous, so process.exit() straight after write()
  // cut off anything past the first pipe buffer (8 KB on macOS): a large result,
  // like a PDF read for page rendering, arrived truncated and the app reported
  // "the bridge exited without producing a result".
  const finish = (event, code) => {
    write(`${JSON.stringify(event)}\n`, () => process.exit(code));
  };

  // finish() exits only once stdout has drained, so nothing after it may run:
  // a request that could not be read must not fall through into runCommand.
  let request;
  let requestError = null;
  try {
    request = await readRequest();
  } catch (error) {
    requestError = error;
  }

  if (requestError !== null) {
    finish(
      { type: 'error', code: 'BAD_REQUEST', message: `could not read the request: ${describe(requestError)}` },
      1,
    );
    return;
  }
  try {
    const value = await runCommand(request, {
      load,
      progress: (progress) => emit({ type: 'progress', ...progress }),
    });
    finish({ type: 'result', value: value ?? null }, 0);
  } catch (error) {
    finish({ type: 'error', code: codeFor(error), message: describe(error) }, 1);
  }
}
