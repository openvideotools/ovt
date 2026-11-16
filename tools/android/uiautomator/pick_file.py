#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Drives the Android Storage Access Framework picker of OVT Studio on an emulator (WI-0.051).

    tools/android/uiautomator/pick_file.py [--serial emulator-5554] [--package org.openvideotools.studio.debug]
                                           [--timeout 30] mini.mp4

Preconditions: a debug build of the app is installed (the "Open file…" button exists only on debuggable builds)
and the file is in /sdcard/Download (`adb push mini.mp4 /sdcard/Download/`). The script starts the app, taps the
button, taps the file in the system picker (the Downloads root is opened if the picker shows another one), then
waits for the logcat line `OVT: opened fd size=<bytes>` and prints it. Exit 0 when the line appeared, 1 when it
did not, 2 for usage or adb errors. It uses `adb` and `uiautomator dump` only (Python standard library), so it works
on any emulator image without installing a test APK. WI-0.035 reuses it.
"""

import argparse
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

OPEN_LABEL = "Open file…"  # DebugInfoOverlay.OPEN_LABEL
OPENED_RE = re.compile(r"OVT\s*:\s*opened fd size=(\d+)")
BOUNDS_RE = re.compile(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]")


class AdbError(RuntimeError):
    pass


class Device:
    def __init__(self, serial):
        self.base = ["adb"] + (["-s", serial] if serial else [])

    def run(self, *args, check=True):
        try:
            done = subprocess.run(self.base + list(args), capture_output=True, text=True, timeout=60, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise AdbError(f"adb {' '.join(args)}: {error}") from error
        if check and done.returncode != 0:
            raise AdbError(f"adb {' '.join(args)} failed: {done.stderr.strip() or done.stdout.strip()}")
        return done.stdout

    def ui(self):
        """The current view hierarchy as an ElementTree root."""
        self.run("shell", "uiautomator", "dump", "/sdcard/ovt_ui.xml")
        return ET.fromstring(self.run("exec-out", "cat", "/sdcard/ovt_ui.xml"))

    def tap(self, node):
        left, top, right, bottom = map(int, BOUNDS_RE.fullmatch(node.get("bounds", "")).groups())
        self.run("shell", "input", "tap", str((left + right) // 2), str((top + bottom) // 2))


def find(root, text):
    """First node whose text or content-desc equals @p text (case-insensitive)."""
    want = text.casefold()
    for node in root.iter("node"):
        if want in (node.get("text", "").casefold(), node.get("content-desc", "").casefold()):
            if BOUNDS_RE.fullmatch(node.get("bounds", "")):
                return node
    return None


def wait_for(device, text, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        node = find(device.ui(), text)
        if node is not None:
            return node
        time.sleep(1)
    return None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("name", help="file name in /sdcard/Download, as the picker shows it")
    parser.add_argument("--serial", help="adb serial (default: the only device)")
    parser.add_argument("--package", default="org.openvideotools.studio.debug")
    parser.add_argument("--timeout", type=int, default=30, help="seconds per wait (default 30)")
    args = parser.parse_args(argv)
    device = Device(args.serial)
    try:
        device.run("logcat", "-c")
        device.run("shell", "am", "start", "-W", "-n", f"{args.package}/org.openvideotools.studio.MainActivity")
        button = wait_for(device, OPEN_LABEL, args.timeout)
        if button is None:
            print(f"no '{OPEN_LABEL}' button: is a debug build installed?", file=sys.stderr)
            return 1
        device.tap(button)
        entry = wait_for(device, args.name, args.timeout)
        if entry is None:
            # The picker may have opened on Recent or another root: open the Downloads root from the drawer.
            drawer = find(device.ui(), "Show roots") or find(device.ui(), "Open drawer")
            if drawer is not None:
                device.tap(drawer)
                downloads = wait_for(device, "Downloads", 5)
                if downloads is not None:
                    device.tap(downloads)
            entry = wait_for(device, args.name, args.timeout)
        if entry is None:
            print(f"'{args.name}' not found in the picker; is it in /sdcard/Download?", file=sys.stderr)
            return 1
        device.tap(entry)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            match = OPENED_RE.search(device.run("logcat", "-d", "-s", "OVT:I"))
            if match:
                print(match.group(0))
                return 0
            time.sleep(1)
        print("logcat shows no 'OVT: opened fd size=' line", file=sys.stderr)
        return 1
    except AdbError as error:
        print(error, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
