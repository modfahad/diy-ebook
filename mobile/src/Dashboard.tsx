// Dashboard: where the phone finds the device, and a first look at it.

import { useState } from 'react';
import { ScrollView } from 'react-native';

import type { DeviceInfo, DeviceStatus } from '@quran-device/protocol';

import { clientFor } from './deviceClient';
import type { Settings } from './settings';
import { Button, Card, describeError, Field, KeyValues, Note } from './ui';
import { formatBytes } from './format';

export default function Dashboard({
  settings,
  update,
}: {
  settings: Settings;
  update: (patch: Partial<Settings>) => void;
}) {
  const [info, setInfo] = useState<DeviceInfo | null>(null);
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const check = async () => {
    setBusy(true);
    setError(null);
    try {
      const client = clientFor(settings);
      setInfo(await client.getInfo());
      // Status needs the pairing token; without one, Identify is all there is.
      setStatus(settings.deviceToken.trim() ? await client.getStatus() : null);
    } catch (caught) {
      setError(describeError(caught));
    } finally {
      setBusy(false);
    }
  };

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      <Card title="Device">
        <Note>
          The device answers only while Wi-Fi transfer mode is on, and the phone must be on the
          same Wi-Fi.
        </Note>
        <Field
          label="Address"
          value={settings.deviceHost}
          onChange={(deviceHost) => update({ deviceHost })}
          placeholder="192.168.1.50"
        />
        <Field
          label="Port"
          value={String(settings.devicePort)}
          onChange={(text) => update({ devicePort: Number(text.replace(/\D/g, '')) || 0 })}
          numeric
        />
        <Field
          label="Pairing token"
          value={settings.deviceToken}
          onChange={(deviceToken) => update({ deviceToken })}
          secure
        />
        <Button title="Check connection" onPress={check} busy={busy} disabled={!settings.deviceHost.trim()} />
        {error ? <Note tone="danger">{error}</Note> : null}
      </Card>

      {info ? (
        <Card title={info.name}>
          <KeyValues
            rows={[
              ['Model', info.model],
              ['Firmware', info.firmwareVersion],
              ['Paired', info.paired ? 'yes' : 'no'],
              ...(info.resetReason ? ([['Last restart', info.resetReason]] as Array<[string, string]>) : []),
              ...(status
                ? ([
                    ['Free storage', status.storage.mounted ? formatBytes(status.storage.freeBytes) : 'no card'],
                    ['Transfer mode', status.transferMode ? 'on' : 'off'],
                  ] as Array<[string, string]>)
                : []),
            ]}
          />
        </Card>
      ) : null}
    </ScrollView>
  );
}
