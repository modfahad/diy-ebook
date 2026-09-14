"""Shape and rasterize Arabic into device-ready 1-bit pages.

    python tools/arabic-pager/render_pages.py <font.ttf> <verses.json> [options]

This is the desktop half of the Arabic story, and the first working piece of
the shaping stage docs/qpk-format.md section 9a calls for.

WHY IT LIVES ON THE DESKTOP, not the device:
Arabic needs contextual shaping (every letter has isolated/initial/medial/
final forms), mark positioning for the harakat, ligatures, and right-to-left
layout. That is HarfBuzz's job and it is far too much to port to an ESP32.
So the desktop does it once, and the device receives pixels it only has to
blit. For a mushaf that is not a compromise: the page layout is fixed, so
there is nothing to re-flow on device anyway.

A LESSON WORTH KEEPING (it cost an hour): do NOT pass init/medi/fina as
HarfBuzz features. They are contextual and applied automatically; naming them
forces them on for every glyph regardless of position and silently produces
wrong letter forms that still look like Arabic at a glance.

Outputs:
  * a PNG per page, at the panel's exact pixel size, for inspection
  * a C header of 1-bit page bitmaps for the firmware to blit
"""
import argparse
import json
import os

import freetype
import uharfbuzz as hb
from PIL import Image, ImageDraw, ImageFont

ARABIC_DIGITS = '٠١٢٣٤٥٦٧٨٩'
AYAH_MARK = '۝'          # END OF AYAH, if the font carries it

# Latin faces to try for the ayah number, in order. Only used for the digits
# inside the ayah medallion -- never for the Quranic text itself.
LATIN_FONT_CANDIDATES = (
    'C:/Windows/Fonts/arial.ttf',
    'C:/Windows/Fonts/segoeui.ttf',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
)


def load_latin_font(explicit, size):
    """The face for the digits inside the ayah circle."""
    candidates = ([explicit] if explicit else []) + list(LATIN_FONT_CANDIDATES)
    for path in candidates:
        if path and os.path.exists(path):
            return ImageFont.truetype(path, size)
    raise SystemExit(
        'no Latin font found for the ayah numbers. Pass --number-font '
        '<file.ttf>, or use --arabic-numbers to keep Arabic-Indic digits.')


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('font')
    parser.add_argument('verses', help='JSON: quran.com /quran/verses/uthmani')
    parser.add_argument('--width', type=int, default=792)
    parser.add_argument('--height', type=int, default=272)
    parser.add_argument('--px', type=int, default=44, help='glyph size')
    parser.add_argument('--lines', type=int, default=3, help='lines per page')
    parser.add_argument('--margin-x', type=int, default=16)
    parser.add_argument('--margin-y', type=int, default=18)
    parser.add_argument('--png-dir', default=None)
    parser.add_argument('--header', default=None)
    parser.add_argument('--name', default='An-Naba')
    parser.add_argument('--arabic-name', default='النبأ',
                        help='surah name in Arabic script, shaped through the '
                             'same HarfBuzz pipeline as the body text and '
                             'drawn into the rail')
    parser.add_argument('--meaning', default='The Great News',
                        help='English meaning of the surah name, shown '
                             'alongside it in the rail')
    parser.add_argument('--symbol', default='kNabaPages')
    parser.add_argument(
        '--translation', default=None,
        help='JSON of an English translation (see fetch_translation.py). When '
             'given, each ayah is laid out as a block: the Arabic, then its '
             'meaning in a smaller line beneath.')
    parser.add_argument('--tr-px', type=int, default=15,
                        help='glyph size for the translation')
    parser.add_argument('--line-spacing', type=float, default=1.28,
                        help='Arabic line pitch as a multiple of glyph size. '
                             'Harakat sit above the letters, so this needs to '
                             'exceed 1.0; much above 1.3 just wastes glass.')
    parser.add_argument('--tr-gap', type=int, default=8,
                        help='vertical gap after each ayah block')
    parser.add_argument(
        '--sidebar-width', type=int, default=52,
        help='width of the vertical surah rail on the left. 0 disables it.')
    parser.add_argument(
        '--arabic-numbers', action='store_true',
        help='render ayah numbers as Arabic-Indic digits with the mushaf end-'
             'of-ayah mark. The default is a small round medallion with '
             'Western digits, which is far more legible at this panel size.')
    parser.add_argument(
        '--number-font', default=None,
        help='TTF for the digits inside the ayah medallion')
    parser.add_argument(
        '--number-scale', type=float, default=0.62,
        help='medallion diameter as a fraction of the glyph size')
    parser.add_argument(
        '--drop-unsupported', action='store_true',
        help='omit codepoints the font lacks, listing every one. Without '
             'this the tool refuses rather than silently altering the text.')
    return parser.parse_args()


