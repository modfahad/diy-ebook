// The Converter tab: a document in, a validated QPK1 package out.
//
// The order on screen is the order the spec requires and the CLI already
// enforces -- convert, validate, preview, *then* write. "Validate & preview"
// is a dry run: it does everything except touch the disk, so the report and
// the preview a person looks at describe exactly the bytes "Convert & write"
// will then produce. (The bridge has a test asserting those two runs agree
// byte for byte; without that, previewing before installing would be
// previewing something else.)
//
// One rule is the pipeline's, not this UI's, and is worth seeing rather than
// discovering: a Quran package can only be built from a structured Quran JSON
// source. Extracting scripture from a PDF with layout heuristics produces a
// package that validates perfectly and is wrong.

import { useCallback, useEffect, useRef, useState } from "react";
import {
  convert,
  coverSource,
  readInputFile,
  type ConvertResult,
  type PagePreview,
  previewPackage,
} from "./bridge";
import { CoverThumb, base64ToBytes, coverPayloadBase64, pictureToCover } from "./cover";
import { bytesToBase64, pageImageDataUrl, renderPdfPages } from "./pdfPages";
import { formatBytes, formatPercent } from "./format";
import { Card, Empty, Failure, Field, KeyValues, Note, PathInput } from "./ui";
import type { Settings } from "./settings";

type ContentTypeChoice = "auto" | "BOOK" | "QURAN";
type InputKindChoice = "auto" | "txt" | "pdf" | "epub" | "quran-json";

function isPreview(value: ConvertResult["preview"]): value is PagePreview {
  return value !== null && !("error" in value);
}

