#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Rewrite ``seq_level_idx[0]`` of every AV1 sequence header in an IVF file (§14.3.3 recipe 51; WI-0.553).

    av1_set_level.py --seq-level-idx N in.ivf out.ivf

Parses each ``OBU_SEQUENCE_HEADER`` up to ``seq_level_idx[0]`` (AV1 spec §5.5.1), writes N in its place, adds or
drops the ``seq_tier[0]`` bit when N crosses 7, and re-frames the OBU (``obu_size``) and the IVF frame. Every
other bit is copied. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from bitstream import BitReader, Obu, payload_bits, read_ivf, split_obus, with_trailing_bits, write_ivf

OBU_SEQUENCE_HEADER = 1


def set_level(payload: bytes, level: int) -> bytes:
    r = BitReader(payload)
    r.u(3)  # seq_profile
    r.u(1)  # still_picture
    if r.u(1):  # reduced_still_picture_header
        start, tier = r.pos, False
    else:
        if r.u(1):  # timing_info_present_flag
            r.u(64)  # num_units_in_display_tick, time_scale
            if r.u(1):  # equal_picture_interval
                r.uvlc()
            if r.u(1):  # decoder_model_info_present_flag
                r.u(5 + 32 + 5 + 5)
        r.u(1)  # initial_display_delay_present_flag
        r.u(5)  # operating_points_cnt_minus_1
        r.u(12)  # operating_point_idc[0]
        start, tier = r.pos, True
    old = r.u(5)
    end = r.pos + (1 if tier and old > 7 else 0)
    new = format(level, "05b") + ("0" if tier and level > 7 else "")  # seq_tier[0] = 0 (Main) when added
    total = payload_bits(payload)
    return with_trailing_bits(r.bits(0, start) + new + r.bits(end, total))


def rewrite(data: bytes, level: int) -> tuple[bytes, int]:
    header, frames = read_ivf(data)
    count, out = 0, []
    for pts, tu in frames:
        obus = split_obus(tu)
        for i, obu in enumerate(obus):
            if obu.type == OBU_SEQUENCE_HEADER:
                obus[i] = Obu(obu.header, set_level(obu.payload, level))
                count += 1
        out.append((pts, b"".join(o.encode() for o in obus)))
    return write_ivf(header, out), count


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seq-level-idx", type=int, required=True, choices=range(32), metavar="0..31")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    try:
        out, count = rewrite(args.input.read_bytes(), args.seq_level_idx)
    except (ValueError, IndexError, OSError) as e:
        print(f"av1_set_level: {e}", file=sys.stderr)
        return 2
    if not count:
        print("av1_set_level: no sequence header found", file=sys.stderr)
        return 2
    args.output.write_bytes(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
