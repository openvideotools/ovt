#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Check that the libde265 build recorded every patch of its series (WI-0.014, chapter 13 §13.3.6).

    check_patches_json.py <prefix>/share/ovt/patches.json <series>

Exit 0 when patches.json lists exactly the series' patches, in series order, each with its SHA-256.
Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path


def series_patches(series: Path) -> list[Path]:
    entries = (line.split("#", 1)[0].strip() for line in series.read_text(encoding="utf-8").splitlines())
    return [series.parent / e.split()[0] for e in entries if e]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    record, series = Path(argv[0]), Path(argv[1])
    if not record.is_file():
        print(f"check_patches_json: {record} does not exist; the patch step did not record", file=sys.stderr)
        return 1
    recorded = json.loads(record.read_text(encoding="utf-8"))["patches"]
    expected = [(p.name, hashlib.sha256(p.read_bytes()).hexdigest()) for p in series_patches(series)]
    got = [(p["name"], p["sha256"]) for p in recorded]
    if got != expected:
        print(f"check_patches_json: {record} lists {got}, the series {series} has {expected}", file=sys.stderr)
        return 1
    print(f"check_patches_json: {len(got)} patch(es) recorded")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
