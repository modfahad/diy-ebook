// Photos: pictures for the device's home screen, and its clock's time zone
// -- the desktop Photos tab on a phone (docs/android.md milestone 5).
//
// The render worker decodes a picture and crops it to 400x480; device-client's
// photo.ts turns that into the panel's four greys here in the app, so the
// preview shows exactly the levels that are uploaded.

import * as DocumentPicker from 'expo-document-picker';
import { File } from 'expo-file-system';
import { useEffect, useState } from 'react';
import { Image, ScrollView, Text, View } from 'react-native';

import {
  PHOTO_HEIGHT,
  PHOTO_WIDTH,
  convertPhoto,
  photoNameFor,
  posixTimeZone,
} from '@quran-device/device-client';
import type { PhotoListing, TimeZoneResponse } from '@quran-device/protocol';

import { decodeBase64 } from './base64';
import { clientFor } from './deviceClient';
import { levelsPngDataUrl } from './png';
import { useRenderWorker } from './render/RenderWorker';
import type { Settings } from './settings';
import { Button, Card, colors, describeError, Meter, Note } from './ui';

interface PendingPhoto {
  key: string;
  name: string;
  source: string;
  rgba: Uint8Array;
  file: Uint8Array;
  preview: string;
  state: 'ready' | 'uploading' | 'uploaded' | 'failed';
  sentBytes: number;
}

/** Unique among `taken`: "sunset", "sunset-2", ... within 32 characters. */
function uniqueName(base: string, taken: Set<string>): string {
  if (!taken.has(base)) return base;
  for (let n = 2; ; n++) {
    const suffix = `-${n}`;
    const candidate = `${base.slice(0, 32 - suffix.length)}${suffix}`;
    if (!taken.has(candidate)) return candidate;
  }
}

function converted(rgba: Uint8Array, contrast: number): { file: Uint8Array; preview: string } {
  const { file, levels } = convertPhoto(rgba, { contrast });
  return { file, preview: levelsPngDataUrl(PHOTO_WIDTH, PHOTO_HEIGHT, levels) };
}

