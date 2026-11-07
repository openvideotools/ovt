#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Insert AV1 HDR metadata OBUs after every sequence header of an IVF file (§14.3.3 recipes 15a, 64; WI-0.553).

    av1_insert_hdr_obus.py [--mdcv "G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)"] [--cll MaxCLL,MaxFALL] in.ivf out.ivf

``--mdcv`` takes the x265 ``--master-display`` notation (chromaticities in 0.00002 units, luminance in
0.0001 cd/m²) and writes ``METADATA_TYPE_HDR_MDCV`` (AV1 spec §5.8.4, §6.7.4: 0.16 chromaticities in R, G, B
order, 24.8 maximum and 18.14 minimum luminance); ``--cll`` writes ``METADATA_TYPE_HDR_CLL`` (§5.8.3). The OBUs
follow each ``OBU_SEQUENCE_HEADER`` (MDCV first), so every random access point carries them. The values as coded
are printed as JSON on stdout for the ``hdr_expected.json`` sidecar. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

from bitstream import Obu, leb128, read_ivf, split_obus, write_ivf

OBU_SEQUENCE_HEADER, OBU_METADATA = 1, 5
METADATA_TYPE_HDR_CLL, METADATA_TYPE_HDR_MDCV = 1, 2
MDCV = re.compile(r"G\((\d+),(\d+)\)B\((\d+),(\d+)\)R\((\d+),(\d+)\)WP\((\d+),(\d+)\)L\((\d+),(\d+)\)")


def parse_mdcv(text: str) -> dict:
    m = MDCV.fullmatch(text.replace(" ", ""))
    if not m:
        raise ValueError(f"--mdcv {text!r} is not G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)")
    gx, gy, bx, by, rx, ry, wx, wy, lmax, lmin = (int(v) for v in m.groups())

    def chroma(v: int) -> int:  # 0.00002 units -> 0.16 fixed point
        return min(round(v * 0.00002 * 65536), 0xFFFF)

    return {
        "primaries": [[chroma(rx), chroma(ry)], [chroma(gx), chroma(gy)], [chroma(bx), chroma(by)]],
        "white_point": [chroma(wx), chroma(wy)],
        "luminance_max": round(lmax * 0.0001 * 256),  # 24.8
        "luminance_min": round(lmin * 0.0001 * 16384),  # 18.14
    }


def metadata_obu(kind: int, body: bytes) -> Obu:
    # obu_type 5, no extension, obu_has_size_field 1; payload ends with trailing_bits (one 0x80 byte)
    return Obu(bytes([OBU_METADATA << 3 | 0x02]), leb128(kind) + body + b"\x80")


def build_obus(mdcv: dict | None, cll: tuple[int, int] | None) -> list[Obu]:
    obus = []
    if mdcv:
        values = [v for xy in mdcv["primaries"] for v in xy] + mdcv["white_point"]
        body = struct.pack(">8H2I", *values, mdcv["luminance_max"], mdcv["luminance_min"])
        obus.append(metadata_obu(METADATA_TYPE_HDR_MDCV, body))
    if cll:
        obus.append(metadata_obu(METADATA_TYPE_HDR_CLL, struct.pack(">2H", *cll)))
    return obus


def insert(data: bytes, extra: list[Obu]) -> tuple[bytes, int]:
    header, frames = read_ivf(data)
    out, count = [], 0
    for pts, tu in frames:
        obus = []
        for obu in split_obus(tu):
            obus.append(obu)
            if obu.type == OBU_SEQUENCE_HEADER:
                obus += extra
                count += 1
        out.append((pts, b"".join(o.encode() for o in obus)))
    return write_ivf(header, out), count


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mdcv", help="mastering display, x265 --master-display notation")
    ap.add_argument("--cll", help="MaxCLL,MaxFALL in cd/m²")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    if not args.mdcv and not args.cll:
        ap.error("give --mdcv, --cll or both")
    try:
        mdcv = parse_mdcv(args.mdcv) if args.mdcv else None
        cll = tuple(int(v) for v in args.cll.split(",")) if args.cll else None
        if cll is not None and (len(cll) != 2 or not all(0 <= v <= 0xFFFF for v in cll)):
            raise ValueError(f"--cll {args.cll!r} is not MaxCLL,MaxFALL")
        out, count = insert(args.input.read_bytes(), build_obus(mdcv, cll))  # type: ignore[arg-type]
    except (ValueError, IndexError, OSError) as e:
        print(f"av1_insert_hdr_obus: {e}", file=sys.stderr)
        return 2
    if not count:
        print("av1_insert_hdr_obus: no sequence header found", file=sys.stderr)
        return 2
    args.output.write_bytes(out)
    record = {"mdcv": mdcv, "cll": {"max_cll": cll[0], "max_fall": cll[1]} if cll else None, "sequence_headers": count}
    print(json.dumps(record, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
