#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""WI-0.088 acceptance tests 3 and 4: the exports parse with Python's json.loads.

    check_json_loads.py COMPACT.json STRICT_NAN.json

COMPACT is the `JsonOptions{compact = true}` export of tiny16_hevc.fa: it has no newline. STRICT_NAN is the
`JsonOptions{strict_numbers = true}` export of a MetricsReport with a NaN score: the value is the string
"nan". json.loads accepts the bare tokens NaN and Infinity by default; here they are an error, so both files
must be RFC 8259 JSON. The files are written by `test_json "[json-files]"` (CTest fixture ovt_json_exports).
Exit 0 on success, 1 on a failed check. Run with `python -I`; standard library only.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path


def _no_constant(token: str) -> float:
    raise ValueError(f"non-standard JSON token {token}")


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 1
    compact = Path(argv[1]).read_text(encoding="utf-8")
    strict = Path(argv[2]).read_text(encoding="utf-8")
    if "\n" in compact:
        print(f"{argv[1]}: the compact export has a newline", file=sys.stderr)
        return 1
    frame = json.loads(compact, parse_constant=_no_constant)
    if frame["plugin_id"] != "ovt-plugin-de265" or len(frame["grid"]["block_id"]) != 16:
        print(f"{argv[1]}: not the tiny16_hevc export", file=sys.stderr)
        return 1
    report = json.loads(strict, parse_constant=_no_constant)
    score = report["results"][0]["frames"][1]["values"][0]
    if score != "nan":
        print(
            f"{argv[2]}: the NaN score is {score!r}, expected the string 'nan'",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
