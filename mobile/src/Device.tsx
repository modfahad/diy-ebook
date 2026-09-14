// Device: status, the installed library, package upload and firmware update
// -- the desktop Device tab on a phone (docs/android.md, milestone 4).
//
// Same constraint as the desktop: only Identify works without a pairing
// token, and the device answers only in transfer mode.

import * as DocumentPicker from 'expo-document-picker';
import { File } from 'expo-file-system';
import { useState } from 'react';
import { Alert, ScrollView, Text, View } from 'react-native';

import type { DeviceStatus, LibraryListing } from '@quran-device/protocol';

import { clientFor } from './deviceClient';
import { formatBytes } from './format';
import type { Settings } from './settings';
import { Button, Card, colors, describeError, KeyValues, Meter, Note } from './ui';

interface Transfer {
  name: string;
  sentBytes: number;
  totalBytes: number;
  resyncs: number;
  done: boolean;
}

/** A file the user picks, read whole: packages and firmware fit easily in memory. */
async function pickBytes(): Promise<{ name: string; bytes: Uint8Array } | null> {
  const result = await DocumentPicker.getDocumentAsync({ copyToCacheDirectory: true, multiple: false });
  if (result.canceled || result.assets.length === 0) return null;
  const asset = result.assets[0]!;
  const bytes = new Uint8Array(await new File(asset.uri).arrayBuffer());
  return { name: asset.name, bytes };
}

export default function Device({ settings }: { settings: Settings }) {
  const [status, setStatus] = useState<DeviceStatus | null>(null);
  const [listing, setListing] = useState<LibraryListing | null>(null);
  const [upload, setUpload] = useState<Transfer | null>(null);
  const [firmware, setFirmware] = useState<Transfer | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);

  const ready = settings.deviceHost.trim() !== '' && settings.deviceToken.trim() !== '';

  /** Every action's busy flag, error and message in one place. */
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

  const refresh = () =>
    run('refresh', async () => {
      const client = clientFor(settings);
      setStatus(await client.getStatus());
      setListing(await client.listLibrary());
    });

  const installPackage = () =>
    run('upload', async () => {
      const picked = await pickBytes();
      if (!picked) return;
      const track = (p: { sentBytes: number; totalBytes: number; resyncs: number }) =>
        setUpload({ name: picked.name, ...p, done: false });
      track({ sentBytes: 0, totalBytes: picked.bytes.length, resyncs: 0 });
      const client = clientFor(settings);
      const reply = await client.uploadPackage(picked.bytes, { onProgress: track });
      setUpload((current) => (current ? { ...current, done: true } : current));
      setListing(await client.listLibrary());
      return `Installed ${picked.name} (${reply.location}).`;
    });

  const remove = (contentId: string, title: string) =>
    Alert.alert('Delete from device?', title, [
      { text: 'Cancel', style: 'cancel' },
      {
        text: 'Delete',
        style: 'destructive',
        onPress: () =>
          void run('delete', async () => {
            const client = clientFor(settings);
            await client.deleteItem(contentId);
            setListing(await client.listLibrary());
            return `Deleted ${title}.`;
          }),
      },
    ]);

  const updateFirmware = () =>
    run('firmware', async () => {
      const picked = await pickBytes();
      if (!picked) return;
      const track = (p: { sentBytes: number; totalBytes: number; resyncs: number }) =>
        setFirmware({ name: picked.name, ...p, done: false });
      track({ sentBytes: 0, totalBytes: picked.bytes.length, resyncs: 0 });
      await clientFor(settings).uploadFirmware(picked.bytes, { onProgress: track });
      setFirmware((current) => (current ? { ...current, done: true } : current));
      return 'Firmware installed. The device is restarting into it.';
    });

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      {!ready ? (
        <Card title="Not connected">
          <Note>Enter the device address and pairing token on the Dashboard first.</Note>
        </Card>
      ) : null}

      <Card title="Status">
        <Button title="Refresh" kind="secondary" onPress={refresh} busy={busy === 'refresh'} disabled={!ready} />
        {status ? (
          <KeyValues
            rows={[
              ['Storage', status.storage.mounted
                ? `${formatBytes(status.storage.freeBytes)} free of ${formatBytes(status.storage.capacityBytes)}`
                : 'no card'],
              ['Transfer mode', status.transferMode ? 'on' : 'off'],
              ['Open uploads', String(status.openSessions)],
            ]}
          />
        ) : null}
        {error ? <Note tone="danger">{error}</Note> : null}
        {message ? <Note>{message}</Note> : null}
      </Card>

      <Card title="Install a package">
        <Note>Pick a .qpk file. An interrupted upload resumes where the device left off.</Note>
        <Button title="Choose package…" onPress={installPackage} busy={busy === 'upload'} disabled={!ready || busy !== null} />
        {upload ? <Progress transfer={upload} /> : null}
      </Card>

      <Card title="On the device">
        {listing === null ? (
          <Note>Refresh to see what is installed.</Note>
        ) : listing.items.length === 0 ? (
          <Note>Nothing installed.</Note>
        ) : (
          listing.items.map((item) => (
            <View
              key={item.contentId}
              style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 6 }}
            >
              <View style={{ flex: 1 }}>
                <Text style={{ color: colors.ink, fontSize: 15 }}>{item.title}</Text>
                <Text style={{ color: colors.muted, fontSize: 12 }}>
                  {item.type} · {item.language} · {formatBytes(item.packageSize)}
                </Text>
              </View>
              <Button
                title="Delete"
                kind="secondary"
                onPress={() => remove(item.contentId, item.title)}
                disabled={busy !== null}
              />
            </View>
          ))
        )}
      </Card>

      <Card title="Update firmware">
        <Note>Pick firmware.bin from a PlatformIO build. The device restarts when it is installed.</Note>
        <Button title="Choose firmware…" onPress={updateFirmware} busy={busy === 'firmware'} disabled={!ready || busy !== null} />
        {firmware ? <Progress transfer={firmware} /> : null}
      </Card>
    </ScrollView>
  );
}

function Progress({ transfer }: { transfer: Transfer }) {
  return (
    <View style={{ gap: 4 }}>
      <Text style={{ color: colors.ink }}>{transfer.name}</Text>
      <Meter value={transfer.sentBytes} total={transfer.totalBytes} />
      <Note>
        {transfer.done
          ? 'Done'
          : `${formatBytes(transfer.sentBytes)} of ${formatBytes(transfer.totalBytes)}`}
        {transfer.resyncs > 0 ? ` · resumed ${transfer.resyncs}×` : ''}
      </Note>
    </View>
  );
}
