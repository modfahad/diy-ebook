// The Photos tab: pictures for the device's home screen, and its clock's
// time zone.
//
// The device shows a 400x480 photo in four greys beside its clock. All the
// image work happens here, on the computer: the webview decodes the picture
// (anything a browser can open), a canvas crops and scales it, and
// device-client's photo.ts turns it into the panel's four greys with error
// diffusion. The preview is drawn from exactly the levels that are uploaded,
// so what you see here is what the panel gets.
//
// Like the Device tab, everything but the conversion needs the device on the
// network and the pairing token.

import { useCallback, useEffect, useRef, useState } from "react";
import {
  PANEL_GREYS,
  PHOTO_HEIGHT,
  PHOTO_WIDTH,
  convertPhoto,
  coverCrop,
  photoNameFor,
  posixTimeZone,
} from "../device-client/src/photo.ts";
import {
  devicePhotoDelete,
  devicePhotoUpload,
  devicePhotos,
  deviceSetTimeZone,
  type DeviceConnection,
  type PhotoListing,
  type TimeZoneResponse,
} from "./bridge";
import { Badge, Card, Empty, Failure, Field, Meter, Note } from "./ui";
import type { Settings } from "./settings";

interface PendingPhoto {
  key: string;
  name: string;
  source: string;
  rgba: Uint8ClampedArray;
  file: Uint8Array;
  levels: Uint8Array;
  state: "ready" | "uploading" | "uploaded" | "failed";
  sentBytes: number;
}

const WIDTH = PHOTO_WIDTH;
const HEIGHT = PHOTO_HEIGHT;

/** Crops and scales a picture file to the photo size, as RGBA. */
async function loadPicture(file: File): Promise<Uint8ClampedArray> {
  const bitmap = await createImageBitmap(file);
  try {
    const crop = coverCrop(bitmap.width, bitmap.height);
    const canvas = document.createElement("canvas");
    canvas.width = WIDTH;
    canvas.height = HEIGHT;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) throw new Error("this webview has no 2D canvas");
    context.fillStyle = "#fff";
    context.fillRect(0, 0, WIDTH, HEIGHT);
    context.imageSmoothingQuality = "high";
    context.drawImage(bitmap, crop.sx, crop.sy, crop.sw, crop.sh, 0, 0, WIDTH, HEIGHT);
    return context.getImageData(0, 0, WIDTH, HEIGHT).data;
  } finally {
    bitmap.close();
  }
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

function Preview({ levels }: { levels: Uint8Array }) {
  const ref = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    const canvas = ref.current;
    const context = canvas?.getContext("2d");
    if (!canvas || !context) return;
    const image = context.createImageData(WIDTH, HEIGHT);
    for (let i = 0; i < levels.length; i++) {
      const grey = PANEL_GREYS[(levels[i] ?? 3) as 0 | 1 | 2 | 3];
      image.data[i * 4] = grey;
      image.data[i * 4 + 1] = grey;
      image.data[i * 4 + 2] = grey;
      image.data[i * 4 + 3] = 255;
    }
    context.putImageData(image, 0, 0);
  }, [levels]);
  return (
    <canvas
      ref={ref}
      width={WIDTH}
      height={HEIGHT}
      style={{ width: 150, height: 180, border: "1px solid #999", imageRendering: "pixelated" }}
    />
  );
}

