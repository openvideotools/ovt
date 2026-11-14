#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""SSIM oracle runner (chapter 9 §9.4.3, WI-3.307): libvmaf's ``float_ssim`` per frame.

    run_libvmaf_ssim.py netflix --golden X.vmaf.json.gz -o OUT.json
    run_libvmaf_ssim.py synth --vmaf PATH_TO_VMAF_CLI -o OUT.json

``netflix`` copies ``float_ssim`` out of a tests/golden/metrics ``*.vmaf.json.gz`` file (the goldens-metrics
workflow of WI-0.565 ran the libvmaf CLI v3.0.0 on the Netflix pair); the output is the golden
core/tests/metrics/golden/ssim_netflix_src01_576x324.json.

``synth`` writes the small deterministic pairs of ``CASES`` as raw YUV 4:2:0 files, runs the ``vmaf`` CLI of libvmaf
v3.0.0 with ``--feature float_ssim`` on each and writes core/tests/metrics/golden/ssim_synth_libvmaf.json. The pairs
exercise libvmaf's down-scaling (``f = max(1, round(min(W, H) / 256))``: 2 and 3, odd sizes) and 10-bit input. The
sample generator below is mirrored sample for sample by core/tests/metrics/test_metrics_ssim.cpp (``synth_sample``).
"""

from __future__ import annotations

import argparse
import gzip
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

# name, width, height, bit depth, frames
CASES = [
    ("scale2_640x384_8bit", 640, 384, 8, 2),
    ("scale3_705x701_8bit", 705, 701, 8, 2),
    ("scale2_640x384_10bit", 640, 384, 10, 2),
]


def _hash(x: np.ndarray, y: np.ndarray, f: int, s: int) -> np.ndarray:
    """32-bit integer hash of (x, y, frame, stream); C++ ``synth_hash`` does the same with uint32 arithmetic."""
    m = np.uint64(0xFFFFFFFF)
    v = (x.astype(np.uint64) * np.uint64(73856093)) & m
    v ^= (y.astype(np.uint64) * np.uint64(19349663)) & m
    v ^= (np.uint64(f) * np.uint64(83492791)) & m
    v ^= (np.uint64(s) * np.uint64(2654435769)) & m
    v = (v * np.uint64(2246822519)) & m
    v ^= v >> np.uint64(15)
    v = (v * np.uint64(3266489917)) & m
    v ^= v >> np.uint64(13)
    return v


def synth_luma(w: int, h: int, bits: int, frame: int, distorted: bool) -> np.ndarray:
    """ref = 40 + ((3x + 2y + 7f) % 128) + noise(0..31); dist = ref + noise(-8..7) + 6 on 4x4 checker cells.
    At 10 bits both are scaled by 4 and get two more random low bits."""
    yy, xx = np.mgrid[0:h, 0:w]
    base = 40 + ((3 * xx + 2 * yy + 7 * frame) % 128) + (_hash(xx, yy, frame, 1) >> np.uint64(27)).astype(np.int64)
    if distorted:
        cells = ((xx // 4 + yy // 4 + frame) & 1) * 6
        base = base + (_hash(xx, yy, frame, 2) >> np.uint64(28)).astype(np.int64) - 8 + cells
    if bits == 10:
        low = (_hash(xx, yy, frame, 4 if distorted else 3) >> np.uint64(30)).astype(np.int64)
        return (base * 4 + low).astype(np.uint16)
    return base.astype(np.uint8)


def write_yuv(path: Path, w: int, h: int, bits: int, frames: int, distorted: bool) -> None:
    cw, ch = (w + 1) // 2, (h + 1) // 2
    with path.open("wb") as out:
        for f in range(frames):
            out.write(synth_luma(w, h, bits, f, distorted).tobytes())
            mid = 128 if bits == 8 else 512
            dtype = np.uint8 if bits == 8 else np.uint16
            out.write(np.full(2 * cw * ch, mid, dtype=dtype).tobytes())


def run_vmaf(vmaf: str, ref: Path, dist: Path, w: int, h: int, bits: int, out_json: Path) -> list[float]:
    cmd = [vmaf, "--reference", str(ref), "--distorted", str(dist), "--width", str(w), "--height", str(h)]
    cmd += ["--pixel_format", "420", "--bitdepth", str(bits), "--feature", "float_ssim", "--json"]
    cmd += ["--output", str(out_json), "--quiet"]
    subprocess.run(cmd, check=True)
    frames = json.loads(out_json.read_text())["frames"]
    return [float(fr["metrics"]["float_ssim"]) for fr in frames]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    n = sub.add_parser("netflix")
    n.add_argument("--golden", type=Path, required=True)
    n.add_argument("-o", "--output", type=Path, required=True)
    s = sub.add_parser("synth")
    s.add_argument("--vmaf", default="vmaf")
    s.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    if args.mode == "netflix":
        with gzip.open(args.golden, "rt") as fh:
            frames = json.load(fh)["frames"]
        doc = {"libvmaf": "3.0.0", "float_ssim": [fr["float_ssim"] for fr in frames]}
    else:
        cases = []
        with tempfile.TemporaryDirectory() as tmp:
            for name, w, h, bits, nframes in CASES:
                ref, dist = Path(tmp) / "ref.yuv", Path(tmp) / "dist.yuv"
                write_yuv(ref, w, h, bits, nframes, False)
                write_yuv(dist, w, h, bits, nframes, True)
                scores = run_vmaf(args.vmaf, ref, dist, w, h, bits, Path(tmp) / "out.json")
                cases.append({"name": name, "width": w, "height": h, "bit_depth": bits, "float_ssim": scores})
        doc = {"libvmaf": "3.0.0", "cases": cases}
    args.output.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
