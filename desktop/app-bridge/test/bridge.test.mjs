// The bridge's own tests.
//
// Two halves, on purpose:
//
//   * `runCommand` against the *real* converter and device-client builds, so
//     the dispatch table is checked against the libraries the app will
//     actually load -- not against mocks that agree with it by construction.
//     Only the device transport is faked, because there is no device.
//   * `run.mjs` as a spawned process, which is the only way to check the part
//     the Rust side depends on: that stdout carries JSON lines and nothing
//     else, and that exit codes distinguish success from failure.

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { after, before, describe, it } from 'node:test';
import { fileURLToPath } from 'node:url';

import { BridgeError, createLoader, runCommand } from '../src/commands.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(join(here, '..', '..', '..'));
const runner = join(here, '..', 'run.mjs');
const load = createLoader(repoRoot);

const BUSHRA_TXT = join(repoRoot, 'desktop', 'converter', 'examples', 'for-bushra.txt');
const FATIHAH_QPK = join(repoRoot, 'desktop', 'converter', 'examples', 'surahs', 'al-fatihah.qpk');

/** Collects progress events so a test can assert on the stream, not just the result. */
function recorder() {
  const events = [];
  return { events, progress: (event) => events.push(event) };
}

let workDir;
before(async () => {
  workDir = await mkdtemp(join(tmpdir(), 'qpk-bridge-'));
});
after(async () => {
  await rm(workDir, { recursive: true, force: true });
});

describe('dispatch', () => {
  it('ping reports both libraries as built', async () => {
    const result = await runCommand({ command: 'ping' }, { load });
    assert.equal(result.bridgeVersion, 1);
    assert.equal(result.libraries.converter.built, true);
    assert.equal(result.libraries['device-client'].built, true);
  });

  it('rejects an unknown command by name, and says what it knows', async () => {
    await assert.rejects(
      () => runCommand({ command: 'nope' }, { load }),
      (error) => {
        assert.ok(error instanceof BridgeError);
        assert.equal(error.code, 'BAD_REQUEST');
        assert.match(error.message, /known commands: .*device\.upload/u);
        return true;
      },
    );
  });

  it('rejects a missing required argument', async () => {
    await assert.rejects(
      () => runCommand({ command: 'convert' }, { load }),
      (error) => error.code === 'BAD_REQUEST' && /"input" is required/u.test(error.message),
    );
  });

  it('reports an unbuilt library as a named condition, not an import stack', async () => {
    const missing = createLoader(join(workDir, 'not-a-checkout'));
    await assert.rejects(
      () => runCommand({ command: 'convert', input: BUSHRA_TXT }, { load: missing }),
      (error) => {
        assert.equal(error.code, 'LIBRARY_NOT_BUILT');
        assert.match(error.message, /npm run build --prefix desktop\/converter/u);
        return true;
      },
    );
  });
});

