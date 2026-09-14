// The UI's side of the Node bridge.
//
// Every call goes through one Tauri command (`bridge_call`, see
// src-tauri/src/bridge.rs), which spawns `desktop/app-bridge/run.mjs` and
// streams its progress lines back as Tauri events. This module is where that
// becomes ordinary typed async functions.
//
// The result types are *imported*, not redeclared: `ConversionReport`,
// `ValidationResult`, `PagePreview` come from `desktop/converter` and
// `DeviceInfo`, `DeviceStatus`, `LibraryListing` from `packages/protocol` --
// the same declarations the libraries themselves are written against, mapped
// in tsconfig.json to their built `.d.ts`. They are type-only imports, so
// nothing from those packages is bundled into the app; the running code is
// still the Node process on the other side of the pipe. Redeclaring these
// shapes here would have added a copy that could silently drift from the
// values actually crossing the wire.

import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type {
  ConversionReport,
  PagePreview,
  ValidationResult,
} from "@quran-device/converter";
import type {
  DeviceInfo,
  DeviceStatus,
  LibraryListing,
  PhotoFinishResponse,
  PhotoListing,
  TimeZoneResponse,
  UploadFinishResponse,
} from "@quran-device/protocol";
import type { UploadProgress as ClientUploadProgress } from "@quran-device/device-client";

export type {
  ConversionReport,
  PagePreview,
  ValidationResult,
  DeviceInfo,
  DeviceStatus,
  LibraryListing,
  PhotoFinishResponse,
  PhotoListing,
  TimeZoneResponse,
  UploadFinishResponse,
};

/** A failed bridge call, as `BridgeFailure` in src-tauri/src/bridge.rs. */
export interface BridgeFailure {
  code: string;
  message: string;
  stderr?: string;
}

export function isBridgeFailure(value: unknown): value is BridgeFailure {
  return (
    typeof value === "object" &&
    value !== null &&
    typeof (value as BridgeFailure).code === "string" &&
    typeof (value as BridgeFailure).message === "string"
  );
}

/** Anything thrown out of a bridge call, as something worth showing a person. */
export function describeFailure(error: unknown): BridgeFailure {
  if (isBridgeFailure(error)) return error;
  return { code: "ERROR", message: String(error) };
}

/**
 * Failures a person can act on, phrased as what to do about them. Everything
 * else falls through to the bridge's own message, which is usually the
 * library's and is usually already specific.
 */
const REMEDIES: Record<string, string> = {
  NODE_NOT_FOUND:
    "Install Node 20 or newer and make sure `node` is on PATH, then restart the app.",
  LIBRARY_NOT_BUILT:
    "Build the TypeScript libraries: `npm install --prefix desktop/converter` " +
    "and `npm run build --prefix desktop/converter` (same for desktop/device-client).",
  REPO_NOT_FOUND:
    "Set QURAN_DEVICE_REPO to the project checkout and restart the app.",
  UNAUTHORIZED:
    "This endpoint needs the pairing token from BLE provisioning. Only " +
    "Identify works without one.",
  DEVICE_UNREACHABLE:
    "Nothing answered at that address. Check the device is awake, on the same " +
    "network, and in transfer mode -- its Wi-Fi is on-demand, not always on.",
  NOT_IN_TRANSFER_MODE:
    "Put the device into transfer mode first: select Transfer Mode in its " +
    "library menu, or long-press the wheel.",
  NO_SPACE: "The device's card is full. Delete something and try again.",
};

export function remedyFor(code: string): string | undefined {
  return REMEDIES[code];
}

export interface BridgeProgress {
  stage: string;
  [key: string]: unknown;
}

/**
 * The client's own progress shape, tagged with the stage the bridge stamps on
 * it. Extending rather than restating means the fields cannot drift from what
 * `DeviceClient.uploadPackage` actually emits -- and it keeps the typecheck
 * honest: this import is what makes a missing `device-client/dist` fail here
 * rather than at the first device call.
 */
export interface UploadProgress extends BridgeProgress, ClientUploadProgress {
  stage: "upload";
}

let callCounter = 0;

/**
 * Runs one bridge request.
 *
 * Each call gets its own event name so a conversion and an upload running at
 * the same time cannot land in each other's progress handlers.
 */
async function call<T>(
  request: Record<string, unknown>,
  onProgress?: (progress: BridgeProgress) => void,
): Promise<T> {
  const progressEvent = onProgress
    ? `bridge://progress/${++callCounter}`
    : undefined;

  const unlisten = progressEvent
    ? await listen<BridgeProgress>(progressEvent, (event) =>
        onProgress?.(event.payload),
      )
    : undefined;

  try {
    return await invoke<T>("bridge_call", { request, progressEvent });
  } finally {
    unlisten?.();
  }
}

// --- environment -------------------------------------------------------------

export interface BridgeEnvironment {
  repoRoot: string | null;
  bridgeScript: string | null;
  /** Whether the checkout was located -- not whether the libraries are built. */
  found: boolean;
  problem?: string;
}

export function bridgeEnvironment(): Promise<BridgeEnvironment> {
  return invoke<BridgeEnvironment>("bridge_environment");
}

export interface BridgePing {
  bridgeVersion: number;
  node: string;
  libraries: Record<string, { built: boolean; error: string | null }>;
}

export function ping(): Promise<BridgePing> {
  return call<BridgePing>({ command: "ping" });
}

// --- conversion --------------------------------------------------------------

export interface ConvertRequest {
  input: string;
  output?: string;
  dryRun?: boolean;
  title?: string;
  author?: string;
  language?: string;
  contentVersion?: number;
  contentType?: "BOOK" | "QURAN";
  inputKind?: "txt" | "pdf" | "epub" | "quran-json";
  includeWordLayout?: boolean;
  singleChapter?: boolean;
  previewPage?: number;
  /** A finished COVER section payload, base64 -- see cover.tsx. */
  cover?: string;
  /** Page pictures, each a PackBits-compressed bitmap in base64 -- see pdfPages.ts. */
  pageImages?: string[];
}

