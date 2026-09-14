"""Shape Arabic verses into a reusable glyph atlas, for QPK's FONT_METADATA
+ ASSETS + shaped WORD_INDEX (docs/qpk-format.md section 9a, kFlagShapedTextData).

    python tools/arabic-pager/shape_glyph_atlas.py <font.ttf> <verses.json> <out.json>

Where render_pages.py composites shaped glyphs straight onto whole pre-baked
pages (the NABA_DEMO path), this instead serializes the shaped output as data:
one record per distinct glyph *appearance* plus, per word, the sequence of
glyph ids that draws it. A desktop pipeline stage (glyph-atlas.ts) turns that
into the wire sections; this script only shapes and rasterizes -- it writes no
QPK bytes itself, matching this repo's split between Python data-prep and the
TypeScript package writer (see fetch_verses.py -> build-test-surahs.mjs).

THE SUBTLETY THIS EXISTS TO GET RIGHT: qpk_format.h documents
xAdvance/xOffset/yOffset as per-glyph-id CONSTANTS -- one triple per glyph id,
reused at every occurrence. But HarfBuzz's GPOS mark positioning is
contextual: the same harakah glyph gets a different y_offset depending on
which base letter it is attached to. Storing one offset per raw HarfBuzz
glyph id would render every occurrence but one wrong. The fix the format
comment prescribes ("mint a distinct glyphId per visually distinct base+mark
combination") is implemented here by keying the atlas not on the font's own
glyph id but on (font_glyph_id, xOffset, yOffset, xAdvance) -- two
occurrences of the same font glyph only share an atlas entry when their
shaped position genuinely matches.

END-OF-AYAH MARKERS are minted too, in a second pass after every word (see
mint comment inside main()). They are one precomposed glyph each in this
font -- reached by shaping the Arabic-Indic digits ALONE, not U+06DD -- and
are emitted contiguously so the device can address them by arithmetic
instead of a lookup table. `--no-ayah-markers` skips them; the resulting
package renders the Latin "(n)" stand-in instead. See docs/qpk-format.md 9b.

Glyph order within a word matches render_pages.py's shape() exactly (forward
through HarfBuzz's output, x_advance accumulating positively) -- that order
is what An-Naba and Al-Fatihah's NABA_DEMO pages already render correctly
with, so this reuses it rather than re-deriving RTL visual order from
scratch. See docs/qpk-format.md 9a: WORD_INDEX glyph runs are stored in
left-to-right visual order, which is exactly what that accumulation
produces once composited -- proven, not assumed.
"""
import argparse
import json
import sys

import freetype
import uharfbuzz as hb

sys.stdout.reconfigure(encoding='utf-8', errors='replace')


def clamp_i8(name, value, missing):
    if value < -128 or value > 127:
        missing.append('%s=%d out of int8 range, clamped' % (name, value))
        return max(-128, min(127, value))
    return value


def clamp_u8(name, value, missing):
    if value < 0 or value > 255:
        missing.append('%s=%d out of uint8 range, clamped' % (name, value))
        return max(0, min(255, value))
    return value


