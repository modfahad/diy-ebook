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
//
// Many books go in and out at once. "Add books" is two steps: pick any number
// of files, fill in the details for each (or for all of them at once), then
// convert the lot (addBooks.ts). The ticked rows are sent to the device one
// after another -- strictly one after another: the device keys an upload
// session by content id and resyncs it, and every upload is its own bridge
// process, so sending in parallel would be wrong, not just slower.

import { useCallback, useRef, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import {
  describeFailure,
  deviceUpload,
  documentDetails,
  inspect,
  scanLibrary,
  type DeviceConnection,
  type InspectResult,
  type PackageSummaryRow,
} from "./bridge";
import { addToLibrary, bookKind, BOOK_EXTENSIONS } from "./addBooks";
import { RENDER_CANCELLED } from "./pdfPages";
import { baseName, formatBytes } from "./format";
import { Badge, Card, Empty, Failure, Meter, Note, PathInput } from "./ui";
import { CoverThumb, pictureToCover } from "./cover";
import type { Settings } from "./settings";

/** One file waiting to be added, with the details filled in for it. */
interface BookRow {
  id: number;
  source: string;
  kind: string;
  title: string;
  author: string;
  language: string;
  cover: Uint8Array | null;
  /** True while the file's own title, author and language are being read. */
  reading: boolean;
  state: "ready" | "working" | "added" | "present" | "failed" | "skipped";
  detail?: string;
}

/** One package in a "send to device" run. */
interface SendItem {
  path: string;
  name: string;
  state: "waiting" | "sending" | "sent" | "failed" | "skipped";
  sentBytes: number;
  totalBytes: number;
  detail?: string;
}

/**
 * Failures that will fail every remaining file the same way. A batch stops on
 * these instead of repeating one error once per book.
 */
const STOP_THE_BATCH = new Set([
  "UNAUTHORIZED",
  "DEVICE_UNREACHABLE",
  "NOT_IN_TRANSFER_MODE",
  "NO_SPACE",
  "NODE_NOT_FOUND",
  "LIBRARY_NOT_BUILT",
  "REPO_NOT_FOUND",
]);

/** Rows that still need converting: new, or failed or skipped last time. */
const toConvert = (row: BookRow) =>
  row.state === "ready" || row.state === "failed" || row.state === "skipped";

let nextBookId = 1;

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

  const [selected, setSelected] = useState<Set<string>>(new Set());

  const [books, setBooks] = useState<BookRow[]>([]);
  const [adding, setAdding] = useState(false);
  const [keepPdfLayout, setKeepPdfLayout] = useState(true);
  const [allAuthor, setAllAuthor] = useState("");
  const [allLanguage, setAllLanguage] = useState("");
  const [pickError, setPickError] = useState<string | null>(null);
  const addAbort = useRef<AbortController | null>(null);
  const pictureInput = useRef<HTMLInputElement>(null);
  const pictureFor = useRef<number | null>(null);

  const [sends, setSends] = useState<SendItem[]>([]);
  const [sending, setSending] = useState(false);
  const [sendStop, setSendStop] = useState<unknown>(null);
  const cancelSend = useRef(false);

  const dir = settings.libraryDir.trim();
  const connection: DeviceConnection = {
    host: settings.deviceHost.trim(),
    port: settings.devicePort,
    token: settings.deviceToken.trim(),
  };

  const scan = useCallback(async () => {
    setScanning(true);
    setScanError(null);
    setChecked({});
    try {
      const found = await scanLibrary(dir);
      setRows(found);
      // Keep ticks on packages that are still there, so a rescan after adding
      // books does not throw away a selection.
      const readable = new Set(found.filter((row) => !row.error).map((row) => row.path));
      setSelected((previous) => new Set([...previous].filter((path) => readable.has(path))));
    } catch (error) {
      setRows(null);
      setSelected(new Set());
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

  // --- adding books: pick, fill in, convert --------------------------------------

  const setBook = (id: number, patch: Partial<BookRow>) =>
    setBooks((previous) => previous.map((book) => (book.id === id ? { ...book, ...patch } : book)));

  const chooseBooks = async () => {
    setPickError(null);
    const picked = await open({
      multiple: true,
      directory: false,
      filters: [{ name: "Books and packages", extensions: BOOK_EXTENSIONS }],
    });
    const sources = Array.isArray(picked) ? picked : typeof picked === "string" ? [picked] : [];
    const listed = new Set(books.map((book) => book.source));
    const fresh = [...new Set(sources)]
      .filter((source) => !listed.has(source))
      .map<BookRow>((source) => {
        const kind = bookKind(source);
        return {
          id: nextBookId++,
          source,
          kind,
          title: "",
          author: allAuthor,
          language: allLanguage,
          cover: null,
          reading: kind !== "qpk",
          state: "ready",
        };
      });
    setBooks((previous) => [...previous, ...fresh]);

    // Fill in what each document says about itself, one at a time. Only
    // fields still empty are filled, so nothing typed meanwhile is lost.
    for (const book of fresh) {
      if (!book.reading) continue;
      try {
        const found = await documentDetails(book.source);
        setBooks((previous) =>
          previous.map((row) =>
            row.id !== book.id
              ? row
              : {
                  ...row,
                  reading: false,
                  title: row.title || found.title || "",
                  author: row.author || found.author || "",
                  language: row.language || found.language || "",
                },
          ),
        );
      } catch (failure) {
        setBook(book.id, {
          reading: false,
          detail: `Could not read its details: ${describeFailure(failure).message}`,
        });
      }
    }
  };

  const fillEveryBook = () =>
    setBooks((previous) =>
      previous.map((book) =>
        book.kind === "qpk" || !toConvert(book)
          ? book
          : { ...book, author: allAuthor, language: allLanguage },
      ),
    );

  const choosePicture = (file: File | undefined) => {
    const id = pictureFor.current;
    pictureFor.current = null;
    if (!file || id === null) return;
    pictureToCover(file)
      .then((cover) => setBook(id, { cover }))
      .catch((failure: unknown) =>
        setBook(id, {
          detail: `Could not open ${file.name}: ${failure instanceof Error ? failure.message : String(failure)}`,
        }),
      );
  };

  const convertBooks = async () => {
    const batch = books.filter(toConvert);
    if (batch.length === 0) return;
    setAdding(true);
    const controller = new AbortController();
    addAbort.current = controller;
    for (const book of batch) setBook(book.id, { state: "ready", detail: undefined });
    try {
      for (const book of batch) {
        if (controller.signal.aborted) {
          setBook(book.id, { state: "skipped", detail: "cancelled" });
          continue;
        }
        setBook(book.id, { state: "working", detail: "starting" });
        try {
          const result = await addToLibrary(book.source, dir, {
            keepPdfLayout,
            details: book,
            signal: controller.signal,
            onStage: (stage) => setBook(book.id, { detail: stage }),
          });
          setBook(book.id, { state: result.added ? "added" : "present", detail: result.path });
        } catch (failure) {
          const message =
            failure instanceof Error ? failure.message : describeFailure(failure).message;
          const cancelled = message === RENDER_CANCELLED || controller.signal.aborted;
          setBook(book.id, {
            state: cancelled ? "skipped" : "failed",
            detail: cancelled ? "cancelled" : message,
          });
        }
      }
    } finally {
      addAbort.current = null;
      setAdding(false);
      await scan();
    }
  };

  // --- sending to the device ---------------------------------------------------

  const setSend = (index: number, patch: Partial<SendItem>) =>
    setSends((previous) => previous.map((item, i) => (i === index ? { ...item, ...patch } : item)));

  const sendSelected = async () => {
    const batch = (rows ?? []).filter((row) => !row.error && selected.has(row.path));
    if (batch.length === 0) return;

    setSends(
      batch.map((row) => ({
        path: row.path,
        name: row.title ?? row.file,
        state: "waiting",
        sentBytes: 0,
        totalBytes: row.packageSize,
      })),
    );
    setSendStop(null);
    setSending(true);
    cancelSend.current = false;
    let stopped = false;
    try {
      for (const [index, row] of batch.entries()) {
        if (stopped || cancelSend.current) {
          setSend(index, { state: "skipped", detail: stopped ? "not sent" : "cancelled" });
          continue;
        }
        setSend(index, { state: "sending" });
        try {
          const result = await deviceUpload(connection, row.path, (progress) =>
            setSend(index, { sentBytes: progress.sentBytes, totalBytes: progress.totalBytes }),
          );
          setSend(index, {
            state: "sent",
            sentBytes: result.packageBytes,
            totalBytes: result.packageBytes,
            detail: result.installed ? result.location : "accepted, but not reported installed",
          });
        } catch (failure) {
          const { code, message } = describeFailure(failure);
          setSend(index, { state: "failed", detail: `${code} ${message}` });
          if (STOP_THE_BATCH.has(code)) {
            stopped = true;
            setSendStop(failure);
          }
        }
      }
    } finally {
      setSending(false);
    }
  };

  const readable = rows?.filter((row) => !row.error) ?? [];
  const totalBytes = readable.reduce((sum, row) => sum + row.packageSize, 0);
  const picked = readable.filter((row) => selected.has(row.path));
  const pickedBytes = picked.reduce((sum, row) => sum + row.packageSize, 0);
  const allPicked = readable.length > 0 && picked.length === readable.length;

  const toggle = (path: string) =>
    setSelected((previous) => {
      const next = new Set(previous);
      if (next.has(path)) next.delete(path);
      else next.add(path);
      return next;
    });

  const sendState = new Map(sends.map((item) => [item.path, item.state]));
  const sendDone = sends.filter((item) => item.state === "sent").length;
  const sendBytes = sends.reduce(
    (sum, item) => sum + (item.state === "sent" ? item.totalBytes : item.sentBytes),
    0,
  );
  const sendTotal = sends.reduce((sum, item) => sum + item.totalBytes, 0);
  const current = sends.find((item) => item.state === "sending");

  const pending = books.filter(toConvert);
  const stillReading = books.some((book) => book.reading);
  const remaining = books.filter((book) => book.state === "ready" || book.state === "working").length;
  const finished = books.filter((book) => book.state === "added" || book.state === "present");

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
            value={settings.libraryDir}
            onChange={(path) => update({ libraryDir: path })}
            placeholder="E:\LIBRARY   or   C:\...\epaper\sdcard-staging\LIBRARY"
          />
          <button type="submit" disabled={scanning || adding || dir === ""}>
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

      <Card
        title="Add books"
        subtitle="Choose any number of files, fill in their details, then convert them all at once. PDF, EPUB and TXT are converted and validated; .qpk packages are copied as they are. Each lands in this library folder under BOOKS, QURAN, TRANSLATIONS or TAFSIR."
      >
        <div className="row wrap">
          <button
            type="button"
            className={books.length === 0 ? "primary" : undefined}
            onClick={() => {
              chooseBooks().catch((failure) => setPickError(String(failure)));
            }}
            disabled={dir === "" || adding}
          >
            {books.length === 0 ? "Choose books..." : "Choose more..."}
          </button>
          {books.length > 0 && !adding && (
            <>
              <button type="button" onClick={() => setBooks([])}>
                Clear list
              </button>
              {finished.length > 0 && (
                <button
                  type="button"
                  onClick={() =>
                    setBooks((previous) =>
                      previous.filter((book) => book.state !== "added" && book.state !== "present"),
                    )
                  }
                >
                  Remove finished
                </button>
              )}
            </>
          )}
        </div>
        {dir === "" && <Note>Choose a library folder above first -- books are added to it.</Note>}
        {pickError && <Note kind="error">{pickError}</Note>}

        {books.length > 0 && (
          <>
            <div className="book-all">
              <span className="field-label">For every book</span>
              <input
                value={allAuthor}
                onChange={(e) => setAllAuthor(e.currentTarget.value)}
                placeholder="Author"
                disabled={adding}
              />
              <input
                value={allLanguage}
                onChange={(e) => setAllLanguage(e.currentTarget.value)}
                placeholder="Language, e.g. en"
                disabled={adding}
              />
              <button type="button" onClick={fillEveryBook} disabled={adding}>
                Fill in every book
              </button>
            </div>

            <div className="book-list">
              {books.map((book) => {
                const editable = book.kind !== "qpk" && toConvert(book) && !adding;
                return (
                  <div key={book.id} className={book.state === "failed" ? "book book-bad" : "book"}>
                    <div className="book-cover">
                      {book.kind === "qpk" ? (
                        <Badge>package</Badge>
                      ) : (
                        <>
                          <CoverThumb
                            levels={book.cover}
                            title={book.title || baseName(book.source)}
                            scale={0.45}
                          />
                          <div className="row">
                            <button
                              type="button"
                              className="small"
                              disabled={!editable}
                              onClick={() => {
                                pictureFor.current = book.id;
                                pictureInput.current?.click();
                              }}
                            >
                              Cover...
                            </button>
                            {book.cover && (
                              <button
                                type="button"
                                className="small"
                                disabled={!editable}
                                onClick={() => setBook(book.id, { cover: null })}
                                aria-label="Remove the chosen cover"
                              >
                                x
                              </button>
                            )}
                          </div>
                        </>
                      )}
                    </div>

                    <div className="book-fields">
                      <div className="row">
                        <code className="grow" title={book.source}>
                          {baseName(book.source)}
                        </code>
                        <Badge>{book.kind.toUpperCase() || "?"}</Badge>
                      </div>
                      {book.kind === "qpk" ? (
                        <p className="muted">Copied as it is -- a package keeps its own details.</p>
                      ) : (
                        <div className="book-inputs">
                          <input
                            className="book-title"
                            value={book.title}
                            onChange={(e) => setBook(book.id, { title: e.currentTarget.value })}
                            placeholder={book.reading ? "Reading details..." : "Title"}
                            disabled={!editable}
                          />
                          <input
                            value={book.author}
                            onChange={(e) => setBook(book.id, { author: e.currentTarget.value })}
                            placeholder="Author"
                            disabled={!editable}
                          />
                          <input
                            value={book.language}
                            onChange={(e) => setBook(book.id, { language: e.currentTarget.value })}
                            placeholder="Language"
                            disabled={!editable}
                          />
                        </div>
                      )}
                      {book.detail && book.state !== "ready" && (
                        <span className={book.state === "failed" ? "bad" : "muted"}>
                          {book.state === "added" || book.state === "present" ? (
                            <code>{book.detail}</code>
                          ) : (
                            book.detail
                          )}
                        </span>
                      )}
                      {book.detail && book.state === "ready" && (
                        <span className="bad">{book.detail}</span>
                      )}
                    </div>

                    <div className="book-state">
                      {book.state === "ready" && <span className="muted">ready</span>}
                      {book.state === "working" && <Badge>working</Badge>}
                      {book.state === "added" && <Badge tone="good">added</Badge>}
                      {book.state === "present" && <Badge>already in library</Badge>}
                      {book.state === "failed" && <Badge tone="bad">failed</Badge>}
                      {book.state === "skipped" && <Badge tone="warn">skipped</Badge>}
                      {!adding && (
                        <button
                          type="button"
                          className="small"
                          onClick={() =>
                            setBooks((previous) => previous.filter((other) => other.id !== book.id))
                          }
                          aria-label={`Remove ${baseName(book.source)} from the list`}
                        >
                          Remove
                        </button>
                      )}
                    </div>
                  </div>
                );
              })}
            </div>
            <input
              ref={pictureInput}
              type="file"
              accept="image/*"
              hidden
              onChange={(e) => {
                choosePicture(e.currentTarget.files?.[0]);
                e.currentTarget.value = "";
              }}
            />

            <div className="row wrap">
              <button
                className="primary"
                onClick={() => void convertBooks()}
                disabled={dir === "" || adding || sending || pending.length === 0 || stillReading}
              >
                {adding
                  ? `Converting... ${remaining} left`
                  : `Convert & add ${pending.length} book${pending.length === 1 ? "" : "s"}`}
              </button>
              {adding && (
                <button type="button" onClick={() => addAbort.current?.abort()}>
                  Cancel
                </button>
              )}
              <label className="check">
                <input
                  type="checkbox"
                  checked={keepPdfLayout}
                  disabled={adding}
                  onChange={(e) => setKeepPdfLayout(e.currentTarget.checked)}
                />
                <span>
                  Keep each PDF's page layout{" "}
                  <span className="muted">(pages as pictures; slower to convert)</span>
                </span>
              </label>
            </div>
            <p className="muted">
              An EPUB without a chosen cover uses its own. To preview pages before writing,
              convert that book in the Converter tab.
            </p>
          </>
        )}
      </Card>

      {rows && rows.length === 0 && (
        <Card>
          <Empty>
            No <code>.qpk</code> files under that folder. Add books above, or point it at
            the staged content in this checkout, <code>sdcard-staging/LIBRARY</code>.
          </Empty>
        </Card>
      )}

      {(sending || sends.length > 0) && (
        <Card
          title="Sending to the device"
          actions={
            sending ? (
              <button onClick={() => (cancelSend.current = true)}>Stop after this one</button>
            ) : (
              <button onClick={() => setSends([])}>Dismiss</button>
            )
          }
        >
          <Meter
            value={sendBytes}
            max={sendTotal || 1}
            label={
              <>
                {sendDone} of {sends.length} sent -- {formatBytes(sendBytes)} of{" "}
                {formatBytes(sendTotal)}
                {current && ` -- now ${current.name}`}
              </>
            }
          />
          {sendStop != null && <Failure error={sendStop} />}
          <ul className="status-list">
            {sends.map((item) => (
              <li key={item.path}>
                {item.state === "waiting" && <span className="muted">waiting</span>}
                {item.state === "sending" && (
                  <Badge>
                    {formatBytes(item.sentBytes)} of {formatBytes(item.totalBytes)}
                  </Badge>
                )}
                {item.state === "sent" && <Badge tone="good">sent</Badge>}
                {item.state === "failed" && <Badge tone="bad">failed</Badge>}
                {item.state === "skipped" && <Badge tone="warn">{item.detail}</Badge>}{" "}
                {item.name}
                {(item.state === "sent" || item.state === "failed") && item.detail && (
                  <span className={item.state === "failed" ? "bad" : "muted"}> -- {item.detail}</span>
                )}
              </li>
            ))}
          </ul>
        </Card>
      )}

      {rows && rows.length > 0 && (
        <Card
          title="Packages"
          actions={
            <>
              <span className="muted">
                {picked.length === 0
                  ? "Tick packages to send several at once"
                  : `${picked.length} selected -- ${formatBytes(pickedBytes)}`}
              </span>
              <button
                className="primary"
                onClick={() => void sendSelected()}
                disabled={picked.length === 0 || sending || adding || connection.host === ""}
              >
                {sending
                  ? "Sending..."
                  : picked.length > 0
                    ? `Send ${picked.length} to device`
                    : "Send to device"}
              </button>
            </>
          }
        >
          {connection.host === "" && picked.length > 0 && (
            <Note kind="warn">
              No device address yet.{" "}
              <button type="button" onClick={() => onGoToTab?.("device")}>
                Set up the device
              </button>
            </Note>
          )}
          <div className="table-scroll">
            <table>
              <thead>
                <tr>
                  <th className="pick">
                    <input
                      type="checkbox"
                      aria-label="Select every package"
                      checked={allPicked}
                      ref={(box) => {
                        if (box) box.indeterminate = picked.length > 0 && !allPicked;
                      }}
                      disabled={readable.length === 0 || sending}
                      onChange={() =>
                        setSelected(allPicked ? new Set() : new Set(readable.map((row) => row.path)))
                      }
                    />
                  </th>
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
                  const sent = sendState.get(row.path);
                  const rowClass = row.error
                    ? "row-bad"
                    : selected.has(row.path)
                      ? "row-picked"
                      : undefined;
                  return (
                    <tr key={row.path} className={rowClass}>
                      <td className="pick">
                        <input
                          type="checkbox"
                          aria-label={`Select ${row.title ?? row.file}`}
                          checked={selected.has(row.path)}
                          disabled={!!row.error || sending}
                          onChange={() => toggle(row.path)}
                        />
                      </td>
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
                        {sent === "sent" && (
                          <>
                            {" "}
                            <Badge tone="good">on device</Badge>
                          </>
                        )}
                        {sent === "failed" && (
                          <>
                            {" "}
                            <Badge tone="bad">send failed</Badge>
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
