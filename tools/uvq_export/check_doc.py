#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that every table row of docs/dev/uvq_preprocessing.md cites a source line (WI-3.314 AT3).

    check_doc.py [--doc docs/dev/uvq_preprocessing.md] [--uvq-dir DIR] [--show]

Every data row of every Markdown table needs at least one `path:line` reference, for example
`utils/video_reader.py:173`. A reference prefixed with `OVT:` points into this repository, the others into the
google/uvq clone. The §9.6.4 table (the one whose header starts with `model version`) must exist and keep the
nine columns of the chapter. With `--uvq-dir` each upstream reference must name an existing file and line;
`OVT:` references are always checked against this repository. `--show` prints each cited line for review.

Messages, one per finding, and exit 1 when there is any:
    E-NOREF <doc>:<line> table row without a source file:line reference
    E-NOTABLE <doc> no table with the §9.6.4 header
    E-COLUMNS <doc>:<line> §9.6.4 table has <n> columns, expected 9
    E-BADREF <doc>:<line> <path>:<n> file or line does not exist

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
DEFAULT_DOC = REPO / "docs" / "dev" / "uvq_preprocessing.md"
REF = re.compile(r"(OVT:\s*)?([A-Za-z0-9_./-]+\.(?:py|csv|md|txt|json)):(\d+)")
SPEC_HEADER = [
    "model version",
    "input tensor name",
    "shape",
    "dtype",
    "channel order",
    "value range/normalisation",
    "frames per chunk",
    "resize filter",
    "source file:line",
]


def cells(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def is_separator(line: str) -> bool:
    return bool(re.fullmatch(r"\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?", line.strip()))


def tables(text: str) -> list[tuple[int, list[str], list[tuple[int, str]]]]:
    """(header line number, header cells, [(line number, row text)]) for each pipe table."""
    lines = text.splitlines()
    found = []
    i = 0
    while i < len(lines) - 1:
        if lines[i].lstrip().startswith("|") and is_separator(lines[i + 1]):
            header = cells(lines[i])
            rows = []
            j = i + 2
            while j < len(lines) and lines[j].lstrip().startswith("|"):
                rows.append((j + 1, lines[j]))
                j += 1
            found.append((i + 1, header, rows))
            i = j
        else:
            i += 1
    return found


def resolve(path: str, ovt: bool, uvq_dir: Path | None) -> Path | None:
    if ovt:
        return REPO / path
    return uvq_dir / path if uvq_dir else None


def check(doc: Path, uvq_dir: Path | None, show: bool = False) -> list[str]:
    text = doc.read_text(encoding="utf-8")
    findings = []
    have_spec_table = False
    for hline, header, rows in tables(text):
        if [h.lower() for h in header[:1]] == ["model version"]:
            have_spec_table = True
            if [h.lower() for h in header] != SPEC_HEADER:
                findings.append(f"E-COLUMNS {doc}:{hline} §9.6.4 table has {len(header)} columns, expected 9")
        for lineno, row in rows:
            refs = REF.findall(row)
            if not refs:
                findings.append(f"E-NOREF {doc}:{lineno} table row without a source file:line reference")
                continue
            for prefix, path, num in refs:
                target = resolve(path, bool(prefix), uvq_dir)
                if target is None:
                    continue
                n = int(num)
                src_lines = target.read_text(encoding="utf-8").splitlines() if target.is_file() else []
                if not 1 <= n <= len(src_lines):
                    findings.append(f"E-BADREF {doc}:{lineno} {path}:{n} file or line does not exist")
                elif show:
                    print(f"{path}:{n}: {src_lines[n - 1].strip()}")
    if not have_spec_table:
        findings.append(f"E-NOTABLE {doc} no table with the §9.6.4 header")
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--doc", type=Path, default=DEFAULT_DOC)
    ap.add_argument("--uvq-dir", type=Path, help="google/uvq clone at UVQ_COMMIT, to check the cited lines")
    ap.add_argument("--show", action="store_true", help="print every cited line")
    a = ap.parse_args(argv)
    findings = check(a.doc, a.uvq_dir, a.show)
    for f in findings:
        print(f)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
