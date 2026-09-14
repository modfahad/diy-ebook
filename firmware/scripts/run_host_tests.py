#!/usr/bin/env python3
"""Compile and run every host unit-test suite with whatever compiler exists.

`pio test -e native` is the normal way to run these, but PlatformIO's native
platform hardcodes gcc/g++, so it cannot use a Windows box that has the Visual
Studio Build Tools and no MinGW. This script does the same job directly and
supports g++, clang++ and MSVC.

    python firmware/scripts/run_host_tests.py            # all suites
    python firmware/scripts/run_host_tests.py test_qpk   # one suite

With MSVC, run it from a Visual Studio developer shell (or call vcvars64.bat
first) so cl.exe, INCLUDE and LIB are set up.

Unity itself is fetched through PlatformIO the first time, into
firmware/.pio/libdeps/native/Unity.

Windows note: Application Control / Smart App Control sometimes refuses to
launch a freshly linked, unsigned test binary (WinError 4551). Short blocks
clear on the retry built into run() below. If a suite stays blocked, delete
firmware/.pio/host-tests and run again -- a clean relink has cleared it every
time it has come up.
"""

import glob
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)

UNITY_SRC = os.path.join(FIRMWARE, ".pio", "libdeps", "native", "Unity", "src")
BUILD_ROOT = os.path.join(FIRMWARE, ".pio", "host-tests")
TEST_ROOT = os.path.join(FIRMWARE, "test")

# Firmware sources that carry no Arduino dependency and can therefore be
# linked into a host test. Keep in step with the `native` env in
# platformio.ini. storage/sd_storage.cpp is deliberately excluded (Arduino
# SD dependency); tests substitute fake_storage.h for it.
SHARED_SOURCES = sorted(
    glob.glob(os.path.join(FIRMWARE, "drivers", "gfx", "*.cpp"))
    + glob.glob(os.path.join(FIRMWARE, "qpk", "*.cpp"))
    + glob.glob(os.path.join(FIRMWARE, "wifi", "*.cpp"))
    + glob.glob(os.path.join(FIRMWARE, "ble", "*.cpp"))
    + glob.glob(os.path.join(FIRMWARE, "ui", "*.cpp"))
    + [os.path.join(FIRMWARE, "storage", "library_index.cpp")]
    + [os.path.join(FIRMWARE, "storage", "reading_progress.cpp")]
)
DEFINES = []


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd))
    # Windows Application Control sometimes refuses a just-linked executable
    # for a moment (WinError 4551). It is a scanning race, not a real policy
    # denial, and it clears on a retry. Without this the whole run dies with a
    # traceback and the remaining suites never execute.
    for attempt in range(4):
        try:
            return subprocess.call(cmd, **kwargs)
        except OSError as error:
            if getattr(error, "winerror", None) != 4551 or attempt == 3:
                print("ERROR: could not launch %s: %s" % (cmd[0], error),
                      file=sys.stderr)
                return 1
            print("  (blocked by Application Control, retrying)")
            time.sleep(1.0)
    return 1


def ensure_unity():
    if os.path.isdir(UNITY_SRC):
        return True
    print("Unity not found, fetching it through PlatformIO...")
    rc = run([sys.executable, "-m", "platformio", "pkg", "install",
              "-d", FIRMWARE, "-e", "native"])
    if rc != 0 or not os.path.isdir(UNITY_SRC):
        print("ERROR: could not obtain Unity. Install PlatformIO "
              "(`pip install platformio`) and retry.", file=sys.stderr)
        return False
    return True


def pick_compiler():
    for cxx, cc, kind in (("g++", "gcc", "gnu"),
                          ("clang++", "clang", "gnu"),
                          ("cl", "cl", "msvc")):
        if shutil.which(cxx) and shutil.which(cc):
            return cxx, cc, kind
    return None, None, None


def discover_suites(selected):
    names = sorted(
        os.path.basename(p) for p in glob.glob(os.path.join(TEST_ROOT, "test_*"))
        if os.path.isdir(p)
    )
    if selected:
        missing = [s for s in selected if s not in names]
        if missing:
            print("ERROR: no such test suite: %s" % ", ".join(missing),
                  file=sys.stderr)
            return None
        return selected
    return names


def c_compiler_for(cxx):
    """The C driver that pairs with a C++ one: clang++ -> clang, g++ -> gcc."""
    directory, name = os.path.split(cxx)
    if name.endswith("clang++"):
        name = name[: -len("clang++")] + "clang"
    elif name.endswith("g++"):
        name = name[: -len("g++")] + "gcc"
    elif name.endswith("c++"):
        name = name[: -len("c++")] + "cc"
    return os.path.join(directory, name) if directory else name


