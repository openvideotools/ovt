#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
# REUSE-IgnoreStart (this file quotes SPDX header lines; only the two above are its own)
"""Insert the SPDX header of chapter 15 §15.1.1 into source files (WI-0.601).

    spdx_header.py <path>... [--check] [--policy tools/license_policy.toml] [--year YYYY]

For each file, the licence is the `[directories]` entry of tools/license_policy.toml whose prefix is the
longest match of the file's repository-relative path. The header is two comment lines:

    <c> SPDX-FileCopyrightText: <year> The Open Video Tools contributors
    <c> SPDX-License-Identifier: <licence>

where <c> is `//` or `#` by file type. A shebang line (`#!`) stays first and the header goes on line 2; any
other file gets it on line 1, above `#pragma once` and include guards.

Exit status: 0 when every file already has a header with the expected licence; 1 when a header was
inserted (or, with --check, would be), or when a file carries a different licence (never rewritten: a
licence change is a `licence:` PR, chapter 15 §15.7.3); 2 when a file type is unknown. Generated files
(`*_generated.h`, `Branding.qml`) get their header from their generator or from REUSE.toml, not from here.

Run with `python -I`; the standard library only (tomllib).
"""

from __future__ import annotations

import argparse
import datetime
import re
import subprocess
import sys
import tomllib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_POLICY = REPO_ROOT / "tools" / "license_policy.toml"
HOLDER = "The Open Video Tools contributors"

SLASH = "//"
HASH = "#"
# Comment syntax per file type: the FR-GEN-105 list, plus Objective-C++ and Gradle Kotlin scripts.
BY_SUFFIX = {
    ".c": SLASH,
    ".h": SLASH,
    ".cpp": SLASH,
    ".hpp": SLASH,
    ".mm": SLASH,
    ".fbs": SLASH,
    ".kt": SLASH,
    ".kts": SLASH,
    ".qml": SLASH,
    ".js": SLASH,
    ".py": HASH,
    ".cmake": HASH,
    ".sh": HASH,
    ".toml": HASH,
    ".yml": HASH,
    ".yaml": HASH,
    ".rb": HASH,
}
BY_NAME = {"CMakeLists.txt": HASH}
GENERATED = re.compile(r"(_generated\.h|/Branding\.qml)$")
ID_RE = re.compile(r"SPDX-License-Identifier:\s*(\S[^*\n]*?)\s*(?:\*/)?\s*$", re.M)
HEADER_LINES = 10  # the identifier must appear within the first lines


class UnknownType(Exception):
    pass


def comment_for(path: Path) -> str:
    if path.name in BY_NAME:
        return BY_NAME[path.name]
    try:
        return BY_SUFFIX[path.suffix]
    except KeyError:
        raise UnknownType(f"{path}: unknown file type (no SPDX comment syntax for '{path.suffix or path.name}')")


def is_source(path: Path) -> bool:
    return (path.name in BY_NAME or path.suffix in BY_SUFFIX) and not GENERATED.search(path.as_posix())


def load_directories(policy: Path) -> dict[str, str]:
    with policy.open("rb") as f:
        data = tomllib.load(f)
    dirs = data.get("directories")
    if not isinstance(dirs, dict) or not dirs:
        raise ValueError(f"{policy}: no [directories] table")
    return {str(k).strip("/"): str(v) for k, v in dirs.items()}


def expected_licence(rel: str, dirs: dict[str, str]) -> str | None:
    """Longest `[directories]` prefix of rel (path components, not characters), or None."""
    best: str | None = None
    for prefix in dirs:
        if prefix == "" or rel == prefix or rel.startswith(prefix + "/"):
            if best is None or len(prefix) > len(best):
                best = prefix
    return dirs[best] if best is not None else None


def header_licence(text: str) -> str | None:
    head = "\n".join(text.splitlines()[:HEADER_LINES])
    m = ID_RE.search(head)
    return m.group(1).strip() if m else None


def relative(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def insert_header(text: str, comment: str, licence: str, year: int) -> str:
    header = f"{comment} SPDX-FileCopyrightText: {year} {HOLDER}\n" f"{comment} SPDX-License-Identifier: {licence}\n"
    lines = text.splitlines(keepends=True)
    if lines and lines[0].startswith("#!"):
        first = lines[0] if lines[0].endswith("\n") else lines[0] + "\n"
        return first + header + "".join(lines[1:])
    return header + ("\n" if lines and lines[0].strip() else "") + text


def process(path: Path, dirs: dict[str, str], root: Path, check: bool, year: int) -> int:
    comment = comment_for(path)
    rel = relative(path, root)
    licence = expected_licence(rel, dirs)
    if licence is None:
        print(f"E-NOPOLICY {rel}: no [directories] entry matches", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    have = header_licence(text)
    if have == licence:
        return 0
    if have is not None:
        print(f"E-HEADER {rel} expected {licence} got {have}", file=sys.stderr)
        return 1
    if check:
        print(f"E-NOHEADER {rel}: would insert the {licence} header", file=sys.stderr)
        return 1
    path.write_text(insert_header(text, comment, licence, year), encoding="utf-8")
    print(f"inserted {licence} header into {rel}")
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", type=Path)
    ap.add_argument("--check", action="store_true", help="report only, change nothing")
    ap.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    ap.add_argument("--root", type=Path, default=None, help="repository root (default: git top level)")
    ap.add_argument("--year", type=int, default=datetime.date.today().year, help="year of new headers")
    args = ap.parse_args(argv)
    root = args.root or git_root(args.paths[0])
    dirs = load_directories(args.policy)
    status = 0
    for path in args.paths:
        try:
            status = max(status, process(path, dirs, root, args.check, args.year))
        except UnknownType as e:
            print(f"E-TYPE {e}", file=sys.stderr)
            status = 2
    return status


def git_root(path: Path) -> Path:
    where = path if path.is_dir() else path.parent
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], cwd=where or Path("."), capture_output=True, text=True
    )
    return Path(out.stdout.strip()) if out.returncode == 0 else REPO_ROOT


if __name__ == "__main__":
    sys.exit(main())
# REUSE-IgnoreEnd
