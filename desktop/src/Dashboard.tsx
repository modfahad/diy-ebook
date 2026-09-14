// The Dashboard: what works right now, and what does not.
//
// This tab exists because the app has two hard runtime dependencies that are
// invisible until something fails -- Node on PATH, and the two TypeScript
// libraries being built. Discovering either of those from a failed conversion
// halfway through an import is a bad way to find out, so the app checks on
// open and says so plainly.
//
// It also carries the honest status of the milestone: the Library and
// Converter paths run against real content on this machine, while everything
// on the Device tab past Identify has never been exercised against hardware.

import { useCallback, useEffect, useState } from "react";
import {
  bridgeEnvironment,
  deviceInfo,
  ping,
  scanLibrary,
  type BridgeEnvironment,
  type BridgePing,
  type DeviceInfo,
  type PackageSummaryRow,
} from "./bridge";
import { formatBytes } from "./format";
import { Badge, Card, Empty, Failure, KeyValues, Note } from "./ui";
import type { Settings } from "./settings";

interface LibrarySummary {
  total: number;
  bytes: number;
  byType: Record<string, number>;
  unreadable: number;
}

function summarise(rows: PackageSummaryRow[]): LibrarySummary {
  const byType: Record<string, number> = {};
  let bytes = 0;
  let unreadable = 0;
  for (const row of rows) {
    if (row.error) {
      unreadable++;
      continue;
    }
    byType[row.packageType] = (byType[row.packageType] ?? 0) + 1;
    bytes += row.packageSize;
  }
  return { total: rows.length, bytes, byType, unreadable };
}

