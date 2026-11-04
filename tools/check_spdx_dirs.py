#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that every source file's SPDX licence matches its directory (chapter 15 §15.1, §15.4.2; WI-0.601).

    check_spdx_dirs.py [--policy tools/license_policy.toml] [paths...]

Without paths it checks every tracked file (`git ls-files`). Only source files are checked: the file types
of FR-GEN-105 (`.c .h .cpp .hpp .fbs .py .kt .qml .js .cmake .sh .toml .yml .yaml .rb`, `CMakeLists.txt`)
plus `.mm` and `.kts`; generated files and patches are skipped (REUSE.toml covers the former, the patched
project's licence the latter). The expected id is the longest-prefix `[directories]` entry; a derived
file may carry extra `SPDX-FileCopyrightText` lines, only the identifier is compared.

Messages, one per finding, and exit 1 when there is any:
    E-HEADER <file> expected <id> got <id>
    E-NOHEADER <file>

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # `python -I` drops the script directory

import spdx_header as sh  # noqa: E402

SKIP_PREFIXES = ("third_party/patches/", "third_party/vcpkg/")


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, check=True, capture_output=True).stdout
    return [p for p in out.decode().split("\0") if p]


def check(paths: list[str], root: Path, dirs: dict[str, str]) -> list[str]:
    findings = []
    for rel in paths:
        path = root / rel
        if not sh.is_source(Path(rel)) or rel.startswith(SKIP_PREFIXES) or "/patches/" in rel:
            continue
        if not path.is_file():
            continue
        expected = sh.expected_licence(rel, dirs)
        if expected is None:
            continue
        have = sh.header_licence(path.read_text(encoding="utf-8", errors="replace"))
        if have is None:
            findings.append(f"E-NOHEADER {rel}")
        elif have != expected:
            findings.append(f"E-HEADER {rel} expected {expected} got {have}")
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="*", help="files to check (default: every tracked file)")
    ap.add_argument("--policy", type=Path, default=sh.DEFAULT_POLICY)
    ap.add_argument("--root", type=Path, default=sh.REPO_ROOT, help="repository root")
    args = ap.parse_args(argv)
    root = args.root.resolve()
    dirs = sh.load_directories(args.policy)
    if args.paths:
        rels = [sh.relative(Path(p) if Path(p).is_absolute() else Path.cwd() / p, root) for p in args.paths]
    else:
        rels = tracked_files(root)
    findings = check(rels, root, dirs)
    for f in findings:
        print(f, file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
