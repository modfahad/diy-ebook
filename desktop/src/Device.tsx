// The Device tab: talk to a device over the Milestone 4 HTTP protocol.
//
// This is the UI half of `desktop/device-client` and `tools/device-cli`, and
// it inherits their constraint exactly: **only Identify works without a
// pairing token.** Everything else -- status, the installed library, upload,
// delete -- is behind `Authorization: Bearer`, and no token has ever been
// obtained on real hardware (BLE provisioning pairs but has never completed
// the credential write from a Windows client; see docs/pending.md section 3).
// So the tab is built in full, and says plainly which parts are unproven
// rather than presenting an untested path as a working one.
//
// The device also only serves any of this while it is in transfer mode: Wi-Fi
// is on-demand by design (docs/architecture.md 4.3).

import { useCallback, useState } from "react";
import {
  deviceAbort,
  deviceDelete,
  deviceInfo,
  deviceList,
  deviceStatus,
  deviceUpload,
  deviceFirmwareUpdate,
  type DeviceConnection,
  type DeviceInfo,
  type DeviceStatus,
  type LibraryListing,
  type UploadProgress,
} from "./bridge";
import { baseName, formatBytes, formatDuration, formatInstalledAt } from "./format";
import { Badge, Card, Empty, Failure, Field, KeyValues, Meter, Note, PathInput } from "./ui";
import type { Settings } from "./settings";

interface UploadState {
  file: string;
  sentBytes: number;
  totalBytes: number;
  resyncs: number;
  done: boolean;
}