describe('convert', () => {
  it('a dry run validates and previews without writing anything', async () => {
    const { events, progress } = recorder();
    const writes = [];
    const result = await runCommand(
      { command: 'convert', input: BUSHRA_TXT, dryRun: true, title: 'For Bushra' },
      { load, progress, writeFile: (path, bytes) => writes.push([path, bytes]) },
    );

    assert.equal(result.written, false);
    assert.equal(writes.length, 0, 'a dry run must not write');
    assert.equal(result.validation.ok, true);
    assert.equal(result.report.contentType, 'BOOK');
    assert.ok(result.report.pageCount > 0);
    assert.equal(result.preview.pageNumber, 1);
    assert.match(result.preview.text, /Bismillah/u);
    assert.deepEqual(
      events.map((e) => e.stage),
      ['parsing', 'validating'],
    );
  });

  it('writes a package whose bytes the converter itself accepts', async () => {
    const output = join(workDir, 'for-bushra.qpk');
    const result = await runCommand(
      { command: 'convert', input: BUSHRA_TXT, output, title: 'For Bushra' },
      { load },
    );

    assert.equal(result.written, true);
    assert.equal(result.output, resolve(output));

    // Read the file back through the same path the Library tab would use.
    const inspected = await runCommand({ command: 'inspect', file: output }, { load });
    assert.equal(inspected.validation.ok, true);
    assert.equal(inspected.packageBytes, result.packageBytes);
    // The content id is derived from title/author/language, so the file on
    // disk must carry the id the conversion reported -- this is the check
    // that the bytes written are the bytes converted.
    assert.equal(
      inspected.validation.summary.contentId,
      result.validation.summary.contentId,
    );
    assert.equal((await readFile(output)).length, result.packageBytes);
  });

  it('a dry run and a real run of the same input agree byte for byte', async () => {
    // Content ids are content-derived, so a UI that previews with a dry run
    // and then commits must get the same package. If this ever stops holding,
    // "preview before install" is previewing something else.
    const dry = await runCommand(
      { command: 'convert', input: BUSHRA_TXT, dryRun: true, title: 'Stability' },
      { load },
    );
    const output = join(workDir, 'stability.qpk');
    const wet = await runCommand(
      { command: 'convert', input: BUSHRA_TXT, output, title: 'Stability' },
      { load },
    );
    assert.equal(dry.packageBytes, wet.packageBytes);
    assert.equal(dry.validation.summary.contentId, wet.validation.summary.contentId);
  });

  it('previewPage is clamped into range rather than throwing', async () => {
    const result = await runCommand(
      { command: 'convert', input: BUSHRA_TXT, dryRun: true, previewPage: 9999 },
      { load },
    );
    assert.equal(result.preview.pageNumber, result.report.pageCount);
  });
});

describe('document details', () => {
  it('reads what a picked file will be converted with, before converting it', async () => {
    const details = await runCommand({ command: 'documentDetails', input: BUSHRA_TXT }, { load });
    // A TXT says nothing about itself, so the title is the file name's.
    assert.deepEqual(details, { kind: 'txt', title: 'for-bushra', untitled: true });
  });
});

describe('inspect and preview', () => {
  it('inspects a real QURAN package built by the converter', async () => {
    const result = await runCommand({ command: 'inspect', file: FATIHAH_QPK }, { load });
    assert.equal(result.validation.ok, true);
    assert.equal(result.validation.summary.type, 'QURAN');
    assert.equal(result.validation.summary.ayahCount, 7);
  });

  it('previews a page of it as the device would see it', async () => {
    const preview = await runCommand({ command: 'preview', file: FATIHAH_QPK, page: 1 }, { load });
    assert.equal(preview.pageNumber, 1);
    assert.ok(preview.text.length > 0);
    assert.match(preview.text, /^1:1\s/u);
  });

  it('refuses to preview something that is not a package', async () => {
    await assert.rejects(
      () => runCommand({ command: 'preview', file: BUSHRA_TXT }, { load }),
      (error) => error.code === 'VALIDATION_FAILED',
    );
  });
});