export interface ConvertResult {
  input: string;
  output: string;
  written: boolean;
  dryRun: boolean;
  packageBytes: number;
  report: ConversionReport;
  validation: ValidationResult;
  preview: PagePreview | { error: string } | null;
}

export function convert(
  request: ConvertRequest,
  onProgress?: (progress: BridgeProgress) => void,
): Promise<ConvertResult> {
  return call<ConvertResult>({ command: "convert", ...request }, onProgress);
}

export interface InspectResult {
  file: string;
  packageBytes: number;
  validation: ValidationResult;
}

export function inspect(file: string): Promise<InspectResult> {
  return call<InspectResult>({ command: "inspect", file });
}

export function previewPackage(file: string, page: number): Promise<PagePreview> {
  return call<PagePreview>({ command: "preview", file, page });
}

/** An EPUB's own cover picture, base64, or null (not an EPUB, or none declared). */
export interface CoverSource {
  mediaType: string;
  data: string;
}

export function coverSource(input: string): Promise<CoverSource | null> {
  return call<CoverSource | null>({ command: "coverSource", input });
}

/** An input file's bytes, base64 -- the webview renders PDF pages from them. */
export function readInputFile(input: string): Promise<{ input: string; bytes: number; data: string }> {
  return call({ command: "readInput", input });
}

// --- device ------------------------------------------------------------------

export interface DeviceConnection {
  host: string;
  port: number;
  token: string;
}

function device(connection: DeviceConnection): Record<string, unknown> {
  return {
    host: connection.host,
    port: connection.port,
    token: connection.token,
  };
}

export function deviceInfo(connection: DeviceConnection): Promise<DeviceInfo> {
  return call<DeviceInfo>({ command: "device.info", ...device(connection) });
}

export function deviceStatus(connection: DeviceConnection): Promise<DeviceStatus> {
  return call<DeviceStatus>({ command: "device.status", ...device(connection) });
}

export function deviceList(connection: DeviceConnection): Promise<LibraryListing> {
  return call<LibraryListing>({ command: "device.list", ...device(connection) });
}

export function deviceUpload(
  connection: DeviceConnection,
  file: string,
  onProgress: (progress: UploadProgress) => void,
): Promise<UploadFinishResponse & { file: string; packageBytes: number }> {
  return call(
    { command: "device.upload", file, ...device(connection) },
    (progress) => onProgress(progress as UploadProgress),
  );
}

export function deviceDelete(
  connection: DeviceConnection,
  contentId: string,
): Promise<{ deleted: string }> {
  return call({ command: "device.delete", contentId, ...device(connection) });
}

export function deviceAbort(
  connection: DeviceConnection,
  contentId: string,
): Promise<{ aborted: string }> {
  return call({ command: "device.abort", contentId, ...device(connection) });
}

// --- home-screen photos and time zone ------------------------------------------

export function devicePhotos(connection: DeviceConnection): Promise<PhotoListing> {
  return call<PhotoListing>({ command: "device.photos", ...device(connection) });
}

/** `file` is a finished .g4 photo file (device-client's convertPhoto). */
export function devicePhotoUpload(
  connection: DeviceConnection,
  name: string,
  file: Uint8Array,
  onProgress: (progress: UploadProgress) => void,
): Promise<PhotoFinishResponse & { bytes: number }> {
  return call(
    { command: "device.photoUpload", name, data: toBase64(file), ...device(connection) },
    (progress) => onProgress(progress as UploadProgress),
  );
}

export interface FirmwareUpdateResult {
  updated: boolean;
  restarting: boolean;
  file: string;
  bytes: number;
}

/** Sends firmware.bin over Wi-Fi; the device installs it and restarts. */
export function deviceFirmwareUpdate(
  connection: DeviceConnection,
  file: string,
  onProgress: (progress: UploadProgress) => void,
): Promise<FirmwareUpdateResult> {
  return call(
    { command: "device.firmwareUpdate", file, ...device(connection) },
    (progress) => onProgress(progress as UploadProgress),
  );
}

export function devicePhotoDelete(
  connection: DeviceConnection,
  name: string,
): Promise<{ deleted: string }> {
  return call({ command: "device.photoDelete", name, ...device(connection) });
}

export function deviceSetTimeZone(
  connection: DeviceConnection,
  tz: string,
): Promise<TimeZoneResponse> {
  return call<TimeZoneResponse>({ command: "device.setTimeZone", tz, ...device(connection) });
}

function toBase64(bytes: Uint8Array): string {
  let binary = "";
  const step = 0x8000; // stay well inside String.fromCharCode's argument limit
  for (let i = 0; i < bytes.length; i += step) {
    binary += String.fromCharCode(...bytes.subarray(i, i + step));
  }
  return btoa(binary);
}

// --- library (Rust side, no Node involved) -----------------------------------

export interface PackageSummaryRow {
  /** Display name. */
  file: string;
  /** Full path, so a row can be handed to the uploader as-is. */
  path: string;
  packageType: string;
  packageSize: number;
  contentVersion: number;
  contentId: string;
  title: string | null;
  author: string | null;
  language: string | null;
  chapterCount: number;
  ayahCount: number | null;
  /** The COVER section's payload bytes, when the package has a well-formed one. */
  cover: number[] | null;
  error: string | null;
}

export function scanLibrary(dir: string): Promise<PackageSummaryRow[]> {
  return invoke<PackageSummaryRow[]>("scan_library", { dir });
}
