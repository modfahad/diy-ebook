// Turning a picture into a home-screen photo for the device, and the
// computer's time zone into a rule the device can apply.
//
// Dependency-free on purpose: the app's Photos tab runs this in the webview
// (a canvas decodes and scales the picture; this does everything after that),
// and the device-client tests run it under Node. The file layout mirrors
// firmware/include/net/photo_store.h and PHOTO_* in @quran-device/protocol;
// the tests check these constants against the protocol package.
//
//   "QPH1" | width u16 LE | height u16 LE | bpp u8 = 2 | 7 zero bytes | pixels
//
// pixels: row-major, 4 per byte, MSB first; 0 black, 1 dark grey,
// 2 light grey, 3 white.

export const PHOTO_WIDTH = 400;
export const PHOTO_HEIGHT = 480;
export const PHOTO_HEADER_BYTES = 16;
export const PHOTO_FILE_BYTES = PHOTO_HEADER_BYTES + (PHOTO_WIDTH / 4) * PHOTO_HEIGHT;
export const PHOTO_NAME_PATTERN = /^[a-z0-9_-]{1,32}$/u;

/** The panel's four levels as 8-bit grey: black, dark grey, light grey, white. */
export const PANEL_GREYS = [0, 85, 170, 255] as const;

const MAGIC = [0x51, 0x50, 0x48, 0x31]; // "QPH1"

export interface CropRect {
  sx: number;
  sy: number;
  sw: number;
  sh: number;
}

/** The centred region of a width x height picture with the photo's 5:6 shape ("cover"). */
export function coverCrop(width: number, height: number): CropRect {
  if (!(width > 0 && height > 0)) throw new Error(`invalid picture size ${width}x${height}`);
  const target = PHOTO_WIDTH / PHOTO_HEIGHT;
  if (width / height > target) {
    const sw = Math.max(1, Math.round(height * target));
    return { sx: Math.floor((width - sw) / 2), sy: 0, sw, sh: height };
  }
  const sh = Math.max(1, Math.round(width / target));
  return { sx: 0, sy: Math.floor((height - sh) / 2), sw: width, sh };
}

export interface ConvertOptions {
  /**
   * -1..1. Positive stretches tones away from mid-grey before quantizing.
   * Default 0.15: four e-paper greys look flat without a little.
   */
  contrast?: number;
  /** -1..1, added to brightness (1 = +255). Default 0. */
  brightness?: number;
  /** Floyd-Steinberg error diffusion (default), or the nearest level per pixel. */
  dither?: boolean;
}

export interface ConvertedPhoto {
  /** The complete .g4 file the device stores. */
  file: Uint8Array;
  /** PHOTO_WIDTH x PHOTO_HEIGHT levels 0..3, row-major -- for a preview. */
  levels: Uint8Array;
}

function clamp(value: number, low: number, high: number): number {
  return value < low ? low : value > high ? high : value;
}

function addAt(values: Float32Array, index: number, amount: number): void {
  values[index] = (values[index] ?? 0) + amount;
}

/**
 * `rgba` is PHOTO_WIDTH x PHOTO_HEIGHT pixels, 4 bytes each, in canvas
 * `getImageData` order -- the picture already cropped and scaled.
 */
export function convertPhoto(rgba: ArrayLike<number>, options: ConvertOptions = {}): ConvertedPhoto {
  const levels = ditherToLevels(rgba, PHOTO_WIDTH, PHOTO_HEIGHT, options);
  return { file: encodePhotoFile(levels), levels };
}

export interface BitsOptions {
  /** Luma at or below this is solid black, never dithered. Default 96. */
  black?: number;
  /** Luma at or above this is solid white, never dithered. Default 200. */
  white?: number;
}

/**
 * A picture as 1 bit per pixel for the black/white page reader: row-major,
 * ceil(width / 8) bytes a row, MSB first, 1 = black (docs/qpk-format.md 9d).
 * Dark and light pixels snap straight to black and white, so text stays crisp;
 * only mid-tones -- pictures, shading, anti-aliased edges -- are
 * error-diffused, which is what keeps photos readable in two colours.
 */
