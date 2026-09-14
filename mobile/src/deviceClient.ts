// The phone's DeviceClient: the desktop's, with base64 chunk bodies because
// React Native's fetch cannot stream a request body (docs/protocol.md).

import { DeviceClient } from '@quran-device/device-client';

import type { Settings } from './settings';

export function clientFor(settings: Settings): DeviceClient {
  const token = settings.deviceToken.trim();
  return new DeviceClient({
    host: settings.deviceHost.trim(),
    port: settings.devicePort,
    ...(token ? { token } : {}),
    chunkEncoding: 'base64',
  });
}
