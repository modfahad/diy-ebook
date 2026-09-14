#!/usr/bin/env node
// One-shot bridge process: JSON in, JSON lines out.
//
//   echo '{"command":"ping"}' | node desktop/app-bridge/run.mjs
//   node desktop/app-bridge/run.mjs '{"command":"device.info","host":"10.0.0.5"}'
//
// The Tauri app spawns this once per operation (src-tauri/src/bridge.rs) and
// reads stdout a line at a time; the contract is in src/main.mjs. A
// line-per-event rather than one JSON blob at the end is what makes upload
// progress possible without a long-lived server, a port, or a sidecar
// lifecycle to supervise. One process per operation also means a wedged
// conversion cannot poison the next one.
//
// This entry loads the converter and device client from the checkout's
// built dist/ folders. The installed app runs standalone.mjs instead, bundled
// with both (desktop/scripts/prepare-standalone.mjs).

import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createLoader } from './src/commands.mjs';
import { startBridge } from './src/main.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(join(here, '..', '..'));

await startBridge(createLoader(repoRoot));
