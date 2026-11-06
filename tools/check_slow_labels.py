#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Speed-tag checker for tests that cite corpus streams (chapter 14 §14.2.5, WI-0.552c).

    check_slow_labels.py [--manifest FILE] [ROOT ...]

The ctest label ``slow`` comes from the Catch2 tag ``[slow]`` (``cmake/OvtCatchAddTests.cmake``), and the ``pr``
lane runs only label ``fast``. A test case that names a stream tagged ``slow`` (and not ``fast``) in the manifest
therefore has to carry ``[slow]`` and must not carry ``[fast]``; otherwise it would pull the big stream into
every pull request. This script finds every ``TEST_CASE`` / ``TEST_CASE_METHOD`` / ``SCENARIO`` in ``*.cpp``
files under the roots (default ``core`` ``render`` ``plugins`` ``cli`` ``app``), takes the string literals in its
body, and reports each case that cites a slow-only manifest id without being tagged ``[slow]``, or that is tagged
both ``[fast]`` and ``[slow]``. A case may also cite a slow stream and stay ``[fast]`` if it carries the tag
``[slow-stream-ok]`` (for a test that only checks the id string).

Exit codes: 0 consistent; 1 a mismatch (printed as file:line); 2 usage.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
DEFAULT_ROOTS = ("core", "render", "plugins", "cli", "app")
CASE = re.compile(r"^[ \t]*(?:TEST_CASE|TEST_CASE_METHOD|SCENARIO)\s*\(", re.M)
STRING = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
TAG = re.compile(r"\[([^\]\[]+)\]")


def slow_only_ids(manifest: Path) -> set[str]:
    entries = json.loads(manifest.read_text(encoding="utf-8"))["entries"]
    return {e["id"] for e in entries if "slow" in e.get("tags", []) and "fast" not in e.get("tags", [])}


def check_text(text: str, slow_ids: set[str], name: str) -> list[str]:
    problems = []
    starts = [m.start() for m in CASE.finditer(text)]
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(text)
        body = text[start:end]
        line = text.count("\n", 0, start) + 1
        strings = STRING.findall(body)
        # The tags are the last string literal of the macro header; string literals of the body come after it.
        header_end = body.find("{")
        header = body[: header_end if header_end > 0 else len(body)]
        tags = set(TAG.findall(" ".join(STRING.findall(header))))
        cited = sorted(s for s in set(strings) if s in slow_ids)
        if "fast" in tags and "slow" in tags:
            problems.append(f"{name}:{line}: tagged both [fast] and [slow]")
        if cited and "slow" not in tags and "slow-stream-ok" not in tags:
            problems.append(f"{name}:{line}: cites slow stream(s) {', '.join(cited)} but lacks the [slow] tag")
    return problems


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=REPO / "tests/corpora/manifest.json")
    p.add_argument("roots", nargs="*", type=Path)
    args = p.parse_args(argv)
    roots = args.roots or [REPO / r for r in DEFAULT_ROOTS]
    try:
        slow_ids = slow_only_ids(args.manifest)
    except (OSError, ValueError, KeyError) as exc:
        print(f"check_slow_labels: cannot read {args.manifest}: {exc}", file=sys.stderr)
        return 2
    problems: list[str] = []
    for root in roots:
        for path in sorted(root.rglob("*.cpp")) if root.is_dir() else []:
            rel = path.relative_to(REPO) if path.is_relative_to(REPO) else path
            problems += check_text(path.read_text(encoding="utf-8", errors="replace"), slow_ids, str(rel))
    for problem in problems:
        print(problem)
    print(f"check_slow_labels: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
