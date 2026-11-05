#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Print the SHA-256 of a FlatBuffers schema file's text (chapter 06 WI-0.080).

    schema_hash.py SCHEMA_FBS [SCHEMA_FBS ...]

The hash is taken over the file's UTF-8 text with CRLF normalised to LF, so that a checkout on any
platform yields the same value; it is the `schema_hash` recorded in the cache `meta.json` (chapter 06
§6.7). One line per file: `<64 hex digits>  <path>` (the sha256sum layout). Run with `python -I`;
standard library only.
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def schema_hash(path: Path) -> str:
    text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if argv else 2
    for arg in argv:
        try:
            print(f"{schema_hash(Path(arg))}  {arg}")
        except OSError as exc:
            print(f"schema_hash: cannot read {arg}: {exc}", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
