"""Design and generate the library category row icons.

Two modes:
    python gen_library_icons.py preview   # draws each icon zoomed way up,
                                           # as PNGs, for a human to check
    python gen_library_icons.py header    # writes the real 16x16 1bpp
                                           # C header the firmware compiles

Run `preview` first and look at the output before trusting `header` --
icon shapes at 16x16 1bpp are easy to get illegibly wrong, and this is the
only way to see them before they exist on real hardware (docs/pending.md:
the row height is 23px, tight for anything detailed).
"""
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
PREVIEW_DIR = os.path.join(HERE, '..', '.pio', 'ui-preview', 'icons')
HEADER_PATH = os.path.join(HERE, '..', 'include', 'app', 'library_icons.h')

SIZE = 16
ZOOM = 12  # preview scale factor


def new_icon():
    # Mode '1': 0 = black (ink), 1 = white (background) -- matches how we
    # read it back for packing (a set bit in the final format means ink).
    img = Image.new('L', (SIZE, SIZE), color=255)
    return img, ImageDraw.Draw(img)


def icon_quran():
    """A closed book with a star -- the one row this needs to read as
    unmistakably "the Quran", not just "a book" (that's the Books icon).
    A star drawn from straight strokes, not a crescent: curves (a crescent
    is two overlapping circles) break up into disconnected dots at 16x16 --
    confirmed by looking at two separate failed attempts, a cut-circle
    crescent and, before that, PIL's arc() for the transfer icon below.
    Straight lines have no such problem at this resolution."""
    img, d = new_icon()
    d.rectangle([1, 4, 10, 14], outline=0, width=1)
    d.line([5, 4, 5, 14], fill=0, width=1)  # spine
    # A 4-point star (plus + X, both centered on the same point) well clear
    # of the book outline, in its own top-right corner.
    d.line([13, 0, 13, 6], fill=0, width=1)
    d.line([10, 3, 16, 3], fill=0, width=1)
    d.point([12, 2], fill=0)
    d.point([14, 2], fill=0)
    d.point([12, 4], fill=0)
    d.point([14, 4], fill=0)
    return img


def icon_books():
    """A stack of two books, offset diagonally -- distinct silhouette from
    the single closed book above. The first attempt (one rect on top of a
    wide bottom bar) read as a table, not two books; this makes the second
    book's top-left corner clearly visible past the first."""
    img, d = new_icon()
    d.rectangle([1, 6, 10, 14], outline=0, width=1)
    d.rectangle([5, 1, 14, 9], outline=0, width=1)
    return img


def icon_translations():
    """A bidirectional arrow -- text converted from one form to another,
    deliberately not another book shape."""
    img, d = new_icon()
    d.line([2, 5, 13, 5], fill=0, width=2)
    d.polygon([(13, 5), (9, 2), (9, 8)], fill=0)  # right-pointing head
    d.line([2, 11, 13, 11], fill=0, width=2)
    d.polygon([(2, 11), (6, 8), (6, 14)], fill=0)  # left-pointing head
    return img


def icon_tafsir():
    """A magnifying glass -- interpretation/study, distinct from every
    other row (none of the others are round)."""
    img, d = new_icon()
    d.ellipse([1, 1, 9, 9], outline=0, width=2)
    d.line([9, 9, 14, 14], fill=0, width=2)
    return img


def icon_transfer():
    """Signal-strength bars -- reads as "connectivity/transfer" and, unlike
    a wifi icon's arcs, renders as crisp filled rectangles instead of
    breaking up into disconnected dots: PIL's arc() at a ~16px radius does
    not have enough pixels to draw a smooth curve, confirmed by looking at
    the first attempt's preview rather than assuming it would be fine."""
    img, d = new_icon()
    d.rectangle([2, 10, 4, 14], fill=0)
    d.rectangle([6, 6, 8, 14], fill=0)
    d.rectangle([10, 2, 12, 14], fill=0)
    return img