export default function Device({
  settings,
  update,
}: {
  settings: Settings;
  update: (patch: Partial<Settings>) => void;
}) {
  const [info, setInfo] = useState<DeviceInfo | null>(null);
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [listing, setListing] = useState<LibraryListing | null>(null);
  const [upload, setUpload] = useState<UploadState | null>(null);
  const [firmwareFile, setFirmwareFile] = useState("");
  const [firmware, setFirmware] = useState<UploadState | null>(null);
  const [abortId, setAbortId] = useState("");
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [message, setMessage] = useState<string | null>(null);

  const uploadFile = settings.uploadFile;

  const connection: DeviceConnection = {
    host: settings.deviceHost.trim(),
    port: settings.devicePort,
    token: settings.deviceToken.trim(),
  };
  const connected = connection.host !== "";
  const hasToken = connection.token !== "";

  /**
   * One place where every call's busy flag, error and success message are
   * handled. Without it each of the seven actions below grows its own
   * try/catch and they drift apart.
   */
  const run = useCallback(
    async <T,>(
      what: string,
      action: () => Promise<T>,
      then?: (value: T) => void,
      // A refresh triggered *by* a mutation must not wipe the confirmation
      // that mutation just set -- otherwise an upload or a delete succeeds
      // and says nothing.
      options: { keepMessage?: boolean } = {},
    ) => {
      setBusy(what);
      setError(null);
      if (!options.keepMessage) setMessage(null);
      try {
        const value = await action();
        then?.(value);
      } catch (failure) {
        setError(failure);
      } finally {
        setBusy(null);
      }
    },
    [],
  );

  const identify = () =>
    run("identify", () => deviceInfo(connection), (value) => {
      setInfo(value);
      setMessage(`${value.name} answered: ${value.model}, firmware ${value.firmwareVersion}.`);
    });

  const refreshStatus = () => run("status", () => deviceStatus(connection), setStatus);

  const refreshLibrary = (keepMessage = false) =>
    run("library", () => deviceList(connection), setListing, { keepMessage });

  const startFirmwareUpdate = () => {
    const file = firmwareFile.trim();
    if (file === "") return;
    setFirmware({ file, sentBytes: 0, totalBytes: 0, resyncs: 0, done: false });
    return run(
      "firmware",
      () =>
        deviceFirmwareUpdate(connection, file, (progress: UploadProgress) =>
          setFirmware({
            file,
            sentBytes: progress.sentBytes,
            totalBytes: progress.totalBytes,
            resyncs: progress.resyncs,
            done: false,
          }),
        ),
      (result) => {
        setFirmware({ file, sentBytes: result.bytes, totalBytes: result.bytes, resyncs: 0, done: true });
        setMessage(
          "Firmware installed. The device is restarting into it -- give it about 30 seconds to come back on Wi-Fi.",
        );
      },
    );
  };

  const startUpload = () => {
    const file = uploadFile.trim();
    if (file === "") return;
    setUpload({ file, sentBytes: 0, totalBytes: 0, resyncs: 0, done: false });
    return run(
      "upload",
      () =>
        deviceUpload(connection, file, (progress: UploadProgress) =>
          setUpload({
            file,
            sentBytes: progress.sentBytes,
            totalBytes: progress.totalBytes,
            resyncs: progress.resyncs,
            done: false,
          }),
        ),
      (result) => {
        setUpload((previous) => (previous ? { ...previous, done: true } : previous));
        setMessage(
          result.installed
            ? `Installed ${baseName(file)} at ${result.location}.`
            : `The device accepted ${baseName(file)} but did not report it installed.`,
        );
        void refreshLibrary(true);
      },
    );
  };

  const remove = (contentId: string, title: string) =>
    run("delete", () => deviceDelete(connection, contentId), () => {
      setMessage(`Deleted ${title || contentId}.`);
      void refreshLibrary(true);
    });

  const abort = () => {
    const contentId = abortId.trim();
    if (contentId === "") return;
    return run("abort", () => deviceAbort(connection, contentId), () => {
      setMessage(`Abandoned any interrupted upload of ${contentId}.`);
      setAbortId("");
    });
  };

  return (
    <div className="stack">
      <Card
        title="Connection"
        subtitle="The device serves this API only while it is in transfer mode."
      >
        <div className="grid">
          <Field label="Host" hint="The address shown on the device's transfer screen.">
            <input
              value={settings.deviceHost}
              onChange={(e) => update({ deviceHost: e.currentTarget.value })}
              placeholder="10.0.0.42"
              spellCheck={false}
            />
          </Field>
          <Field label="Port">
            <input
              type="number"
              value={settings.devicePort}
              onChange={(e) => update({ devicePort: Number(e.currentTarget.value) || 8080 })}
            />
          </Field>
          <Field
            label="Pairing token"
            hint="From BLE provisioning. Everything except Identify needs it."
            wide
          >
            <input
              type="password"
              value={settings.deviceToken}
              onChange={(e) => update({ deviceToken: e.currentTarget.value })}
              placeholder="(none)"
              spellCheck={false}
            />
          </Field>
        </div>

        <div className="row">
          <button onClick={identify} disabled={!connected || busy !== null}>
            {busy === "identify" ? "Identifying..." : "Identify"}
          </button>
          <button onClick={refreshStatus} disabled={!connected || busy !== null}>
            {busy === "status" ? "Reading..." : "Status"}
          </button>
          <button onClick={() => refreshLibrary()} disabled={!connected || busy !== null}>
            {busy === "library" ? "Listing..." : "Installed content"}
          </button>
        </div>

        {!hasToken && (
          <Note kind="warn">
            No pairing token set. <code>GET /api/device/info</code> (Identify) is the only
            endpoint that works unpaired -- everything else will answer{" "}
            <code>UNAUTHORIZED</code>. Provisioning is documented in{" "}
            <code>docs/provisioning.md</code>; it has paired successfully on hardware but
            has never completed the credential write from a Windows client
            (<code>docs/pending.md</code> section 3).
          </Note>
        )}

        {error != null && <Failure error={error} />}
        {message && <div className="notice notice-good">{message}</div>}
      </Card>

      {info && (
        <Card title="Identity">
          <KeyValues
            rows={[
              ["Name", info.name],
              ["Model", info.model],
              ["Device id", <code key="id">{info.deviceId}</code>],
              ["Firmware", info.firmwareVersion],
              [
                "Last restart",
                info.resetReason
                  ? `${info.resetReason}${info.bootCount !== undefined ? ` (boot ${info.bootCount})` : ""}` +
                    (info.resetReason === "brownout"
                      ? " -- the supply voltage dipped: check the cable and USB port"
                      : info.resetReason === "power-on"
                        ? " -- plugged in, or it lost power"
                        : "")
                  : "not reported by this firmware",
              ],
              ["Protocol", `v${info.protocolVersion}`],
              ["Max chunk", formatBytes(info.maxChunkBytes)],
              [
                "Paired",
                info.paired ? (
                  <Badge tone="good">paired</Badge>
                ) : (
                  <Badge tone="warn">not paired</Badge>
                ),
              ],
            ]}
          />
        </Card>
      )}

      {status && (
        <Card title="Status">
          {status.storage.mounted ? (
            <Meter
              value={status.storage.usedBytes}
              max={status.storage.capacityBytes}
              label={`${formatBytes(status.storage.usedBytes)} used of ${formatBytes(
                status.storage.capacityBytes,
              )} -- ${formatBytes(status.storage.freeBytes)} free`}
            />
          ) : (
            <Note kind="warn">
              The card is not mounted. Content lives on the card, so nothing can be
              installed until it is.
            </Note>
          )}
          <KeyValues
            rows={[
              [
                "Transfer mode",
                status.transferMode ? (
                  <Badge tone="good">on</Badge>
                ) : (
                  <Badge tone="warn">off</Badge>
                ),
              ],
              ["Uptime", formatDuration(status.uptimeSeconds)],
              [
                "Open upload sessions",
                status.openSessions === 0 ? "none" : String(status.openSessions),
              ],
              [
                "Battery",
                status.battery.available
                  ? `${status.battery.millivolts ?? "?"} mV`
                  : "no battery sense on this board revision",
              ],
            ]}
          />
        </Card>
      )}

      <Card
        title="Update firmware"
        subtitle="Over Wi-Fi: the new firmware goes into the device's spare slot, is checked, and only then replaces the running one. The device restarts when it is done."
      >
        {firmware && (
          <Meter
            value={firmware.sentBytes}
            max={firmware.totalBytes || 1}
            label={
              <>
                {baseName(firmware.file)} -- {formatBytes(firmware.sentBytes)}
                {firmware.totalBytes > 0 && ` of ${formatBytes(firmware.totalBytes)}`}
                {firmware.done && " -- installed, restarting"}
              </>
            }
          />
        )}
        <div className="row">
          <PathInput
            className="grow"
            kind="file"
            filters={[{ name: "Firmware image", extensions: ["bin"] }]}
            value={firmwareFile}
            onChange={setFirmwareFile}
            placeholder="firmware/.pio/build/crowpanel_579/firmware.bin"
          />
          <button
            onClick={startFirmwareUpdate}
            disabled={!connected || !hasToken || firmwareFile.trim() === "" || busy !== null}
          >
            {busy === "firmware" ? "Updating..." : "Update firmware"}
          </button>
        </div>
        <Note kind="warn">
          Send <code>firmware.bin</code> from <code>pio run</code>, not a merged image. If the
          new firmware does not start, flash it over USB as before.
        </Note>
      </Card>

      <Card
        title="Install a package"
        subtitle="Resumable: an interrupted transfer picks up from whatever the device already has on disk, not from what this app thinks it sent."
      >
        <div className="row">
          <PathInput
            className="grow"
            kind="file"
            filters={[{ name: "QPK package", extensions: ["qpk"] }]}
            value={uploadFile}
            onChange={(path) => update({ uploadFile: path })}
            placeholder="path to a .qpk file"
          />
          <button
            onClick={startUpload}
            disabled={!connected || uploadFile.trim() === "" || busy !== null}
          >
            {busy === "upload" ? "Uploading..." : "Upload"}
          </button>
        </div>

        {upload && (
          <>
            <Meter
              value={upload.sentBytes}
              max={upload.totalBytes || 1}
              label={
                <>
                  {baseName(upload.file)} -- {formatBytes(upload.sentBytes)}
                  {upload.totalBytes > 0 && ` of ${formatBytes(upload.totalBytes)}`}
                  {upload.done && " -- installed"}
                </>
              }
            />
            {upload.resyncs > 0 && (
              <p className="muted">
                {upload.resyncs} resync{upload.resyncs > 1 ? "s" : ""}: the device
                rewound this transfer to the offset it had actually stored. That is the
                protocol working, not a fault.
              </p>
            )}
          </>
        )}

        <div className="row">
          <input
            className="grow"
            value={abortId}
            onChange={(e) => setAbortId(e.currentTarget.value)}
            placeholder="content id of an interrupted upload to abandon"
            spellCheck={false}
          />
          <button onClick={abort} disabled={!connected || abortId.trim() === "" || busy !== null}>
            {busy === "abort" ? "Abandoning..." : "Abort upload"}
          </button>
        </div>
        <p className="muted">
          The upload session id is the content id, which is why an interrupted transfer
          can be named at all. The Library tab prints it for any local package.
        </p>
      </Card>

      <Card
        title="Installed content"
        actions={
          <button onClick={() => refreshLibrary()} disabled={!connected || busy !== null}>
            Refresh
          </button>
        }
      >
        {!listing && <Empty>Not read yet.</Empty>}
        {listing && listing.items.length === 0 && <Empty>The device's library is empty.</Empty>}
        {listing && listing.items.length > 0 && (
          <div className="table-scroll">
            <table>
              <thead>
                <tr>
                  <th>Title</th>
                  <th>Type</th>
                  <th>Lang</th>
                  <th>Ver</th>
                  <th>Size</th>
                  <th>Installed</th>
                  <th>Location</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {listing.items.map((item) => (
                  <tr key={item.contentId}>
                    <td>{item.title || <span className="muted">{item.contentId}</span>}</td>
                    <td>
                      <Badge>{item.type}</Badge>
                    </td>
                    <td>{item.language}</td>
                    <td>{item.contentVersion}</td>
                    <td className="nowrap">{formatBytes(item.packageSize)}</td>
                    <td className="nowrap">{formatInstalledAt(item.installedAt)}</td>
                    <td>
                      <code>{item.location}</code>
                    </td>
                    <td>
                      <button
                        className="danger"
                        disabled={busy !== null}
                        onClick={() => remove(item.contentId, item.title)}
                      >
                        Delete
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Card>
    </div>
  );
}