export function ditherToBits(
  rgba: ArrayLike<number>,
  width: number,
  height: number,
  options: BitsOptions = {},
): Uint8Array {
  const count = width * height;
  if (rgba.length !== count * 4) {
    throw new Error(`expected ${count * 4} RGBA bytes for ${width}x${height}, got ${rgba.length}`);
  }
  const black = options.black ?? 96;
  const white = options.white ?? 200;
  // `original` decides solid versus dithered; `grey` carries the diffused
  // error. Deciding on the diffused value instead would let error snap a
  // mid-tone pixel to solid and drop that error, darkening every grey.
  const original = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    const alpha = (rgba[i * 4 + 3] ?? 255) / 255;
    original[i] =
      (0.299 * (rgba[i * 4] ?? 0) + 0.587 * (rgba[i * 4 + 1] ?? 0) + 0.114 * (rgba[i * 4 + 2] ?? 0)) *
        alpha +
      255 * (1 - alpha);
  }
  const grey = Float32Array.from(original);

  const stride = Math.ceil(width / 8);
  const bits = new Uint8Array(stride * height);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = y * width + x;
      const source = original[i] ?? 255;
      const value = grey[i] ?? 255;
      let ink: boolean;
      if (source <= black) {
        ink = true;
      } else if (source >= white) {
        ink = false;
      } else {
        ink = value < 128;
        const error = value - (ink ? 0 : 255);
        if (x + 1 < width) addAt(grey, i + 1, (error * 7) / 16);
        if (y + 1 < height) {
          if (x > 0) addAt(grey, i + width - 1, (error * 3) / 16);
          addAt(grey, i + width, (error * 5) / 16);
          if (x + 1 < width) addAt(grey, i + width + 1, error / 16);
        }
      }
      if (ink) {
        const at = y * stride + (x >> 3);
        bits[at] = (bits[at] ?? 0) | (0x80 >> (x & 7));
      }
    }
  }
  return bits;
}

/**
 * The same conversion for a picture of any size: `rgba` is width x height
 * pixels in `getImageData` order; the result is one level 0..3 per pixel,
 * row-major. Book covers (docs/qpk-format.md 9c) use it at 108x144.
 */
export function ditherToLevels(
  rgba: ArrayLike<number>,
  width: number,
  height: number,
  options: ConvertOptions = {},
): Uint8Array {
  const count = width * height;
  if (rgba.length !== count * 4) {
    throw new Error(`expected ${count * 4} RGBA bytes for ${width}x${height}, got ${rgba.length}`);
  }
  const contrast = clamp(options.contrast ?? 0.15, -1, 1);
  const brightness = clamp(options.brightness ?? 0, -1, 1);
  const factor = contrast >= 0 ? 1 / (1 - contrast * 0.9) : 1 + contrast;

  const grey = new Float32Array(count);
  for (let i = 0; i < count; i++) {
    const r = rgba[i * 4] ?? 0;
    const g = rgba[i * 4 + 1] ?? 0;
    const b = rgba[i * 4 + 2] ?? 0;
    const alpha = (rgba[i * 4 + 3] ?? 255) / 255;
    // Rec. 601 luma, over a white background where the picture is transparent.
    let y = (0.299 * r + 0.587 * g + 0.114 * b) * alpha + 255 * (1 - alpha);
    y = (y - 127.5) * factor + 127.5 + brightness * 255;
    grey[i] = clamp(y, 0, 255);
  }

  const levels = new Uint8Array(count);
  const dither = options.dither ?? true;
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const i = y * width + x;
      const value = grey[i] ?? 0;
      const level = clamp(Math.round(value / 85), 0, 3);
      levels[i] = level;
      if (!dither) continue;
      const error = value - PANEL_GREYS[level as 0 | 1 | 2 | 3];
      if (x + 1 < width) addAt(grey, i + 1, (error * 7) / 16);
      if (y + 1 < height) {
        if (x > 0) addAt(grey, i + width - 1, (error * 3) / 16);
        addAt(grey, i + width, (error * 5) / 16);
        if (x + 1 < width) addAt(grey, i + width + 1, error / 16);
      }
    }
  }
  return levels;
}

/** Header plus pixels, from PHOTO_WIDTH x PHOTO_HEIGHT levels 0..3. */
export function encodePhotoFile(levels: ArrayLike<number>): Uint8Array {
  const count = PHOTO_WIDTH * PHOTO_HEIGHT;
  if (levels.length !== count) throw new Error(`expected ${count} levels, got ${levels.length}`);
  const file = new Uint8Array(PHOTO_FILE_BYTES);
  file.set(MAGIC, 0);
  file[4] = PHOTO_WIDTH & 0xff;
  file[5] = PHOTO_WIDTH >> 8;
  file[6] = PHOTO_HEIGHT & 0xff;
  file[7] = PHOTO_HEIGHT >> 8;
  file[8] = 2;
  const stride = PHOTO_WIDTH / 4;
  for (let y = 0; y < PHOTO_HEIGHT; y++) {
    for (let x = 0; x < PHOTO_WIDTH; x++) {
      const level = clamp(Math.trunc(levels[y * PHOTO_WIDTH + x] ?? 3), 0, 3);
      const index = PHOTO_HEADER_BYTES + y * stride + (x >> 2);
      file[index] = (file[index] ?? 0) | (level << (6 - 2 * (x & 3)));
    }
  }
  return file;
}

