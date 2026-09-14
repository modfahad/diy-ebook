// Deriving a package's content id.
//
// The content id is the *work's* identity, not the file's. `contentVersion` is
// the revision. That split is what makes an update an update: re-converting a
// corrected copy of the same book yields the same id with a higher version, so
// the device replaces it in place rather than installing a second copy beside
// the first.
//
// So the id hashes only the fields that say *which work this is* -- type,
// title, author, language -- and deliberately not the text. Hashing the bytes
// would give every edit a new identity, and the library would fill up with
// near-duplicates that the reader has no way to tell apart.
//
// Without this every converted book carried an all-zero id, which collides on
// the very first pair of books: the library index keys on the content id, and
// so does the upload session, so two different books would overwrite each
// other's entry and resume into each other's partial transfer.

// Pure-JS SHA-256 rather than node:crypto: the same digest, and it also runs
// in the Android app, which has no Node (docs/android.md).
import { sha256 } from '@noble/hashes/sha2.js';

import { CONTENT_ID_BYTES, type PackageType } from '@quran-device/qpk-format';

export interface ContentIdentity {
  type: PackageType;
  title: string;
  author?: string | undefined;
  language?: string | undefined;
  /** For a Quran package: which script. Two scripts are two works to a reader. */
  script?: string | undefined;
}

/**
 * A stable 16-byte id for a work. Deterministic: the same identity always
 * gives the same id, on any machine, in any order of conversion.
 */
export function deriveContentId(identity: ContentIdentity): Uint8Array {
  // Unit-separated so ("ab", "c") and ("a", "bc") cannot collide.
  const material = [
    String(identity.type),
    identity.title.trim(),
    (identity.author ?? '').trim(),
    (identity.language ?? '').trim(),
    (identity.script ?? '').trim(),
  ].join('');

  const digest = sha256(new TextEncoder().encode(material));
  return digest.slice(0, CONTENT_ID_BYTES);
}

export function contentIdToHex(id: Uint8Array): string {
  return [...id].map((b) => b.toString(16).padStart(2, '0')).join('');
}
