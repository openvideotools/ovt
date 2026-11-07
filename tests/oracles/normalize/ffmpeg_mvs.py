#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""``ffmpeg_mvs`` CSV -> ``block-grid`` motion vectors or QP blocks (chapter 14 §14.4.3a, WI-0.555).

Motion vectors: rows ``framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,
motion_scale`` (one per ``AVMotionVector``, as FFmpeg's ``doc/examples/extract_mvs.c`` prints them; the
header row is optional). Each becomes ``{list, x, y, w, h, mv_x, mv_y}``: ``list`` 0 for ``source < 0``,
1 for ``source > 0``; the rectangle from the block centre ``(dstx, dsty)``; the vector in 1/16 luma
samples, ``motion_x × 16 / motion_scale``.

``--qp``: rows ``framenum,src_x,src_y,w,h,qp,delta_qp`` (``AVVideoBlockParams``) become
``{x, y, w, h, qp}`` with ``qp = qp + delta_qp − 6 × (bit_depth − 8)`` (FFmpeg includes QpBdOffsetY).

``framenum`` counts decoded frames from 1 as ``extract_mvs.c`` does, so ``frame = framenum − 1``. Frames
without rows (intra frames) do not appear.
"""

from __future__ import annotations

import argparse
import csv
import io
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

MV_COLUMNS = "framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale".split(",")
QP_COLUMNS = "framenum,src_x,src_y,w,h,qp,delta_qp".split(",")


# SPEC-ASSUMPTION(#93): MVs and QP blocks go into block-grid frames as "mvs" / "qp_blocks".
def rows(text: str, columns: list[str]) -> list[dict[str, int]]:
    out = []
    for n, row in enumerate(csv.reader(io.StringIO(text)), start=1):
        if not row or row[0].startswith("#") or row[0].strip() == "framenum":
            continue
        if len(row) != len(columns):
            raise canon.NormalizeError(f"line {n}: {len(row)} columns, expected {len(columns)}")
        # flags is printed as 0x%PRIx64 and not used
        out.append({c: canon.to_int(v, f"line {n} {c}") for c, v in zip(columns, row) if c != "flags"})
    return out


def motion_vector(r: dict[str, int]) -> dict[str, int]:
    if r["source"] == 0 or r["motion_scale"] <= 0:
        raise canon.NormalizeError(f"frame {r['framenum']}: source 0 or motion_scale <= 0")
    w, h = r["blockw"], r["blockh"]
    vec = []
    for comp in ("motion_x", "motion_y"):
        num = r[comp] * 16
        if num % r["motion_scale"]:
            raise canon.NormalizeError(f"frame {r['framenum']}: {comp} not a whole number of 1/16 samples")
        vec.append(num // r["motion_scale"])
    return {
        "list": 0 if r["source"] < 0 else 1,
        "x": r["dstx"] - w // 2,
        "y": r["dsty"] - h // 2,
        "w": w,
        "h": h,
        "mv_x": vec[0],
        "mv_y": vec[1],
    }


def convert(args: argparse.Namespace) -> dict[str, Any]:
    text = canon.read_text(args.input)
    frames: dict[int, list[dict[str, int]]] = {}
    if args.qp:
        offset = 6 * (args.bit_depth - 8)
        for r in rows(text, QP_COLUMNS):
            frames.setdefault(r["framenum"] - 1, []).append(
                {"x": r["src_x"], "y": r["src_y"], "w": r["w"], "h": r["h"], "qp": r["qp"] + r["delta_qp"] - offset}
            )
        key = "qp_blocks"
    else:
        for r in rows(text, MV_COLUMNS):
            frames.setdefault(r["framenum"] - 1, []).append(motion_vector(r))
        key = "mvs"
    if any(f < 0 for f in frames):
        raise canon.NormalizeError("framenum < 1")
    order = ("list", "y", "x", "h", "w") if key == "mvs" else ("y", "x", "h", "w")
    return {
        "frames": [
            {"frame": f, key: sorted(v, key=lambda e: tuple(e[k] for k in order))} for f, v in sorted(frames.items())
        ]
    }


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--qp", action="store_true", help="the input is the QP CSV (<id>.ffmpeg-qp.csv)")
    parser.add_argument("--bit-depth", type=int, default=8, help="luma bit depth for --qp (default 8)")


if __name__ == "__main__":
    sys.exit(canon.run("block-grid", __doc__.splitlines()[0], convert, add_args))