/** The levels back out of a .g4 file. Throws if it is not one. */
export function decodePhotoFile(file: Uint8Array): Uint8Array {
  if (file.length !== PHOTO_FILE_BYTES) {
    throw new Error(`a photo file is ${PHOTO_FILE_BYTES} bytes, this is ${file.length}`);
  }
  const magicOk = MAGIC.every((byte, i) => file[i] === byte);
  const width = (file[4] ?? 0) | ((file[5] ?? 0) << 8);
  const height = (file[6] ?? 0) | ((file[7] ?? 0) << 8);
  const reservedZero = file.subarray(9, PHOTO_HEADER_BYTES).every((byte) => byte === 0);
  if (!magicOk || width !== PHOTO_WIDTH || height !== PHOTO_HEIGHT || file[8] !== 2 || !reservedZero) {
    throw new Error('not a QPH1 photo file');
  }
  const levels = new Uint8Array(PHOTO_WIDTH * PHOTO_HEIGHT);
  const stride = PHOTO_WIDTH / 4;
  for (let y = 0; y < PHOTO_HEIGHT; y++) {
    for (let x = 0; x < PHOTO_WIDTH; x++) {
      const byte = file[PHOTO_HEADER_BYTES + y * stride + (x >> 2)] ?? 0;
      levels[y * PHOTO_WIDTH + x] = (byte >> (6 - 2 * (x & 3))) & 3;
    }
  }
  return levels;
}

/** A device-safe photo name from a file name: "Beach Day (2).JPG" -> "beach-day-2". */
export function photoNameFor(fileName: string): string {
  const base = fileName.replace(/^.*[\\/]/u, '').replace(/\.[^.]*$/u, '');
  const name = base
    .toLowerCase()
    .replace(/[^a-z0-9_-]+/gu, '-')
    .replace(/-+/gu, '-')
    .replace(/^[-_]+|[-_]+$/gu, '')
    .slice(0, 32)
    .replace(/[-_]+$/u, '');
  return name === '' ? 'photo' : name;
}

// --- time zone -----------------------------------------------------------------

function offsetMinutesEast(utcMs: number): number {
  // getTimezoneOffset() is minutes WEST of UTC; `|| 0` turns -0 into 0.
  return -new Date(utcMs).getTimezoneOffset() || 0;
}

/** A POSIX offset: positive WEST of UTC, e.g. India (east 330 min) -> "-5:30". */
function posixOffset(minutesEast: number): string {
  const west = -minutesEast;
  const sign = west < 0 ? '-' : '';
  const abs = Math.abs(west);
  const hours = Math.floor(abs / 60);
  const minutes = abs % 60;
  return `${sign}${hours}${minutes ? `:${String(minutes).padStart(2, '0')}` : ''}`;
}

/** The instant (UTC ms, minute resolution) in [start, end) at which the offset first equals `after`. */
function findTransition(start: number, end: number, after: number): number {
  let low = start;
  let high = end;
  while (high - low > 60_000) {
    const mid = low + Math.floor((high - low) / 120_000) * 60_000;
    if (offsetMinutesEast(mid) === after) high = mid;
    else low = mid;
  }
  return high;
}

/** "Mm.w.d/time" for a transition, in the wall-clock time in force just before it. */
function posixRule(transitionUtcMs: number, offsetBefore: number): string {
  const local = new Date(transitionUtcMs + offsetBefore * 60_000);
  const year = local.getUTCFullYear();
  const month = local.getUTCMonth();
  const day = local.getUTCDate();
  const daysInMonth = new Date(Date.UTC(year, month + 1, 0)).getUTCDate();
  const week = day + 7 > daysInMonth ? 5 : Math.ceil(day / 7);
  const hours = local.getUTCHours();
  const minutes = local.getUTCMinutes();
  const time = `${hours}${minutes ? `:${String(minutes).padStart(2, '0')}` : ''}`;
  return `M${month + 1}.${week}.${local.getUTCDay()}/${time}`;
}

/**
 * This computer's local time zone as a POSIX TZ rule the device applies with
 * no time-zone database: "LOC-5:30" for India, "LOC-1LDT-2,M3.5.0/2,M10.5.0/3"
 * for Central Europe. Derived from the JavaScript clock for `year`, so a rule
 * that changes between years is right for that year; the app sends it again
 * whenever it talks to the device.
 */
export function posixTimeZone(year: number = new Date().getFullYear()): string {
  const january = offsetMinutesEast(Date.UTC(year, 0, 1));
  const july = offsetMinutesEast(Date.UTC(year, 6, 1));
  const standard = Math.min(january, july);
  const daylight = Math.max(january, july);
  if (standard === daylight) return `LOC${posixOffset(standard)}`;

  let toDaylight: string | null = null;
  let toStandard: string | null = null;
  let previous = offsetMinutesEast(Date.UTC(year, 0, 1));
  for (let day = 1; day <= 366; day++) {
    const dayStart = Date.UTC(year, 0, day);
    const next = offsetMinutesEast(dayStart + 86_400_000);
    if (next !== previous) {
      const at = findTransition(dayStart, dayStart + 86_400_000, next);
      if (next === daylight) toDaylight = posixRule(at, previous);
      else toStandard = posixRule(at, previous);
    }
    previous = next;
  }
  if (toDaylight === null || toStandard === null) return `LOC${posixOffset(standard)}`;
  return `LOC${posixOffset(standard)}LDT${posixOffset(daylight)},${toDaylight},${toStandard}`;
}
