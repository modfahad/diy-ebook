// Every operation the desktop UI can ask for, as one dispatch table.
//
// This file holds no I/O policy of its own beyond reading and writing the
// files a request names: the modules it drives (`@quran-device/converter`,
// `@quran-device/device-client`) and the sink progress goes to are injected,
// so the whole table is exercisable by `node --test` without a Tauri window,
// a device, or a subprocess.
//
// Why a Node process at all, when the UI is a webview that could in principle
// run TypeScript itself: the converter is not browser-portable. `yauzl` (EPUB)
// is Node-only, `pdfjs-dist` needs its legacy build, and content ids come from
// a synchronous `node:crypto` hash whose bytes the device format depends on --
// swapping it for WebCrypto would change a hash the firmware and the golden
// fixture both pin. Porting that is rework; the pending doc's own framing for
// Milestone 5's UI is "assembly, not rework". So Node stays, and since Node is
// already in the process tree for conversion, the device client rides the same
// mechanism rather than earning a second one (a Rust HTTP proxy) purely to
// dodge the webview's CORS rules.

import { readFile, writeFile } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';

export class BridgeError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'BridgeError';
    this.code = code;
  }
}

/**
 * Default module loader: imports the two libraries from their built `dist/`
 * output. They are built, not bundled, so a stale or missing build is a
 * normal condition the UI has to be able to explain -- hence a named error
 * code rather than a bare import failure.
 */
export function createLoader(repoRoot) {
  const entries = {
    converter: join(repoRoot, 'desktop', 'converter', 'dist', 'src', 'index.js'),
    'device-client': join(repoRoot, 'desktop', 'device-client', 'dist', 'src', 'index.js'),
  };
  const cache = new Map();

  return async function load(name) {
    if (cache.has(name)) return cache.get(name);
    const entry = entries[name];
    if (!entry) throw new BridgeError('BAD_REQUEST', `no such library "${name}"`);
    let module;
    try {
      module = await import(`file://${entry.replace(/\\/gu, '/')}`);
    } catch (error) {
      throw new BridgeError(
        'LIBRARY_NOT_BUILT',
        `@quran-device/${name} is not built. Run:\n` +
          `  npm install --prefix desktop/${name}\n` +
          `  npm run build --prefix desktop/${name}\n` +
          `\n(${error.message})`,
      );
    }
    cache.set(name, module);
    return module;
  };
}

// --- conversion --------------------------------------------------------------

function conversionOptions(request) {
  // Only keys the caller actually set are forwarded: the converter's own
  // defaults (includeWordLayout: true, sniffed input kind, title from the
  // document) are better than anything guessed here, and
  // exactOptionalPropertyTypes makes an explicit `undefined` different from
  // an absent key on the TypeScript side.
  const options = {};
  for (const key of [
    'title',
    'author',
    'language',
    'publisher',
    'source',
    'inputKind',
    'contentType',
  ]) {
    const value = request[key];
    if (typeof value === 'string' && value.trim() !== '') options[key] = value.trim();
  }
  if (typeof request.contentVersion === 'number') options.contentVersion = request.contentVersion;
  if (typeof request.includeWordLayout === 'boolean') {
    options.includeWordLayout = request.includeWordLayout;
  }
  if (typeof request.singleChapter === 'boolean') options.singleChapter = request.singleChapter;
  if (typeof request.rightToLeft === 'boolean') options.rightToLeft = request.rightToLeft;
  if (typeof request.cover === 'string' && request.cover !== '') {
    // A finished COVER section payload, base64. The app's webview builds it,
    // because that is where a picture can be decoded.
    options.cover = new Uint8Array(Buffer.from(request.cover, 'base64'));
  }
  if (Array.isArray(request.pageImages) && request.pageImages.length > 0) {
    // Page pictures, each a PackBits-compressed bitmap in base64, rendered by
    // the app's webview (qpk-format.md 9d).
    options.pageImages = request.pageImages.map(
      (page) => new Uint8Array(Buffer.from(String(page), 'base64')),
    );
    if (typeof request.pageImageWidth === 'number') options.pageImageWidth = request.pageImageWidth;
    if (typeof request.pageImageHeight === 'number') {
      options.pageImageHeight = request.pageImageHeight;
    }
  }
  return options;
}

/** The input file's bytes, base64 -- for the webview to render a PDF's pages. */
async function readInputCommand(request, ctx) {
  const input = requireString(request, 'input');
  const data = await ctx.readFile(input);
  return { input: resolve(input), bytes: data.length, data: Buffer.from(data).toString('base64') };
}

/**
 * The cover picture an EPUB declares, as base64 plus its media type, for the
 * app to crop and turn into a COVER section. null for anything else, or an
 * EPUB without one -- the app then offers "choose a picture" instead.
 */
async function coverSourceCommand(request, ctx) {
  const input = requireString(request, 'input');
  const converter = await ctx.load('converter');
  const data = new Uint8Array(await ctx.readFile(input));
  if (converter.detectInputKind(data, basename(input)) !== 'epub') return null;
  const cover = await converter.extractEpubCover(data);
  if (!cover) return null;
  return { mediaType: cover.mediaType, data: Buffer.from(cover.bytes).toString('base64') };
}

