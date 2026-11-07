#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write the dense ``FrameAnalysis`` perf inputs (§14.3.3 non-stream fixtures, §14.9.3; WI-0.553).

    gen_dense_fa.py [--size 4k|1080p] --out fa_4k_dense.bin

An AV1 inter frame (3840×2160 by default) in which every 4×4 cell is an inter ``Block`` with one MV, a qindex
ramp of 0..255 from left to right (``grid.qp``, ``Block.qp``, ``qp_hist``) and 4×4 transforms: the input of
``bench_overlay_4k_mv_arrows``, ``bench_overlay_build``, ``bench_validator`` and (at 1080p) ``bench_cache_write``.
The buffer is a plain FlatBuffer with file identifier ``OVTA``, built by ``gen_fixture_fb.build_mv``. Exit 0 ok,
2 usage.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from gen_fixture_fb import SIZES, build_mv, load_schema


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--size", choices=("4k", "1080p"), default="4k")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    args.out.write_bytes(build_mv(load_schema(), *SIZES[args.size], dense=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
