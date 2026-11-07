#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Daala ``dump_psnrhvs`` / ``dump_ciede2000`` output -> ``scores`` (chapter 14 §14.4.13, WI-0.555).

``--metric psnr_hvs`` reads ``tools/dump_psnrhvs.c``: per frame ``%08i: %-8G  (Y': %-8G  Cb: %-8G  Cr: %-8G)``,
or ``%08i: %8G`` with ``-y`` (luma only), then ``Total: ...`` in the same shape. Keys: ``psnr_hvs`` (the
weighted value), ``psnr_hvs_y``, ``psnr_hvs_cb``, ``psnr_hvs_cr`` (libvmaf's names, chapter 9 §9.5); with
``-y`` the single value is ``psnr_hvs_y``. ``Total`` goes to ``pooled.<key>.total``: Daala converts the
mean of the raw per-frame scores to dB, which is neither the mean nor the harmonic mean of the dB values.

``--metric ciede2000`` reads ``tools/dump_ciede2000.py``: per frame ``%08d: %2.4f`` and ``Total: %2.4f``
(the mean of the per-frame values) -> ``ciede2000`` and ``pooled.ciede2000.mean``. Its ``Opening ...``
lines are ignored.

``i`` counts frame lines from 0; the printed counters must go up by one, whatever their base.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

NUM = r"(\S+)"
FULL_RE = re.compile(rf"^(\d+|Total): +{NUM} +\(Y': +{NUM} +Cb: +{NUM} +Cr: +{NUM} *\)\s*$")
ONE_RE = re.compile(rf"^(\d+|Total): +{NUM}\s*$")
KEYS = {"psnr_hvs": ["psnr_hvs", "psnr_hvs_y", "psnr_hvs_cb", "psnr_hvs_cr"], "ciede2000": ["ciede2000"]}


def convert(args: argparse.Namespace) -> dict[str, Any]:
    frames: list[dict[str, Any]] = []
    numbers: list[int] = []
    pooled: dict[str, dict[str, float]] = {}
    pool = "mean" if args.metric == "ciede2000" else "total"
    for n, line in enumerate(canon.read_text(args.input).splitlines(), start=1):
        if not line.strip() or line.startswith("Opening "):
            continue
        m = FULL_RE.match(line) if args.metric == "psnr_hvs" else None
        if m:
            keys = KEYS["psnr_hvs"]
        else:
            m = ONE_RE.match(line)
            keys = ["psnr_hvs_y"] if args.metric == "psnr_hvs" else KEYS["ciede2000"]
        if not m:
            raise canon.NormalizeError(f"line {n}: not a {args.metric} line: {line!r}")
        values = {k: canon.to_score(v, f"line {n} {k}") for k, v in zip(keys, m.groups()[1:])}
        if m.group(1) == "Total":
            pooled.update({k: {pool: v} for k, v in values.items()})
        else:
            numbers.append(int(m.group(1)))
            frames.append({"i": len(frames), **values})
    if not frames:
        raise canon.NormalizeError("no frame lines")
    canon.numbered_frames(numbers, args.metric)
    out: dict[str, Any] = {"frames": frames}
    if pooled:
        out["pooled"] = pooled
    return out


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--metric", required=True, choices=sorted(KEYS), help="which Daala tool wrote the input")


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert, add_args))
