#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CI lint ``no_direct_file_access`` (chapter 08 §8.8, FR-CNT-104, WI-1.234).

    lint_no_direct_file_access.py [ROOT ...]

The GUI (``app/``) and the Python package (``python/``) obtain file bytes only through ``ovt_container_bytes`` /
``ovt_stream_unit_bytes`` and must not map or open the media file themselves: Android content URIs cannot be
mapped reliably, ``VirtualFile`` has no backing file, and the page cache of the byte service bounds memory. The
spec check is ``grep -rn "mmap|QFile::map" app/ python/`` returning nothing; this script runs that search,
extended with the other spellings of the same thing (``memmap``, ``QFileDevice::map``, ``MapViewOfFile``,
``MappedByteBuffer``, ``FileChannel.map``), over every text file under the roots (default ``app`` and ``python``).

Exit codes: 0 no match; 1 at least one match (printed as ``file:line: text``); 2 usage.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOTS = ("app", "python")
PATTERN = re.compile(r"mmap|memmap|QFile::map|QFileDevice::map|MapViewOfFile|MappedByteBuffer|FileChannel\.map")
SKIP_DIRS = {"build", "node_modules", "__pycache__", ".git"}


def scan(root: Path) -> list[str]:
    found: list[str] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or SKIP_DIRS & set(path.relative_to(root).parts):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue  # binary or unreadable
        rel = path.relative_to(REPO) if path.is_relative_to(REPO) else path
        for number, line in enumerate(text.splitlines(), 1):
            if PATTERN.search(line):
                found.append(f"{rel}:{number}: {line.strip()}")
    return found


def main(argv: list[str] | None = None) -> int:
    args = sys.argv[1:] if argv is None else argv
    if any(a.startswith("-") for a in args):
        print(__doc__, file=sys.stderr)
        return 2
    roots = [Path(a) for a in args] or [REPO / r for r in DEFAULT_ROOTS]
    problems: list[str] = []
    for root in roots:
        if root.is_dir():
            problems += scan(root)
    for problem in problems:
        print(problem)
    if problems:
        print(f"lint_no_direct_file_access: {len(problems)} direct file access match(es); use ovt_container_bytes")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
