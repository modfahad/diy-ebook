#!/usr/bin/env node
// Makes the desktop app standalone, so it runs on a computer without this
// checkout or a Node install:
//
//   1. bundles the bridge, with the converter and device client inside it,
//      into src-tauri/bridge/bridge.mjs (shipped as an app resource), and
//   2. copies the Node running this script to
//      src-tauri/binaries/node-<Rust target triple>[.exe] (Tauri's sidecar).
//
// Run it after `npm run build` in desktop/converter and desktop/device-client,
// and before `npm run tauri build`. CI runs the same steps on each platform,
// which is how the Windows build gets a Windows node.exe.

import { execFileSync } from 'node:child_process';
import { chmodSync, copyFileSync, mkdirSync, statSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { build } from 'esbuild';

const desktop = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const tauri = join(desktop, 'src-tauri');
const bundle = join(tauri, 'bridge', 'bridge.mjs');

mkdirSync(dirname(bundle), { recursive: true });
await build({
  entryPoints: [join(desktop, 'app-bridge', 'standalone.mjs')],
  bundle: true,
  platform: 'node',
  format: 'esm',
  target: 'node20',
  outfile: bundle,
  // pdf.js only reaches for a canvas when rendering, which the bridge never
  // does (the webview renders PDF pages); the text path works without it.
  external: ['@napi-rs/canvas', 'canvas'],
  // Bundled CommonJS dependencies (yauzl) call require(), which ESM lacks.
  banner: {
    js: "import { createRequire as __bridgeRequire } from 'node:module'; const require = __bridgeRequire(import.meta.url);",
  },
  logLevel: 'warning',
});

const rustc = execFileSync('rustc', ['-vV'], { encoding: 'utf8' });
const triple = /^host: (.+)$/m.exec(rustc)?.[1]?.trim();
if (!triple) throw new Error('could not read the host target triple from `rustc -vV`');
const extension = process.platform === 'win32' ? '.exe' : '';
const sidecar = join(tauri, 'binaries', `node-${triple}${extension}`);
mkdirSync(dirname(sidecar), { recursive: true });
copyFileSync(process.execPath, sidecar);
if (process.platform !== 'win32') chmodSync(sidecar, 0o755);

const megabytes = (path) => (statSync(path).size / 1048576).toFixed(1);
console.log(`bridge  ${bundle} (${megabytes(bundle)} MB)`);
console.log(`node    ${sidecar} (${process.version}, ${megabytes(sidecar)} MB)`);
