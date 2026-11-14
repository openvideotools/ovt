#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""x264 SSIM oracle runner (chapter 9 §9.4.4, WI-3.308): per-frame Y/U/V of the vendored x264 kernel.

    run_x264_ssim.py netflix --golden X.x264ssim.json.gz -o OUT.json
    run_x264_ssim.py synth --oracle PATH_TO_x264_ssim -o OUT.json

``netflix`` copies the per-frame ``ssim_x264_{y,u,v}`` out of a tests/golden/metrics ``*.x264ssim.json.gz`` file (the
goldens-metrics workflow of WI-0.565 ran ``x264_ssim`` on the Netflix pair); the output is the golden
core/tests/metrics/golden/ssim_x264_netflix_src01_576x324.json.

``synth`` writes the small deterministic Y4M pairs of ``CASES``, runs the ``x264_ssim`` executable
(tests/oracles/x264_ssim, built with ``cmake -S tests/oracles/x264_ssim -B build/x264_ssim``) on each and writes
core/tests/metrics/golden/ssim_x264_synth.json. The pairs cover the 8-bit integer path and the 10-bit float path, a
plane whose block count is odd and sizes that are not a multiple of 4 after the 2-sample offset. The sample generator
is ``synth_luma`` of run_libvmaf_ssim.py, applied to every plane (chroma with the frame number + 16 * plane as its
seed); core/tests/metrics/test_metrics_ssim_x264.cpp mirrors it sample for sample (``synth_plane``).
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_libvmaf_ssim import synth_luma  # noqa: E402

# name, width, height, bit depth, frames
CASES = [
    ("8bit_342x226", 342, 226, 8, 2),
    ("8bit_330x194", 330, 194, 8, 2),
    ("10bit_330x194", 330, 194, 10, 2),
    ("10bit_342x226", 342, 226, 10, 2),
]

LINE_RE = re.compile(r"^n:(\d+) Y:(\S+) U:(\S+) V:(\S+)\s*$")


def write_y4m(path: Path, w: int, h: int, bits: int, frames: int, distorted: bool) -> None:
    cw, ch = (w + 1) // 2, (h + 1) // 2
    colour = "420jpeg" if bits == 8 else "420p10"
    with path.open("wb") as out:
        out.write(f"YUV4MPEG2 W{w} H{h} F25:1 Ip A1:1 C{colour}\n".encode())
        for f in range(frames):
            out.write(b"FRAME\n")
            for p, (pw, ph) in enumerate([(w, h), (cw, ch), (cw, ch)]):
                out.write(synth_luma(pw, ph, bits, f + 16 * p, distorted).tobytes())


def run_oracle(exe: str, ref: Path, dist: Path) -> dict[str, list[float]]:
    res = subprocess.run([exe, str(ref), str(dist)], check=True, capture_output=True, text=True)
    out: dict[str, list[float]] = {"y": [], "u": [], "v": []}
    for line in res.stdout.splitlines():
        m = LINE_RE.match(line)
        if m:
            for plane, text in zip("yuv", m.groups()[1:]):
                out[plane].append(float(text))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    n = sub.add_parser("netflix")
    n.add_argument("--golden", type=Path, required=True)
    n.add_argument("-o", "--output", type=Path, required=True)
    s = sub.add_parser("synth")
    s.add_argument("--oracle", default="x264_ssim")
    s.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    if args.mode == "netflix":
        with gzip.open(args.golden, "rt") as fh:
            frames = json.load(fh)["frames"]
        doc = {"oracle": "x264_ssim on x264 r3191"}
        for plane in "yuv":
            doc[plane] = [fr[f"ssim_x264_{plane}"] for fr in frames]
    else:
        cases = []
        with tempfile.TemporaryDirectory() as tmp:
            for name, w, h, bits, nframes in CASES:
                ref, dist = Path(tmp) / "ref.y4m", Path(tmp) / "dist.y4m"
                write_y4m(ref, w, h, bits, nframes, False)
                write_y4m(dist, w, h, bits, nframes, True)
                scores = run_oracle(args.oracle, ref, dist)
                cases.append({"name": name, "width": w, "height": h, "bit_depth": bits, **scores})
        doc = {"oracle": "x264_ssim on x264 r3191", "cases": cases}
    args.output.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
