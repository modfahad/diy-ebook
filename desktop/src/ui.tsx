// The few presentational pieces every tab needs. No app logic lives here.

import type { ReactNode } from "react";
import { open, save, type DialogFilter } from "@tauri-apps/plugin-dialog";
import { describeFailure, remedyFor } from "./bridge";

export function Card({
  title,
  subtitle,
  actions,
  children,
}: {
  title?: ReactNode;
  subtitle?: ReactNode;
  actions?: ReactNode;
  children: ReactNode;
}) {
  return (
    <section className="card">
      {(title || actions) && (
        <header className="card-head">
          <div>
            {title && <h2>{title}</h2>}
            {subtitle && <p className="muted">{subtitle}</p>}
          </div>
          {actions && <div className="row">{actions}</div>}
        </header>
      )}
      {children}
    </section>
  );
}

export function Field({
  label,
  hint,
  children,
  wide,
}: {
  label: string;
  hint?: string;
  children: ReactNode;
  wide?: boolean;
}) {
  return (
    <label className={wide ? "field field-wide" : "field"}>
      <span className="field-label">{label}</span>
      {children}
      {hint && <span className="field-hint">{hint}</span>}
    </label>
  );
}

/**
 * A path box with a "Browse..." button that opens the system's own file or
 * folder dialog. The box stays editable, so a path can still be typed or
 * pasted. `kind` picks the dialog: an existing file, a folder, or a file to
 * write.
 */
export function PathInput({
  value,
  onChange,
  kind,
  filters,
  placeholder,
  className,
}: {
  value: string;
  onChange: (path: string) => void;
  kind: "file" | "folder" | "save";
  filters?: DialogFilter[];
  placeholder?: string;
  className?: string;
}) {
  const browse = async () => {
    const defaultPath = value.trim() !== "" ? value.trim() : undefined;
    const picked =
      kind === "save"
        ? await save({ defaultPath, filters })
        : await open({ defaultPath, filters, directory: kind === "folder", multiple: false });
    if (typeof picked === "string") onChange(picked);
  };
  return (
    <div className={className} style={{ display: "flex", gap: "0.5rem" }}>
      <input
        style={{ flex: 1, minWidth: 0 }}
        value={value}
        onChange={(e) => onChange(e.currentTarget.value)}
        placeholder={placeholder}
        spellCheck={false}
      />
      <button
        type="button"
        onClick={(e) => {
          e.preventDefault();
          browse().catch((error) => console.error("dialog failed", error));
        }}
      >
        Browse...
      </button>
    </div>
  );
}

/**
 * A failure, with the remedy attached when there is one. Bridge errors carry
 * a machine-readable code precisely so the app can say what to do about
 * "UNAUTHORIZED" instead of only repeating it.
 */
export function Failure({ error }: { error: unknown }) {
  const failure = describeFailure(error);
  const remedy = remedyFor(failure.code);
  return (
    <div className="notice notice-error" role="alert">
      <p>
        <strong>{failure.code}</strong> {failure.message}
      </p>
      {remedy && <p className="notice-remedy">{remedy}</p>}
      {failure.stderr && (
        <details>
          <summary>Output from the bridge process</summary>
          <pre>{failure.stderr}</pre>
        </details>
      )}
    </div>
  );
}

export function Note({
  kind = "info",
  children,
}: {
  kind?: "info" | "warn" | "error";
  children: ReactNode;
}) {
  return <div className={`notice notice-${kind}`}>{children}</div>;
}

export function Badge({
  tone = "neutral",
  children,
}: {
  tone?: "neutral" | "good" | "warn" | "bad";
  children: ReactNode;
}) {
  return <span className={`badge badge-${tone}`}>{children}</span>;
}

export function Meter({
  value,
  max,
  label,
}: {
  value: number;
  max: number;
  label?: ReactNode;
}) {
  const fraction = max > 0 ? Math.min(Math.max(value / max, 0), 1) : 0;
  return (
    <div className="meter-wrap">
      <div className="meter" role="progressbar" aria-valuenow={Math.round(fraction * 100)} aria-valuemin={0} aria-valuemax={100}>
        <div className="meter-fill" style={{ width: `${fraction * 100}%` }} />
      </div>
      {label && <div className="meter-label">{label}</div>}
    </div>
  );
}

export function KeyValues({ rows }: { rows: Array<[string, ReactNode]> }) {
  return (
    <dl className="kv">
      {rows.map(([key, value]) => (
        <div key={key}>
          <dt>{key}</dt>
          <dd>{value}</dd>
        </div>
      ))}
    </dl>
  );
}

export function Empty({ children }: { children: ReactNode }) {
  return <p className="empty">{children}</p>;
}
