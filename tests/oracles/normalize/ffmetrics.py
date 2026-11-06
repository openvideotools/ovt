#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""FFmpeg ``psnr`` stats file or ``libvmaf`` filter log -> ``scores`` (chapter 14 §14.4.10, WI-0.555).

* ``psnr=stats_file=``: one line per frame, ``n:<1-based> mse_avg:.. mse_y:.. .. psnr_avg:.. psnr_y:..``
  (``libavfilter/vf_psnr.c``; plane suffixes are ``y/u/v``, or ``r/g/b/a`` for RGB input; values with
  ``%0.2f``, ``inf`` for identical planes). Each line becomes ``{"i": n - 1, "<key>": value}`` for every
  ``key:value`` pair after ``n``. FFmpeg prints the averages to the log only, so there is no ``pooled``.
* ``libvmaf=log_fmt=json``: the libvmaf JSON of §14.4.11, normalised exactly as ``vmaf_json.py`` does.

The input kind is detected from its first non-blank character (``{`` = JSON).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402
import vmaf_json  # noqa: E402

PAIR_RE = re.compile(r"^([a-z_]+):(\S+)$")


def psnr_stats(text: str) -> dict[str, Any]:
    frames = []
    for n, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        pairs = [m.groups() for m in map(PAIR_RE.match, line.split()) if m]
        if len(pairs) != len(line.split()) or pairs[0][0] != "n":
            raise canon.NormalizeError(f"line {n}: not a psnr stats line: {line!r}")
        row: dict[str, Any] = {"i": canon.to_int(pairs[0][1], f"line {n} n") - 1}
        row.update((key, canon.to_score(value, f"line {n} {key}")) for key, value in pairs[1:])
        frames.append(row)
    if not frames or frames[0]["i"] < 0:
        raise canon.NormalizeError("no frames, or n < 1")
    canon.numbered_frames([f["i"] for f in frames], "psnr stats")
    return {"frames": frames}


def convert(args: argparse.Namespace) -> dict[str, Any]:
    text = canon.read_text(args.input)
    if text.lstrip().startswith("{"):
        return vmaf_json.scores_from_vmaf(canon.parse_json(text, args.input))
    return psnr_stats(text)


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert))