export default function Photos({ settings }: { settings: Settings }) {
  const worker = useRenderWorker();
  const [contrast, setContrast] = useState(0.15);
  const [pending, setPending] = useState<PendingPhoto[]>([]);
  const [listing, setListing] = useState<PhotoListing | null>(null);
  const [timeZone, setTimeZone] = useState<TimeZoneResponse | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const ready = settings.deviceHost.trim() !== '' && settings.deviceToken.trim() !== '';
  let localRule = 'UTC0';
  try {
    localRule = posixTimeZone();
  } catch {
    // Keep UTC if this JavaScript engine's clock cannot say.
  }

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

  // Re-dither everything not yet uploaded when the contrast changes.
  useEffect(() => {
    setPending((photos) =>
      photos.map((photo) => (photo.state === 'uploaded' ? photo : { ...photo, ...converted(photo.rgba, contrast) })),
    );
  }, [contrast]);

  const refresh = () =>
    run('list', async () => {
      setListing(await clientFor(settings).listPhotos());
    });

  const addPictures = () =>
    run('convert', async () => {
      const picked = await DocumentPicker.getDocumentAsync({
        type: 'image/*',
        multiple: true,
        copyToCacheDirectory: true,
      });
      if (picked.canceled) return;
      const taken = new Set([...(listing?.items.map((item) => item.name) ?? []), ...pending.map((p) => p.name)]);
      const added: PendingPhoto[] = [];
      for (const asset of picked.assets) {
        const bytes = new Uint8Array(await new File(asset.uri).arrayBuffer());
        const reply = await worker.call<{ rgba: string }>(
          'photoRgba',
          { mediaType: asset.mimeType ?? '' },
          { bytes: { key: bytes } },
        );
        const rgba = decodeBase64(reply.rgba);
        const name = uniqueName(photoNameFor(asset.name), taken);
        taken.add(name);
        added.push({
          key: `${name}-${Date.now()}-${Math.random()}`,
          name,
          source: asset.name,
          rgba,
          ...converted(rgba, contrast),
          state: 'ready',
          sentBytes: 0,
        });
      }
      setPending((photos) => [...photos, ...added]);
      return `Converted ${added.length} picture${added.length === 1 ? '' : 's'}.`;
    });

  const uploadAll = () =>
    run('upload', async () => {
      const client = clientFor(settings);
      let uploaded = 0;
      for (const photo of pending) {
        if (photo.state === 'uploaded') continue;
        const update = (patch: Partial<PendingPhoto>) =>
          setPending((photos) => photos.map((p) => (p.key === photo.key ? { ...p, ...patch } : p)));
        update({ state: 'uploading', sentBytes: 0 });
        try {
          await client.uploadPhoto(photo.name, photo.file, {
            onProgress: (progress) => update({ sentBytes: progress.sentBytes }),
          });
          update({ state: 'uploaded', sentBytes: photo.file.length });
          uploaded++;
        } catch (caught) {
          update({ state: 'failed' });
          throw caught;
        }
      }
      setListing(await client.listPhotos());
      return `Uploaded ${uploaded} photo${uploaded === 1 ? '' : 's'}. The device shows a new one within a few seconds.`;
    });

  const remove = (name: string) =>
    run('delete', async () => {
      const client = clientFor(settings);
      await client.deletePhoto(name);
      setListing(await client.listPhotos());
      return `Deleted ${name} from the device.`;
    });

  const sendTimeZone = () =>
    run('time', async () => {
      const reply = await clientFor(settings).setTimeZone(localRule);
      setTimeZone(reply);
      return `The device's clock now uses ${reply.tz}.`;
    });

  const waiting = pending.filter((photo) => photo.state !== 'uploaded').length;

  return (
    <ScrollView contentContainerStyle={{ padding: 16 }}>
      {!ready ? (
        <Card title="Not connected">
          <Note>Set the device address and pairing token on the Dashboard to upload or send the time zone.</Note>
        </Card>
      ) : null}

      <Card title="Clock time zone">
        <Note>Sent as a POSIX rule, so the device needs no time-zone database.</Note>
        <Text style={{ fontFamily: 'monospace', color: colors.ink }}>{localRule}</Text>
        <Button title="Send to device" onPress={sendTimeZone} busy={busy === 'time'} disabled={!ready || busy !== null} />
        {timeZone ? (
          <Note>
            Device: {timeZone.tz} · {timeZone.synced ? 'time synced' : 'not synced yet'}
          </Note>
        ) : null}
      </Card>

      <Card title="Add photos">
        <Note>
          Pictures are cropped to {PHOTO_WIDTH}x{PHOTO_HEIGHT} from the centre and turned into the panel's four
          greys.
        </Note>
        <Button
          title="Choose pictures…"
          onPress={addPictures}
          busy={busy === 'convert'}
          disabled={busy !== null || !worker.ready}
        />
        {!worker.ready ? <Note>Starting the picture converter…</Note> : null}
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8 }}>
          <Button title="−" kind="secondary" onPress={() => setContrast((c) => Math.max(-0.5, +(c - 0.05).toFixed(2)))} />
          <Text style={{ color: colors.ink, minWidth: 110, textAlign: 'center' }}>Contrast {contrast.toFixed(2)}</Text>
          <Button title="+" kind="secondary" onPress={() => setContrast((c) => Math.min(0.8, +(c + 0.05).toFixed(2)))} />
        </View>
        <Button
          title={`Upload ${waiting} photo${waiting === 1 ? '' : 's'}`}
          onPress={uploadAll}
          busy={busy === 'upload'}
          disabled={!ready || waiting === 0 || busy !== null}
        />
        {error ? <Note tone="danger">{error}</Note> : null}
        {message ? <Note>{message}</Note> : null}

        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: 12 }}>
          {pending.map((photo) => (
            <View key={photo.key} style={{ width: 150, gap: 4 }}>
              <Image
                source={{ uri: photo.preview }}
                style={{ width: 150, height: 180, borderWidth: 1, borderColor: colors.line }}
              />
              <Text style={{ color: colors.ink }}>{photo.name}</Text>
              {photo.state === 'uploaded' ? <Note>on device</Note> : null}
              {photo.state === 'failed' ? <Note tone="danger">failed</Note> : null}
              {photo.state === 'uploading' ? <Meter value={photo.sentBytes} total={photo.file.length} /> : null}
              {photo.state === 'ready' ? (
                <Button
                  title="Remove"
                  kind="secondary"
                  onPress={() => setPending((photos) => photos.filter((p) => p.key !== photo.key))}
                  disabled={busy !== null}
                />
              ) : null}
            </View>
          ))}
        </View>
      </Card>

      <Card title="On the device">
        <Note>Shown in name order, a new one every 10 minutes.</Note>
        <Button title="Refresh" kind="secondary" onPress={refresh} busy={busy === 'list'} disabled={!ready || busy !== null} />
        {listing === null ? <Note>Not read yet.</Note> : null}
        {listing && listing.items.length === 0 ? <Note>No photos on the device.</Note> : null}
        {listing?.items.map((item) => (
          <View key={item.name} style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' }}>
            <Text style={{ color: colors.ink }}>{item.name}</Text>
            <Button title="Delete" kind="secondary" onPress={() => remove(item.name)} disabled={busy !== null} />
          </View>
        ))}
      </Card>
    </ScrollView>
  );
}
