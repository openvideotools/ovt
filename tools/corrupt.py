#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Deterministic corruption of H.264 / H.265 Annex B streams (§14.3.3 recipes 49, 50; chapter 7; WI-0.553).

    corrupt.py [--codec h264|h265] MODE in out

Modes (exactly one):

* ``--truncate-nal TYPE --keep-bytes N``: cut the first NAL unit of TYPE to its first N bytes (header included);
* ``--drop-nal TYPE``: remove every NAL unit of TYPE;
* ``--flip-slice-data --frame F --byte B``: invert byte B (counted from the NAL header) of the first slice of
  picture F (decode order, from 0);
* ``--flip-hash-sei --frame F``: invert the lowest bit of the first hash byte of the H.265 decoded picture hash
  SEI (payloadType 132) of picture F;
* ``--byte-xor OFFSET``: invert the byte at file offset OFFSET (chapter 7 ``tests/golden/h265/corrupt``).

TYPE is a name (``vps``, ``sps``, ``pps``, ``sei``, ``aud``) or a ``nal_unit_type`` number. The codec defaults
from the input extension (``.265``/``.hevc``/``.h265`` are H.265). A JSON record of the edit goes to stdout.
Exit 0 ok, 2 bad input or nothing to corrupt.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from bitstream import BitReader, Nal, escape, h264_type, h265_type, join_annexb, split_annexb, unescape

NAMES = {
    "h264": {"sps": 7, "pps": 8, "sei": 6, "aud": 9},
    "h265": {"vps": 32, "sps": 33, "pps": 34, "aud": 35, "sei": 39, "suffix_sei": 40},
}
DECODED_PICTURE_HASH = 132
EMULATION = re.compile(b"\x00\x00[\x00-\x03]")


def nal_type(codec: str, nal: Nal) -> int:
    return h265_type(nal.data) if codec == "h265" else h264_type(nal.data)


def is_vcl(codec: str, t: int) -> bool:
    return t < 32 if codec == "h265" else 1 <= t <= 5


def first_slice_of_picture(codec: str, nal: Nal) -> bool:
    rbsp = unescape(nal.data[:16])
    if codec == "h265":
        return bool(rbsp[2] & 0x80)  # first_slice_segment_in_pic_flag
    return BitReader(rbsp, 8).ue() == 0  # first_mb_in_slice


def picture_index(codec: str, nals: list[Nal]) -> list[int]:
    """Per NAL, the index of the picture it belongs to (non-VCL units count towards the next picture)."""
    out, current = [], -1
    for nal in nals:
        t = nal_type(codec, nal)
        if is_vcl(codec, t) and first_slice_of_picture(codec, nal):
            current += 1
        suffix = codec == "h265" and t in (38, 40)  # FD and suffix SEI follow their picture
        out.append(current if is_vcl(codec, t) or suffix else current + 1)
    return out


def type_number(codec: str, name: str) -> int:
    return int(name) if name.isdigit() else NAMES[codec][name]


def flip_hash(rbsp: bytes) -> bytes | None:
    pos = 2
    while pos < len(rbsp) and rbsp[pos:] != b"\x80":
        ptype = psize = 0
        while rbsp[pos] == 0xFF:
            ptype, pos = ptype + 255, pos + 1
        ptype, pos = ptype + rbsp[pos], pos + 1
        while rbsp[pos] == 0xFF:
            psize, pos = psize + 255, pos + 1
        psize, pos = psize + rbsp[pos], pos + 1
        if ptype == DECODED_PICTURE_HASH:
            target = pos + 1  # after hash_type
            return rbsp[:target] + bytes([rbsp[target] ^ 0x01]) + rbsp[target + 1 :]
        pos += psize
    return None


def corrupt(data: bytes, codec: str, args: argparse.Namespace) -> tuple[bytes, dict]:
    if args.byte_xor is not None:
        if not 0 <= args.byte_xor < len(data):
            raise ValueError(f"offset {args.byte_xor} is outside the file")
        return data[: args.byte_xor] + bytes([data[args.byte_xor] ^ 0xFF]) + data[args.byte_xor + 1 :], {
            "offset": args.byte_xor
        }
    nals = split_annexb(data)
    if args.drop_nal:
        t = type_number(codec, args.drop_nal)
        dropped = [i for i, n in enumerate(nals) if nal_type(codec, n) == t]
        if not dropped:
            raise ValueError(f"no NAL unit of type {args.drop_nal}")
        return join_annexb([n for n in nals if nal_type(codec, n) != t]), {"dropped": dropped}
    if args.truncate_nal:
        t = type_number(codec, args.truncate_nal)
        i = next((i for i, n in enumerate(nals) if nal_type(codec, n) == t), None)
        if i is None or args.keep_bytes is None or not 0 < args.keep_bytes < len(nals[i].data):
            raise ValueError(f"no NAL unit of type {args.truncate_nal} longer than --keep-bytes")
        nals[i].data = nals[i].data[: args.keep_bytes]
        return join_annexb(nals), {"nal": i, "keep_bytes": args.keep_bytes}
    pics = picture_index(codec, nals)
    if args.flip_slice_data:
        i = next((i for i, n in enumerate(nals) if pics[i] == args.frame and is_vcl(codec, nal_type(codec, n))), None)
        if i is None or not 2 <= args.byte < len(nals[i].data):
            raise ValueError(f"picture {args.frame} has no slice with byte {args.byte}")
        d = nals[i].data
        nals[i].data = d[: args.byte] + bytes([d[args.byte] ^ 0xFF]) + d[args.byte + 1 :]
        window = slice(max(0, args.byte - 2), args.byte + 3)
        if EMULATION.search(nals[i].data[window]) and not EMULATION.search(d[window]):
            raise ValueError(f"inverting byte {args.byte} would create a start code or emulation sequence")
        return join_annexb(nals), {"nal": i, "frame": args.frame, "byte": args.byte}
    if codec != "h265":
        raise ValueError("--flip-hash-sei needs an H.265 stream")
    for i, n in enumerate(nals):
        if pics[i] == args.frame and nal_type(codec, n) in (39, 40):
            flipped = flip_hash(unescape(n.data))
            if flipped is not None:
                nals[i].data = escape(flipped)
                return join_annexb(nals), {"nal": i, "frame": args.frame}
    raise ValueError(f"picture {args.frame} has no decoded picture hash SEI")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--codec", choices=("h264", "h265"))
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--truncate-nal", metavar="TYPE")
    mode.add_argument("--drop-nal", metavar="TYPE")
    mode.add_argument("--flip-slice-data", action="store_true")
    mode.add_argument("--flip-hash-sei", action="store_true")
    mode.add_argument("--byte-xor", type=int, metavar="OFFSET")
    ap.add_argument("--keep-bytes", type=int)
    ap.add_argument("--frame", type=int)
    ap.add_argument("--byte", type=int)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    if (args.flip_slice_data or args.flip_hash_sei) and args.frame is None:
        ap.error("--frame is required")
    if args.flip_slice_data and args.byte is None:
        ap.error("--byte is required with --flip-slice-data")
    codec = args.codec or ("h265" if args.input.suffix.lower() in (".265", ".hevc", ".h265") else "h264")
    try:
        out, record = corrupt(args.input.read_bytes(), codec, args)
    except (ValueError, KeyError, IndexError, OSError) as e:
        print(f"corrupt: {e}", file=sys.stderr)
        return 2
    args.output.write_bytes(out)
    print(json.dumps(record))
    return 0


if __name__ == "__main__":
    sys.exit(main())
