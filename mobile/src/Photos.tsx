// Photos: home-screen pictures and the time zone.
// Milestone 5 in docs/android.md -- picture decoding (the render worker) is
// not built yet; the time zone already works.

import { useState } from 'react';
import { ScrollView } from 'react-native';

import { clientFor } from './deviceClient';
import type { Settings } from './settings';
import { Button, Card, describeError, Note } from './ui';

export default function Photos({ settings }: { settings: Settings }) {
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;

  const sendTimeZone = async () => {
    setBusy(true);
    setError(null);
    setMessage(null);
    try {
      const reply = await clientFor(settings).setTimeZone(zone);
      setMessage(`Device time zone set (${reply.tz}).`);
    } catch (caught) {
      setError(describeError(caught));
    } finally {
      setBusy(false);
    }
  };

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      <Card title="Time zone">
        <Note>This phone's time zone: {zone}</Note>
        <Button
          title="Set on device"
          onPress={sendTimeZone}
          busy={busy}
          disabled={!settings.deviceHost.trim() || !settings.deviceToken.trim()}
        />
        {message ? <Note>{message}</Note> : null}
        {error ? <Note tone="danger">{error}</Note> : null}
      </Card>
      <Card title="Photos">
        <Note>Choosing and converting pictures on the phone is coming (docs/android.md, milestone 5).</Note>
      </Card>
    </ScrollView>
  );
}
