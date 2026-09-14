#!/usr/bin/env node
// Talks to a device over the Milestone 4 HTTP protocol.
//
//   node tools/device-cli/device.mjs <command> [options]
//
// Commands:
//   info                      identity and capabilities (works unpaired)
//   status                    storage, battery, transfer mode, open sessions
//   list                      installed content
//   upload <file.qpk>         resumable install, resuming automatically
//   delete <contentId>        remove installed content
//   abort <contentId>         abandon an interrupted upload
//
// Options:
//   --host <addr>   device address (default 127.0.0.1), or set QR_DEVICE_HOST
//   --port <n>      default 8080
//   --token <tok>   pairing token from provisioning, or set QR_DEVICE_TOKEN
//   --json          machine-readable output
//
// The device only serves this while it is in transfer mode -- Wi-Fi is
// on-demand by design (see docs/architecture.md 4.3), so put the device into
// transfer mode before running anything but `info`.
//
// Requires the client to be built:
//   npm install --prefix desktop/device-client
//   npm run build --prefix desktop/device-client

import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const entry = join(here, '..', '..', 'desktop', 'device-client', 'dist', 'src', 'index.js');

let clientModule;
try {
  clientModule = await import(`file://${entry.replace(/\\/g, '/')}`);
} catch (error) {
  console.error(
    'Could not load @quran-device/device-client.\n' +
      'Build it first:\n' +
      '  npm install --prefix desktop/device-client\n' +
      '  npm run build --prefix desktop/device-client\n' +
      `\n(${error.message})`,
  );
  process.exit(2);
}

const argv = process.argv.slice(2);
const OPTIONS_WITH_VALUES = new Set(['--host', '--port', '--token']);
const flag = (name) => argv.includes(name);
const value = (name, fallback) => {
  const at = argv.indexOf(name);
  return at >= 0 && at + 1 < argv.length ? argv[at + 1] : fallback;
};

const positional = [];
for (let i = 0; i < argv.length; i++) {
  const arg = argv[i];
  if (arg.startsWith('-')) {
    if (OPTIONS_WITH_VALUES.has(arg)) i++;
    continue;
  }
  positional.push(arg);
}

const [command, argument] = positional;
if (!command) {
  console.error('usage: device.mjs <info|status|list|upload|delete|abort> [options]');
  process.exit(2);
}

const asJson = flag('--json');
const client = new clientModule.DeviceClient({
  host: value('--host', process.env.QR_DEVICE_HOST ?? '127.0.0.1'),
  port: Number(value('--port', process.env.QR_DEVICE_PORT ?? '8080')),
  token: value('--token', process.env.QR_DEVICE_TOKEN ?? ''),
  timeoutMs: 20000,
});

try {
  switch (command) {
    case 'info':
      await show(await client.getInfo());
      break;

    case 'status':
      await show(await client.getStatus());
      break;

    case 'list': {
      const listing = await client.listLibrary();
      if (asJson) {
        console.log(JSON.stringify(listing, null, 2));
      } else if (listing.items.length === 0) {
        console.log('(the library is empty)');
      } else {
        console.log('type         ver  size       title');
        for (const item of listing.items) {
          console.log(
            `${String(item.type).padEnd(12)} ${String(item.contentVersion).padStart(3)}  ` +
              `${String(item.packageSize).padStart(9)}  ${item.title}`,
          );
        }
      }
      break;
    }

    case 'upload': {
      if (!argument) {
        console.error('usage: device.mjs upload <file.qpk>');
        process.exit(2);
      }
      const bytes = new Uint8Array(readFileSync(argument));
      let lastPercent = -1;
      const result = await client.uploadPackage(bytes, {
        onProgress: ({ sentBytes, totalBytes, resyncs }) => {
          if (asJson) return;
          const percent = Math.floor((sentBytes / totalBytes) * 100);
          if (percent === lastPercent) return;
          lastPercent = percent;
          const note = resyncs > 0 ? `  (${resyncs} resync${resyncs > 1 ? 's' : ''})` : '';
          process.stdout.write(
            `\rUPLOAD    ${String(percent).padStart(3)}%  ${sentBytes}/${totalBytes}${note}   `,
          );
        },
      });
      if (!asJson) process.stdout.write('\n');
      await show(result);
      break;
    }

    case 'delete':
      if (!argument) {
        console.error('usage: device.mjs delete <contentId>');
        process.exit(2);
      }
      await client.deleteItem(argument);
      await show({ deleted: argument });
      break;

    case 'abort':
      // The session id IS the content id (see firmware/wifi/README.md), which
      // is what makes an abort possible at all: `upload` never printed a
      // session id, so without that identity there would be nothing to name.
      if (!argument) {
        console.error('usage: device.mjs abort <contentId>');
        console.error('  the content id of the interrupted upload; the');
        console.error('  inspector prints it for a local .qpk file');
        process.exit(2);
      }
      await client.abortUpload(argument);
      await show({ aborted: argument });
      break;

    default:
      console.error(`unknown command "${command}"`);
      process.exit(2);
  }
} catch (error) {
  if (asJson) {
    console.log(JSON.stringify({ ok: false, code: error.code ?? null, error: error.message }, null, 2));
  } else {
    console.error(`\nFAILED    ${error.message}`);
    if (error.code === 'UNAUTHORIZED') {
      console.error('          pass --token, or set QR_DEVICE_TOKEN');
    }
    if (error.code === 'NOT_IN_TRANSFER_MODE') {
      console.error('          put the device into transfer mode first');
    }
  }
  process.exit(1);
}

async function show(body) {
  if (asJson) {
    console.log(JSON.stringify(body, null, 2));
    return;
  }
  for (const [key, raw] of Object.entries(body)) {
    if (raw !== null && typeof raw === 'object') {
      console.log(`${key}:`);
      for (const [inner, innerValue] of Object.entries(raw)) {
        console.log(`  ${inner.padEnd(16)} ${innerValue}`);
      }
    } else {
      console.log(`${key.padEnd(18)} ${raw}`);
    }
  }
}