def pack_1bpp(data, width, height, pitch):
    """FreeType's 8bpp antialiased buffer -> 1bpp, MSB-first, row-padded to a
    whole byte -- exactly qpk-format.md 9a's documented ASSETS bitmap layout.
    Threshold matches render_pages.py's own convention (>=128 is ink)."""
    out_stride = (width + 7) // 8
    out = bytearray(out_stride * height)
    for y in range(height):
        row_base = y * pitch
        out_base = y * out_stride
        for x in range(width):
            if data[row_base + x] >= 128:
                out[out_base + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font')
    parser.add_argument('verses', help='JSON: quran.com /quran/verses/{script}')
    parser.add_argument('out')
    parser.add_argument('--px', type=int, default=36, help='glyph size')
    parser.add_argument(
        '--drop-unsupported', action='store_true',
        help='omit codepoints the font lacks, listing every one -- same '
             'refuse-by-default policy as render_pages.py')
    parser.add_argument(
        '--no-ayah-markers', action='store_true',
        help='skip the end-of-ayah marker glyphs (see mint_ayah_markers)')
    args = parser.parse_args()

    with open(args.verses, encoding='utf-8') as handle:
        verses = json.load(handle)['verses']

    blob = hb.Blob.from_file_path(args.font)
    face = hb.Face(blob)
    hbfont = hb.Font(face)
    hbfont.scale = (face.upem, face.upem)
    scale = args.px / float(face.upem)

    ft = freetype.Face(args.font)
    ft.set_pixel_sizes(0, args.px)

    from fontTools.ttLib import TTFont
    cmap = TTFont(args.font).getBestCmap()

    missing = {}
    for verse in verses:
        for ch in verse['text_uthmani']:
            if ch != ' ' and ord(ch) not in cmap:
                missing[ch] = missing.get(ch, 0) + 1
    if missing:
        print('FONT COVERAGE GAPS (%d codepoint(s)):' % len(missing))
        for ch, count in sorted(missing.items()):
            print('  U+%04X  x%-3d' % (ord(ch), count))
        if not args.drop_unsupported:
            raise SystemExit(
                'refusing to shape. Pass --drop-unsupported to omit these.')
        print('  --drop-unsupported given: omitting the above.')
    dropped = set(missing) if args.drop_unsupported else set()

    def strip(text):
        return ''.join(ch for ch in text if ch not in dropped)

    bitmap_cache = {}

    def rasterize(font_gid):
        if font_gid not in bitmap_cache:
            ft.load_glyph(font_gid, freetype.FT_LOAD_RENDER)
            bmp = ft.glyph.bitmap
            bitmap_cache[font_gid] = (
                bytes(bmp.buffer), bmp.width, bmp.rows, bmp.pitch,
                ft.glyph.bitmap_left, ft.glyph.bitmap_top,
            )
        return bitmap_cache[font_gid]

    atlas = {}       # (font_gid, xOff, yOff, xAdv) -> atlas record
    next_id = 1
    warnings = []
    words_out = []

    for verse in verses:
        surah, ayah = (int(p) for p in verse['verse_key'].split(':'))
        for word_index, word_text in enumerate(strip(verse['text_uthmani']).split()):
            buf = hb.Buffer()
            buf.add_str(word_text)
            buf.guess_segment_properties()
            buf.direction = 'rtl'
            buf.script = 'Arab'
            hb.shape(hbfont, buf, None)

            glyph_ids = []
            for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
                font_gid = info.codepoint
                x_off = round(pos.x_offset * scale)
                y_off = round(-pos.y_offset * scale)
                x_adv = round(pos.x_advance * scale)
                key = (font_gid, x_off, y_off, x_adv)

                if key not in atlas:
                    data, gw, gh, gpitch, left, top = rasterize(font_gid)
                    if gw == 0 or gh == 0:
                        # A zero-size glyph (e.g. a pure positioning mark with
                        # no ink of its own) still needs an atlas id -- words
                        # reference glyphs by id, not conditionally -- but it
                        # carries no bitmap.
                        packed = b''
                    else:
                        packed = pack_1bpp(data, gw, gh, gpitch)
                    atlas[key] = {
                        'id': next_id,
                        'fontGlyphId': font_gid,
                        'width': clamp_u8('width', gw, warnings),
                        'height': clamp_u8('height', gh, warnings),
                        'xAdvance': clamp_i8('xAdvance', x_adv, warnings),
                        'xOffset': clamp_i8('xOffset', x_off + left, warnings),
                        'yOffset': clamp_i8('yOffset', y_off - top, warnings),
                        'bitmapHex': packed.hex(),
                    }
                    next_id += 1
                glyph_ids.append(atlas[key]['id'])

            words_out.append({
                'surah': surah,
                'ayah': ayah,
                'wordIndex': word_index,
                'text': word_text,
                'glyphIds': glyph_ids,
            })

    # --- end-of-ayah markers --------------------------------------------
    # A separate pass, deliberately AFTER every word, because the device
    # addresses these by arithmetic rather than by a lookup table: marker for
    # ayah n is atlas id first_glyph_id + (n - 1). That only holds if the ids
    # are contiguous, so they are minted last and unconditionally -- never
    # through the dedup dict above, whose whole job is to hand back an
    # existing id. A duplicated bitmap costs a few dozen bytes; a
    # non-contiguous id space would cost a lookup table in every package.
    #
    # WHAT THE FONT ACTUALLY DOES, checked rather than assumed: KFGQPC
    # Uthmanic Script composes the whole marker into ONE precomposed glyph,
    # reached by shaping the Arabic-Indic digits ALONE -- U+0660..U+0669, no
    # U+06DD. All 286 numbers an ayah can have shape to exactly one glyph,
    # every one 27x33 at 36px, all 286 distinct, no collisions. Prefixing
    # U+06DD (the obvious reading of "end-of-ayah mark") is wrong here: it
    # shapes to TWO glyphs, the composed numeral plus a bare empty circle,
    # which would draw the marker twice.
    ayah_markers = None
    if not args.no_ayah_markers:
        max_ayah = max(int(v['verse_key'].split(':')[1]) for v in verses)
        first_marker_id = next_id
        arabic_indic = '٠١٢٣٤٥٦٧٨٩'
        for number in range(1, max_ayah + 1):
            text = ''.join(arabic_indic[int(d)] for d in str(number))
            buf = hb.Buffer()
            buf.add_str(text)
            buf.guess_segment_properties()
            buf.direction = 'rtl'
            buf.script = 'Arab'
            hb.shape(hbfont, buf, None)
            if len(buf.glyph_infos) != 1:
                raise SystemExit(
                    'ayah marker %d shaped to %d glyphs, not 1. The '
                    'one-glyph-per-number contract the device relies on does '
                    'not hold for this font; markers need a per-number run '
                    'table before they can ship.'
                    % (number, len(buf.glyph_infos)))
            info = buf.glyph_infos[0]
            pos = buf.glyph_positions[0]
            font_gid = info.codepoint
            x_off = round(pos.x_offset * scale)
            y_off = round(-pos.y_offset * scale)
            x_adv = round(pos.x_advance * scale)
            data, gw, gh, gpitch, left, top = rasterize(font_gid)
            packed = pack_1bpp(data, gw, gh, gpitch) if gw and gh else b''
            atlas[('marker', number)] = {
                'id': next_id,
                'fontGlyphId': font_gid,
                'width': clamp_u8('marker %d width' % number, gw, warnings),
                'height': clamp_u8('marker %d height' % number, gh, warnings),
                'xAdvance': clamp_i8('marker %d xAdvance' % number, x_adv, warnings),
                'xOffset': clamp_i8('marker %d xOffset' % number, x_off + left, warnings),
                'yOffset': clamp_i8('marker %d yOffset' % number, y_off - top, warnings),
                'bitmapHex': packed.hex(),
            }
            next_id += 1
        ayah_markers = {'firstGlyphId': first_marker_id, 'count': max_ayah}

    out = {
        'font': args.font,
        'px': args.px,
        'glyphs': sorted(atlas.values(), key=lambda g: g['id']),
        'words': words_out,
        'ayahMarkers': ayah_markers,
        'warnings': warnings,
    }
    with open(args.out, 'w', encoding='utf-8') as handle:
        json.dump(out, handle, ensure_ascii=False, indent=2)

    print('words         %d' % len(words_out))
    print('unique glyphs %d (from %d font glyph ids seen)'
          % (len(atlas), len(set(g['fontGlyphId'] for g in atlas.values()))))
    if ayah_markers is None:
        print('ayah markers  none (--no-ayah-markers)')
    else:
        print('ayah markers  %d, glyph ids %d..%d'
              % (ayah_markers['count'], ayah_markers['firstGlyphId'],
                 ayah_markers['firstGlyphId'] + ayah_markers['count'] - 1))
    if warnings:
        print('warnings:')
        for w in warnings:
            print('  ' + w)
    print('wrote         %s' % args.out)


if __name__ == '__main__':
    main()
