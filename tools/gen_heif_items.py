#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write a HEIF image made of many ``hvc1`` tile items and one ``grid`` (§14.3.3 recipe 32; WI-0.553).

    gen_heif_items.py --items N --tile S --hevc tile.265 --out file.heic

A pure-Python ISOBMFF / HEIF (ISO/IEC 23008-12) box writer, so no ``heif-convert`` or libheif is needed.
``tile.265`` is one S×S 8-bit 4:2:0 picture encoded by the recipe with the pinned x265; its VPS/SPS/PPS go into
one shared ``hvcC`` property and its slice NAL units (4-byte length prefixed) are the data of every tile item.
Item 1 is the primary ``grid`` (rows × columns = N, as close to square as N's divisors allow); items 2..N+1 are the tiles,
linked by one ``dimg`` reference. All data lives in one ``mdat``. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path

from bitstream import h265_type, split_annexb, unescape


def box(kind: bytes, *body: bytes) -> bytes:
    payload = b"".join(body)
    return struct.pack(">I", 8 + len(payload)) + kind + payload


def full(kind: bytes, version: int, flags: int, *body: bytes) -> bytes:
    return box(kind, struct.pack(">I", version << 24 | flags), *body)


def hvcc(vps: bytes, sps: bytes, pps: bytes) -> bytes:
    rbsp = unescape(sps)
    ptl = rbsp[3:15]  # general profile_space .. general_level_idc (H.265 §7.3.3), 12 bytes
    head = bytes([1]) + ptl + bytes([0xF0, 0x00, 0xFC, 0xFD, 0xF8, 0xF8, 0, 0, 0x0F, 3])  # 4:2:0, 8 bit, 4-byte lengths
    arrays = b"".join(
        bytes([0x80 | t]) + struct.pack(">HH", 1, len(n)) + n for t, n in ((32, vps), (33, sps), (34, pps))
    )
    return box(b"hvcC", head, arrays)


def tile_parts(data: bytes) -> tuple[bytes, bytes, bytes, bytes]:
    nals: dict[int, bytes] = {}
    vcl = b""
    for nal in split_annexb(data):
        t = h265_type(nal.data)
        if t in (32, 33, 34):
            nals.setdefault(t, nal.data)
        elif t < 32:
            if vcl and unescape(nal.data[:3])[2] & 0x80:
                break  # only the first picture
            vcl += struct.pack(">I", len(nal.data)) + nal.data
    if len(nals) != 3 or not vcl:
        raise ValueError("the HEVC input needs a VPS, SPS, PPS and one picture")
    return nals[32], nals[33], nals[34], vcl


def build(items: int, tile: int, hevc: bytes) -> bytes:
    vps, sps, pps, sample = tile_parts(hevc)
    cols = next(d for d in range(math.ceil(math.sqrt(items)), items + 1) if items % d == 0)
    rows = items // cols
    if rows > 256 or cols > 256:
        raise ValueError(f"{items} items do not fill a grid of at most 256 x 256 (got {rows} x {cols})")
    grid = struct.pack(">BBBBHH", 0, 0, rows - 1, cols - 1, cols * tile, rows * tile)
    ids = range(2, items + 2)
    ftyp = box(b"ftyp", b"heic", struct.pack(">I", 0), b"mif1heic")

    def meta(mdat_start: int) -> bytes:
        infe = [full(b"infe", 2, 0, struct.pack(">HH", 1, 0), b"grid\0")]
        infe += [full(b"infe", 2, 1, struct.pack(">HH", i, 0), b"hvc1\0") for i in ids]  # flags 1: hidden tiles
        iinf = full(b"iinf", 0, 0, struct.pack(">H", items + 1), *infe)
        iref = full(b"iref", 0, 0, box(b"dimg", struct.pack(">HH", 1, items), *(struct.pack(">H", i) for i in ids)))
        ispe_tile = full(b"ispe", 0, 0, struct.pack(">II", tile, tile))
        ispe_grid = full(b"ispe", 0, 0, struct.pack(">II", cols * tile, rows * tile))
        ipco = box(b"ipco", hvcc(vps, sps, pps), ispe_tile, ispe_grid)
        assoc = [struct.pack(">HBB", 1, 1, 3)] + [struct.pack(">HBBB", i, 2, 0x81, 2) for i in ids]
        ipma = full(b"ipma", 0, 0, struct.pack(">I", items + 1), *assoc)
        extents = [(1, mdat_start, len(grid))]
        extents += [(i, mdat_start + len(grid) + k * len(sample), len(sample)) for k, i in enumerate(ids)]
        iloc = full(
            b"iloc",
            1,
            0,
            struct.pack(">BBH", 0x44, 0x00, items + 1),
            *(struct.pack(">HHHHII", i, 0, 0, 1, off, n) for i, off, n in extents),
        )
        return full(
            b"meta",
            0,
            0,
            full(b"hdlr", 0, 0, b"\0\0\0\0pict", bytes(12), b"\0"),
            full(b"pitm", 0, 0, b"\0\x01"),
            iloc,
            iinf,
            iref,
            box(b"iprp", ipco, ipma),
        )

    start = len(ftyp) + len(meta(0)) + 8
    return ftyp + meta(start) + box(b"mdat", grid, sample * items)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--items", type=int, required=True)
    ap.add_argument("--tile", type=int, required=True)
    ap.add_argument("--hevc", type=Path, required=True, help="Annex B H.265 of one tile-sized picture")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    try:
        args.out.write_bytes(build(args.items, args.tile, args.hevc.read_bytes()))
    except (ValueError, IndexError, OSError) as e:
        print(f"gen_heif_items: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
