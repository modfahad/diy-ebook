// Book covers in the app: turning a picture into a package's COVER section,
// and drawing one back.
//
// A cover is 108x144 in the panel's four greys (docs/qpk-format.md 9c). The
// crop and dithering happen here in the webview, like the Photos tab's,
// because this is where pictures can be decoded; the converter only packages
// the finished payload. Thumbnails are drawn from exactly those levels, so
// they show what the device will.

import { useMemo } from "react";
import {
  COVER_HEIGHT,
  COVER_WIDTH,
  decodeCover,
  encodeCover,
} from "../../packages/qpk-format/src/format.ts";
import { PANEL_GREYS, ditherToLevels } from "../device-client/src/photo.ts";

/** Crops a picture to the cover's 3:4 from the centre and dithers it to four greys. */
export async function pictureToCover(picture: Blob, contrast = 0.15): Promise<Uint8Array> {
  const bitmap = await createImageBitmap(picture);
  try {
    const target = COVER_WIDTH / COVER_HEIGHT;
    let sw = bitmap.width;
    let sh = bitmap.height;
    if (sw / sh > target) sw = Math.max(1, Math.round(sh * target));
    else sh = Math.max(1, Math.round(sw / target));

    const canvas = document.createElement("canvas");
    canvas.width = COVER_WIDTH;
    canvas.height = COVER_HEIGHT;
    const context = canvas.getContext("2d");
    if (!context) throw new Error("this webview has no 2D canvas");
    context.fillStyle = "#fff";
    context.fillRect(0, 0, COVER_WIDTH, COVER_HEIGHT);
    context.imageSmoothingQuality = "high";
    context.drawImage(
      bitmap,
      Math.floor((bitmap.width - sw) / 2),
      Math.floor((bitmap.height - sh) / 2),
      sw,
      sh,
      0,
      0,
      COVER_WIDTH,
      COVER_HEIGHT,
    );
    const { data } = context.getImageData(0, 0, COVER_WIDTH, COVER_HEIGHT);
    return ditherToLevels(data, COVER_WIDTH, COVER_HEIGHT, { contrast });
  } finally {
    bitmap.close();
  }
}

/** The COVER payload for these levels, base64, as the bridge's convert request takes it. */
export function coverPayloadBase64(levels: Uint8Array): string {
  let binary = "";
  for (const byte of encodeCover(levels)) binary += String.fromCharCode(byte);
  return btoa(binary);
}

export function base64ToBytes(data: string): Uint8Array {
  const binary = atob(data);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

function levelsToDataUrl(levels: Uint8Array): string {
  const canvas = document.createElement("canvas");
  canvas.width = COVER_WIDTH;
  canvas.height = COVER_HEIGHT;
  const context = canvas.getContext("2d");
  if (!context) return "";
  const image = context.createImageData(COVER_WIDTH, COVER_HEIGHT);
  levels.forEach((level, i) => {
    const grey = PANEL_GREYS[(level & 3) as 0 | 1 | 2 | 3];
    image.data[i * 4] = grey;
    image.data[i * 4 + 1] = grey;
    image.data[i * 4 + 2] = grey;
    image.data[i * 4 + 3] = 255;
  });
  context.putImageData(image, 0, 0);
  return canvas.toDataURL("image/png");
}

function BookGlyph({ size }: { size: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" aria-hidden="true">
      <path
        d="M3 5c3-1 6-1 9 1 3-2 6-2 9-1v14c-3-1-6-1-9 1-3-2-6-2-9-1z M12 6v14"
        fill="none"
        stroke="currentColor"
        strokeWidth="1.6"
        strokeLinejoin="round"
      />
    </svg>
  );
}

/**
 * A cover thumbnail from `levels` (a picture being prepared) or `payload` (a
 * package's COVER section, as the library scan returns it). With neither, or
 * a malformed payload, it is the drawn placeholder -- book icon and title --
 * that the device also shows.
 */
export function CoverThumb({
  levels,
  payload,
  title,
  scale = 1,
}: {
  levels?: Uint8Array | null;
  payload?: ArrayLike<number> | null;
  title?: string | null;
  scale?: number;
}) {
  const resolved = useMemo(() => {
    if (levels) return levels;
    if (!payload) return null;
    try {
      return decodeCover(Uint8Array.from(payload));
    } catch {
      return null;
    }
  }, [levels, payload]);
  const url = useMemo(() => (resolved ? levelsToDataUrl(resolved) : null), [resolved]);

  const width = Math.round(COVER_WIDTH * scale);
  const height = Math.round(COVER_HEIGHT * scale);
  if (url) {
    return (
      <img
        src={url}
        alt={title ? `Cover of ${title}` : "Cover"}
        width={width}
        height={height}
        style={{ imageRendering: "pixelated", border: "1px solid #888", display: "block" }}
      />
    );
  }
  return (
    <div
      title="No cover picture: the device draws this instead"
      style={{
        width,
        height,
        boxSizing: "border-box",
        border: "2px solid #222",
        background: "#fff",
        color: "#111",
        display: "flex",
        flexDirection: "column",
        alignItems: "center",
        justifyContent: "center",
        gap: 4,
        padding: 4,
        textAlign: "center",
        fontSize: Math.max(9, Math.round(11 * scale)),
        lineHeight: 1.2,
        overflow: "hidden",
        wordBreak: "break-word",
      }}
    >
      <BookGlyph size={Math.round(28 * scale)} />
      <span>{title || "Untitled"}</span>
    </div>
  );
}
