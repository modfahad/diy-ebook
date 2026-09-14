// Entry point of the bridge the installed app carries.
//
// desktop/scripts/prepare-standalone.mjs bundles this into one file with
// esbuild. The converter and device client are imported here, statically,
// instead of from the checkout's dist/ folders as run.mjs does -- so the
// installed app needs Node (shipped beside it) and nothing else.

import * as pdfjsWorker from '../converter/node_modules/pdfjs-dist/legacy/build/pdf.worker.mjs';
import * as converter from '../converter/dist/src/index.js';
import * as deviceClient from '../device-client/dist/src/index.js';
import { BridgeError } from './src/commands.mjs';
import { startBridge } from './src/main.mjs';

// pdf.js loads its worker from a file beside pdf.mjs, which a one-file bundle
// does not have. Given the worker module here, it runs the worker on the main
// thread instead -- fine for a process that exits after one request.
globalThis.pdfjsWorker = pdfjsWorker;

const libraries = { converter, 'device-client': deviceClient };

await startBridge(async (name) => {
  const library = libraries[name];
  if (!library) throw new BridgeError('BAD_REQUEST', `no such library "${name}"`);
  return library;
});