export default function Converter({
  settings,
  update,
}: {
  settings: Settings;
  update: (patch: Partial<Settings>) => void;
}) {
  const [output, setOutput] = useState("");
  const [title, setTitle] = useState("");
  const [author, setAuthor] = useState("");
  const [language, setLanguage] = useState("");
  const [contentVersion, setContentVersion] = useState(1);
  const [contentType, setContentType] = useState<ContentTypeChoice>("auto");
  const [inputKind, setInputKind] = useState<InputKindChoice>("auto");
  const [includeWordLayout, setIncludeWordLayout] = useState(true);
  const [singleChapter, setSingleChapter] = useState(false);

  const [result, setResult] = useState<ConvertResult | null>(null);
  const [preview, setPreview] = useState<PagePreview | null>(null);
  const [page, setPage] = useState(1);
  const [stage, setStage] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);

  const input = settings.converterInput.trim();

  // The package's cover picture, as four-grey levels. "epub" covers follow
  // the input file and are dropped when it changes; a chosen picture stays.
  const [cover, setCover] = useState<{ levels: Uint8Array; origin: "epub" | "picture" } | null>(
    null,
  );
  const [coverNote, setCoverNote] = useState<string | null>(null);
  const pictureInput = useRef<HTMLInputElement>(null);
  const coverBase64 = cover ? coverPayloadBase64(cover.levels) : null;

  useEffect(() => {
    setCover((current) => (current?.origin === "epub" ? null : current));
    setCoverNote(null);
    if (!/\.epub$/iu.test(input)) return;
    let cancelled = false;
    const timer = setTimeout(() => {
      coverSource(input)
        .then(async (found) => {
          if (cancelled) return;
          if (!found) {
            setCoverNote("This EPUB declares no cover picture. Choose one, or the device draws a cover.");
            return;
          }
          const bytes = base64ToBytes(found.data);
          const levels = await pictureToCover(
            new Blob([bytes.buffer as ArrayBuffer], { type: found.mediaType }),
          );
          if (cancelled) return;
          setCover((current) => (current?.origin === "picture" ? current : { levels, origin: "epub" }));
          setCoverNote("Cover taken from the EPUB.");
        })
        .catch((failure: unknown) => {
          if (!cancelled) {
            setCoverNote(
              `Could not use the EPUB's cover: ${failure instanceof Error ? failure.message : String(failure)}`,
            );
          }
        });
    }, 400);
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [input]);

  // A PDF's pages as pictures (pdfPages.ts), rendered once per input and
  // trim setting and reused by every dry run, write and page turn after that.
  const [keepLayout, setKeepLayout] = useState(true);
  const [trimMargins, setTrimMargins] = useState(true);
  const renderAbort = useRef<AbortController | null>(null);
  const [pageImages, setPageImages] = useState<{
    key: string;
    pages: Uint8Array[];
    base64: string[];
    bytes: number;
  } | null>(null);
  const isPdf = inputKind === "pdf" || (inputKind === "auto" && /\.pdf$/iu.test(input));
  const imagesKey = `${input}|${trimMargins}`;

  const ensurePageImages = async (): Promise<string[] | null> => {
    if (!isPdf || !keepLayout) return null;
    if (pageImages?.key === imagesKey) return pageImages.base64;
    setStage("reading the PDF");
    const file = await readInputFile(input);
    const controller = new AbortController();
    renderAbort.current = controller;
    const started = Date.now();
    let pages: Uint8Array[];
    try {
      pages = await renderPdfPages(base64ToBytes(file.data), {
        trimMargins,
        signal: controller.signal,
        onProgress: (done, total) => {
          const seconds = (Date.now() - started) / 1000;
          const left = done > 0 ? Math.round((seconds / done) * (total - done)) : 0;
          setStage(`rendering page ${done} of ${total}${done > 3 ? `, about ${left}s left` : ""}`);
        },
      });
    } finally {
      renderAbort.current = null;
    }
    const base64 = pages.map(bytesToBase64);
    setPageImages({
      key: imagesKey,
      pages,
      base64,
      bytes: pages.reduce((sum, page) => sum + page.length, 0),
    });
    return base64;
  };

  const choosePicture = (file: File | undefined) => {
    if (!file) return;
    pictureToCover(file)
      .then((levels) => {
        setCover({ levels, origin: "picture" });
        setCoverNote(`Cover from ${file.name}.`);
      })
      .catch((failure: unknown) =>
        setCoverNote(
          `Could not open ${file.name}: ${failure instanceof Error ? failure.message : String(failure)}`,
        ),
      );
  };

  const request = useCallback(
    (dryRun: boolean, previewPage: number) => ({
      input,
      dryRun,
      previewPage,
      ...(output.trim() !== "" ? { output: output.trim() } : {}),
      ...(title.trim() !== "" ? { title: title.trim() } : {}),
      ...(author.trim() !== "" ? { author: author.trim() } : {}),
      ...(language.trim() !== "" ? { language: language.trim() } : {}),
      ...(contentVersion !== 1 ? { contentVersion } : {}),
      ...(contentType !== "auto" ? { contentType } : {}),
      ...(inputKind !== "auto" ? { inputKind } : {}),
      ...(includeWordLayout ? {} : { includeWordLayout: false }),
      ...(singleChapter ? { singleChapter: true } : {}),
      ...(coverBase64 ? { cover: coverBase64 } : {}),
    }),
    [
      coverBase64,
      input,
      output,
      title,
      author,
      language,
      contentVersion,
      contentType,
      inputKind,
      includeWordLayout,
      singleChapter,
    ],
  );

  const run = async (dryRun: boolean) => {
    if (input === "") return;
    setBusy(true);
    setError(null);
    setStage("starting");
    try {
      const images = await ensurePageImages();
      const value = await convert(
        // A picture book's reader never uses word coordinates, and they are
        // most of the package the device checks when it opens a book.
        { ...request(dryRun, page), ...(images ? { pageImages: images, includeWordLayout: false } : {}) },
        (progress) => setStage(String(progress.stage)),
      );
      setResult(value);
      setPreview(isPreview(value.preview) ? value.preview : null);
      setPage(isPreview(value.preview) ? value.preview.pageNumber : 1);
    } catch (failure) {
      setError(failure);
      setResult(null);
      setPreview(null);
    } finally {
      setBusy(false);
      setStage(null);
    }
  };

  /**
   * Turning a page after a write reads the finished file, which is cheap.
   * Before a write there is no file, so the only way to see another page is to
   * convert again -- honest, and the reason the button says so.
   */
  const goToPage = async (next: number) => {
    if (!result) return;
    setBusy(true);
    setError(null);
    try {
      if (result.written) {
        setPreview(await previewPackage(result.output, next));
      } else {
        const images = await ensurePageImages();
        const value = await convert({
          ...request(true, next),
          ...(images ? { pageImages: images, includeWordLayout: false } : {}),
        });
        setResult(value);
        setPreview(isPreview(value.preview) ? value.preview : null);
      }
      setPage(next);
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(false);
    }
  };

  const report = result?.report;
  const validation = result?.validation;
  const pageCount = report?.pageCount ?? 0;

  return (
    <div className="stack">
      <Card
        title="Source"
        subtitle="PDF, EPUB, TXT, or a structured Quran JSON source. The format is sniffed from the bytes; the extension is only a fallback."
      >
        <Field label="Input file" wide>
          <PathInput
            kind="file"
            filters={[{ name: "Documents", extensions: ["pdf", "epub", "txt", "json"] }]}
            value={settings.converterInput}
            onChange={(path) => update({ converterInput: path })}
            placeholder="C:\...\book.pdf"
          />
        </Field>
        <Field
          label="Output package"
          hint="Defaults to the input path with a .qpk extension."
          wide
        >
          <PathInput
            kind="save"
            filters={[{ name: "QPK package", extensions: ["qpk"] }]}
            value={output}
            onChange={setOutput}
            placeholder={input ? `${input.replace(/\.[^.]+$/u, "")}.qpk` : "(alongside the input)"}
          />
        </Field>

        <div className="grid">
          <Field label="Title" hint="Overrides whatever the document declares.">
            <input value={title} onChange={(e) => setTitle(e.currentTarget.value)} />
          </Field>
          <Field label="Author">
            <input value={author} onChange={(e) => setAuthor(e.currentTarget.value)} />
          </Field>
          <Field label="Language" hint="BCP 47, e.g. en or ar.">
            <input value={language} onChange={(e) => setLanguage(e.currentTarget.value)} />
          </Field>
          <Field
            label="Content version"
            hint="Bump to replace an installed copy instead of sitting beside it."
          >
            <input
              type="number"
              min={1}
              value={contentVersion}
              onChange={(e) => setContentVersion(Number(e.currentTarget.value) || 1)}
            />
          </Field>
          <Field label="Input format">
            <select
              value={inputKind}
              onChange={(e) => setInputKind(e.currentTarget.value as InputKindChoice)}
            >
              <option value="auto">detect from the bytes</option>
              <option value="txt">txt</option>
              <option value="pdf">pdf</option>
              <option value="epub">epub</option>
              <option value="quran-json">quran-json</option>
            </select>
          </Field>
          <Field label="Content type">
            <select
              value={contentType}
              onChange={(e) => setContentType(e.currentTarget.value as ContentTypeChoice)}
            >
              <option value="auto">decide from the source</option>
              <option value="BOOK">BOOK</option>
              <option value="QURAN">QURAN (Quran JSON only)</option>
            </select>
          </Field>
        </div>

        <div className="field field-wide">
          <span className="field-label">Cover</span>
          <div className="row" style={{ alignItems: "flex-start" }}>
            <CoverThumb levels={cover?.levels} title={title || undefined} />
            <div className="stack">
              <div className="row">
                <button type="button" onClick={() => pictureInput.current?.click()}>
                  Choose picture...
                </button>
                <button type="button" onClick={() => setCover(null)} disabled={!cover}>
                  Remove
                </button>
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
              </div>
              {coverNote && <span className="muted">{coverNote}</span>}
              <span className="field-hint">
                Shown on the device's Books shelf, in four greys. An EPUB's own cover is
                used automatically; for a PDF or TXT, choose a picture. Without one the
                device draws a book icon with the title.
              </span>
            </div>
          </div>
        </div>

        {isPdf && (
          <div className="row checks">
            <label className="check">
              <input
                type="checkbox"
                checked={keepLayout}
                onChange={(e) => setKeepLayout(e.currentTarget.checked)}
              />
              <span>
                Keep the PDF's page layout{" "}
                <span className="muted">(pages as pictures, like an e-reader)</span>
              </span>
            </label>
            <label className="check">
              <input
                type="checkbox"
                checked={trimMargins}
                disabled={!keepLayout}
                onChange={(e) => setTrimMargins(e.currentTarget.checked)}
              />
              <span>
                Trim margins <span className="muted">(bigger text on the device)</span>
              </span>
            </label>
          </div>
        )}
        {isPdf && keepLayout && pageImages?.key === imagesKey && (
          <div className="row" style={{ alignItems: "flex-start" }}>
            <img
              src={pageImageDataUrl(
                pageImages.pages[Math.min(Math.max(page, 1), pageImages.pages.length) - 1] ??
                  pageImages.pages[0]!,
              )}
              alt="How this page looks on the device"
              width={240}
              height={400}
              style={{ border: "1px solid #888", imageRendering: "pixelated" }}
            />
            <p className="muted">
              {pageImages.pages.length} page pictures, {formatBytes(pageImages.bytes)}. This is
              page {Math.min(Math.max(page, 1), pageImages.pages.length)} exactly as the device
              will show it, held upright.
            </p>
          </div>
        )}

        <div className="row checks">
          <label className="check">
            <input
              type="checkbox"
              checked={includeWordLayout}
              onChange={(e) => setIncludeWordLayout(e.currentTarget.checked)}
            />
            <span>
              Word coordinates <span className="muted">(needed for highlighting)</span>
            </span>
          </label>
          <label className="check">
            <input
              type="checkbox"
              checked={singleChapter}
              onChange={(e) => setSingleChapter(e.currentTarget.checked)}
            />
            <span>
              One chapter <span className="muted">(skip structure detection)</span>
            </span>
          </label>
        </div>

        <div className="row">
          <button onClick={() => run(true)} disabled={input === "" || busy}>
            {busy && stage ? `${stage}...` : "Validate & preview"}
          </button>
          <button
            className="primary"
            onClick={() => run(false)}
            disabled={input === "" || busy}
          >
            Convert & write
          </button>
          {busy && stage?.startsWith("rendering") && (
            <button type="button" onClick={() => renderAbort.current?.abort()}>
              Cancel
            </button>
          )}
        </div>
        <p className="muted">
          Nothing is written unless validation passes. A Quran package is only ever
          built from a structured Quran JSON source -- <code>convert()</code> refuses to
          derive scripture from a PDF's layout heuristics.
        </p>

        {error != null && <Failure error={error} />}
      </Card>

      {result && (
        <Card
          title={result.written ? "Written" : "Dry run"}
          subtitle={
            result.written
              ? result.output
              : "Nothing has been written yet. This is what would be."
          }
        >
          {validation && !validation.ok && (
            <div className="notice notice-error">
              <p>
                <strong>This package does not validate.</strong> It will not be written.
              </p>
              <ul>
                {validation.errors.map((line) => (
                  <li key={line}>{line}</li>
                ))}
              </ul>
            </div>
          )}
          {validation?.ok && result.written && (
            <div className="notice notice-good">
              Installed-ready package written to <code>{result.output}</code> --{" "}
              {formatBytes(result.packageBytes)}. Send it with the Device tab.
            </div>
          )}

          {report && (
            <KeyValues
              rows={[
                ["Type", report.contentType],
                ["Package size", formatBytes(report.packageSize)],
                ["Pages", String(report.pageCount)],
                ["Chapters", String(report.chapterCount)],
                ["Words", report.wordCount.toLocaleString()],
                [
                  "Layout space",
                  `${report.layoutSpace.width} x ${report.layoutSpace.height}`,
                ],
                [
                  "Estimated word boxes",
                  report.estimatedWordFraction > 0 ? (
                    <span title="Word boxes apportioned across a multi-word text run rather than measured. Word-level highlighting is only as good as this.">
                      {formatPercent(report.estimatedWordFraction)}
                    </span>
                  ) : (
                    "none -- every box was measured"
                  ),
                ],
                [
                  "Clamped coordinates",
                  report.clampedCoordinates === 0
                    ? "none"
                    : `${report.clampedCoordinates} (exceeded the u16 field)`,
                ],
                [
                  "Content id",
                  <code key="cid">{validation?.summary?.contentId ?? "-"}</code>,
                ],
              ]}
            />
          )}

          {(report?.warnings.length || validation?.warnings.length) && (
            <Note kind="warn">
              <ul>
                {[...(report?.warnings ?? []), ...(validation?.warnings ?? [])].map(
                  (warning) => (
                    <li key={warning}>{warning}</li>
                  ),
                )}
              </ul>
            </Note>
          )}

          {validation?.summary && (
            <details className="sections">
              <summary>{validation.summary.sections.length} sections</summary>
              <div className="table-scroll">
                <table>
                  <thead>
                    <tr>
                      <th>Id</th>
                      <th>Section</th>
                      <th>Records</th>
                      <th>Bytes</th>
                    </tr>
                  </thead>
                  <tbody>
                    {validation.summary.sections.map((section) => (
                      <tr key={section.id}>
                        <td>{section.id}</td>
                        <td>{section.name}</td>
                        <td>{section.records || ""}</td>
                        <td>{formatBytes(section.length)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            </details>
          )}
        </Card>
      )}

      {result && (
        <Card
          title="Preview"
          subtitle="Read back out of the finished package -- what the device will get, not what the converter meant."
          actions={
            pageCount > 1 ? (
              <>
                <button onClick={() => goToPage(page - 1)} disabled={page <= 1 || busy}>
                  Previous
                </button>
                <span className="muted">
                  page {page} of {pageCount}
                </span>
                <button
                  onClick={() => goToPage(page + 1)}
                  disabled={page >= pageCount || busy}
                >
                  Next
                </button>
              </>
            ) : undefined
          }
        >
          {!preview && <Empty>No preview: the package has no pages to show.</Empty>}
          {preview && (
            <>
              {preview.heading && <h3 className="preview-heading">{preview.heading}</h3>}
              <pre className="preview" dir="auto">
                {preview.text}
              </pre>
              <p className="muted">
                {preview.words.length > 0
                  ? `${preview.words.length} word boxes on this page.`
                  : "No word boxes on this page."}
                {!result.written &&
                  pageCount > 1 &&
                  " Turning a page re-runs the dry run, since there is no file to read from yet."}
              </p>
            </>
          )}
        </Card>
      )}

      {!result && !error && (
        <Card title="What this does">
          <ol className="steps">
            <li>
              <strong>Parse.</strong> PDF and EPUB go through extraction heuristics; TXT
              is taken as written. Word boxes come out measured where the source gives
              one run per word, apportioned otherwise -- the report says how much.
            </li>
            <li>
              <strong>Structure.</strong> Chapter headings are detected unless you turn
              that off.
            </li>
            <li>
              <strong>Validate.</strong> Twice: strictly (every checksum, full index
              sweep) and again the way the firmware parses at open, so a package that
              would be rejected on the device is caught here instead.
            </li>
            <li>
              <strong>Preview, then write.</strong> In that order.
            </li>
          </ol>
          <p className="muted">
            Sample inputs to try: <code>desktop/converter/examples/for-bushra.txt</code>{" "}
            and <code>desktop/converter/examples/quran-source.example.json</code>.
          </p>
        </Card>
      )}
    </div>
  );
}
