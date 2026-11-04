#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pinned Qt 6.8.3 installer built on aqtinstall (chapter 13 §13.3.1 and §13.10, WI-0.018).

    install_qt.py --target desktop|android [--target ...] [--dest DIR]

  --target desktop   the host kit: `macos` (macOS), `gcc_64` (Linux x86-64) or `gcc_arm64` (Linux arm64).
  --target android   `android_arm64_v8a` and `android_x86_64`, plus the host kit (Qt needs it as QT_HOST_PATH).
  --dest DIR         install root; default ~/Qt/ovt.

One `aqt install-qt` runs per kit, with `-m qtshadertools qtgraphs qtimageformats` (add-on modules only:
qtbase, qtdeclarative and qtsvg are implicit and aqt rejects them). A kit whose
`<dest>/6.8.3/<kit>/bin/qmake -query QT_VERSION` already prints 6.8.3 is skipped, so a re-run is fast.
The last stdout line is `QT_ROOT_DIR=<dest>/6.8.3`; CMakePresets.json appends `/<kit>` to it.
Everything else goes to stderr. Run with `python -I`; needs aqtinstall (tools/requirements.txt).
"""

from __future__ import annotations

import argparse
import platform
import subprocess
import sys
from pathlib import Path

# SPEC-ASSUMPTION(#39): QT_ROOT_DIR is <dest>/6.8.3 (aqt layout), not --dest.
QT_VERSION = "6.8.3"
MODULES = (
    "qtshadertools",
    "qtgraphs",
    "qtimageformats",
)  # add-on modules only (§13.3.1)
ANDROID_KITS = ("android_arm64_v8a", "android_x86_64")
DEFAULT_DEST = Path.home() / "Qt" / "ovt"


class UnsupportedHost(Exception):
    pass


def host_kit() -> tuple[str, str, str]:
    """(aqt host, aqt desktop arch, kit directory name) for the machine running the script."""
    system, machine = platform.system(), platform.machine().lower()
    if system == "Darwin":
        return "mac", "clang_64", "macos"
    if system == "Linux":
        if machine in ("x86_64", "amd64"):
            return "linux", "linux_gcc_64", "gcc_64"
        if machine in ("aarch64", "arm64"):
            return "linux_arm64", "linux_gcc_arm64", "gcc_arm64"
    raise UnsupportedHost(f"unsupported host {system}/{machine}: Qt kits exist for macOS and Linux only")


def plan(targets: list[str]) -> list[tuple[str, str, str, str]]:
    """Kits to install as (aqt host, aqt target, aqt arch, kit directory), host kit first, no duplicates."""
    host, desktop_arch, desktop_dir = host_kit()
    kits = [(host, "desktop", desktop_arch, desktop_dir)]
    if "android" in targets:
        kits += [(host, "android", k, k) for k in ANDROID_KITS]
    return kits


def installed(qt_root: Path, kit_dir: str) -> bool:
    qmake = qt_root / kit_dir / "bin" / "qmake"
    if not qmake.exists():
        return False
    try:
        out = subprocess.run(
            [str(qmake), "-query", "QT_VERSION"],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return out.returncode == 0 and out.stdout.strip() == QT_VERSION


def aqt_command(dest: Path, host: str, target: str, arch: str) -> list[str]:
    return [
        sys.executable, "-m", "aqt", "install-qt", host, target, QT_VERSION, arch,
        "-O", str(dest), "-m", *MODULES,
    ]  # fmt: skip


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--target", action="append", choices=("desktop", "android"), required=True)
    ap.add_argument("--dest", type=Path, default=None)
    args = ap.parse_args(argv)

    dest = (args.dest or DEFAULT_DEST).expanduser().resolve()
    qt_root = dest / QT_VERSION
    try:
        kits = plan(args.target)
    except UnsupportedHost as e:
        print(f"install_qt: {e}", file=sys.stderr)
        return 2

    for host, target, arch, kit_dir in kits:
        if installed(qt_root, kit_dir):
            print(
                f"install_qt: {kit_dir} already at {QT_VERSION}, skipped",
                file=sys.stderr,
            )
            continue
        cmd = aqt_command(dest, host, target, arch)
        print("install_qt: " + " ".join(cmd), file=sys.stderr)
        sys.stderr.flush()
        rc = subprocess.run(cmd, stdout=sys.stderr, check=False).returncode
        if rc != 0:
            print(f"install_qt: aqt failed for {kit_dir} (exit {rc}); is aqtinstall installed (tools/bootstrap.sh)?",
                  file=sys.stderr)  # fmt: skip
            return rc
        if not installed(qt_root, kit_dir):
            print(
                f"install_qt: {kit_dir}: qmake -query QT_VERSION did not print {QT_VERSION}",
                file=sys.stderr,
            )
            return 1
    print(f"QT_ROOT_DIR={qt_root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
