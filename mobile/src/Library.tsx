// Library: packages kept on the phone -- added from files or written by the
// Converter -- with covers, validation, sending to the device and sharing.
// The phone-side twin of the desktop Library tab (docs/android.md milestone 6).
//
// Many books go in and out at once: "Add books" converts or copies a whole
// selection (addBooks.ts), and the ticked packages are sent to the device one
// after another. Strictly one after another -- the device keys an upload
// session by content id and resyncs it, so sending in parallel would be
// wrong, not just slower.

import * as Sharing from 'expo-sharing';
import { useCallback, useEffect, useRef, useState } from 'react';
import { Alert, Image, Pressable, ScrollView, Switch, Text, View } from 'react-native';

import { addToLibrary, pickBooks } from './addBooks';
import { clientFor } from './deviceClient';
import { formatBytes } from './format';
import { listLibrary, readBytes, type LibraryPackage } from './packageStore';
import { useRenderWorker } from './render/RenderWorker';
import type { Settings } from './settings';
import { Button, Card, colors, describeError, Meter, Note } from './ui';

interface Validation {
  ok: boolean;
  errors: string[];
  warnings: string[];
}

/** One file in an "Add books" run. */
interface AddItem {
  name: string;
  state: 'waiting' | 'working' | 'added' | 'present' | 'failed' | 'skipped';
  detail?: string;
}

/** One package in a "send to device" run. */
interface SendItem {
  uri: string;
  name: string;
  state: 'waiting' | 'sending' | 'sent' | 'failed' | 'skipped';
  sent: number;
  total: number;
  detail?: string;
}

/**
 * Device answers that will fail every remaining package the same way. A
 * batch stops on these instead of repeating one error per book.
 */
const STOP_THE_BATCH = new Set(['UNAUTHORIZED', 'NOT_IN_TRANSFER_MODE', 'NO_SPACE']);

function stopsTheBatch(error: unknown): boolean {
  const code = (error as { code?: unknown } | null)?.code;
  // No code at all means the request never got an answer: the phone is off
  // the device's Wi-Fi, or the device went to sleep.
  return typeof code !== 'string' || STOP_THE_BATCH.has(code);
}

const ADD_LABELS: Record<AddItem['state'], string> = {
  waiting: 'Waiting',
  working: 'Working',
  added: 'Added',
  present: 'Already in library',
  failed: 'Failed',
  skipped: 'Skipped',
};

const SEND_LABELS: Record<SendItem['state'], string> = {
  waiting: 'Waiting',
  sending: 'Sending',
  sent: 'Sent',
  failed: 'Failed',
  skipped: 'Not sent',
};

function Tick({ on, disabled, onPress }: { on: boolean; disabled?: boolean; onPress: () => void }) {
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      hitSlop={10}
      accessibilityRole="checkbox"
      accessibilityState={{ checked: on, disabled: Boolean(disabled) }}
      style={{
        width: 26,
        height: 26,
        borderRadius: 6,
        borderWidth: 2,
        borderColor: on ? colors.accent : colors.line,
        backgroundColor: on ? colors.accent : colors.card,
        alignItems: 'center',
        justifyContent: 'center',
        opacity: disabled ? 0.4 : 1,
      }}
    >
      {on ? <Text style={{ color: '#fff', fontWeight: '700' }}>✓</Text> : null}
    </Pressable>
  );
}

