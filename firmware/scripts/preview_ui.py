#!/usr/bin/env python3
"""Build tools/render_ui_preview.cpp and render a scene straight to PNG.

    python firmware/scripts/preview_ui.py library-categories out.png
    python firmware/scripts/preview_ui.py library-transfer out.png

Exists because there was previously no way to see a ui:: screen without
flashing real hardware -- see render_ui_preview.cpp's own header for why
that matters for icon/layout work specifically. Reuses run_host_tests.py's
compiler discovery (g++, clang++, or MSVC) rather than assuming g++.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)

sys.path.insert(0, HERE)
from run_host_tests import pick_compiler  # noqa: E402

SOURCES = [
    "tools/render_ui_preview.cpp",
    "drivers/gfx/canvas.cpp",
    "drivers/gfx/font5x7.cpp",
    "storage/library_index.cpp",
    "qpk/crc32.cpp",
    "qpk/glyph_blitter.cpp",
    "qpk/qpk_format.cpp",
    "qpk/qpk_reader.cpp",
    "ui/home_screen.cpp",
    "ui/page_image_screen.cpp",
    "ui/library_screen.cpp",
    "ui/quran_screen.cpp",
    "ui/reader_screen.cpp",
    "ui/selftest_screen.cpp",
    "ui/surah_picker_screen.cpp",
    "qpk/translation_text.cpp",
    "wifi/protocol.cpp",
]

# board::kWidth x board::kHeight (include/board/board_crowpanel_579.h).
WIDTH, HEIGHT = "800", "480"


def main():
    if len(sys.argv) != 3:
        print("usage: preview_ui.py <scene> <out.png>", file=sys.stderr)
        return 2
    scene, out_png = sys.argv[1], sys.argv[2]

    cxx, _, kind = pick_compiler()
    if kind is None:
        print("ERROR: no host C++ compiler found on PATH.", file=sys.stderr)
        return 2

    build_dir = os.path.join(FIRMWARE, ".pio", "ui-preview")
    os.makedirs(build_dir, exist_ok=True)
    exe = os.path.join(build_dir, "preview.exe" if kind != "gnu" else "preview")
    sources = [os.path.join(FIRMWARE, s) for s in SOURCES]

    # "test" is on the include path too: the quran-shaped scene pulls in
    # test_qpk/qpk_test_package.h for a synthetic shaped fixture, the same
    # one the host tests use -- see run_host_tests.py's TEST_ROOT comment
    # for the precedent (test_net reaches test_qpk's fixtures the same way).
    include_dirs = [os.path.join(FIRMWARE, "include"), os.path.join(FIRMWARE, "test")]
    if kind == "gnu":
        cmd = [cxx, "-std=c++11"] + ["-I" + d for d in include_dirs] + ["-o", exe] + sources
    else:
        cmd = (["cl", "/nologo", "/EHsc"] + ["/I" + d for d in include_dirs]
               + sources + ["/Fe" + exe])
    print("+ " + " ".join(cmd))
    if subprocess.call(cmd) != 0:
        return 1

    raw_path = os.path.join(build_dir, scene + ".raw")
    if subprocess.call([exe, scene, raw_path]) != 0:
        return 1

    return subprocess.call([sys.executable,
                            os.path.join(HERE, "raw_canvas_to_png.py"),
                            raw_path, out_png, WIDTH, HEIGHT])


if __name__ == "__main__":
    sys.exit(main())
