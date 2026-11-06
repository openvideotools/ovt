#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Line-coverage gate for core/src/ (chapter 04 NFR-028, chapter 13 §13.7.1 `coverage`, WI-0.031).

  coverage_gate.py check   --summary export.json [--root DIR]
  coverage_gate.py objects --test-dir BUILD_DIR

`check` reads the JSON of `llvm-cov export -summary-only` and fails (exit 1) when the line coverage of
core/src/ is below 80 % overall or below 90 % for bitreader/, syntax/ or container/. Every failure names the
directory and its percentage. A gated directory that has source files but no instrumented line in the export
counts as 0 %, so deleting its tests cannot hide it; a gated directory without any source file is reported
and skipped (container/ has no sources yet). Exit 2 means unusable input.

`objects` prints the test executables registered with CTest in BUILD_DIR, one per line, for the
`llvm-cov export -object` arguments.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

SRC = "core/src"
OVERALL_MIN = 80.0
STRICT_MIN = 90.0
STRICT_DIRS = ("bitreader", "syntax", "container")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx"}


def has_sources(directory: Path) -> bool:
    return directory.is_dir() and any(p.suffix in SOURCE_SUFFIXES for p in directory.rglob("*") if p.is_file())


def relative_to_root(filename: str, root: Path) -> str | None:
    try:
        return Path(filename).resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return None


def collect(summary: dict[str, Any], root: Path) -> dict[str, tuple[int, int]]:
    """Return {path relative to root: (lines, covered)} for the files under core/src/."""
    files: dict[str, tuple[int, int]] = {}
    for entry in summary["data"]:
        for item in entry.get("files", []):
            rel = relative_to_root(item["filename"], root)
            if rel is None or not rel.startswith(SRC + "/"):
                continue
            lines = item["summary"]["lines"]
            count, covered = files.get(rel, (0, 0))
            files[rel] = (count + int(lines["count"]), covered + int(lines["covered"]))
    return files


def percent(count: int, covered: int) -> float:
    return 100.0 * covered / count if count else 0.0


def gate(files: dict[str, tuple[int, int]], root: Path) -> tuple[list[str], list[str]]:
    failures: list[str] = []
    notes: list[str] = []
    total = sum(c for c, _ in files.values())
    covered = sum(v for _, v in files.values())
    overall = percent(total, covered)
    notes.append(f"{SRC}/: {overall:.1f} % of {total} lines (minimum {OVERALL_MIN:.0f} %)")
    if overall < OVERALL_MIN:
        failures.append(f"{SRC}/ overall: {overall:.1f} % < {OVERALL_MIN:.0f} %")
    for name in STRICT_DIRS:
        prefix = f"{SRC}/{name}/"
        in_dir = [v for k, v in files.items() if k.startswith(prefix)]
        count = sum(c for c, _ in in_dir)
        if count == 0:
            if has_sources(root / SRC / name):
                failures.append(f"{name}/: 0.0 % < {STRICT_MIN:.0f} % (sources exist but no line is instrumented)")
            else:
                notes.append(f"{name}/: no source files yet; skipped")
            continue
        pct = percent(count, sum(v for _, v in in_dir))
        notes.append(f"{name}/: {pct:.1f} % of {count} lines (minimum {STRICT_MIN:.0f} %)")
        if pct < STRICT_MIN:
            failures.append(f"{name}/: {pct:.1f} % < {STRICT_MIN:.0f} %")
    return failures, notes


def cmd_check(args: argparse.Namespace) -> int:
    try:
        summary = json.loads(Path(args.summary).read_text())
        files = collect(summary, Path(args.root))
    except (OSError, ValueError, KeyError, TypeError) as err:
        print(f"coverage_gate: cannot use {args.summary}: {err}", file=sys.stderr)
        return 2
    if not files:
        print(f"coverage_gate: no file under {SRC}/ in {args.summary} (was OVT_COVERAGE=ON?)", file=sys.stderr)
        return 2
    failures, notes = gate(files, Path(args.root))
    for note in notes:
        print(note)
    for failure in failures:
        print(f"::error::coverage below threshold: {failure}")
    return 1 if failures else 0


def cmd_objects(args: argparse.Namespace) -> int:
    out = subprocess.run(
        ["ctest", "--test-dir", args.test_dir, "--show-only=json-v1"], capture_output=True, text=True, check=False
    )
    if out.returncode != 0:
        print(out.stderr, file=sys.stderr)
        return 2
    seen: dict[str, None] = {}
    for test in json.loads(out.stdout).get("tests", []):
        command = test.get("command") or []
        if command and Path(command[0]).is_file():
            seen[command[0]] = None
    print("\n".join(seen))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    check = sub.add_parser("check")
    check.add_argument("--summary", required=True)
    check.add_argument("--root", default=".")
    check.set_defaults(func=cmd_check)
    objects = sub.add_parser("objects")
    objects.add_argument("--test-dir", required=True)
    objects.set_defaults(func=cmd_objects)
    args = parser.parse_args(argv)
    code: int = args.func(args)
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