/**
 * A picked document's title, author and language from its own metadata (the
 * title falling back to the file name), for the app to show as editable
 * details before converting. Metadata only: no page is read.
 */
async function documentDetailsCommand(request, ctx) {
  const input = requireString(request, 'input');
  const converter = await ctx.load('converter');
  const data = new Uint8Array(await ctx.readFile(input));
  return converter.readDocumentDetails(data, basename(input));
}

function defaultOutputPath(input) {
  const directory = dirname(input);
  const name = basename(input).replace(/\.[^.]+$/u, '');
  return join(directory, `${name}.qpk`);
}

/**
 * Convert, validate, preview -- and only then write.
 *
 * That order is the spec's ("validate, preview, then install") and it is the
 * reason `dryRun` is worth having in a GUI at all: the UI runs a dry run to
 * populate the report and preview, the user looks, and the same call runs
 * again with `dryRun: false` to commit. Nothing reaches disk that failed
 * validation, exactly as `tools/pdf-converter/convert.mjs` enforces at the CLI.
 */
async function convertCommand(request, ctx) {
  const input = requireString(request, 'input');
  const converter = await ctx.load('converter');

  const data = new Uint8Array(await ctx.readFile(input));
  ctx.progress({ stage: 'parsing', file: basename(input), bytes: data.length });

  const options = conversionOptions(request);
  options.filename = basename(input);

  const result = await converter.convertAndValidate(data, options);
  ctx.progress({ stage: 'validating', pageCount: result.report.pageCount });

  let preview = null;
  if (result.validation.ok && result.report.pageCount > 0) {
    const page = clampPage(request.previewPage, result.report.pageCount);
    try {
      preview = converter.previewPage(result.bytes, page);
    } catch (error) {
      // A preview that cannot be built is worth reporting, not worth failing
      // a conversion over: the package itself already validated.
      preview = { error: String(error instanceof Error ? error.message : error) };
    }
  }

  const dryRun = request.dryRun === true;
  const output = resolve(
    typeof request.output === 'string' && request.output.trim() !== ''
      ? request.output.trim()
      : defaultOutputPath(input),
  );

  let written = false;
  if (!dryRun) {
    if (!result.validation.ok) {
      throw new BridgeError(
        'VALIDATION_FAILED',
        `the package did not validate, so nothing was written:\n  ${result.validation.errors.join('\n  ')}`,
      );
    }
    ctx.progress({ stage: 'writing', output });
    await ctx.writeFile(output, result.bytes);
    written = true;
  }

  return {
    input: resolve(input),
    output,
    written,
    dryRun,
    packageBytes: result.bytes.length,
    report: result.report,
    validation: result.validation,
    preview,
  };
}

function clampPage(value, pageCount) {
  const page = typeof value === 'number' && Number.isFinite(value) ? Math.trunc(value) : 1;
  return Math.min(Math.max(page, 1), pageCount);
}

/** Validate a package that already exists on disk, without rebuilding it. */
async function inspectCommand(request, ctx) {
  const file = requireString(request, 'file');
  const converter = await ctx.load('converter');
  const bytes = new Uint8Array(await ctx.readFile(file));
  const validation = converter.validatePackage(bytes);
  return { file: resolve(file), packageBytes: bytes.length, validation };
}

/** Read one page back out of a finished package -- the device's view of it. */
async function previewCommand(request, ctx) {
  const file = requireString(request, 'file');
  const converter = await ctx.load('converter');
  const bytes = new Uint8Array(await ctx.readFile(file));
  const validation = converter.validatePackage(bytes);
  if (!validation.ok) {
    throw new BridgeError(
      'VALIDATION_FAILED',
      `cannot preview a package that does not parse:\n  ${validation.errors.join('\n  ')}`,
    );
  }
  const pageCount = validation.summary?.pageCount ?? 0;
  if (pageCount === 0) throw new BridgeError('NO_PAGES', 'this package has no PAGE_INDEX records');
  return converter.previewPage(bytes, clampPage(request.page, pageCount));
}

// --- device ------------------------------------------------------------------

async function deviceClient(request, ctx) {
  const { DeviceClient } = await ctx.load('device-client');
  const host = requireString(request, 'host');
  const port = typeof request.port === 'number' && request.port > 0 ? request.port : 8080;
  const token = typeof request.token === 'string' ? request.token.trim() : '';
  const timeoutMs =
    typeof request.timeoutMs === 'number' && request.timeoutMs > 0 ? request.timeoutMs : 20000;
  return new DeviceClient({ host, port, token, timeoutMs });
}

async function uploadCommand(request, ctx) {
  const file = requireString(request, 'file');
  const client = await deviceClient(request, ctx);
  const bytes = new Uint8Array(await ctx.readFile(file));

  // Progress is the whole reason this bridge streams instead of returning
  // once: a Quran package over the device's Wi-Fi is minutes of transfer, and
  // resyncs (the device rewinding the client) are worth showing rather than
  // hiding -- see DeviceClient.uploadPackage's own comment.
  const result = await client.uploadPackage(bytes, {
    onProgress: ({ sentBytes, totalBytes, resyncs }) => {
      ctx.progress({ stage: 'upload', sentBytes, totalBytes, resyncs });
    },
  });
  return { file: resolve(file), packageBytes: bytes.length, ...result };
}