def icon_clock():
    """A clock face for the home-screen tile. Square with clipped corners
    rather than a circle, for the same reason as the star above: a 16px
    circle breaks into dots. Hands at ten past, the way clocks are drawn."""
    img, d = new_icon()
    d.rectangle([1, 1, 14, 14], outline=0, width=2)
    for corner in ((1, 1), (14, 1), (1, 14), (14, 14)):
        d.point(corner, fill=255)                  # soften the corners
    d.rectangle([7, 4, 8, 8], fill=0)             # hour hand, up to twelve
    d.rectangle([7, 7, 11, 8], fill=0)            # minute hand, across to three
    return img


def icon_device():
    """Settings sliders -- three rails with a knob on each at a different
    place. Reads as "settings/diagnostics" and, unlike a gear, is all
    straight edges."""
    img, d = new_icon()
    for y, knob in ((3, 10), (8, 4), (13, 8)):
        d.line([1, y, 14, y], fill=0, width=1)
        d.rectangle([knob - 1, y - 2, knob + 1, y + 2], fill=0)
    return img


def icon_bookmark():
    """A ribbon bookmark: a tall solid band with a V notch cut from its foot.
    Solid rather than outlined, so it reads at 16px as a shape, not a frame."""
    img, d = new_icon()
    d.polygon([(4, 1), (11, 1), (11, 14), (8, 10), (7, 10), (4, 14)], fill=0, outline=0)
    return img


ICONS = [
    ('quran', icon_quran),
    ('books', icon_books),
    ('translations', icon_translations),
    ('tafsir', icon_tafsir),
    ('transfer', icon_transfer),
    ('clock', icon_clock),
    ('device', icon_device),
    ('bookmark', icon_bookmark),
]


def pack(img):
    """16x16 -> 32 bytes, row-major, MSB first, 1 = ink -- same convention
    as gfx::font5x7's fixed glyphs."""
    px = img.load()
    stride = (SIZE + 7) // 8
    out = bytearray(stride * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            if px[x, y] < 128:  # dark -> ink
                out[y * stride + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def do_preview():
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    sheet = Image.new('L', (SIZE * ZOOM * len(ICONS) + 20 * (len(ICONS) + 1),
                            SIZE * ZOOM + 20), color=200)
    x = 20
    for name, fn in ICONS:
        icon = fn()
        icon.save(os.path.join(PREVIEW_DIR, name + '.png'))
        zoomed = icon.resize((SIZE * ZOOM, SIZE * ZOOM), Image.NEAREST)
        sheet.paste(zoomed, (x, 10))
        x += SIZE * ZOOM + 20
    sheet_path = os.path.join(PREVIEW_DIR, 'sheet.png')
    sheet.save(sheet_path)
    print('wrote %s and one PNG per icon in %s' % (sheet_path, PREVIEW_DIR))


def do_header():
    stride = (SIZE + 7) // 8
    lines = []
    for name, fn in ICONS:
        data = pack(fn())
        rows = []
        for i in range(0, len(data), stride):
            row = data[i:i + stride]
            rows.append('  { ' + ', '.join('0x%02X' % b for b in row) + ' },')
        lines.append('constexpr uint8_t kIcon%s[%d][%d] = {\n%s\n};' % (
            name.capitalize(), SIZE, stride, '\n'.join(rows)))

    header = '''// GENERATED FILE -- DO NOT EDIT.
//
// Library screen row icons, from firmware/scripts/gen_library_icons.py.
// Run `python firmware/scripts/gen_library_icons.py preview` first and look
// at .pio/ui-preview/icons/ before changing the shapes -- 16x16 1bpp icons
// are easy to get illegibly wrong, and there was no way to see them before
// this script and tools/render_ui_preview.cpp existed.
//
// %d x %d, 1 bit per pixel, row-major, MSB first, 1 = ink. Same convention
// as gfx::font5x7's fixed glyphs.

#pragma once

#include <stdint.h>

namespace app {

constexpr uint8_t kLibraryIconSize = %d;

%s

}  // namespace app
''' % (SIZE, SIZE, SIZE, '\n\n'.join(lines))

    os.makedirs(os.path.dirname(HEADER_PATH), exist_ok=True)
    with open(HEADER_PATH, 'w', encoding='utf-8', newline='\n') as f:
        f.write(header)
    print('wrote %s' % HEADER_PATH)


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    if mode == 'preview':
        do_preview()
    elif mode == 'header':
        do_header()
    else:
        print(__doc__)
        sys.exit(2)
