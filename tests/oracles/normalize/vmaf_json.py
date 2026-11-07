#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""libvmaf JSON (``vmaf --json`` or OVT ``--format vmaf-json``) -> ``scores`` (chapter 14 §14.4.11, WI-0.555).

libvmaf (``libvmaf/src/output.c``) writes ``{"version", "fps", "frames": [{"frameNum": n, "metrics":
{name: value}}], "pooled_metrics": {name: {"min", "max", "mean", "harmonic_mean"}}, "aggregate_metrics":
{}}`` with every value as ``%.6f``, or ``null`` when the value is not finite. The output keeps the metric
names as libvmaf writes them (``vmaf``, ``psnr_y``, ``float_ssim``, ``integer_adm2``, ...):
``{"frames": [{"i": frameNum, name: value}], "pooled": {name: {method: value}}}``. ``version``, ``fps``
and ``aggregate_metrics`` are dropped (§14.4.11), as are ``null`` values: the same normaliser runs on
OVT's export, so a non-finite value is dropped on both sides. ``--subsample`` output keeps the
``frameNum`` of each written frame.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402


def number(value: Any, what: str) -> float | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise canon.NormalizeError(f"{what}: {value!r} is not a number")
    return float(value)


def scores_from_vmaf(doc: Any) -> dict[str, Any]:
    if not isinstance(doc, dict) or not isinstance(doc.get("frames"), list):
        raise canon.NormalizeError("not a libvmaf JSON document (no frames array)")
    frames = []
    seen: set[int] = set()
    for n, frame in enumerate(doc["frames"]):
        try:
            index, metrics = frame["frameNum"], frame["metrics"]
        except (KeyError, TypeError) as exc:
            raise canon.NormalizeError(f"frames[{n}]: missing {exc}") from exc
        if not isinstance(index, int) or index < 0 or index in seen or not isinstance(metrics, dict):
            raise canon.NormalizeError(f"frames[{n}]: bad or repeated frameNum {index!r}")
        seen.add(index)
        row: dict[str, Any] = {"i": index}
        for name, value in metrics.items():
            score = number(value, f"frame {index} {name}")
            if score is not None:
                row[name] = score
        frames.append(row)
    pooled: dict[str, dict[str, float]] = {}
    for name, methods in (doc.get("pooled_metrics") or {}).items():
        if not isinstance(methods, dict):
            raise canon.NormalizeError(f"pooled_metrics.{name} is not an object")
        values = {m: number(v, f"pooled {name}.{m}") for m, v in methods.items()}
        pooled[name] = {m: v for m, v in values.items() if v is not None}
    out: dict[str, Any] = {"frames": sorted(frames, key=lambda f: f["i"])}
    if pooled:
        out["pooled"] = pooled
    return out


def convert(args: argparse.Namespace) -> dict[str, Any]:
    return scores_from_vmaf(canon.read_json(args.input))


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert))