describe('device commands', () => {
  // There is no device to talk to (see docs/pending.md section 3: no pairing
  // token has ever been obtained, so every endpoint but /api/device/info is
  // unproven on hardware). What is testable here is the bridge's own half:
  // that it constructs the client from the request, streams upload progress,
  // and turns a DeviceError into a coded failure.
  function fakeDeviceClient(behaviour) {
    return {
      async load(name) {
        if (name !== 'device-client') return load(name);
        return {
          DeviceClient: class {
            constructor(options) {
              behaviour.options = options;
            }
            getInfo = async () => behaviour.info;
            listLibrary = async () => behaviour.listing;
            deleteItem = async (id) => behaviour.deleted.push(id);
            abortUpload = async (id) => behaviour.aborted.push(id);
            uploadPackage = async (bytes, options) => {
              for (const sent of behaviour.progressSteps) {
                options.onProgress({ sentBytes: sent, totalBytes: bytes.length, resyncs: 0 });
              }
              return { contentId: 'abc', installed: true, location: 'QURAN/abc.qpk' };
            };
          },
        };
      },
    };
  }

  it('passes host, port, token and timeout through to the client', async () => {
    const behaviour = { info: { deviceId: 'd1', paired: false } };
    const fake = fakeDeviceClient(behaviour);
    const info = await runCommand(
      { command: 'device.info', host: ' 10.0.0.5 ', port: 9000, token: ' t ', timeoutMs: 5000 },
      fake,
    );
    assert.equal(info.deviceId, 'd1');
    assert.deepEqual(behaviour.options, {
      host: '10.0.0.5',
      port: 9000,
      token: 't',
      timeoutMs: 5000,
    });
  });

  it('defaults the port and timeout when the request omits them', async () => {
    const behaviour = { info: {} };
    await runCommand({ command: 'device.info', host: '10.0.0.5' }, fakeDeviceClient(behaviour));
    assert.equal(behaviour.options.port, 8080);
    assert.equal(behaviour.options.timeoutMs, 20000);
  });

  it('requires a host', async () => {
    await assert.rejects(
      () => runCommand({ command: 'device.info' }, { load }),
      (error) => error.code === 'BAD_REQUEST' && /"host" is required/u.test(error.message),
    );
  });

  it('streams upload progress and returns the install result', async () => {
    const behaviour = { progressSteps: [0, 4, 8] };
    const { events, progress } = recorder();
    const result = await runCommand(
      { command: 'device.upload', host: '10.0.0.5', file: FATIHAH_QPK },
      { ...fakeDeviceClient(behaviour), progress, readFile: async () => Buffer.alloc(8) },
    );
    assert.deepEqual(
      events.map((e) => e.sentBytes),
      [0, 4, 8],
    );
    assert.ok(events.every((e) => e.stage === 'upload' && e.totalBytes === 8));
    assert.equal(result.installed, true);
    assert.equal(result.packageBytes, 8);
  });

  it('delete and abort both name what they acted on', async () => {
    const behaviour = { deleted: [], aborted: [] };
    const fake = fakeDeviceClient(behaviour);
    assert.deepEqual(
      await runCommand({ command: 'device.delete', host: 'h', contentId: 'aa' }, fake),
      { deleted: 'aa' },
    );
    assert.deepEqual(
      await runCommand({ command: 'device.abort', host: 'h', contentId: 'bb' }, fake),
      { aborted: 'bb' },
    );
    assert.deepEqual(behaviour.deleted, ['aa']);
    assert.deepEqual(behaviour.aborted, ['bb']);
  });
});

describe('photo and time zone commands', () => {
  function fakePhotoClient(behaviour) {
    return {
      async load(name) {
        if (name !== 'device-client') return load(name);
        return {
          DeviceClient: class {
            constructor(options) {
              behaviour.options = options;
            }
            listPhotos = async () => behaviour.listing;
            deletePhoto = async (photo) => behaviour.deleted.push(photo);
            setTimeZone = async (tz) => ({ tz, synced: false, nowUnix: 0 });
            uploadPhoto = async (photo, bytes, options) => {
              behaviour.uploaded = { photo, bytes };
              options.onProgress({ sentBytes: 0, totalBytes: bytes.length, resyncs: 0 });
              options.onProgress({ sentBytes: bytes.length, totalBytes: bytes.length, resyncs: 0 });
              return { name: photo, installed: true };
            };
          },
        };
      },
    };
  }

  it('lists photos', async () => {
    const behaviour = { listing: { width: 400, height: 480, items: [{ name: 'a', bytes: 48016 }] } };
    const listing = await runCommand({ command: 'device.photos', host: 'h' }, fakePhotoClient(behaviour));
    assert.deepEqual(listing, behaviour.listing);
  });

  it('decodes the base64 file, streams progress and reports the install', async () => {
    const behaviour = {};
    const { events, progress } = recorder();
    const file = Buffer.from([1, 2, 3, 250]);
    const result = await runCommand(
      { command: 'device.photoUpload', host: 'h', name: 'sunset', data: file.toString('base64') },
      { ...fakePhotoClient(behaviour), progress },
    );
    assert.deepEqual(result, { name: 'sunset', installed: true, bytes: 4 });
    assert.equal(behaviour.uploaded.photo, 'sunset');
    assert.deepEqual([...behaviour.uploaded.bytes], [1, 2, 3, 250]);
    assert.deepEqual(events.map((e) => [e.stage, e.sentBytes]), [['upload', 0], ['upload', 4]]);
  });

  it('requires a name and data for an upload', async () => {
    await assert.rejects(
      () => runCommand({ command: 'device.photoUpload', host: 'h', name: 'x' }, fakePhotoClient({})),
      (error) => error.code === 'BAD_REQUEST' && /"data" is required/u.test(error.message),
    );
  });

  it('deletes a photo and sets the time zone', async () => {
    const behaviour = { deleted: [] };
    const fake = fakePhotoClient(behaviour);
    assert.deepEqual(await runCommand({ command: 'device.photoDelete', host: 'h', name: 'a' }, fake), {
      deleted: 'a',
    });
    assert.deepEqual(behaviour.deleted, ['a']);
    assert.deepEqual(
      await runCommand({ command: 'device.setTimeZone', host: 'h', tz: 'LOC-5:30' }, fake),
      { tz: 'LOC-5:30', synced: false, nowUnix: 0 },
    );
  });
});

