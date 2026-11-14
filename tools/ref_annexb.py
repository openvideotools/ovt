#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Reference Annex B splitter (chapter 08 §8.6.3; WI-1.226; D27).

    ref_annexb.py [--codec h264|h265] FILE

Prints one ``offset,length,nal_type`` line per NAL unit of an H.264/H.265 Annex B byte stream, using the §8.6.3
boundary rule and nothing else: a NAL unit starts after ``00 00 01``, ends before the next ``00 00 00`` or
``00 00 01`` (``00 00 02`` and ``00 00 03`` are not boundaries) or at the end of the file, and loses its trailing
zero bytes. ``offset`` and ``length`` are the NAL unit's ``file_span``; an empty NAL unit is not printed.
``nal_type`` is ``b0 & 0x1F`` (h264) or ``(b0 >> 1) & 0x3F`` (h265); the codec defaults to h265 for
``.265/.h265/.hevc/.bin`` extensions and to h264 otherwise. It is the oracle of the C++ splitter
(``core/tests/container/annexb_test.cpp``) and of WI-1.570. Written independently of the C++ code: a regular
expression search per NAL unit instead of a byte-wise state machine. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

START = re.compile(rb"\x00\x00\x01")
BOUNDARY = re.compile(rb"\x00\x00[\x00\x01]")


def split(data: bytes) -> list[tuple[int, int, int]]:
    """[(offset, length, first_header_byte)] of every non-empty NAL unit."""
    out: list[tuple[int, int, int]] = []
    pos = 0
    while True:
        sc = START.search(data, pos)
        if sc is None:
            return out
        start = sc.end()
        end_match = BOUNDARY.search(data, start)
        end = end_match.start() if end_match else len(data)
        while end > start and data[end - 1] == 0:
            end -= 1
        if end > start:
            out.append((start, end - start, data[start]))
        pos = end


def nal_type(header_byte: int, codec: str) -> int:
    return (header_byte >> 1) & 0x3F if codec == "h265" else header_byte & 0x1F


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--codec", choices=["h264", "h265"])
    ap.add_argument("file", type=Path)
    args = ap.parse_args(argv)
    try:
        data = args.file.read_bytes()
    except OSError as e:
        print(f"ref_annexb.py: {e}", file=sys.stderr)
        return 2
    codec = args.codec or ("h265" if args.file.suffix.lower() in {".265", ".h265", ".hevc", ".bin"} else "h264")
    for offset, length, header in split(data):
        print(f"{offset},{length},{nal_type(header, codec)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
