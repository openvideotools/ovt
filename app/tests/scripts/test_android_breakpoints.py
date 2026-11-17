# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""WI-A.451 acceptance test 1 on an Android emulator: 1300 -> 900 -> 700 dp switches the layouts three-pane ->
two-pane -> vertical without losing ``Selection.frame``.

The app logs ``OVT: layout <class> widthDp=<n> frame=<decode index>`` (CompactLayout.qml) whenever the layout class
changes. Set ``OVT_ANDROID_MEDIA_URI`` to open a file first so that "the frame is kept" is tested on a real frame.
The test installs nothing: it needs a debug build of the app on a running emulator (``adb`` on the PATH,
``OVT_ANDROID_SERIAL`` for a particular one) and is skipped otherwise, because the emulator CI (WI-0.035, #487) is
not merged; the run is recorded in docs/acceptance/wi/WI-A.451.md. It changes the window size with ``adb shell wm
size`` at the device's own density (dp = px * 160 / density, so the Configuration the app reads is the one a user
resize produces) and restores both on exit. A full-screen window is the window the app reads
(``Configuration.screenWidthDp``), which is also the display here.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import time

import pytest

PACKAGE = os.environ.get("OVT_ANDROID_PACKAGE", "org.openvideotools.studio.debug")
SERIAL = os.environ.get("OVT_ANDROID_SERIAL")
# content:// URI of a video on the device (see docs/acceptance/wi/WI-A.450.md test 2); without it no file is open and
# Selection.frame stays -1 across the switches, which proves the class sequence only.
MEDIA_URI = os.environ.get("OVT_ANDROID_MEDIA_URI")
LAYOUT_RE = re.compile(r"OVT: layout (\w+) widthDp=(\d+) frame=(-?\d+)")
# The widths of the acceptance test and the class §11.5.2 gives each.
STEPS = [(1300, "three"), (900, "two"), (700, "vertical")]


def adb(*args: str, check: bool = True) -> str:
    base = ["adb"] + (["-s", SERIAL] if SERIAL else [])
    done = subprocess.run(base + list(args), capture_output=True, text=True, timeout=60, check=False)
    if check and done.returncode != 0:
        raise RuntimeError(f"adb {' '.join(args)}: {done.stderr.strip() or done.stdout.strip()}")
    return done.stdout


def layouts() -> list[tuple[str, int, int]]:
    log = adb("logcat", "-d", "-s", "qml:D")
    found = [(m.group(1), int(m.group(2)), int(m.group(3))) for m in LAYOUT_RE.finditer(log)]
    # console.log is level D. A class is logged again when the width settles or the frame changes: keep the last
    # entry of each run of one class.
    return [entry for i, entry in enumerate(found) if i + 1 == len(found) or found[i + 1][0] != entry[0]]


def wait_for_class(name: str, timeout: float = 20.0) -> tuple[str, int, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        seen = layouts()
        if seen and seen[-1][0] == name:
            return seen[-1]
        time.sleep(0.5)
    raise AssertionError(f"no 'OVT: layout {name}' line; last seen: {layouts()[-3:]}")


@pytest.fixture
def emulator():
    if shutil.which("adb") is None:
        pytest.skip("adb not found")
    try:
        state = adb("get-state").strip()
    except (RuntimeError, subprocess.TimeoutExpired, OSError):
        pytest.skip("no emulator or device attached")
    if state != "device":
        pytest.skip(f"device state is {state!r}")
    if PACKAGE not in adb("shell", "pm", "list", "packages", PACKAGE):
        pytest.skip(f"{PACKAGE} is not installed (assembleFossDebug installFossDebug)")
    density = int(re.search(r"(\d+)\s*$", adb("shell", "wm", "density").splitlines()[-1]).group(1))
    size = re.search(r"(\d+)x(\d+)", adb("shell", "wm", "size"))
    physical_height = int(size.group(2))
    yield density, physical_height
    adb("shell", "wm", "size", "reset", check=False)
    adb("shell", "wm", "density", "reset", check=False)


def test_resize_switches_layouts_keeping_the_frame(emulator):
    density, height = emulator
    adb("logcat", "-c")
    adb("shell", "am", "force-stop", PACKAGE)
    # Start at 1300 dp so that the first layout the app logs is the three-pane one.
    adb("shell", "wm", "size", f"{round(1300 * density / 160)}x{height}")
    start = ["shell", "am", "start", "-W", "-n", f"{PACKAGE}/org.openvideotools.studio.MainActivity"]
    if MEDIA_URI:
        # A file open, so that Selection.frame is a frame and not -1 (see the module docstring).
        start += ["-a", "android.intent.action.VIEW", "-t", "video/mp4", "--grant-read-uri-permission", "-d", MEDIA_URI]
    adb(*start)
    _, first_width, frame = wait_for_class("three")
    assert abs(first_width - 1300) <= 1
    if MEDIA_URI:
        deadline = time.monotonic() + 20
        while frame < 0 and time.monotonic() < deadline:
            time.sleep(1)
            frame = layouts()[-1][2]
        assert frame >= 0, "the file did not open (no selected frame)"
    for width_dp, expected in STEPS[1:]:
        adb("shell", "wm", "size", f"{round(width_dp * density / 160)}x{height}")
        _, reported, now = wait_for_class(expected)
        assert abs(reported - width_dp) <= 1, f"app read {reported} dp for a {width_dp} dp window"
        assert now == frame, f"Selection.frame changed from {frame} to {now} switching to {expected}"
    # The Activity was not recreated by any of the resizes (§11.5.3): one process, one start.
    classes = [c for c, _, _ in layouts()]
    assert classes == ["three", "two", "vertical"], classes