// --- table -------------------------------------------------------------------

const COMMANDS = {
  async ping(_request, ctx) {
    // Which halves are actually usable right now. The UI shows this instead
    // of letting the first real command fail with an import error.
    const libraries = {};
    for (const name of ['converter', 'device-client']) {
      try {
        await ctx.load(name);
        libraries[name] = { built: true, error: null };
      } catch (error) {
        libraries[name] = { built: false, error: error.message };
      }
    }
    return { bridgeVersion: 1, node: process.version, libraries };
  },

  convert: convertCommand,
  inspect: inspectCommand,
  preview: previewCommand,
  coverSource: coverSourceCommand,
  readInput: readInputCommand,
  documentDetails: documentDetailsCommand,

  async 'device.info'(request, ctx) {
    return (await deviceClient(request, ctx)).getInfo();
  },
  async 'device.status'(request, ctx) {
    return (await deviceClient(request, ctx)).getStatus();
  },
  async 'device.list'(request, ctx) {
    return (await deviceClient(request, ctx)).listLibrary();
  },
  'device.upload': uploadCommand,
  async 'device.delete'(request, ctx) {
    const contentId = requireString(request, 'contentId');
    await (await deviceClient(request, ctx)).deleteItem(contentId);
    return { deleted: contentId };
  },
  async 'device.abort'(request, ctx) {
    // The session id IS the content id -- see tools/device-cli/device.mjs.
    const contentId = requireString(request, 'contentId');
    await (await deviceClient(request, ctx)).abortUpload(contentId);
    return { aborted: contentId };
  },

  // --- home-screen photos and time zone ----------------------------------------
  //
  // The picture is converted in the app's webview (a canvas decodes it;
  // device-client's photo.ts dithers it), so what arrives here is the finished
  // .g4 file, base64 in `data`.
  async 'device.photos'(request, ctx) {
    return (await deviceClient(request, ctx)).listPhotos();
  },
  async 'device.photoUpload'(request, ctx) {
    const name = requireString(request, 'name');
    const bytes = new Uint8Array(Buffer.from(requireString(request, 'data'), 'base64'));
    const client = await deviceClient(request, ctx);
    const result = await client.uploadPhoto(name, bytes, {
      onProgress: ({ sentBytes, totalBytes, resyncs }) => {
        ctx.progress({ stage: 'upload', sentBytes, totalBytes, resyncs });
      },
    });
    return { ...result, bytes: bytes.length };
  },
  // Firmware over Wi-Fi: `file` is the app image, e.g.
  // firmware/.pio/build/crowpanel_579/firmware.bin.
  async 'device.firmwareUpdate'(request, ctx) {
    const file = requireString(request, 'file');
    const bytes = new Uint8Array(await ctx.readFile(file));
    const client = await deviceClient(request, ctx);
    const result = await client.uploadFirmware(bytes, {
      onProgress: ({ sentBytes, totalBytes, resyncs }) => {
        ctx.progress({ stage: 'firmware', sentBytes, totalBytes, resyncs });
      },
    });
    return { ...result, file: resolve(file), bytes: bytes.length };
  },
  async 'device.photoDelete'(request, ctx) {
    const name = requireString(request, 'name');
    await (await deviceClient(request, ctx)).deletePhoto(name);
    return { deleted: name };
  },
  async 'device.setTimeZone'(request, ctx) {
    const tz = requireString(request, 'tz');
    return (await deviceClient(request, ctx)).setTimeZone(tz);
  },
};

export const COMMAND_NAMES = Object.keys(COMMANDS);

function requireString(request, key) {
  const value = request[key];
  if (typeof value !== 'string' || value.trim() === '') {
    throw new BridgeError('BAD_REQUEST', `"${key}" is required`);
  }
  return value.trim();
}

/**
 * Runs one request.
 *
 * `ctx` supplies everything that touches the world: `load` (the two
 * libraries), `progress` (the sink streamed lines go to), `readFile` and
 * `writeFile`. Tests pass their own; `run.mjs` passes the real ones.
 */
export async function runCommand(request, ctx) {
  if (!request || typeof request !== 'object') {
    throw new BridgeError('BAD_REQUEST', 'the request must be a JSON object');
  }
  const handler = COMMANDS[request.command];
  if (!handler) {
    throw new BridgeError(
      'BAD_REQUEST',
      `unknown command "${String(request.command)}"; known commands: ${COMMAND_NAMES.join(', ')}`,
    );
  }
  return handler(request, {
    load: ctx.load,
    progress: ctx.progress ?? (() => {}),
    readFile: ctx.readFile ?? ((path) => readFile(path)),
    writeFile: ctx.writeFile ?? ((path, bytes) => writeFile(path, bytes)),
  });
}
