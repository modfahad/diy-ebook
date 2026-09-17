# Android app

**Status (2026-09-14): planned and being implemented; not yet built or run.**
The Android SDK is not installed on the development Mac yet, so nothing
here has run on a phone. Testing is planned on a real Android phone on the
same Wi-Fi as the device.

## Goal

Everything the desktop app does, on an Android phone: talk to the device
(info, status, library, package and photo uploads, time zone, firmware
update), manage packages, and convert PDF, EPUB and TXT into packages -- with
the same device protocol and the same package bytes as the desktop.

## Shape

- `mobile/` -- an Expo (React Native, TypeScript) app with the desktop's
  tabs: Dashboard, Library, Device, Converter, Photos.
- The shared TypeScript libraries run inside the app's JavaScript engine
  directly: `packages/qpk-format`, `packages/protocol`,
  `desktop/device-client` and the pure parts of `desktop/converter`. Metro
  is pointed at them (`watchFolders`), so there is one copy of each, not a
  fork.
- There is no Node bridge on the phone. The desktop needs one only because a
  webview cannot run Node libraries; on Android the libraries are made
  platform-neutral instead (below).

## Desktop pieces and their Android replacements

| Desktop | Android |
|---|---|
| Node bridge subprocess | the libraries run in the app's JS engine |
| `node:crypto` SHA-256 (content ids) and MD5 (firmware) | `@noble/hashes`: the same digests in pure JS, used on desktop too so both produce identical packages |
| `yauzl` + `Buffer` for EPUB zips | `fflate` (`unzipSync`), used on desktop too |
| pdf.js text extraction (Node legacy build) and page pictures (webview canvas) | one hidden `react-native-webview` "render worker" page running pdf.js and a canvas: it extracts PDF text, renders page pictures, and decodes photos and covers to RGBA for `photo.ts` |
| file paths and Browse dialogs | `expo-document-picker` and `expo-file-system` (content URIs read as bytes) |
| Library folder scan (Rust) | pick packages with the system picker; headers read with `qpk-format` |
| Save a `.qpk` next to the source | save into the app's documents, and share with `expo-sharing` |
| chunk uploads with a streamed, chunked request body | React Native's `fetch` cannot stream a request body, so it would send Content-Length, which the device's chunked body reader refuses. The device will also accept a chunk as base64 text (`X-Qr-Body: base64`), and `DeviceClient` uses that when streaming bodies are unavailable |
| plain HTTP to the device on the LAN | Android 9+ blocks cleartext HTTP by default: `expo-build-properties` with `usesCleartextTraffic: true` |

## Milestones

1. **Shared libraries platform-neutral** -- `@noble/hashes` and `fflate` in
   place of `node:crypto` and `yauzl`, desktop tests unchanged. *(done)*
2. **Base64 chunk bodies on the device** -- every chunk endpoint (packages,
   photos, firmware) accepts `X-Qr-Body: base64`; `DeviceClient` option
   `chunkEncoding: 'base64'`. *(done: host-tested decoder and mock-device
   test; not yet exercised against the board)*
3. **Scaffold** `mobile/` with Expo, Metro wired to the shared libraries,
   tabs, settings (device address and token) kept with `expo-secure-store`.
   *(done, type-checked; not yet run)* Expo SDK 57 (React Native 0.86).
   The shared libraries are consumed as built, so run `npm run build` in
   `packages/protocol`, `packages/qpk-format`, `desktop/device-client` and
   `desktop/converter` before `npm install` in `mobile/`.
4. **Device tab** -- info, status, library list and delete, package upload
   from a picked file, firmware update, last restart. *(done, type-checked;
   not yet run)* Library, Converter and Photos are placeholder tabs; Photos
   already sets the time zone.

To check on the first run: Hermes provides `TextEncoder`/`TextDecoder`
(the converter needs the decoder), and a picked `content://` file reads
through `expo-file-system`'s `File.arrayBuffer()`.
5. **Photos tab** -- pick pictures, 4-grey preview via the render worker,
   upload, time zone. *(done, type-checked and bundled; not yet run)*
6. **Library tab** -- open packages, validate, covers. *(done, type-checked
   and bundled; not yet run)* Packages live in the app's documents folder
   (`library/`): added from the file picker or saved by the Converter; each
   can be validated, sent to the device, shared, or deleted. "Add books…"
   takes many files at once: each book's title, author and language are
   read from its own metadata and can be edited, with a cover per book (or an author and language for all), then convert them together --
   PDF, EPUB and TXT in the render worker, `.qpk` copied, a package already
   present skipped. Ticked
   packages are sent to the device together, one after another.
   *(app code type-checked; the render worker's side is not, and none of it
   has run yet)*
7. **Converter tab** -- TXT and EPUB first (pure JS), then PDF through the
   render worker (text and page pictures), covers. *(done, type-checked and
   bundled; not yet run)* In the end all three formats go through the render
   worker, not just PDF: it runs the converter exactly as the desktop bridge
   does, which avoids depending on Hermes for `TextDecoder` and keeps pdf.js
   out of the app bundle.

### The render worker

`render-worker/worker.ts`, bundled by `npm run worker`
(`scripts/build-render-worker.mjs`, esbuild) into one HTML page,
`src/render/workerHtml.generated.ts` (1.8 MB, not committed). The app mounts
it once in a hidden `react-native-webview` (`src/render/RenderWorker.tsx`)
and calls it with `worker.call(command, args, { bytes, onProgress })`.
Commands: `photoRgba` (decode + 400x480 crop), `coverFromPicture`,
`epubCover`, `convert` (with PDF page pictures when `keepLayout`), `preview`,
`validate`. Bytes travel as base64 in 512 KB pieces both ways; the last
converted package stays in the page so preview page turns do not resend it.
pdf.js's worker runs on the page's own thread, as in the desktop's standalone
bridge. The app itself dithers photos (`convertPhoto`) and draws every preview
as a PNG it encodes (`src/png.ts`), since React Native has no canvas.

## Building and running (once the SDK is installed)

1. Install a JDK 17 and Android Studio (for the SDK and platform tools), then
   set `ANDROID_HOME` to `~/Library/Android/sdk`.
2. On the phone: enable Developer options and USB debugging, connect it with
   a cable, and accept the prompt.
3. Build and install a development build:

```bash
cd mobile
npm install
npx expo prebuild --platform android
npx expo run:android --device
```

Or build in Expo's cloud instead (needs a free Expo account, `npx eas login`
run by you): `npx eas build --platform android --profile preview`.