def build_and_run(suite, cxx, kind):
    suite_dir = os.path.join(TEST_ROOT, suite)
    sources = sorted(glob.glob(os.path.join(suite_dir, "*.cpp"))) + SHARED_SOURCES
    if not sources:
        print("skipping %s: no sources" % suite)
        return 0

    build_dir = os.path.join(BUILD_ROOT, suite)
    os.makedirs(build_dir, exist_ok=True)
    # TEST_ROOT lets a suite reach fixtures in a sibling suite directory, e.g.
    # test_net includes "test_qpk/qpk_test_package.h".
    includes = [os.path.join(FIRMWARE, "include"), suite_dir, TEST_ROOT, UNITY_SRC]
    unity_c = os.path.join(UNITY_SRC, "unity.c")
    exe = os.path.join(build_dir, "tests.exe" if kind == "msvc" else "tests")

    print("\n=== %s ===" % suite)
    if kind == "gnu":
        # Unity is C. It is compiled on its own with the matching C compiler:
        # passing it to clang++ behind "-x c" in the same command keeps
        # -std=c++11 in force, which clang rejects for C input (gcc only warns).
        unity_obj = os.path.join(build_dir, "unity.o")
        cmd = [c_compiler_for(cxx), "-c", "-o", unity_obj]
        cmd += ["-I" + d for d in includes]
        cmd += ["-D" + d for d in DEFINES]
        cmd += [unity_c]
        if run(cmd) != 0:
            return 1
        cmd = [cxx, "-std=c++11", "-Wall", "-Wextra", "-o", exe]
        cmd += ["-I" + d for d in includes]
        cmd += ["-D" + d for d in DEFINES]
        cmd += sources + [unity_obj]
        if run(cmd) != 0:
            return 1
    else:
        objects = []
        for source in sources + [unity_c]:
            obj = os.path.join(
                build_dir, os.path.basename(source).rsplit(".", 1)[0] + ".obj")
            objects.append(obj)
            cmd = ["cl", "/nologo", "/EHsc", "/W3", "/c", source, "/Fo" + obj]
            cmd += ["/I" + d for d in includes]
            cmd += ["/D" + d for d in DEFINES]
            if run(cmd) != 0:
                return 1
        if run(["link", "/nologo", "/OUT:" + exe] + objects) != 0:
            return 1

    print()
    return run([exe])


# Host-only dev tools: their own main(), so they cannot join a suite, but they
# link against the same ui::/qpk:: code and would otherwise rot silently the
# next time a signature moves. Built, never run -- a build failure is the
# signal, and not running them also sidesteps the Application Control retry
# dance for a binary nothing asserts on.
DEV_TOOLS = ["render_ui_preview.cpp"]


def build_dev_tools(cxx, kind):
    build_dir = os.path.join(BUILD_ROOT, "tools")
    os.makedirs(build_dir, exist_ok=True)
    includes = [os.path.join(FIRMWARE, "include"), TEST_ROOT]
    failed = 0
    for tool in DEV_TOOLS:
        source = os.path.join(FIRMWARE, "tools", tool)
        stem = tool.rsplit(".", 1)[0]
        exe = os.path.join(build_dir, stem + (".exe" if kind == "msvc" else ""))
        print("\n=== tools/%s (build only) ===" % tool)
        if kind == "gnu":
            cmd = [cxx, "-std=c++11", "-Wall", "-Wextra", "-o", exe]
            cmd += ["-I" + d for d in includes]
            cmd += ["-D" + d for d in DEFINES]
            cmd += [source] + SHARED_SOURCES
            if run(cmd) != 0:
                failed += 1
        else:
            objects = []
            ok = True
            for src in [source] + SHARED_SOURCES:
                obj = os.path.join(
                    build_dir, os.path.basename(src).rsplit(".", 1)[0] + ".obj")
                objects.append(obj)
                cmd = ["cl", "/nologo", "/EHsc", "/W3", "/c", src, "/Fo" + obj]
                cmd += ["/I" + d for d in includes]
                cmd += ["/D" + d for d in DEFINES]
                if run(cmd) != 0:
                    ok = False
                    break
            if not ok or run(["link", "/nologo", "/OUT:" + exe] + objects) != 0:
                failed += 1
    return failed


def main():
    if not ensure_unity():
        return 2

    cxx, _, kind = pick_compiler()
    if kind is None:
        print("ERROR: no host C++ compiler found on PATH.\n"
              "  Linux/macOS: install g++ or clang++\n"
              "  Windows:     run this from a Visual Studio developer shell,\n"
              "               or install MinGW-w64 and put g++ on PATH",
              file=sys.stderr)
        return 2

    suites = discover_suites(sys.argv[1:])
    if suites is None:
        return 2

    print("Using %s (%s) for suites: %s" % (cxx, kind, ", ".join(suites)))
    failures = []
    for suite in suites:
        if build_and_run(suite, cxx, kind) != 0:
            failures.append(suite)

    # Only on a full run: naming a suite means "just that one", and a dev-tool
    # build is not what the caller asked for.
    tools_failed = 0 if sys.argv[1:] else build_dev_tools(cxx, kind)

    print("\n==================== host test summary ====================")
    for suite in suites:
        print("  %-12s %s" % (suite, "FAILED" if suite in failures else "ok"))
    if not sys.argv[1:]:
        print("  %-12s %s" % ("dev-tools", "FAILED" if tools_failed else "built"))
    return 1 if failures or tools_failed else 0


if __name__ == "__main__":
    sys.exit(main())
