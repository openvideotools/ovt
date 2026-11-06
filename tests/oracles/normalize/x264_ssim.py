#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""``x264_ssim`` kernel oracle output -> ``scores`` (chapter 14 §14.4.12, WI-0.555).

The oracle program (``tests/oracles/x264_ssim/main.c``, WI-3.308) prints one line per frame,
``n:<frame> Y:<ssim> U:<ssim> V:<ssim>`` with ``%.9f``. Each line becomes ``{"i", "ssim_x264_y",
"ssim_x264_u", "ssim_x264_v"}`` (metric id ``ssim_x264`` of chapter 9 §9.4.4 and its values ``y/u/v``).
``i`` counts lines from 0; the printed counters must go up by one, whatever their base. Lines that do not
start with ``n:`` (a version banner, a mean) are ignored.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

LINE_RE = re.compile(r"^n:(\d+) Y:(\S+) U:(\S+) V:(\S+)\s*$")


def convert(args: argparse.Namespace) -> dict[str, Any]:
    frames: list[dict[str, Any]] = []
    numbers: list[int] = []
    for n, line in enumerate(canon.read_text(args.input).splitlines(), start=1):
        if not line.startswith("n:"):
            continue
        m = LINE_RE.match(line)
        if not m:
            raise canon.NormalizeError(f"line {n}: not an x264_ssim line: {line!r}")
        numbers.append(int(m.group(1)))
        row: dict[str, Any] = {"i": len(frames)}
        for plane, text in zip("yuv", m.groups()[1:]):
            row[f"ssim_x264_{plane}"] = canon.to_score(text, f"line {n} {plane}")
        frames.append(row)
    if not frames:
        raise canon.NormalizeError("no n:<frame> lines")
    canon.numbered_frames(numbers, "x264_ssim")
    return {"frames": frames}


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert))
