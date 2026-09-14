// The handful of things the app remembers between runs.
//
// Deliberately small and deliberately in localStorage: none of it is secret
// except the pairing token, and the token is not a secret this app is in a
// position to protect anyway -- it lives in the webview's own storage, which
// is per-app on every platform Tauri targets. A real trust store
// (device-client's `TrustStore` shape, keyed by deviceId) belongs on the Rust
// side with OS keychain backing; that is not this milestone, and pretending
// otherwise by encrypting it here would be theatre. See docs/pending.md.

import { useCallback, useState } from "react";

export interface Settings {
  /** Folder the Library tab scans -- an SD card root or sdcard-staging. */
  libraryDir: string;
  deviceHost: string;
  devicePort: number;
  deviceToken: string;
  /** Last document converted, so the Converter tab opens where you left it. */
  converterInput: string;
  /** Package queued for upload -- how the Library tab hands one to the Device tab. */
  uploadFile: string;
}

const DEFAULTS: Settings = {
  libraryDir: "",
  deviceHost: "",
  devicePort: 8080,
  deviceToken: "",
  converterInput: "",
  uploadFile: "",
};

const KEY = "quran-device.settings.v1";

function load(): Settings {
  try {
    const raw = window.localStorage.getItem(KEY);
    if (!raw) return DEFAULTS;
    const parsed = JSON.parse(raw) as Partial<Settings>;
    return {
      ...DEFAULTS,
      ...parsed,
      // A stored port that is not a usable number would break every request
      // with a confusing error; fall back rather than propagate it.
      devicePort:
        typeof parsed.devicePort === "number" && parsed.devicePort > 0
          ? parsed.devicePort
          : DEFAULTS.devicePort,
    };
  } catch {
    // Corrupt or unavailable storage is not worth failing to start over.
    return DEFAULTS;
  }
}

export function useSettings(): [Settings, (patch: Partial<Settings>) => void] {
  const [settings, setSettings] = useState<Settings>(load);

  const update = useCallback((patch: Partial<Settings>) => {
    setSettings((previous) => {
      const next = { ...previous, ...patch };
      try {
        window.localStorage.setItem(KEY, JSON.stringify(next));
      } catch {
        // Storage being full or blocked must not stop the app working; the
        // setting simply does not survive a restart.
      }
      return next;
    });
  }, []);

  return [settings, update];
}
