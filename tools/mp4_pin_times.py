#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pin the creation and modification times of an ISOBMFF file to 0 (§14.3.3 reproducibility rule 2; WI-0.553).

    mp4_pin_times.py in.mp4 out.mp4

Bento4 ``mp4fragment`` and ``mp4encrypt`` write the wall-clock time into ``mvhd`` and ignore
``SOURCE_DATE_EPOCH``, so two runs of the same recipe differ in those bytes. This rewrites the
``creation_time`` and ``modification_time`` fields of every ``mvhd``, ``tkhd`` and ``mdhd`` box (inside
``moov``/``trak``/``mdia``) to 0 (1904-01-01, what ``MP4Box -for-test`` writes). Version 0 boxes have 32-bit
fields and version 1 boxes 64-bit fields. Only those bytes change; box sizes and every other byte stay the same.
The patched box offsets are printed as JSON on stdout. Exit 0 ok, 2 bad input (not ISOBMFF, no ``mvhd``).
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

CONTAINERS = {b"moov", b"trak", b"mdia"}
TIMED = {b"mvhd", b"tkhd", b"mdhd"}


def boxes(data: bytearray, start: int, end: int):
    """Yield (type, box start, payload start, box end) for the boxes in data[start:end]."""
    pos = start
    while pos < end:
        if end - pos < 8:
            raise ValueError(f"truncated box header at {pos}")
        size, kind = struct.unpack_from(">I4s", data, pos)
        header = 8
        if size == 1:
            if end - pos < 16:
                raise ValueError(f"truncated largesize header at {pos}")
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            raise ValueError(f"box {kind!r} at {pos} has invalid size {size}")
        yield kind, pos, pos + header, pos + size
        pos += size


def pin_times(data: bytearray) -> list[dict]:
    """Zero the times in place; returns one record per patched box."""
    patched: list[dict] = []

    def walk(start: int, end: int) -> None:
        for kind, box, payload, box_end in boxes(data, start, end):
            if kind in CONTAINERS:
                walk(payload, box_end)
            elif kind in TIMED:
                version = data[payload]
                width = 8 if version == 1 else 4
                if box_end - payload < 4 + 2 * width:
                    raise ValueError(f"{kind.decode()} at {box} is too short")
                data[payload + 4 : payload + 4 + 2 * width] = bytes(2 * width)
                patched.append({"box": kind.decode(), "offset": box, "version": version})

    walk(0, len(data))
    if not any(p["box"] == "mvhd" for p in patched):
        raise ValueError("no moov/mvhd box")
    return patched


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    data = bytearray(args.input.read_bytes())
    try:
        patched = pin_times(data)
    except ValueError as exc:
        print(f"mp4_pin_times: {args.input}: {exc}", file=sys.stderr)
        return 2
    args.output.write_bytes(bytes(data))
    print(json.dumps(patched))
    return 0


if __name__ == "__main__":
    sys.exit(main())