export default function Photos({ settings }: { settings: Settings }) {
  const [contrast, setContrast] = useState(0.15);
  const [pending, setPending] = useState<PendingPhoto[]>([]);
  const [listing, setListing] = useState<PhotoListing | null>(null);
  const [timeZone, setTimeZone] = useState<TimeZoneResponse | null>(null);
  const [busy, setBusy] = useState<string | null>(null);
  const [error, setError] = useState<unknown>(null);
  const [message, setMessage] = useState<string | null>(null);

  const connection: DeviceConnection = {
    host: settings.deviceHost.trim(),
    port: settings.devicePort,
    token: settings.deviceToken.trim(),
  };
  const connected = connection.host !== "";
  const localRule = posixTimeZone();

  const run = useCallback(async <T,>(what: string, action: () => Promise<T>, then?: (value: T) => void) => {
    setBusy(what);
    setError(null);
    try {
      then?.(await action());
    } catch (failure) {
      setError(failure);
    } finally {
      setBusy(null);
    }
  }, []);

  // Re-dither everything already chosen when the contrast changes.
  useEffect(() => {
    setPending((photos) =>
      photos.map((photo) =>
        photo.state === "uploaded" ? photo : { ...photo, ...convertPhoto(photo.rgba, { contrast }) },
      ),
    );
  }, [contrast]);

  const refresh = (keepMessage = false) =>
    run("list", () => devicePhotos(connection), (value) => {
      setListing(value);
      if (!keepMessage) setMessage(null);
    });

  const addFiles = (files: FileList | null) => {
    if (!files || files.length === 0) return;
    const chosen = Array.from(files);
    void run("convert", async () => {
      const taken = new Set([
        ...(listing?.items.map((item) => item.name) ?? []),
        ...pending.map((photo) => photo.name),
      ]);
      const added: PendingPhoto[] = [];
      for (const file of chosen) {
        const rgba = await loadPicture(file);
        const name = uniqueName(photoNameFor(file.name), taken);
        taken.add(name);
        added.push({
          key: `${name}-${Date.now()}-${Math.random()}`,
          name,
          source: file.name,
          rgba,
          ...convertPhoto(rgba, { contrast }),
          state: "ready",
          sentBytes: 0,
        });
      }
      return added;
    }, (added) => {
      setPending((photos) => [...photos, ...added]);
      setMessage(`Converted ${added.length} picture${added.length === 1 ? "" : "s"}.`);
    });
  };

  const uploadAll = () =>
    run("upload", async () => {
      let uploaded = 0;
      for (const photo of pending) {
        if (photo.state === "uploaded") continue;
        const update = (patch: Partial<PendingPhoto>) =>
          setPending((photos) => photos.map((p) => (p.key === photo.key ? { ...p, ...patch } : p)));
        update({ state: "uploading", sentBytes: 0 });
        try {
          await devicePhotoUpload(connection, photo.name, photo.file, (progress) =>
            update({ sentBytes: progress.sentBytes }),
          );
          update({ state: "uploaded", sentBytes: photo.file.length });
          uploaded++;
        } catch (failure) {
          update({ state: "failed" });
          throw failure;
        }
      }
      return uploaded;
    }, (uploaded) => {
      setMessage(`Uploaded ${uploaded} photo${uploaded === 1 ? "" : "s"}. The device shows a new one within a few seconds.`);
      void refresh(true);
    });

  const remove = (name: string) =>
    run("delete", () => devicePhotoDelete(connection, name), () => {
      setMessage(`Deleted ${name} from the device.`);
      void refresh(true);
    });

  const sendTimeZone = () =>
    run("time", () => deviceSetTimeZone(connection, localRule), (value) => {
      setTimeZone(value);
      setMessage(`The device's clock now uses ${value.tz}.`);
    });

  const waiting = pending.filter((photo) => photo.state !== "uploaded").length;

  return (
    <div className="stack">
      <Card
        title="Clock time zone"
        subtitle="The device gets the time from the internet; this tells it which local time to show."
      >
        <div className="row">
          <Field label="This computer" hint="Sent as a POSIX rule, so the device needs no time-zone database." wide>
            <code>{localRule}</code>
          </Field>
          <button onClick={sendTimeZone} disabled={!connected || busy !== null}>
            {busy === "time" ? "Sending..." : "Send to device"}
          </button>
        </div>
        {timeZone && (
          <p className="muted">
            Device: <code>{timeZone.tz}</code>{" "}
            {timeZone.synced ? <Badge tone="good">time synced</Badge> : <Badge tone="warn">not synced yet</Badge>}
          </p>
        )}
      </Card>

      <Card
        title="Add photos"
        subtitle={`Pictures are cropped to ${WIDTH}x${HEIGHT} from the centre and turned into the panel's four greys.`}
      >
        <div className="row">
          <input
            type="file"
            accept="image/*"
            multiple
            onChange={(e) => {
              addFiles(e.currentTarget.files);
              e.currentTarget.value = "";
            }}
            disabled={busy !== null}
          />
          <Field label={`Contrast ${contrast.toFixed(2)}`}>
            <input
              type="range"
              min={-0.5}
              max={0.8}
              step={0.05}
              value={contrast}
              onChange={(e) => setContrast(Number(e.currentTarget.value))}
            />
          </Field>
          <button onClick={uploadAll} disabled={!connected || waiting === 0 || busy !== null}>
            {busy === "upload" ? "Uploading..." : `Upload ${waiting} photo${waiting === 1 ? "" : "s"}`}
          </button>
        </div>

        {!connected && (
          <Note kind="warn">Set the device's address and pairing token in the Device tab first.</Note>
        )}
        {error != null && <Failure error={error} />}
        {message && <div className="notice notice-good">{message}</div>}

        {pending.length === 0 ? (
          <Empty>No pictures chosen yet.</Empty>
        ) : (
          <div className="row" style={{ flexWrap: "wrap", alignItems: "flex-start", gap: 16 }}>
            {pending.map((photo) => (
              <div key={photo.key} style={{ width: 150 }}>
                <Preview levels={photo.levels} />
                <div>
                  <code>{photo.name}</code>
                </div>
                <div className="muted" title={photo.source}>
                  {photo.state === "uploaded" && <Badge tone="good">on device</Badge>}
                  {photo.state === "failed" && <Badge tone="bad">failed</Badge>}
                  {photo.state === "ready" && (
                    <button
                      className="danger"
                      onClick={() => setPending((photos) => photos.filter((p) => p.key !== photo.key))}
                      disabled={busy !== null}
                    >
                      Remove
                    </button>
                  )}
                </div>
                {photo.state === "uploading" && (
                  <Meter value={photo.sentBytes} max={photo.file.length} />
                )}
              </div>
            ))}
          </div>
        )}
      </Card>

      <Card
        title="On the device"
        subtitle="Shown in name order, a new one every 10 minutes."
        actions={
          <button onClick={() => refresh()} disabled={!connected || busy !== null}>
            {busy === "list" ? "Listing..." : "Refresh"}
          </button>
        }
      >
        {!listing && <Empty>Not read yet.</Empty>}
        {listing && listing.items.length === 0 && <Empty>No photos on the device.</Empty>}
        {listing && listing.items.length > 0 && (
          <table>
            <tbody>
              {listing.items.map((item) => (
                <tr key={item.name}>
                  <td>
                    <code>{item.name}</code>
                  </td>
                  <td>
                    <button className="danger" onClick={() => remove(item.name)} disabled={busy !== null}>
                      Delete
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Card>
    </div>
  );
}
