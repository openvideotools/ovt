#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Per-frame MaxCLL / FALL of a PQ Y4M in float64 -> ``scores`` (chapter 14 §14.4.15, WI-0.555).

Input: a Y4M file of the decoded frames (``ffmpeg -i <in> -pix_fmt yuv420p10le -f yuv4mpegpipe out.y4m``;
``C420p10``, ``C420p12``, ``C422p10``, ``C444p10``, ... or 8-bit ``C420``). For each frame (or only ``--frame
n``, 0-based):

1. Y'CbCr to normalised values: limited range ``Y' = (Y − 16·2^(b−8)) / (219·2^(b−8))``,
   ``C = (C − 128·2^(b−8)) / (224·2^(b−8))``, or full range (``--full-range`` or the Y4M
   ``XCOLORRANGE=FULL`` tag) ``Y' = Y / (2^b − 1)``, ``C = (C − 2^(b−1)) / (2^b − 1)``. Chroma is
   upsampled by sample repetition (each chroma sample covers its 2×2 / 2×1 luma block).
2. BT.2020 non-constant-luminance matrix (Kr = 0.2627, Kb = 0.0593) to R'G'B', clipped to [0, 1].
3. ST 2084 EOTF to cd/m² (10000 × the PQ inverse), per component.
4. ``max_cll_nits`` = the largest max(R, G, B) of the frame; ``fall_nits`` = the mean of max(R, G, B).

Output: ``{"frames": [{"i", "max_cll_nits", "fall_nits"}], "pooled": {"max_cll_nits": {"max"},
"fall_nits": {"max"}}}`` (the sequence MaxCLL and MaxFALL). §14.4.15 shows the per-frame pair as a flat
``{"max_cll_nits", "fall_nits"}`` object; it is a ``scores`` frame here so that every normaliser output has
a family (acceptance test 3). Needs numpy (oracle image and ``tools/requirements-test.txt``).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

KR, KB = 0.2627, 0.0593
KG = 1 - KR - KB
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
SUBSAMPLING = {"420": (2, 2), "422": (2, 1), "444": (1, 1)}


def pq_eotf(e: np.ndarray) -> np.ndarray:
    p = np.power(e, 1 / M2)
    return 10000.0 * np.power(np.maximum(p - C1, 0) / (C2 - C3 * p), 1 / M1)


def parse_header(line: bytes) -> dict[str, str]:
    tokens = line.decode("ascii", "replace").split()
    if not tokens or tokens[0] != "YUV4MPEG2":
        raise canon.NormalizeError("not a Y4M file")
    params: dict[str, str] = {}
    for tok in tokens[1:]:
        if tok[0] == "X" and "=" in tok:
            key, _, value = tok[1:].partition("=")
            params[key] = value
        else:
            params[tok[0]] = tok[1:]
    return params


def frame_nits(planes: list[np.ndarray], depth: int, full: bool, sub: tuple[int, int]) -> tuple[float, float]:
    scale = float(1 << (depth - 8))
    y, cb, cr = (p.astype(np.float64) for p in planes)
    if full:
        top = float((1 << depth) - 1)
        y, cb, cr = y / top, (cb - (1 << (depth - 1))) / top, (cr - (1 << (depth - 1))) / top
    else:
        y, cb, cr = (
            (y - 16 * scale) / (219 * scale),
            (cb - 128 * scale) / (224 * scale),
            (cr - 128 * scale) / (224 * scale),
        )
    cb = np.repeat(np.repeat(cb, sub[1], axis=0), sub[0], axis=1)[: y.shape[0], : y.shape[1]]
    cr = np.repeat(np.repeat(cr, sub[1], axis=0), sub[0], axis=1)[: y.shape[0], : y.shape[1]]
    r = y + 2 * (1 - KR) * cr
    g = y - (2 * KB * (1 - KB) / KG) * cb - (2 * KR * (1 - KR) / KG) * cr
    b = y + 2 * (1 - KB) * cb
    peak = pq_eotf(np.clip(np.maximum(np.maximum(r, g), b), 0.0, 1.0))  # the EOTF is monotonic
    return float(peak.max()), float(peak.mean())


def convert(args: argparse.Namespace) -> dict[str, Any]:
    data = sys.stdin.buffer.read() if args.input == "-" else Path(args.input).read_bytes()
    end = data.find(b"\n")
    params = parse_header(data[: max(end, 0)])
    try:
        width, height = int(params["W"]), int(params["H"])
    except (KeyError, ValueError) as exc:
        raise canon.NormalizeError("Y4M header without W/H") from exc
    cs = params.get("C", "420jpeg")
    fmt, _, bits = cs.partition("p")
    fmt = fmt[:3]
    if fmt not in SUBSAMPLING or (bits and not bits.isdigit()):
        raise canon.NormalizeError(f"unsupported Y4M colour space C{cs}")
    depth = int(bits) if bits else 8
    sub = SUBSAMPLING[fmt]
    full = args.full_range or params.get("COLORRANGE") == "FULL"
    cw, ch = -(-width // sub[0]), -(-height // sub[1])
    dtype = np.dtype("<u2") if depth > 8 else np.dtype("u1")
    sizes = [width * height, cw * ch, cw * ch]
    frame_bytes = sum(sizes) * dtype.itemsize
    frames, pos, index = [], end + 1, 0
    while pos < len(data):
        nl = data.find(b"\n", pos)
        if nl < 0 or not data[pos:nl].startswith(b"FRAME"):
            raise canon.NormalizeError(f"frame {index}: missing FRAME header")
        pos = nl + 1
        if pos + frame_bytes > len(data):
            raise canon.NormalizeError(f"frame {index}: truncated")
        if args.frame is None or args.frame == index:
            raw = np.frombuffer(data, dtype=dtype, count=sum(sizes), offset=pos)
            y = raw[: sizes[0]].reshape(height, width)
            cb = raw[sizes[0] : sizes[0] + sizes[1]].reshape(ch, cw)
            cr = raw[sizes[0] + sizes[1] :].reshape(ch, cw)
            cll, fall = frame_nits([y, cb, cr], depth, full, sub)
            frames.append({"i": index, "max_cll_nits": cll, "fall_nits": fall})
        pos += frame_bytes
        index += 1
    if not frames:
        raise canon.NormalizeError("no frames" if args.frame is None else f"no frame {args.frame}")
    pooled = {k: {"max": max(f[k] for f in frames)} for k in ("max_cll_nits", "fall_nits")}
    return {"frames": frames, "pooled": pooled}


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--frame", type=int, help="only this frame (0-based)")
    parser.add_argument("--full-range", action="store_true", help="treat samples as full range")


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert, add_args))
