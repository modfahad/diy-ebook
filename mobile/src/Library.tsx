// Library: packages kept on the phone -- added from files or written by the
// Converter -- with covers, validation, sending to the device and sharing.
// The phone-side twin of the desktop Library tab (docs/android.md milestone 6).
//
// Many books go in and out at once. "Add books" is two steps: choose any
// number of files, fill in the details for each (or for all of them at once),
// then convert the lot (addBooks.ts). The ticked packages are sent to the
// device one after another -- strictly one after another: the device keys an
// upload session by content id and resyncs it, so sending in parallel would
// be wrong, not just slower.

import * as DocumentPicker from 'expo-document-picker';
import { File } from 'expo-file-system';
import * as Sharing from 'expo-sharing';
import { useCallback, useEffect, useRef, useState } from 'react';
import { Alert, Image, Pressable, ScrollView, Switch, Text, View } from 'react-native';

import { COVER_HEIGHT, COVER_WIDTH } from '@quran-device/qpk-format';

import { addToLibrary, bookKind, pickBooks, type BookDetails, type PickedBook } from './addBooks';
import { decodeBase64 } from './base64';
import { clientFor } from './deviceClient';
import { formatBytes } from './format';
import { listLibrary, readBytes, type LibraryPackage } from './packageStore';
import { levelsPngDataUrl } from './png';
import { useRenderWorker } from './render/RenderWorker';
import type { Settings } from './settings';
import { Button, Card, colors, describeError, Field, Meter, Note } from './ui';

interface Validation {
  ok: boolean;
  errors: string[];
  warnings: string[];
}

/** One file waiting to be added, with the details filled in for it. */
interface BookRow extends BookDetails {
  id: number;
  book: PickedBook;
  kind: string;
  /** True while the file's own title, author and language are being read. */
  reading: boolean;
  /** The file names no title of its own; `fileTitle` is what it gets unless one is typed. */
  untitled?: boolean;
  fileTitle?: string;
  state: 'ready' | 'working' | 'added' | 'present' | 'failed' | 'skipped';
  detail?: string;
}

/** Rows that still need converting: new, or failed or skipped last time. */
const toConvert = (row: BookRow) => row.state === 'ready' || row.state === 'failed' || row.state === 'skipped';

let nextBookId = 1;

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