export default function Dashboard({
  settings,
  onGoToTab,
}: {
  settings: Settings;
  onGoToTab: (tab: "library" | "device" | "converter") => void;
}) {
  const [environment, setEnvironment] = useState<BridgeEnvironment | null>(null);
  const [health, setHealth] = useState<BridgePing | null>(null);
  const [healthError, setHealthError] = useState<unknown>(null);
  const [library, setLibrary] = useState<LibrarySummary | null>(null);
  const [libraryError, setLibraryError] = useState<string | null>(null);
  const [device, setDevice] = useState<DeviceInfo | null>(null);
  const [deviceError, setDeviceError] = useState<unknown>(null);
  const [checking, setChecking] = useState(false);

  const check = useCallback(async () => {
    setChecking(true);
    setHealthError(null);
    try {
      setEnvironment(await bridgeEnvironment());
      setHealth(await ping());
    } catch (error) {
      setHealth(null);
      setHealthError(error);
    } finally {
      setChecking(false);
    }
  }, []);

  useEffect(() => {
    void check();
  }, [check]);

  useEffect(() => {
    const dir = settings.libraryDir.trim();
    if (dir === "") {
      setLibrary(null);
      setLibraryError(null);
      return;
    }
    let cancelled = false;
    scanLibrary(dir)
      .then((rows) => {
        if (!cancelled) {
          setLibrary(summarise(rows));
          setLibraryError(null);
        }
      })
      .catch((error: unknown) => {
        if (!cancelled) {
          setLibrary(null);
          setLibraryError(String(error));
        }
      });
    return () => {
      cancelled = true;
    };
  }, [settings.libraryDir]);

  const identify = async () => {
    setDeviceError(null);
    try {
      setDevice(
        await deviceInfo({
          host: settings.deviceHost.trim(),
          port: settings.devicePort,
          token: settings.deviceToken.trim(),
        }),
      );
    } catch (error) {
      setDevice(null);
      setDeviceError(error);
    }
  };

  const converterBuilt = health?.libraries.converter?.built ?? false;
  const clientBuilt = health?.libraries["device-client"]?.built ?? false;

  return (
    <div className="stack">
      <Card
        title="Toolchain"
        subtitle="The converter and the device client are Node libraries this app drives as a subprocess. Both must be built."
        actions={
          <button onClick={check} disabled={checking}>
            {checking ? "Checking..." : "Recheck"}
          </button>
        }
      >
        <KeyValues
          rows={[
            [
              "Project checkout",
              environment?.repoRoot ? (
                <code>{environment.repoRoot}</code>
              ) : (
                <Badge tone="bad">not found</Badge>
              ),
            ],
            ["Node", health ? <code>{health.node}</code> : <Badge tone="bad">unavailable</Badge>],
            [
              "converter",
              converterBuilt ? <Badge tone="good">built</Badge> : <Badge tone="bad">not built</Badge>,
            ],
            [
              "device-client",
              clientBuilt ? <Badge tone="good">built</Badge> : <Badge tone="bad">not built</Badge>,
            ],
          ]}
        />

        {environment && !environment.found && (
          <Note kind="warn">{environment.problem}</Note>
        )}
        {healthError != null && <Failure error={healthError} />}
        {health && (!converterBuilt || !clientBuilt) && (
          <Note kind="warn">
            <p>Build what is missing, from the project root:</p>
            <pre>
              {!converterBuilt &&
                "npm install --prefix desktop/converter\nnpm run build --prefix desktop/converter\n"}
              {!clientBuilt &&
                "npm install --prefix desktop/device-client\nnpm run build --prefix desktop/device-client"}
            </pre>
            {!converterBuilt && health.libraries.converter?.error && (
              <details>
                <summary>Why the converter did not load</summary>
                <pre>{health.libraries.converter.error}</pre>
              </details>
            )}
          </Note>
        )}
      </Card>

      <div className="columns">
        <Card
          title="Library"
          subtitle="A folder laid out the way the device's card is."
          actions={<button onClick={() => onGoToTab("library")}>Open</button>}
        >
          {settings.libraryDir.trim() === "" && (
            <Empty>No folder chosen yet. Pick one in the Library tab.</Empty>
          )}
          {libraryError && <Note kind="warn">{libraryError}</Note>}
          {library && (
            <>
              <p className="big">
                {library.total} package{library.total === 1 ? "" : "s"}
                <span className="muted"> -- {formatBytes(library.bytes)}</span>
              </p>
              <div className="row wrap">
                {Object.entries(library.byType).map(([type, count]) => (
                  <Badge key={type}>
                    {type} {count}
                  </Badge>
                ))}
                {library.unreadable > 0 && (
                  <Badge tone="bad">{library.unreadable} unreadable</Badge>
                )}
              </div>
              <p className="muted">
                <code>{settings.libraryDir}</code>
              </p>
            </>
          )}
        </Card>

        <Card
          title="Device"
          subtitle="Reachable only while the device is in transfer mode."
          actions={<button onClick={() => onGoToTab("device")}>Open</button>}
        >
          {settings.deviceHost.trim() === "" ? (
            <Empty>No device address set.</Empty>
          ) : (
            <>
              <p className="big">
                <code>
                  {settings.deviceHost}:{settings.devicePort}
                </code>
              </p>
              <div className="row wrap">
                <button onClick={identify}>Identify</button>
                {settings.deviceToken.trim() === "" ? (
                  <Badge tone="warn">no pairing token</Badge>
                ) : (
                  <Badge tone="good">token set</Badge>
                )}
              </div>
              {device && (
                <p className="muted">
                  {device.name} -- {device.model}, firmware {device.firmwareVersion},{" "}
                  {device.paired ? "paired" : "not paired"}
                </p>
              )}
              {deviceError != null && <Failure error={deviceError} />}
            </>
          )}
        </Card>
      </div>

      <Card title="Where this is" subtitle="Milestone 5, desktop side.">
        <ul className="status-list">
          <li>
            <Badge tone="good">works</Badge> <strong>Library</strong> -- reads real QPK1
            packages off a folder or a card, header and metadata, in Rust.
          </li>
          <li>
            <Badge tone="good">works</Badge> <strong>Converter</strong> -- the full
            pipeline: parse, structure, validate twice, preview, then write. Run against
            real documents on this machine.
          </li>
          <li>
            <Badge tone="warn">partly proven</Badge> <strong>Device</strong> -- Identify
            (<code>GET /api/device/info</code>) has answered from real hardware over real
            Wi-Fi. Status, listing, upload, delete and abort are implemented and
            host-tested but have <em>never</em> run against a device, because every one
            of them needs a pairing token and BLE provisioning has never delivered one.
            See <code>docs/pending.md</code> section 3.
          </li>
        </ul>
      </Card>
    </div>
  );
}
