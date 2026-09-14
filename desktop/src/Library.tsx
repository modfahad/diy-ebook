// The Library tab: what is on a card, or in a folder laid out like one.
//
// The scan is Rust (src-tauri/src/library.rs), not the Node bridge: it is
// plain filesystem work and an independent reader of the same QPK1 header the
// TypeScript and C++ readers implement. A package that fails to parse is
// listed as an error row rather than skipped, so a corrupt file on the card is
// visible instead of silently absent.
//
// Validating a row goes the other way, through the bridge, because that is the
// converter's full `validatePackage` -- every checksum and a full index sweep,
// plus a second pass in the firmware's own weaker profile. Browsing and
// validating are deliberately different jobs.

import { useCallback, useState } from "react";
import { inspect, scanLibrary, type InspectResult, type PackageSummaryRow } from "./bridge";
import { formatBytes } from "./format";
import { Badge, Card, Empty, Failure, Note, PathInput } from "./ui";
import { CoverThumb } from "./cover";
import type { Settings } from "./settings";

export default function Library({
  settings,
  update,
  onGoToTab,
}: {
  settings: Settings;
  update: (patch: Partial<Settings>) => void;
  onGoToTab?: (tab: "device") => void;
}) {
  const [rows, setRows] = useState<PackageSummaryRow[] | null>(null);
  const [scanError, setScanError] = useState<string | null>(null);
  const [scanning, setScanning] = useState(false);
  const [checked, setChecked] = useState<Record<string, InspectResult | { failure: unknown }>>({});
  const [checking, setChecking] = useState<string | null>(null);

  const dir = settings.libraryDir;

  const scan = useCallback(async () => {
    setScanning(true);
    setScanError(null);
    setChecked({});
    try {
      setRows(await scanLibrary(dir));
    } catch (error) {
      setRows(null);
      setScanError(String(error));
    } finally {
      setScanning(false);
    }
  }, [dir]);

  const validate = async (row: PackageSummaryRow) => {
    setChecking(row.path);
    try {
      const result = await inspect(row.path);
      setChecked((previous) => ({ ...previous, [row.path]: result }));
    } catch (failure) {
      setChecked((previous) => ({ ...previous, [row.path]: { failure } }));
    } finally {
      setChecking(null);
    }
  };

  const sendToDevice = (row: PackageSummaryRow) => {
    update({ uploadFile: row.path });
    onGoToTab?.("device");
  };

  const readable = rows?.filter((row) => !row.error) ?? [];
  const totalBytes = readable.reduce((sum, row) => sum + row.packageSize, 0);

  return (
    <div className="stack">
      <Card
        title="Scan a folder"
        subtitle="Looks two levels deep, matching the card's own layout: LIBRARY/{QURAN,BOOKS,TRANSLATIONS,TAFSIR}/*.qpk"
      >
        <form
          className="row"
          onSubmit={(e) => {
            e.preventDefault();
            void scan();
          }}
        >
          <PathInput
            className="grow"
            kind="folder"
            value={dir}
            onChange={(path) => update({ libraryDir: path })}
            placeholder="E:\LIBRARY   or   C:\...\epaper\sdcard-staging\LIBRARY"
          />
          <button type="submit" disabled={scanning || dir.trim() === ""}>
            {scanning ? "Scanning..." : "Scan"}
          </button>
        </form>

        {scanError && <Note kind="warn">{scanError}</Note>}
        {rows && (
          <p className="muted">
            {rows.length} package{rows.length === 1 ? "" : "s"} -- {formatBytes(totalBytes)}
            {rows.length > readable.length &&
              ` -- ${rows.length - readable.length} could not be read`}
          </p>
        )}
      </Card>

      {rows && rows.length === 0 && (
        <Card>
          <Empty>
            No <code>.qpk</code> files under that folder. The staged content in this
            checkout lives at <code>sdcard-staging/LIBRARY</code>.
          </Empty>
        </Card>
      )}

      {rows && rows.length > 0 && (
        <Card title="Packages">
          <div className="table-scroll">
            <table>
              <thead>
                <tr>
                  <th>Cover</th>
                  <th>Title</th>
                  <th>Type</th>
                  <th>Author</th>
                  <th>Lang</th>
                  <th>Chapters</th>
                  <th>Ayat</th>
                  <th>Ver</th>
                  <th>Size</th>
                  <th>File</th>
                  <th />
                </tr>
              </thead>
              <tbody>
                {rows.map((row) => {
                  const check = checked[row.path];
                  return (
                    <tr key={row.path} className={row.error ? "row-bad" : undefined}>
                      <td>
                        <CoverThumb payload={row.cover} title={row.title} scale={0.5} />
                      </td>
                      <td>
                        {row.error ? (
                          <span className="bad" title={row.error}>
                            (unreadable)
                          </span>
                        ) : (
                          row.title ?? <span className="muted">(no title)</span>
                        )}
                        {check && "validation" in check && (
                          <>
                            {" "}
                            {check.validation.ok ? (
                              <Badge tone="good">valid</Badge>
                            ) : (
                              <Badge tone="bad">invalid</Badge>
                            )}
                          </>
                        )}
                      </td>
                      <td>
                        <Badge>{row.packageType}</Badge>
                      </td>
                      <td>{row.author ?? ""}</td>
                      <td>{row.language ?? ""}</td>
                      <td>{row.chapterCount || ""}</td>
                      <td>{row.ayahCount ?? ""}</td>
                      <td>{row.contentVersion || ""}</td>
                      <td className="nowrap">{formatBytes(row.packageSize)}</td>
                      <td>
                        <code title={`${row.path}\ncontent id ${row.contentId}`}>
                          {row.file}
                        </code>
                      </td>
                      <td className="row-actions">
                        <button
                          onClick={() => validate(row)}
                          disabled={checking !== null || !!row.error}
                        >
                          {checking === row.path ? "..." : "Validate"}
                        </button>
                        <button onClick={() => sendToDevice(row)} disabled={!!row.error}>
                          Send to device
                        </button>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>

          {Object.entries(checked).map(([path, check]) => {
            if ("failure" in check) {
              return (
                <div key={path}>
                  <p className="muted">{path}</p>
                  <Failure error={check.failure} />
                </div>
              );
            }
            if (check.validation.ok && check.validation.warnings.length === 0) return null;
            return (
              <Note key={path} kind={check.validation.ok ? "warn" : "error"}>
                <p>
                  <code>{path}</code>
                </p>
                <ul>
                  {check.validation.errors.map((line) => (
                    <li key={line} className="bad">
                      {line}
                    </li>
                  ))}
                  {check.validation.warnings.map((line) => (
                    <li key={line}>{line}</li>
                  ))}
                </ul>
              </Note>
            );
          })}
        </Card>
      )}
    </div>
  );
}