const ADD_LABELS: Record<BookRow['state'], string> = {
  ready: 'Ready',
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
  const [books, setBooks] = useState<BookRow[]>([]);
  const [allAuthor, setAllAuthor] = useState('');
  const [allLanguage, setAllLanguage] = useState('');
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

  const setBook = (id: number, patch: Partial<BookRow>) =>
    setBooks((previous) => previous.map((row) => (row.id === id ? { ...row, ...patch } : row)));

  const chooseBooks = () =>
    run('pick', async () => {
      const picked = await pickBooks();
      const listed = new Set(books.map((row) => row.book.uri));
      const fresh = picked
        .filter((book) => !listed.has(book.uri))
        .map<BookRow>((book) => {
          const kind = bookKind(book.name);
          return {
            id: nextBookId++,
            book,
            kind,
            title: '',
            author: allAuthor,
            language: allLanguage,
            cover: null,
            reading: kind === 'pdf' || kind === 'epub' || kind === 'txt',
            state: 'ready',
          };
        });
      setBooks((previous) => [...previous, ...fresh]);

      // Fill in what each document says about itself, one at a time. Only
      // fields still empty are filled, so nothing typed meanwhile is lost.
      for (const row of fresh) {
        if (!row.reading) continue;
        try {
          const found = await worker.call<{ title?: string; author?: string; language?: string; untitled?: boolean }>(
            'documentDetails',
            { filename: row.book.name },
            { bytes: { key: await readBytes(new File(row.book.uri)) } },
          );
          setBooks((previous) =>
            previous.map((other) =>
              other.id !== row.id
                ? other
                : {
                    ...other,
                    reading: false,
                    // A title made from the file name is not filled in, so the
                    // empty field asks for a real one.
                    title: other.title || (found.untitled ? '' : found.title || ''),
                    untitled: found.untitled === true,
                    fileTitle: found.title,
                    author: other.author || found.author || '',
                    language: other.language || found.language || '',
                  },
            ),
          );
        } catch (caught) {
          setBook(row.id, { reading: false, detail: `Could not read its details: ${describeError(caught)}` });
        }
      }
    });

  const fillEveryBook = () =>
    setBooks((previous) =>
      previous.map((row) =>
        row.kind === 'qpk' || !toConvert(row) ? row : { ...row, author: allAuthor, language: allLanguage },
      ),
    );

  const chooseCover = (row: BookRow) =>
    run(`cover:${row.id}`, async () => {
      const picked = await DocumentPicker.getDocumentAsync({ type: 'image/*', copyToCacheDirectory: true });
      if (picked.canceled || picked.assets.length === 0) return;
      const asset = picked.assets[0]!;
      const bytes = new Uint8Array(await new File(asset.uri).arrayBuffer());
      const reply = await worker.call<{ levels: string }>(
        'coverFromPicture',
        { mediaType: asset.mimeType ?? '' },
        { bytes: { key: bytes } },
      );
      setBook(row.id, { cover: decodeBase64(reply.levels) });
    });

  const convertBooks = () =>
    run('add', async () => {
      const batch = books.filter(toConvert);
      if (batch.length === 0) return;
      cancel.current = false;

      // Without the current listing, "already in the library" cannot be told.
      const current = await refresh();
      if (!current) return;
      const known = new Set(current.flatMap((entry) => (entry.identity ? [entry.identity] : [])));
      let added = 0;
      for (const row of batch) {
        if (cancel.current) {
          setBook(row.id, { state: 'skipped', detail: 'cancelled' });
          continue;
        }
        setBook(row.id, { state: 'working', detail: 'starting' });
        try {
          const result = await addToLibrary(row.book, worker, known, {
            keepPdfLayout,
            details: row,
            onStage: (stage) => setBook(row.id, { detail: stage }),
          });
          setBook(row.id, {
            state: result.added ? 'added' : 'present',
            detail: result.added ? result.name : 'the same book is already on this phone',
          });
          if (result.added) added++;
        } catch (caught) {
          setBook(row.id, { state: 'failed', detail: describeError(caught) });
        }
      }
      await refresh();
      return `Added ${added} of ${batch.length} book${batch.length === 1 ? '' : 's'}.`;
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
  const pending = books.filter(toConvert);
  const remaining = books.filter((row) => row.state === 'ready' || row.state === 'working').length;
  const converting = busy === 'add';

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }} keyboardShouldPersistTaps="handled">
      <Card title="Library on this phone">
        <Note>
          Packages you add here, or convert in the Converter tab, stay on the phone until you delete
          them. Send them to the device, or share one to a computer.
        </Note>
        <Button
          title={books.length === 0 ? 'Add books…' : 'Choose more books…'}
          onPress={chooseBooks}
          busy={busy === 'pick'}
          disabled={busy !== null || !worker.ready}
        />
        <Note>
          Choose any number of files, fill in their details, then convert them all at once. PDF, EPUB
          and TXT are converted; .qpk packages are copied as they are.
        </Note>
        {!worker.ready ? <Note>Starting the converter…</Note> : null}
        {packages ? (
          <Note>
            {packages.length} package{packages.length === 1 ? '' : 's'}, {formatBytes(total)}
          </Note>
        ) : null}
        {error ? <Note tone="danger">{error}</Note> : null}
        {message ? <Note>{message}</Note> : null}
      </Card>

      {books.length > 0 ? (
        <Card title={`Books to add (${books.length})`}>
          <View style={{ gap: 8, padding: 10, borderRadius: 8, backgroundColor: colors.ground }}>
            <Text style={{ color: colors.ink, fontWeight: '600' }}>For every book</Text>
            <Field label="Author" value={allAuthor} onChange={setAllAuthor} />
            <Field label="Language (e.g. en, ar)" value={allLanguage} onChange={setAllLanguage} />
            <Button title="Fill in every book" kind="secondary" onPress={fillEveryBook} disabled={busy !== null} />
          </View>

          {books.map((row) => {
            const editable = row.kind !== 'qpk' && toConvert(row) && busy === null;
            return (
              <View
                key={row.id}
                style={{
                  gap: 8,
                  paddingVertical: 10,
                  borderTopWidth: 1,
                  borderTopColor: colors.line,
                }}
              >
                <Text style={{ color: row.state === 'failed' ? colors.danger : colors.ink, fontWeight: '600' }} numberOfLines={2}>
                  {ADD_LABELS[row.state]} · {row.book.name}
                </Text>
                {row.kind === 'qpk' ? (
                  <Note>Copied as it is -- a package keeps its own details.</Note>
                ) : editable ? (
                  <>
                    <View style={{ flexDirection: 'row', gap: 12, alignItems: 'flex-start' }}>
                      {row.cover ? (
                        <Image
                          source={{ uri: levelsPngDataUrl(COVER_WIDTH, COVER_HEIGHT, row.cover) }}
                          style={{ width: 54, height: 72, borderWidth: 1, borderColor: colors.line }}
                        />
                      ) : (
                        <View style={{ width: 54, height: 72, borderWidth: 2, borderColor: colors.ink, alignItems: 'center', justifyContent: 'center' }}>
                          <Text style={{ fontSize: 9, color: colors.ink, textAlign: 'center' }}>
                            {row.kind === 'epub' ? "EPUB's own, or the title" : 'Title on the cover'}
                          </Text>
                        </View>
                      )}
                      <View style={{ flex: 1, gap: 6 }}>
                        <Button title="Choose cover…" kind="secondary" onPress={() => chooseCover(row)} busy={busy === `cover:${row.id}`} disabled={busy !== null} />
                        {row.cover ? (
                          <Button title="Remove cover" kind="secondary" onPress={() => setBook(row.id, { cover: null })} />
                        ) : null}
                      </View>
                    </View>
                    <Field
                      label="Title"
                      value={row.title}
                      onChange={(title) => setBook(row.id, { title })}
                      placeholder={row.untitled && row.fileTitle ? row.fileTitle : 'Title'}
                    />
                    {row.untitled && !row.title.trim() ? (
                      <Note>
                        This file has no title of its own. Type one -- it is also written on the cover. Left empty, the
                        book is called "{row.fileTitle ?? row.book.name}".
                      </Note>
                    ) : null}
                    <Field label="Author" value={row.author} onChange={(author) => setBook(row.id, { author })} />
                    <Field label="Language" value={row.language} onChange={(language) => setBook(row.id, { language })} />
                  </>
                ) : (
                  <Note>
                    {row.reading
                      ? 'Reading details…'
                      : [row.title || null, row.author || null, row.language || null].filter(Boolean).join(' · ') ||
                        'Details from the document'}
                  </Note>
                )}
                {row.detail ? (
                  <Note tone={row.state === 'failed' ? 'danger' : 'muted'}>{row.detail}</Note>
                ) : null}
                {busy === null ? (
                  <Button
                    title="Remove from list"
                    kind="secondary"
                    onPress={() => setBooks((previous) => previous.filter((other) => other.id !== row.id))}
                  />
                ) : null}
              </View>
            );
          })}

          <Pressable
            onPress={() => busy === null && setKeepPdfLayout(!keepPdfLayout)}
            style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' }}
          >
            <Text style={{ color: colors.ink, flex: 1 }}>Keep each PDF's page layout (slower)</Text>
            <Switch value={keepPdfLayout} onValueChange={setKeepPdfLayout} disabled={busy !== null} />
          </Pressable>
          <Button
            title={
              converting
                ? `Converting… ${remaining} left`
                : `Convert & add ${pending.length} book${pending.length === 1 ? '' : 's'}`
            }
            onPress={convertBooks}
            disabled={busy !== null || !worker.ready || pending.length === 0}
          />
          {converting ? (
            <Button title="Stop after this one" kind="secondary" onPress={() => (cancel.current = true)} />
          ) : (
            <Button title="Clear list" kind="secondary" onPress={() => setBooks([])} disabled={busy !== null} />
          )}
        </Card>
      ) : null}

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
