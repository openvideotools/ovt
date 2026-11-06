#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""GPAC ``MP4Box -dts -std`` text -> ``sample-table`` (chapter 14 §14.4.6, WI-0.555).

GPAC prints, per track, ``#dumping track ID <id> timing:``, a tab-separated header row starting ``Num  DTS
CTS  Size  RAP  Offset`` and one row per sample: ``Sample <n>  DTS <dts>  CTS <cts>  <size>  <rap>
<offset> ...``. Each row becomes ``{i: n - 1, dts, cts, size, offset, sync}`` with ``sync = 1`` when RAP
(GPAC's SAP type) is non-zero. Times are raw media ticks (no edit list), as OVT's ``sample-table``.

With several tracks in the dump every row carries ``track``; ``--track <id>`` keeps one track and drops it.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

TRACK_RE = re.compile(r"^#dumping track ID (\d+) timing")
HEADER = "Num\tDTS\tCTS\tSize\tRAP\tOffset"
ROW_RE = re.compile(r"^Sample (\d+)\tDTS (-?\d+)\tCTS (-?\d+)\t(\d+)\t(\d+)\t(\d+)(?:\t|$)")


def convert(args: argparse.Namespace) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    track: int | None = None
    for n, line in enumerate(canon.read_text(args.input).splitlines(), start=1):
        t = TRACK_RE.match(line)
        if t:
            track = int(t.group(1))
            continue
        if line.startswith("Num\t") and not line.startswith(HEADER):
            raise canon.NormalizeError(f"line {n}: unexpected column order {line!r}")
        if not line.startswith("Sample "):
            continue
        m = ROW_RE.match(line)
        if not m:
            raise canon.NormalizeError(f"line {n}: not a -dts sample row: {line!r}")
        if track is None:
            raise canon.NormalizeError(f"line {n}: sample row before '#dumping track ID'")
        num, dts, cts, size, rap, offset = (int(g) for g in m.groups())
        rows.append(
            {"track": track, "i": num - 1, "dts": dts, "cts": cts, "size": size, "offset": offset, "sync": int(rap > 0)}
        )
    if args.track is not None:
        rows = [r for r in rows if r["track"] == args.track]
        if not rows:
            raise canon.NormalizeError(f"no samples for track {args.track}")
    if args.track is not None or len({r["track"] for r in rows}) == 1:
        for r in rows:
            del r["track"]
    return rows


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--track", type=int, help="keep only this track_ID")


if __name__ == "__main__":
    sys.exit(canon.run("sample-table", __doc__.splitlines()[0], convert, add_args))
