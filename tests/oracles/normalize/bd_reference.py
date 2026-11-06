#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""RD point set -> BD-rate / BD-metric from the FAU-LMS ``bjontegaard`` package, as ``scores`` (§14.4.13).

Input (``tests/golden/metrics/rd_points/<set>.json``): ``{"metric": "psnr_y", "anchor": {"rate": [...],
"dist": [...]}, "test": {"rate": [...], "dist": [...]}}`` with rates in kbit/s (any unit, the same for both
curves). For every interpolator the script calls ``bjontegaard.bd_rate`` and ``bjontegaard.bd_psnr`` (the
package's name for the distortion delta of any metric) with the package defaults and writes
``{"frames": [], "pooled": {"bd_rate": {"<method>": percent}, "bd_<metric>": {"<method>": delta}}}``.

``cubic`` (the third-order fit) is computed only when both curves have exactly 4 points (§14.4.13; see
SPEC-ASSUMPTION(#142): the package itself fits 6 points without an error). A method whose result is NaN (the
package's answer for curves that do not overlap) is left out. The package is pinned in
``tests/oracles/requirements.txt`` and present only in the oracle image; without it the script exits 3.
"""

from __future__ import annotations

import argparse
import importlib
import math
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

METHODS = ("cubic", "pchip", "akima")


def curve(doc: dict[str, Any], name: str) -> tuple[list[float], list[float]]:
    c = doc.get(name)
    if not isinstance(c, dict):
        raise canon.NormalizeError(f"{name}: missing")
    rate, dist = c.get("rate"), c.get("dist")
    if not isinstance(rate, list) or not isinstance(dist, list) or len(rate) != len(dist) or len(rate) < 2:
        raise canon.NormalizeError(f"{name}: rate and dist must be lists of the same length (>= 2)")
    if not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in rate + dist):
        raise canon.NormalizeError(f"{name}: non-numeric value")
    if min(rate) <= 0:
        raise canon.NormalizeError(f"{name}: rates must be positive")
    return [float(v) for v in rate], [float(v) for v in dist]


def convert(args: argparse.Namespace) -> dict[str, Any]:
    doc = canon.read_json(args.input)
    if not isinstance(doc, dict) or not isinstance(doc.get("metric"), str):
        raise canon.NormalizeError('expected {"metric": ..., "anchor": {...}, "test": {...}}')
    (ra, da), (rt, dt) = curve(doc, "anchor"), curve(doc, "test")
    try:
        bd = importlib.import_module("bjontegaard")
    except ImportError as exc:
        raise canon.NormalizeError("the bjontegaard package is not installed (tests/oracles/requirements.txt)") from exc
    rates: dict[str, float] = {}
    deltas: dict[str, float] = {}
    for method in METHODS:
        if method == "cubic" and not len(ra) == len(rt) == 4:
            continue
        try:
            rate = float(bd.bd_rate(ra, da, rt, dt, method=method))
            delta = float(bd.bd_psnr(ra, da, rt, dt, method=method))
        except ValueError as exc:
            raise canon.NormalizeError(f"bjontegaard ({method}): {exc}") from exc
        if not math.isnan(rate):
            rates[method] = rate
        if not math.isnan(delta):
            deltas[method] = delta
    return {"frames": [], "pooled": {"bd_rate": rates, f"bd_{doc['metric']}": deltas}}


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert))
