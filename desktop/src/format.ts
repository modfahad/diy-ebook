// Small formatters shared by every tab. Kept together so a size reads the
// same way in the Library table, the conversion report and the device's
// storage bar.

export function formatBytes(n: number): string {
  if (!Number.isFinite(n)) return "-";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  if (n < 1024 * 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
  return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

export function formatDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return "-";
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days}d ${hours}h`;
  if (hours > 0) return `${hours}h ${minutes}m`;
  if (minutes > 0) return `${minutes}m ${Math.floor(seconds % 60)}s`;
  return `${Math.floor(seconds)}s`;
}

/**
 * `installedAt` is a Unix timestamp, and 0 has a documented meaning: the
 * device had no clock when the package was installed (no NTP sync yet, or no
 * Internet behind the access point). Showing "1 Jan 1970" would present that
 * as a date rather than as the absence of one.
 */
export function formatInstalledAt(unixSeconds: number): string {
  if (!unixSeconds) return "unknown";
  return new Date(unixSeconds * 1000).toLocaleString();
}

export function formatPercent(fraction: number): string {
  return `${(fraction * 100).toFixed(fraction > 0 && fraction < 0.01 ? 2 : 0)}%`;
}

/** File name from a path, on either separator. */
export function baseName(path: string): string {
  const parts = path.split(/[\\/]/u);
  return parts[parts.length - 1] || path;
}