def main():
    args = parse_args()

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
    mark = AYAH_MARK if 0x06DD in cmap else ''

    def shape(text):
        """(glyphs, advance_px). Glyphs come back in visual order."""
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        buf.direction = 'rtl'
        buf.script = 'Arab'
        hb.shape(hbfont, buf, None)   # see the module docstring
        glyphs = []
        x = 0.0
        for info, pos in zip(buf.glyph_infos, buf.glyph_positions):
            glyphs.append((info.codepoint,
                           (x + pos.x_offset) * scale,
                           -pos.y_offset * scale))
            x += pos.x_advance
        return glyphs, x * scale

    # --- line breaking ------------------------------------------------------
    # Shaped word at a time: Arabic does not join across a space, so this is
    # correct and turns wrapping into simple measurement.
    # The rail eats into the line width, so wrapping has to know about it
    # or the text would run underneath it.
    usable = args.width - 2 * args.margin_x - args.sidebar_width
    space_w = shape(' ')[1]

    # Coverage first, and it is a hard stop by default. Silently dropping a
    # codepoint the font cannot draw would alter scripture without saying so;
    # rendering it as .notdef would put boxes on the page. Neither is
    # acceptable without the operator explicitly choosing it.
    missing = {}
    for verse in verses:
        for ch in verse['text_uthmani']:
            if ch != ' ' and ord(ch) not in cmap:
                missing[ch] = missing.get(ch, 0) + 1

    if missing:
        import unicodedata
        print('FONT COVERAGE GAPS (%d codepoint(s)):' % len(missing))
        for ch, count in sorted(missing.items()):
            try:
                name = unicodedata.name(ch)
            except ValueError:
                name = '<unnamed>'
            print('  U+%04X  x%-3d  %s' % (ord(ch), count, name))
        if not args.drop_unsupported:
            raise SystemExit(
                'refusing to render. Pass --drop-unsupported to omit these '
                '(they will be listed above and in the generated header), or '
                'use a font that carries them.')
        print('  --drop-unsupported given: omitting the above.')

    dropped = set(missing) if args.drop_unsupported else set()

    def strip(text):
        return ''.join(ch for ch in text if ch not in dropped)

    # The ayah medallion: a small circle with Western digits. Arabic-Indic
    # digits at this panel size are hard to read at a glance, and the mushaf
    # end-of-ayah mark is a single dense glyph that renders to mush at 44px on
    # a 1-bit panel. A drawn circle stays crisp because it is drawn, not
    # rasterized from an outline.
    medallion = int(round(args.px * args.number_scale))
    latin = None if args.arabic_numbers else load_latin_font(
        args.number_font, max(7, int(round(args.px * 0.34))))
    tiles = {}

    def medallion_tile(number):
        if number in tiles:
            return tiles[number]
        # Drawn in greyscale and thresholded, exactly like the glyphs, so the
        # circle gets the same antialiasing treatment as the text around it.
        tile = Image.new('L', (medallion, medallion), 255)
        pen = ImageDraw.Draw(tile)
        ring = max(2, int(round(args.px * 0.045)))
        pen.ellipse([ring // 2, ring // 2,
                     medallion - 1 - ring // 2, medallion - 1 - ring // 2],
                    outline=0, width=ring)
        box = pen.textbbox((0, 0), number, font=latin)
        pen.text(((medallion - (box[2] - box[0])) / 2.0 - box[0],
                  (medallion - (box[3] - box[1])) / 2.0 - box[1]),
                 number, font=latin, fill=0)
        tiles[number] = tile
        return tile

    # The surah rail is built further down, once bitmap() exists: the name is
    # shaped and rasterized through the same HarfBuzz/FreeType pipeline as the
    # body text, not drawn with PIL.
    surah_number = int(verses[0]['verse_key'].split(':')[0]) if verses else 0

    translation = {}
    if args.translation:
        with open(args.translation, encoding='utf-8') as handle:
            payload = json.load(handle)
        translation = {v['verse_key']: v['text'] for v in payload['verses']}
        print('translation %s (%s)'
              % (payload.get('translation_name'), payload.get('author')))

    tr_font = (load_latin_font(args.number_font, args.tr_px)
               if translation else None)
    measure = ImageDraw.Draw(Image.new('L', (1, 1)))

    def wrap_latin(text, width):
        """Greedy wrap by measured width -- proportional, so counting
        characters would be wrong."""
        words, out, line = text.split(), [], ''
        for word in words:
            trial = (line + ' ' + word).strip()
            if line and measure.textlength(trial, font=tr_font) > width:
                out.append(line)
                line = word
            else:
                line = trial
        if line:
            out.append(line)
        return out

    arabic_pitch = int(round(args.px * args.line_spacing))
    tr_pitch = int(round(args.tr_px * 1.30))

    # Each ayah becomes a BLOCK of rows, and blocks are packed whole. Breaking
    # a page between an ayah and its own meaning would be the one layout error
    # a bilingual mushaf must not make.
    blocks = []
    for verse in verses:
        index = verse['verse_key'].split(':')[1]
        tokens = [('text',) + shape(word)
                  for word in strip(verse['text_uthmani']).split()]
        if args.arabic_numbers:
            arabic = ''.join(ARABIC_DIGITS[int(d)] for d in index)
            tokens.append(('text',) + shape(mark + arabic))
        else:
            tokens.append(('ayah', index, float(medallion)))

        rows, current, current_w = [], [], 0.0
        for token in tokens:
            w = token[2]
            extra = w if not current else w + space_w
            if current and current_w + extra > usable:
                rows.append(('ar', current, arabic_pitch))
                current, current_w = [token], w
            else:
                current.append(token)
                current_w += extra
        if current:
            rows.append(('ar', current, arabic_pitch))

        if translation:
            for text in wrap_latin(translation.get(verse['verse_key'], ''),
                                   usable):
                rows.append(('en', text, tr_pitch))
            # A hairline between one ayah's block and the next, not just
            # blank space -- the gap alone did not read as a boundary.
            rows.append(('rule', None, args.tr_gap))

        blocks.append(rows)

    if translation:
        budget = args.height - 2 * args.margin_y
        pages, page, used = [], [], 0
        for rows in blocks:
            need = sum(r[2] for r in rows)
            if page and used + need > budget:
                pages.append(page)
                page, used = [], 0
            page.extend(rows)
            used += need
        if page:
            pages.append(page)
        lines = [r for rows in blocks for r in rows if r[0] == 'ar']
    else:
        # Continuous flow: no translation to keep an ayah attached to.
        lines = [r for rows in blocks for r in rows]
        pages = [[('ar', line[1], 0) for line in lines[i:i + args.lines]]
                 for i in range(0, len(lines), args.lines)]

    pitch = (args.height - 2 * args.margin_y) // args.lines

    cache = {}

    def bitmap(gid):
        if gid not in cache:
            ft.load_glyph(gid, freetype.FT_LOAD_RENDER)
            bmp = ft.glyph.bitmap
            cache[gid] = (bytes(bmp.buffer), bmp.width, bmp.rows, bmp.pitch,
                          ft.glyph.bitmap_left, ft.glyph.bitmap_top)
        return cache[gid]

    def arabic_tile(text):
        """Greyscale tile of shaped RTL text, rasterized through bitmap() --
        the same HarfBuzz-shaped, FreeType-rendered glyphs as the body text,
        just composited into a standalone tile instead of onto the page."""
        glyphs, advance = shape(text)
        boxes = []
        rendered = []
        for gid, dx, dy in glyphs:
            data, gw, gh, gpitch, left, top = bitmap(gid)
            if gw == 0 or gh == 0:
                continue
            rendered.append((data, gw, gh, gpitch, dx + left, dy - top))
            boxes.append((dx + left, dy - top, dx + left + gw, dy - top + gh))
        if not boxes:
            return Image.new('L', (max(1, int(round(advance))), 1), 255)
        min_x = min(b[0] for b in boxes)
        min_y = min(b[1] for b in boxes)
        width = max(1, int(round(max(b[2] for b in boxes) - min_x)))
        height = max(1, int(round(max(b[3] for b in boxes) - min_y)))
        tile = Image.new('L', (width, height), 255)
        pixels = tile.load()
        for data, gw, gh, gpitch, ox, oy in rendered:
            ox = int(round(ox - min_x))
            oy = int(round(oy - min_y))
            for yy in range(gh):
                base = yy * gpitch
                py = oy + yy
                if not (0 <= py < height):
                    continue
                for xx in range(gw):
                    px = ox + xx
                    if 0 <= px < width:
                        # bitmap() coverage is high=ink; tiles elsewhere in
                        # this file are low=ink (PIL's own text convention),
                        # so invert to match what blit()/paste expect.
                        pixels[px, py] = min(pixels[px, py],
                                             255 - data[base + xx])
        return tile

    # --- the vertical surah rail ------------------------------------------
    #
    # A mushaf carries the surah's identity in the margin, and on a 792x272
    # panel the left edge is the only margin wide enough to hold it. Built
    # horizontally and rotated 90 degrees, so it reads bottom-to-top the way a
    # book spine does. The name is shaped Arabic (arabic_tile); the meaning,
    # number and ayah count stay Latin, set after it.
    def build_rail():
        if args.sidebar_width <= 0:
            return None
        strip = Image.new('L', (args.height, args.sidebar_width), 255)
        pen = ImageDraw.Draw(strip)

        name_tile = arabic_tile(args.arabic_name)
        # The tile is rasterized at the body glyph size, which was never sized
        # for the rail's thickness -- at --px 44 it is taller than the default
        # 52px strip. Fit height first: an oversized tile pastes cropped, not
        # scaled, and cropping a mushaf's own surah name is not acceptable.
        padding = 4
        if name_tile.height > args.sidebar_width - padding:
            scale = (args.sidebar_width - padding) / float(name_tile.height)
            name_tile = name_tile.resize(
                (max(1, int(name_tile.width * scale)),
                 max(1, int(name_tile.height * scale))), Image.LANCZOS)

        meta = '   %s   %d   %d Ayat' % (args.meaning, surah_number, len(verses))
        # The rail's usable length is fixed (args.height, pre-rotation), but
        # the meaning makes the string longer than the name-only version this
        # was sized for. Shrink the Latin font rather than clip the text.
        size = max(10, int(round(args.px * 0.40)))
        while True:
            font = load_latin_font(args.number_font, size)
            box = pen.textbbox((0, 0), meta, font=font)
            meta_w = box[2] - box[0]
            if name_tile.width + meta_w <= args.height or size <= 8:
                break
            size -= 1
        # A Latin size of 8 may still not leave room for the Arabic name --
        # shrink the tile itself further rather than clip.
        if name_tile.width + meta_w > args.height:
            scale = max(0.35, (args.height - meta_w) / float(name_tile.width))
            name_tile = name_tile.resize(
                (max(1, int(name_tile.width * scale)),
                 max(1, int(name_tile.height * scale))), Image.LANCZOS)

        name_y = (args.sidebar_width - name_tile.height) // 2
        strip.paste(name_tile, (0, name_y))
        meta_y = (args.sidebar_width - (box[3] - box[1])) // 2 - box[1]
        pen.text((name_tile.width, meta_y), meta, font=font, fill=0)

        # expand=True turns the 272xN strip into an Nx272 rail.
        return strip.rotate(90, expand=True)

    rail = build_rail()

    def latin_tile(text, font):
        """Greyscale tile of LTR text, thresholded on blit like the glyphs."""
        box = measure.textbbox((0, 0), text, font=font)
        tile = Image.new('L', (max(1, box[2] - box[0] + 2),
                               max(1, box[3] - box[1] + 2)), 255)
        ImageDraw.Draw(tile).text((1 - box[0], 1 - box[1]), text,
                                  font=font, fill=0)
        return tile

    def blit(tile, ox, oy, pixels):
        for yy in range(tile.height):
            py = oy + yy
            if not (0 <= py < args.height):
                continue
            for xx in range(tile.width):
                if tile.getpixel((xx, yy)) < 128:
                    px = ox + xx
                    if 0 <= px < args.width:
                        pixels[px, py] = 0

    def draw(page_rows):
        image = Image.new('1', (args.width, args.height), 1)
        pixels = image.load()
        # A y-cursor, not fixed row slots: an Arabic line and a translation
        # line are different heights, so rows have to advance by their own.
        pen_y = args.margin_y
        for kind, payload, row_h in page_rows:
            height = row_h if row_h else pitch
            if kind == 'gap':
                pen_y += height
                continue
            if kind == 'rule':
                yy = pen_y + height // 2
                if 0 <= yy < args.height:
                    for xx in range(args.sidebar_width + args.margin_x,
                                    args.width - args.margin_x):
                        pixels[xx, yy] = 0
                pen_y += height
                continue
            if kind == 'en':
                # LTR and left-aligned, against the Arabic's right alignment:
                # the asymmetry is what makes the two scripts read as separate
                # voices rather than one muddled column.
                blit(latin_tile(payload, tr_font),
                     args.sidebar_width + args.margin_x, pen_y, pixels)
                pen_y += height
                continue

            line = payload
            total = sum(t[2] for t in line) + space_w * (len(line) - 1)
            # RTL: the run ends at the right margin, so its left origin is the
            # right edge minus the whole line's width.
            pen = args.width - args.margin_x - total
            baseline = pen_y + int(height * 0.72)
            for token in reversed(line):
                kind, payload, w = token
                if kind == 'ayah':
                    tile = medallion_tile(payload)
                    # Sat on the text's body rather than the baseline, so the
                    # circle reads as sitting in the line, not hanging below.
                    ox = int(round(pen))
                    oy = baseline - int(round(medallion * 0.78))
                    for yy in range(medallion):
                        py = oy + yy
                        if not (0 <= py < args.height):
                            continue
                        for xx in range(medallion):
                            if tile.getpixel((xx, yy)) < 128:
                                px = ox + xx
                                if 0 <= px < args.width:
                                    pixels[px, py] = 0
                    pen += w + space_w
                    continue
                glyphs = payload
                for gid, dx, dy in glyphs:
                    data, gw, gh, gpitch, left, top = bitmap(gid)
                    if gw == 0 or gh == 0:
                        continue
                    ox = int(round(pen + dx)) + left
                    oy = int(round(baseline + dy)) - top
                    for yy in range(gh):
                        base = yy * gpitch
                        py = oy + yy
                        if not (0 <= py < args.height):
                            continue
                        for xx in range(gw):
                            if data[base + xx] >= 128:
                                px = ox + xx
                                if 0 <= px < args.width:
                                    pixels[px, py] = 0
                pen += w + space_w
            pen_y += height

        if rail is not None:
            for yy in range(min(rail.height, args.height)):
                for xx in range(min(rail.width, args.width)):
                    if rail.getpixel((xx, yy)) < 128:
                        pixels[xx, yy] = 0
            # A hairline between the rail and the text, so the two read as
            # separate columns rather than one crowded block.
            rule_x = args.sidebar_width
            if 0 <= rule_x < args.width:
                for yy in range(args.margin_y // 2,
                                args.height - args.margin_y // 2):
                    pixels[rule_x, yy] = 0
        return image

    images = [draw(page) for page in pages]

    print('font        %s' % os.path.basename(args.font))
    print('glyph size  %dpx' % args.px)
    print('page        %dx%d' % (args.width, args.height))
    print('lines       %d total, %d per page' % (len(lines), args.lines))
    print('pages       %d' % len(pages))

    if args.png_dir:
        os.makedirs(args.png_dir, exist_ok=True)
        for index, image in enumerate(images, start=1):
            image.save(os.path.join(args.png_dir, 'page_%02d.png' % index))
        print('png         %s/page_01.png .. page_%02d.png'
              % (args.png_dir, len(images)))

    if args.header:
        write_header(args, images)
        print('header      %s' % args.header)


def write_header(args, images):
    """1 bit per pixel, row-major, MSB first, 1 = black (ink)."""
    stride = (args.width + 7) // 8
    rows = []
    for image in images:
        pixels = image.load()
        data = bytearray(stride * args.height)
        for y in range(args.height):
            base = y * stride
            for x in range(args.width):
                if pixels[x, y] == 0:                 # black
                    data[base + (x >> 3)] |= 0x80 >> (x & 7)
        rows.append(bytes(data))

    lines = []
    for page in rows:
        chunks = []
        for i in range(0, len(page), 16):
            chunks.append('    ' + ', '.join('0x%02X' % b
                                             for b in page[i:i + 16]) + ',')
        lines.append('  {\n' + '\n'.join(chunks) + '\n  },')

    header = '''// GENERATED FILE -- DO NOT EDIT.
//
// %s, shaped and rasterized on the desktop by
// tools/arabic-pager/render_pages.py using the KFGQPC mushaf font.
//
// The device cannot shape Arabic -- contextual forms, mark positioning and
// RTL are HarfBuzz's job, not an ESP32's -- so it receives finished pixels
// and only has to blit them. See that script's header for the reasoning, and
// docs/qpk-format.md section 9a for where this fits.
//
// 1 bit per pixel, row-major, MSB first within each byte, 1 = black.

#pragma once

#include <stdint.h>

namespace app {

constexpr const char* %sName = "%s";
constexpr uint16_t %sWidth = %d;
constexpr uint16_t %sHeight = %d;
constexpr uint16_t %sStride = %d;
constexpr uint16_t %sCount = %d;

const uint8_t %s[%d][%d] = {
%s
};

}  // namespace app
''' % (args.name, args.symbol, args.name, args.symbol, args.width,
       args.symbol, args.height, args.symbol, stride, args.symbol,
       len(rows), args.symbol, len(rows), stride * args.height,
       '\n'.join(lines))

    os.makedirs(os.path.dirname(args.header), exist_ok=True)
    with open(args.header, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(header)


if __name__ == '__main__':
    main()
