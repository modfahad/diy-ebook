// A minimal EPUB writer for test fixtures.
//
// Entries are STORED (no compression), which is a valid ZIP and keeps the
// fixture inspectable. The CRC-32 comes from @quran-device/qpk-format, which
// is the same implementation the package format uses -- one CRC in the repo,
// already tested against the canonical check value.

import { crc32 } from '@quran-device/qpk-format';

interface Entry {
  name: string;
  data: Uint8Array;
}

function concat(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const part of parts) {
    out.set(part, at);
    at += part.length;
  }
  return out;
}

function zipStored(entries: Entry[]): Uint8Array {
  const locals: Uint8Array[] = [];
  const centrals: Uint8Array[] = [];
  let offset = 0;

  for (const entry of entries) {
    const name = new TextEncoder().encode(entry.name);
    const crc = crc32(entry.data);

    const local = new Uint8Array(30 + name.length);
    const lv = new DataView(local.buffer);
    lv.setUint32(0, 0x04034b50, true);
    lv.setUint16(4, 20, true); // version needed
    lv.setUint16(6, 0, true); // flags
    lv.setUint16(8, 0, true); // method: stored
    lv.setUint16(10, 0, true); // time
    lv.setUint16(12, 0x21, true); // date (1980-01-01)
    lv.setUint32(14, crc, true);
    lv.setUint32(18, entry.data.length, true);
    lv.setUint32(22, entry.data.length, true);
    lv.setUint16(26, name.length, true);
    lv.setUint16(28, 0, true);
    local.set(name, 30);

    const central = new Uint8Array(46 + name.length);
    const cv = new DataView(central.buffer);
    cv.setUint32(0, 0x02014b50, true);
    cv.setUint16(4, 20, true); // version made by
    cv.setUint16(6, 20, true); // version needed
    cv.setUint16(8, 0, true);
    cv.setUint16(10, 0, true);
    cv.setUint16(12, 0, true);
    cv.setUint16(14, 0x21, true);
    cv.setUint32(16, crc, true);
    cv.setUint32(20, entry.data.length, true);
    cv.setUint32(24, entry.data.length, true);
    cv.setUint16(28, name.length, true);
    cv.setUint16(30, 0, true);
    cv.setUint16(32, 0, true);
    cv.setUint16(34, 0, true);
    cv.setUint16(36, 0, true);
    cv.setUint32(38, 0, true);
    cv.setUint32(42, offset, true);
    central.set(name, 46);

    locals.push(local, entry.data);
    centrals.push(central);
    offset += local.length + entry.data.length;
  }

  const centralBytes = concat(centrals);
  const end = new Uint8Array(22);
  const ev = new DataView(end.buffer);
  ev.setUint32(0, 0x06054b50, true);
  ev.setUint16(4, 0, true);
  ev.setUint16(6, 0, true);
  ev.setUint16(8, entries.length, true);
  ev.setUint16(10, entries.length, true);
  ev.setUint32(12, centralBytes.length, true);
  ev.setUint32(16, offset, true);
  ev.setUint16(20, 0, true);

  return concat([...locals, centralBytes, end]);
}

export interface EpubChapter {
  /** File name inside OEBPS, e.g. "ch1.xhtml". */
  href: string;
  title: string;
  paragraphs: string[];
}

export interface EpubSpec {
  title: string;
  author: string;
  language: string;
  chapters: EpubChapter[];
  /**
   * Emit the spine in reverse manifest order. Used to prove the reader follows
   * the spine rather than the archive's entry order.
   */
  shuffleManifest?: boolean;
}

export function makeEpub(spec: EpubSpec): Uint8Array {
  const utf8 = (text: string) => new TextEncoder().encode(text);

  const manifestItems = spec.chapters
    .map((chapter, i) => `<item id="c${i}" href="${chapter.href}" media-type="application/xhtml+xml"/>`)
    .join('');
  const spineItems = spec.chapters.map((_, i) => `<itemref idref="c${i}"/>`).join('');

  const opf =
    `<?xml version="1.0" encoding="UTF-8"?>` +
    `<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">` +
    `<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">` +
    `<dc:title>${spec.title}</dc:title>` +
    `<dc:creator>${spec.author}</dc:creator>` +
    `<dc:language>${spec.language}</dc:language>` +
    `</metadata>` +
    `<manifest>${manifestItems}</manifest>` +
    `<spine>${spineItems}</spine>` +
    `</package>`;

  const container =
    `<?xml version="1.0"?>` +
    `<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">` +
    `<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>` +
    `</container>`;

  const chapterEntries: Entry[] = spec.chapters.map((chapter) => ({
    name: `OEBPS/${chapter.href}`,
    data: utf8(
      `<?xml version="1.0" encoding="UTF-8"?>` +
        `<html xmlns="http://www.w3.org/1999/xhtml"><body>` +
        `<h1>${chapter.title}</h1>` +
        chapter.paragraphs.map((p) => `<p>${p}</p>`).join('') +
        `</body></html>`,
    ),
  }));

  // Archive order is deliberately not reading order when shuffleManifest is
  // set: the spine is the only authority.
  const ordered = spec.shuffleManifest ? [...chapterEntries].reverse() : chapterEntries;

  return zipStored([
    { name: 'mimetype', data: utf8('application/epub+zip') },
    { name: 'META-INF/container.xml', data: utf8(container) },
    { name: 'OEBPS/content.opf', data: utf8(opf) },
    ...ordered,
  ]);
}
