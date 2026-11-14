#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that the FFmpeg patch series applies to the pinned tarball (WI-4.100).

    check_patches_apply.py --archive ffmpeg-<v>.tar.xz --series patches/series --apply-patches tools/apply_patches.py

Extracts from the archive only the files the patches modify, into a temporary directory, and runs
tools/apply_patches.py --check on that tree (the same tool and flags as an ExternalProject patch step,
chapter 13 §13.3.6), so a patch that no longer applies to the pin fails here with the patch name.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

DIFF_RE = re.compile(r"^diff --git a/(\S+) b/(\S+)$")


def series_patches(series: Path) -> list[Path]:
    out = []
    for line in series.read_text(encoding="utf-8").splitlines():
        entry = line.split("#", 1)[0].strip()
        if entry:
            out.append(series.parent / entry.split()[0])
    return out


def touched_paths(patches: list[Path]) -> set[str]:
    paths: set[str] = set()
    for patch in patches:
        for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
            m = DIFF_RE.match(line)
            if m:
                paths.add(m.group(1))
    return paths


def extract(archive: Path, wanted: set[str], dest: Path) -> Path:
    """Extracts the members <top>/<path> for <path> in wanted; returns <dest>/<top>."""
    with tarfile.open(archive) as tar:
        members = []
        top = None
        for member in tar:
            parts = member.name.split("/", 1)
            if top is None:
                top = parts[0]
            if len(parts) == 2 and parts[0] == top and parts[1] in wanted and member.isfile():
                members.append(member)
        if top is None:
            raise SystemExit(f"{archive}: empty archive")
        if hasattr(tarfile, "data_filter"):
            tar.extractall(dest, members=members, filter="data")
        else:  # pragma: no cover - Python without PEP 706
            tar.extractall(dest, members=members)
    root = dest / top
    root.mkdir(parents=True, exist_ok=True)
    return root


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--archive", type=Path, required=True)
    ap.add_argument("--series", type=Path, required=True)
    ap.add_argument("--apply-patches", type=Path, required=True)
    args = ap.parse_args(argv)

    args.series = args.series.resolve()
    patches = series_patches(args.series)
    if not patches:
        print(f"{args.series}: no patches", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="ovt-ffmpeg-patches-") as tmp:
        root = extract(args.archive, touched_paths(patches), Path(tmp))
        result = subprocess.run(
            [
                sys.executable,
                "-I",
                str(args.apply_patches),
                "--check",
                "--series",
                str(args.series),
                "--source",
                str(root),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            return result.returncode
    print(f"check_patches_apply: {len(patches)} patch(es) apply to {args.archive.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