export default function Library({ settings }: { settings: Settings }) {
  const worker = useRenderWorker();
  const [packages, setPackages] = useState<LibraryPackage[] | null>(null);
  const [checked, setChecked] = useState<Record<string, Validation | { failure: string }>>({});
  const [sending, setSending] = useState<{ uri: string; sent: number; total: number } | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [keepPdfLayout, setKeepPdfLayout] = useState(true);
  const [adds, setAdds] = useState<AddItem[]>([]);
  const [sends, setSends] = useState<SendItem[]>([]);
  const [stopReason, setStopReason] = useState<string | null>(null);
  const cancel = useRef(false);

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

  const refresh = useCallback(async (): Promise<LibraryPackage[] | null> => {
    try {
      const found = await listLibrary();
      setPackages(found);
      // Keep ticks on packages that are still there.
      const present = new Set(found.filter((entry) => !entry.error).map((entry) => entry.file.uri));
      setSelected((previous) => new Set([...previous].filter((uri) => present.has(uri))));
      return found;
    } catch (caught) {
      setError(describeError(caught));
      return null;
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  // --- adding books ------------------------------------------------------------

  const setAdd = (index: number, patch: Partial<AddItem>) =>
    setAdds((previous) => previous.map((item, i) => (i === index ? { ...item, ...patch } : item)));

  const addBooks = () =>
    run('add', async () => {
      setAdds([]);
      const books = await pickBooks();
      if (books.length === 0) return;
      setAdds(books.map((book) => ({ name: book.name, state: 'waiting' })));
      cancel.current = false;

      // Without the current listing, "already in the library" cannot be told.
      const current = await refresh();
      if (!current) return;
      const known = new Set(current.flatMap((entry) => (entry.identity ? [entry.identity] : [])));
      let added = 0;
      for (const [index, book] of books.entries()) {
        if (cancel.current) {
          setAdd(index, { state: 'skipped', detail: 'cancelled' });
          continue;
        }
        setAdd(index, { state: 'working', detail: 'starting' });
        try {
          const result = await addToLibrary(book, worker, known, {
            keepPdfLayout,
            onStage: (stage) => setAdd(index, { detail: stage }),
          });
          setAdd(index, { state: result.added ? 'added' : 'present', detail: result.added ? result.name : undefined });
          if (result.added) added++;
        } catch (caught) {
          setAdd(index, { state: 'failed', detail: describeError(caught) });
        }
      }
      await refresh();
      return `Added ${added} of ${books.length} book${books.length === 1 ? '' : 's'}.`;
    });

  // --- sending to the device ---------------------------------------------------

  const setSend = (index: number, patch: Partial<SendItem>) =>
    setSends((previous) => previous.map((item, i) => (i === index ? { ...item, ...patch } : item)));

  const sendSelected = () =>
    run('sendMany', async () => {
      const batch = (packages ?? []).filter((entry) => !entry.error && selected.has(entry.file.uri));
      if (batch.length === 0) return;
      setSends(
        batch.map((entry) => ({
          uri: entry.file.uri,
          name: entry.title ?? entry.name,
          state: 'waiting',
          sent: 0,
          total: entry.size,
        })),
      );
      setStopReason(null);
      cancel.current = false;

      const client = clientFor(settings);
      let stopped = false;
      let sent = 0;
      for (const [index, entry] of batch.entries()) {
        if (stopped || cancel.current) {
          setSend(index, { state: 'skipped' });
          continue;
        }
        setSend(index, { state: 'sending' });
        // A file the phone cannot read fails only itself, never the batch.
        let bytes: Uint8Array;
        try {
          bytes = await readBytes(entry.file);
        } catch (caught) {
          setSend(index, { state: 'failed', detail: describeError(caught) });
          continue;
        }
        try {
          const reply = await client.uploadPackage(bytes, {
            onProgress: (p) => setSend(index, { sent: p.sentBytes, total: p.totalBytes }),
          });
          setSend(index, { state: 'sent', sent: bytes.length, total: bytes.length, detail: reply.location });
          sent++;
        } catch (caught) {
          setSend(index, { state: 'failed', detail: describeError(caught) });
          if (stopsTheBatch(caught)) {
            stopped = true;
            setStopReason(describeError(caught));
          }
        }
      }
      return `Sent ${sent} of ${batch.length} to the device.`;
    });

  // --- one package at a time ------------------------------------------------------

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

  const toggle = (uri: string) =>
    setSelected((previous) => {
      const next = new Set(previous);
      if (next.has(uri)) next.delete(uri);
      else next.add(uri);
      return next;
    });

  const total = packages?.reduce((sum, entry) => sum + entry.size, 0) ?? 0;
  const readable = packages?.filter((entry) => !entry.error) ?? [];
  const picked = readable.filter((entry) => selected.has(entry.file.uri));
  const pickedBytes = picked.reduce((sum, entry) => sum + entry.size, 0);
  const allPicked = readable.length > 0 && picked.length === readable.length;

  const sendState = new Map(sends.map((item) => [item.uri, item.state]));
  const sendBytes = sends.reduce((sum, item) => sum + (item.state === 'sent' ? item.total : item.sent), 0);
  const sendTotal = sends.reduce((sum, item) => sum + item.total, 0);
  const sendDone = sends.filter((item) => item.state === 'sent').length;
  const addDone = adds.filter((item) => item.state !== 'waiting' && item.state !== 'working').length;

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      <Card title="Library on this phone">
        <Note>
          Packages you add here, or convert in the Converter tab, stay on the phone until you delete
          them. Send them to the device, or share one to a computer.
        </Note>
        <Button
          title={
            busy !== 'add'
              ? 'Add books…'
              : adds.length === 0
                ? 'Choosing…'
                : `Adding ${Math.min(addDone + 1, adds.length)} of ${adds.length}…`
          }
          onPress={addBooks}
          disabled={busy !== null || !worker.ready}
        />
        {busy === 'add' ? (
          <Button title="Stop after this one" kind="secondary" onPress={() => (cancel.current = true)} />
        ) : null}
        <Pressable
          onPress={() => busy === null && setKeepPdfLayout(!keepPdfLayout)}
          style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' }}
        >
          <Text style={{ color: colors.ink, flex: 1 }}>Keep each PDF's page layout (slower)</Text>
          <Switch value={keepPdfLayout} onValueChange={setKeepPdfLayout} disabled={busy !== null} />
        </Pressable>
        <Note>
          Pick as many as you like: PDF, EPUB and TXT are converted, .qpk packages are copied. To set a
          title or cover for a book, convert it in the Converter tab instead.
        </Note>
        {!worker.ready ? <Note>Starting the converter…</Note> : null}
        {packages ? (
          <Note>
            {packages.length} package{packages.length === 1 ? '' : 's'}, {formatBytes(total)}
          </Note>
        ) : null}
        {error ? <Note tone="danger">{error}</Note> : null}
        {message ? <Note>{message}</Note> : null}

        {adds.map((item, index) => (
          <View key={`${index}:${item.name}`} style={{ gap: 2 }}>
            <Text style={{ color: item.state === 'failed' ? colors.danger : colors.ink }} numberOfLines={1}>
              {ADD_LABELS[item.state]} · {item.name}
            </Text>
            {item.detail && item.state !== 'waiting' ? (
              <Note tone={item.state === 'failed' ? 'danger' : 'muted'}>{item.detail}</Note>
            ) : null}
          </View>
        ))}
        {adds.length > 0 && busy !== 'add' ? (
          <Button title="Clear list" kind="secondary" onPress={() => setAdds([])} />
        ) : null}
      </Card>

      {readable.length > 0 ? (
        <Card title="Send to device">
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
            <Tick
              on={allPicked}
              disabled={busy !== null}
              onPress={() => setSelected(allPicked ? new Set() : new Set(readable.map((entry) => entry.file.uri)))}
            />
            <Text style={{ color: colors.ink, flex: 1 }}>
              {picked.length === 0
                ? 'Select all, or tick packages below'
                : `${picked.length} selected, ${formatBytes(pickedBytes)}`}
            </Text>
          </View>
          <Button
            title={busy === 'sendMany' ? 'Sending…' : picked.length > 0 ? `Send ${picked.length} to device` : 'Send to device'}
            onPress={sendSelected}
            disabled={picked.length === 0 || !ready || busy !== null}
          />
          {busy === 'sendMany' ? (
            <Button title="Stop after this one" kind="secondary" onPress={() => (cancel.current = true)} />
          ) : null}

          {sends.length > 0 ? (
            <View style={{ gap: 6 }}>
              <Meter value={sendBytes} total={sendTotal} />
              <Note>
                {sendDone} of {sends.length} sent · {formatBytes(sendBytes)} of {formatBytes(sendTotal)}
              </Note>
              {stopReason ? (
                <Note tone="danger">
                  Stopped: {stopReason}
                  {'\n'}Check the device is in transfer mode and on the same Wi-Fi, then send again.
                </Note>
              ) : null}
              {sends.map((item) => (
                <View key={item.uri} style={{ gap: 2 }}>
                  <Text style={{ color: item.state === 'failed' ? colors.danger : colors.ink }} numberOfLines={1}>
                    {SEND_LABELS[item.state]} · {item.name}
                  </Text>
                  {item.state === 'sending' ? (
                    <Note>
                      {formatBytes(item.sent)} of {formatBytes(item.total)}
                    </Note>
                  ) : item.detail && (item.state === 'failed' || item.state === 'sent') ? (
                    <Note tone={item.state === 'failed' ? 'danger' : 'muted'}>{item.detail}</Note>
                  ) : null}
                </View>
              ))}
              {busy !== 'sendMany' ? (
                <Button title="Clear list" kind="secondary" onPress={() => setSends([])} />
              ) : null}
            </View>
          ) : null}
        </Card>
      ) : null}

      {packages?.map((entry) => {
        const check = checked[entry.file.uri];
        const isPicked = selected.has(entry.file.uri);
        const sent = sendState.get(entry.file.uri);
        return (
          <Card key={entry.file.uri} title={entry.error ? `${entry.name} (unreadable)` : entry.title ?? entry.name}>
            <Pressable
              onPress={() => !entry.error && busy === null && toggle(entry.file.uri)}
              style={{ flexDirection: 'row', gap: 12 }}
            >
              <Tick on={isPicked} disabled={Boolean(entry.error) || busy !== null} onPress={() => toggle(entry.file.uri)} />
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
                {sent === 'sent' ? <Text style={{ color: colors.accent, fontWeight: '600' }}>On the device</Text> : null}
                {sent === 'failed' ? <Note tone="danger">Send failed</Note> : null}
              </View>
            </Pressable>

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
          <Note>Add books from Downloads or Drive, or make one in the Converter tab.</Note>
        </Card>
      ) : null}
      {!ready ? <Note>To send packages, set the device address and pairing token on the Dashboard.</Note> : null}
    </ScrollView>
  );
}
