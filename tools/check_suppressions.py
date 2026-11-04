#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that every sanitizer suppression links its upstream issue (chapter 14 §14.8.1; WI-0.558).

    check_suppressions.py [file.supp ...]

Without arguments it checks tests/asan.supp, tests/ubsan.supp and tests/tsan.supp. The sanitizer runtimes
do not accept trailing comments on an entry line, so the rule is positional: an entry must be in a block
whose comment lines (`# ...`) contain an `https://` URL with a path, and the block is the run of lines
directly above the entry, up to the previous blank line or entry-free start of file. Consecutive entries
share the comment above the first one. A blank line ends the block.

The entry kind (the text before the first `:`) is checked against the sanitizer named by the file
(`asan*.supp`, `ubsan*.supp`, `tsan*.supp`). Messages, one per finding, exit 1 when there is any:
    E-NOURL <file>:<line> <entry>
    E-KIND <file>:<line> unknown entry kind '<kind>'
    E-EMPTY <file>:<line> entry has no pattern
Exit 2 when a file cannot be read. Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

URL = re.compile(r"https://[A-Za-z0-9.-]+\.[A-Za-z]{2,}/\S+")

KINDS = {
    "asan": {"leak", "interceptor_name", "interceptor_via_fun", "interceptor_via_lib", "odr_violation"},
    "ubsan": {
        "alignment",
        "bool",
        "bounds",
        "builtin",
        "enum",
        "float-cast-overflow",
        "float-divide-by-zero",
        "function",
        "implicit-conversion",
        "implicit-integer-sign-change",
        "implicit-signed-integer-truncation",
        "implicit-unsigned-integer-truncation",
        "integer-divide-by-zero",
        "nonnull-attribute",
        "null",
        "nullability-arg",
        "nullability-assign",
        "nullability-return",
        "object-size",
        "pointer-overflow",
        "return",
        "returns-nonnull-attribute",
        "shift",
        "shift-base",
        "shift-exponent",
        "signed-integer-overflow",
        "unreachable",
        "unsigned-integer-overflow",
        "unsigned-shift-base",
        "vla-bound",
        "vptr",
    },
    "tsan": {"race", "race_top", "thread", "mutex", "signal", "deadlock", "called_from_lib"},
}


def sanitizer_of(path: Path) -> str | None:
    for name in ("ubsan", "asan", "tsan"):
        if path.name.startswith(name):
            return name
    return None


def check_file(path: Path) -> list[str]:
    findings: list[str] = []
    kinds = KINDS.get(sanitizer_of(path) or "")
    block_has_url = False
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.strip()
        if not line:
            block_has_url = False
            continue
        if line.startswith("#"):
            block_has_url = block_has_url or bool(URL.search(line))
            continue
        where = f"{path}:{number}"
        kind, sep, pattern = line.partition(":")
        if kinds is not None and kind not in kinds:
            findings.append(f"E-KIND {where} unknown entry kind '{kind}'")
        if not sep or not pattern.strip():
            findings.append(f"E-EMPTY {where} entry has no pattern")
        if not block_has_url:
            findings.append(f"E-NOURL {where} {line}")
    return findings


def main(argv: list[str]) -> int:
    root = Path(__file__).resolve().parent.parent
    paths = [Path(a) for a in argv] or [root / "tests" / f"{n}.supp" for n in ("asan", "ubsan", "tsan")]
    findings: list[str] = []
    for path in paths:
        try:
            findings += check_file(path)
        except OSError as exc:
            print(f"check_suppressions.py: {exc}", file=sys.stderr)
            return 2
    for finding in findings:
        print(finding)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
