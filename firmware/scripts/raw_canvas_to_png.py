"""Convert a gfx::Canvas raw dump (from tools/render_ui_preview.cpp) to PNG.

    python firmware/scripts/raw_canvas_to_png.py out.raw out.png 800 480

1 bit per pixel, row-major, MSB first within each byte, matching canvas.h's
documented memory format -- bit value 1 = white, 0 = black. The preview tool
always renders with rotation=0 and seam_x=0 (see its own header comment), so
there is no un-rotating to do here: byte (x // 8, y), bit (0x80 >> (x % 8))
maps directly to pixel (x, y).
"""
import sys

from PIL import Image


def main():
    if len(sys.argv) != 5:
        print("usage: raw_canvas_to_png.py <in.raw> <out.png> <width> <height>",
              file=sys.stderr)
        return 2

    raw_path, png_path, width, height = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    stride = (width + 7) // 8

    with open(raw_path, 'rb') as handle:
        data = handle.read()
    expected = stride * height
    if len(data) != expected:
        print("warning: expected %d bytes (stride %d x height %d), got %d" %
              (expected, stride, height, len(data)), file=sys.stderr)

    # Mode 'L' (0-255 grayscale), not '1': PIL's 1-bit mode pixel values are
    # easy to get backwards across Pillow versions. Plain black/white bytes
    # have no such ambiguity.
    img = Image.new('L', (width, height), color=255)
    pixels = img.load()
    for y in range(height):
        row_base = y * stride
        for x in range(width):
            byte = data[row_base + (x >> 3)]
            bit = (byte >> (7 - (x & 7))) & 1
            pixels[x, y] = 255 if bit else 0  # 1 = white, 0 = black

    img.save(png_path)
    print("wrote %s (%dx%d)" % (png_path, width, height))


if __name__ == '__main__':
    sys.exit(main() or 0)
