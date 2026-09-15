// Library: packages kept on the phone -- added from files or written by the
// Converter -- with covers, validation, sending to the device and sharing.
// The phone-side twin of the desktop Library tab (docs/android.md milestone 6).

import * as Sharing from 'expo-sharing';
import { useCallback, useEffect, useState } from 'react';
import { Alert, Image, ScrollView, Text, View } from 'react-native';

import { clientFor } from './deviceClient';
import { formatBytes } from './format';
import { addFromPicker, listLibrary, readBytes, type LibraryPackage } from './packageStore';
import { useRenderWorker } from './render/RenderWorker';
import type { Settings } from './settings';
import { Button, Card, colors, describeError, Meter, Note } from './ui';

interface Validation {
  ok: boolean;
  errors: string[];
  warnings: string[];
}

export default function Library({ settings }: { settings: Settings }) {
  const worker = useRenderWorker();
  const [packages, setPackages] = useState<LibraryPackage[] | null>(null);
  const [checked, setChecked] = useState<Record<string, Validation | { failure: string }>>({});
  const [sending, setSending] = useState<{ uri: string; sent: number; total: number } | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const ready = settings.deviceHost.trim() !== '' && settings.deviceToken.trim() !== '';

  const run = async (what: string, action: () => Promise<string | void>) => {
    setBusy(what);
    setError(null);
    setMessage(null);
    try {
      const done = await action();
      if (done) setMessage(done);
    } catch (caught) {
      setError(describeError(caught));
    } finally {
      setBusy(null);
    }
  };

  const refresh = useCallback(async () => {
    try {
      setPackages(await listLibrary());
    } catch (caught) {
      setError(describeError(caught));
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const add = () =>
    run('add', async () => {
      const added = await addFromPicker();
      await refresh();
      return added > 0 ? `Added ${added} package${added === 1 ? '' : 's'}.` : undefined;
    });

  const validate = (entry: LibraryPackage) =>
    run(`validate:${entry.file.uri}`, async () => {
      try {
        const result = await worker.call<Validation>('validate', {}, {
          bytes: { key: await readBytes(entry.file) },
        });
        setChecked((previous) => ({ ...previous, [entry.file.uri]: result }));
      } catch (caught) {
        setChecked((previous) => ({ ...previous, [entry.file.uri]: { failure: describeError(caught) } }));
      }
    });

  const send = (entry: LibraryPackage) =>
    run(`send:${entry.file.uri}`, async () => {
      const bytes = await readBytes(entry.file);
      setSending({ uri: entry.file.uri, sent: 0, total: bytes.length });
      try {
        const reply = await clientFor(settings).uploadPackage(bytes, {
          onProgress: (p) => setSending({ uri: entry.file.uri, sent: p.sentBytes, total: p.totalBytes }),
        });
        return `Installed on the device: ${entry.title ?? entry.name} (${reply.location}).`;
      } finally {
        setSending(null);
      }
    });

  const share = (entry: LibraryPackage) =>
    run('share', async () => {
      if (!(await Sharing.isAvailableAsync())) return 'Sharing is not available on this phone.';
      await Sharing.shareAsync(entry.file.uri, { mimeType: 'application/octet-stream' });
    });

  const remove = (entry: LibraryPackage) =>
    Alert.alert('Delete from this phone?', entry.title ?? entry.name, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Delete',
        style: 'destructive',
        onPress: () =>
          void run('delete', async () => {
            entry.file.delete();
            await refresh();
            return `Deleted ${entry.title ?? entry.name} from the phone.`;
          }),
      },
    ]);

  const total = packages?.reduce((sum, entry) => sum + entry.size, 0) ?? 0;

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      <Card title="Library on this phone">
        <Note>
          Packages you add here, or convert in the Converter tab, stay on the phone until you delete
          them. Send one to the device, or share it to a computer.
        </Note>
        <Button title="Add packages…" onPress={add} busy={busy === 'add'} disabled={busy !== null} />
        {packages ? (
          <Note>
            {packages.length} package{packages.length === 1 ? '' : 's'}, {formatBytes(total)}
          </Note>
        ) : null}
        {error ? <Note tone="danger">{error}</Note> : null}
        {message ? <Note>{message}</Note> : null}
      </Card>

      {packages?.map((entry) => {
        const check = checked[entry.file.uri];
        return (
          <Card key={entry.file.uri} title={entry.error ? `${entry.name} (unreadable)` : entry.title ?? entry.name}>
            <View style={{ flexDirection: 'row', gap: 12 }}>
              {entry.cover ? (
                <Image source={{ uri: entry.cover }} style={{ width: 81, height: 108, borderWidth: 1, borderColor: colors.line }} />
              ) : (
                <View
                  style={{
                    width: 81,
                    height: 108,
                    borderWidth: 2,
                    borderColor: colors.ink,
                    alignItems: 'center',
                    justifyContent: 'center',
                    padding: 4,
                  }}
                >
                  <Text style={{ fontSize: 10, textAlign: 'center', color: colors.ink }} numberOfLines={4}>
                    {entry.title ?? 'No cover'}
                  </Text>
                </View>
              )}
              <View style={{ flex: 1, gap: 2 }}>
                {entry.error ? <Note tone="danger">{entry.error}</Note> : null}
                {entry.type ? <Text style={{ color: colors.ink }}>{entry.type}</Text> : null}
                {entry.author ? <Note>{entry.author}</Note> : null}
                <Note>
                  {[entry.language, entry.pageCount ? `${entry.pageCount} pages` : null, formatBytes(entry.size)]
                    .filter(Boolean)
                    .join(' · ')}
                </Note>
                <Note>{entry.name}</Note>
              </View>
            </View>

            {check && 'failure' in check ? <Note tone="danger">{check.failure}</Note> : null}
            {check && 'ok' in check ? (
              <Note tone={check.ok ? 'muted' : 'danger'}>
                {check.ok ? 'Valid.' : 'Does not validate:'}
                {[...check.errors, ...check.warnings].map((line) => `\n• ${line}`).join('')}
              </Note>
            ) : null}
            {sending?.uri === entry.file.uri ? <Meter value={sending.sent} total={sending.total} /> : null}

            <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 8 }}>
              <Button
                title="Validate"
                kind="secondary"
                onPress={() => validate(entry)}
                busy={busy === `validate:${entry.file.uri}`}
                disabled={busy !== null || !worker.ready || Boolean(entry.error)}
              />
              <Button
                title="Send to device"
                onPress={() => send(entry)}
                busy={busy === `send:${entry.file.uri}`}
                disabled={busy !== null || !ready || Boolean(entry.error)}
              />
              <Button title="Share" kind="secondary" onPress={() => share(entry)} disabled={busy !== null} />
              <Button title="Delete" kind="secondary" onPress={() => remove(entry)} disabled={busy !== null} />
            </View>
          </Card>
        );
      })}

      {packages && packages.length === 0 ? (
        <Card title="Nothing here yet">
          <Note>Add .qpk files from Downloads or Drive, or make one in the Converter tab.</Note>
        </Card>
      ) : null}
      {!ready ? <Note>To send packages, set the device address and pairing token on the Dashboard.</Note> : null}
    </ScrollView>
  );
}
