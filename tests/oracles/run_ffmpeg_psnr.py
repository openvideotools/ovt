#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""PSNR oracle runner (chapter 9 §9.4.2, WI-3.306): per-frame PSNR of a distorted/reference pair from FFmpeg's
``psnr`` filter, merged with libvmaf's ``psnr_y/psnr_cb/psnr_cr`` taken from a goldens-metrics JSON file.

    run_ffmpeg_psnr.py --ref REF.y4m --dist DIST.y4m [--libvmaf-golden X.vmaf.json.gz] [--ffmpeg PATH] -o OUT.json

OUT.json: ``{"ffmpeg_version": str, "frames": [{"i": n, "ffmpeg": {psnr_y, psnr_u, psnr_v, psnr_avg, mse_y, mse_u,
mse_v, mse_avg}, "libvmaf": {psnr_y, psnr_cb, psnr_cr}}]}``. FFmpeg prints 2 decimals and ``inf`` for identical
frames; ``inf`` is kept as the string ``"inf"`` (the kernel reports psnr_max there, §9.4.2). The output is the golden
core/tests/metrics/golden/psnr_netflix_src01_576x324.json. The checked-in copy was made with FFmpeg 6.1.1; the tests
compare its 2-decimal values, which the oracle image of tests/oracles/ORACLE_VERSIONS.md reproduces.
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


def ffmpeg_psnr(ffmpeg: str, ref: Path, dist: Path) -> tuple[str, list[dict[str, object]]]:
    with tempfile.TemporaryDirectory() as tmp:
        stats = Path(tmp) / "stats.log"
        # input 0 = distorted ("main"), input 1 = reference, as the psnr filter documents.
        cmd = [
            ffmpeg,
            "-nostdin",
            "-v",
            "error",
            "-i",
            str(dist),
            "-i",
            str(ref),
            "-lavfi",
            f"[0:v][1:v]psnr=stats_file={stats}",
            "-f",
            "null",
            "-",
        ]
        subprocess.run(cmd, check=True)
        version = subprocess.run([ffmpeg, "-version"], check=True, capture_output=True, text=True).stdout.splitlines()[
            0
        ]
        frames = []
        for line in stats.read_text().splitlines():
            kv = dict(item.split(":", 1) for item in line.split())
            row: dict[str, object] = {}
            for key in ("mse_avg", "mse_y", "mse_u", "mse_v", "psnr_avg", "psnr_y", "psnr_u", "psnr_v"):
                row[key] = kv[key] if kv[key] == "inf" else float(kv[key])
            frames.append(row)
        return version, frames


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--ref", type=Path, required=True)
    ap.add_argument("--dist", type=Path, required=True)
    ap.add_argument("--libvmaf-golden", type=Path)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args()

    version, ff = ffmpeg_psnr(args.ffmpeg, args.ref, args.dist)
    lv: list[dict[str, float]] = []
    if args.libvmaf_golden:
        opener = gzip.open if args.libvmaf_golden.suffix == ".gz" else open
        with opener(args.libvmaf_golden, "rt") as fh:
            for f in json.load(fh)["frames"]:
                lv.append({k: f[k] for k in ("psnr_y", "psnr_cb", "psnr_cr")})
        if len(lv) != len(ff):
            print(f"frame count mismatch: ffmpeg {len(ff)}, libvmaf {len(lv)}", file=sys.stderr)
            return 1
    out = {"ffmpeg_version": re.sub(r"\s+Copyright.*", "", version), "frames": []}
    for i, row in enumerate(ff):
        entry: dict[str, object] = {"i": i, "ffmpeg": row}
        if lv:
            entry["libvmaf"] = lv[i]
        out["frames"].append(entry)
    args.output.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