describe('run.mjs as a process', () => {
  function runProcess(request, { asArgv = false } = {}) {
    return new Promise((resolvePromise, reject) => {
      const args = asArgv ? [runner, JSON.stringify(request)] : [runner];
      const child = spawn(process.execPath, args, { stdio: ['pipe', 'pipe', 'pipe'] });
      let stdout = '';
      let stderr = '';
      child.stdout.on('data', (d) => (stdout += d));
      child.stderr.on('data', (d) => (stderr += d));
      child.on('error', reject);
      child.on('close', (code) => resolvePromise({ code, stdout, stderr }));
      if (!asArgv) {
        child.stdin.write(JSON.stringify(request));
        child.stdin.end();
      }
    });
  }

  function lines(stdout) {
    return stdout
      .split('\n')
      .filter((line) => line.trim() !== '')
      .map((line) => JSON.parse(line)); // throws if stdout carried anything but JSON
  }

  it('answers a request on stdin with one result line and exit 0', async () => {
    const { code, stdout } = await runProcess({ command: 'ping' });
    assert.equal(code, 0);
    const events = lines(stdout);
    assert.equal(events.length, 1);
    assert.equal(events[0].type, 'result');
    assert.equal(events[0].value.libraries.converter.built, true);
  });

  it('accepts the same request on argv', async () => {
    const { code, stdout } = await runProcess({ command: 'ping' }, { asArgv: true });
    assert.equal(code, 0);
    assert.equal(lines(stdout)[0].type, 'result');
  });

  it('emits progress lines before the result, all of them parseable JSON', async () => {
    const { code, stdout } = await runProcess({
      command: 'convert',
      input: BUSHRA_TXT,
      dryRun: true,
    });
    assert.equal(code, 0);
    const events = lines(stdout);
    assert.ok(events.length >= 3);
    assert.ok(events.slice(0, -1).every((e) => e.type === 'progress'));
    assert.equal(events.at(-1).type, 'result');
  });

  it('names an unreachable device instead of passing "fetch failed" along', async () => {
    // A real connection refusal against a port nothing listens on: the whole
    // point is that the UI can say what to check, which a bare TypeError from
    // fetch does not support. (A high port on purpose -- fetch refuses the
    // low "bad ports" outright, which is a different failure.)
    const { code, stdout } = await runProcess({
      command: 'device.info',
      host: '127.0.0.1',
      port: 8099,
      timeoutMs: 3000,
    });
    assert.equal(code, 1);
    const event = lines(stdout)[0];
    assert.equal(event.type, 'error');
    assert.equal(event.code, 'DEVICE_UNREACHABLE');
    assert.match(event.message, /fetch failed: /u);
  });

  it('reports a failure as one error line and exit 1', async () => {
    const { code, stdout } = await runProcess({ command: 'nope' });
    assert.equal(code, 1);
    const events = lines(stdout);
    assert.equal(events.length, 1);
    assert.equal(events[0].type, 'error');
    assert.equal(events[0].code, 'BAD_REQUEST');
  });

  it('treats a malformed request as an error rather than a crash', async () => {
    const child = spawn(process.execPath, [runner, 'this is not json']);
    let stdout = '';
    child.stdout.on('data', (d) => (stdout += d));
    const code = await new Promise((r) => child.on('close', r));
    assert.equal(code, 1);
    assert.equal(JSON.parse(stdout.trim()).code, 'BAD_REQUEST');
  });
});
