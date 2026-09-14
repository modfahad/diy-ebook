// Device address and pairing token, kept with expo-secure-store (the Android
// Keystore) -- the token is a credential, so not in plain AsyncStorage.

import * as SecureStore from 'expo-secure-store';
import { useCallback, useEffect, useState } from 'react';

import { DEFAULT_PORT } from '@quran-device/protocol';

export interface Settings {
  deviceHost: string;
  devicePort: number;
  deviceToken: string;
}

export const DEFAULT_SETTINGS: Settings = {
  deviceHost: '',
  devicePort: DEFAULT_PORT,
  deviceToken: '',
};

const KEY = 'quran-device.settings.v1';

async function load(): Promise<Settings> {
  try {
    const text = await SecureStore.getItemAsync(KEY);
    if (!text) return DEFAULT_SETTINGS;
    const stored = JSON.parse(text) as Partial<Settings>;
    return { ...DEFAULT_SETTINGS, ...stored };
  } catch {
    return DEFAULT_SETTINGS; // unreadable or from an older shape: start fresh
  }
}

export function useSettings(): {
  settings: Settings;
  loaded: boolean;
  update: (patch: Partial<Settings>) => void;
} {
  const [settings, setSettings] = useState<Settings>(DEFAULT_SETTINGS);
  const [loaded, setLoaded] = useState(false);

  useEffect(() => {
    void load().then((value) => {
      setSettings(value);
      setLoaded(true);
    });
  }, []);

  const update = useCallback((patch: Partial<Settings>) => {
    setSettings((current) => {
      const next = { ...current, ...patch };
      void SecureStore.setItemAsync(KEY, JSON.stringify(next));
      return next;
    });
  }, []);

  return { settings, loaded, update };
}
