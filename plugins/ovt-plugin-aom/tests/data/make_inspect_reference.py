#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-2-Clause
"""Writes inspect_reference.json, the oracle data of test_aom_map_mi.cpp (WI-2.156, chapter 07 §7.6.1).

For each stream: the per-MI layers of libaom's ``examples/inspect`` (v3.12.1, built with CONFIG_INSPECTION=1 by
tests/oracles/build_libaom_inspect.sh) for the first frames with tile data, run-length encoded, plus the
``ref_frame_idx[]`` of those frames from FFmpeg's ``trace_headers`` bitstream filter (an independent parser).

    make_inspect_reference.py --inspect <inspect> --ffmpeg <ffmpeg> --out inspect_reference.json \\
        aom_av1_1_b8_02_allintra=<path>:1 aom_av1_1_b8_05_mv=<path>:4 syn_av1_tiles_cdef_lr=<path>:3

``<corpus id>=<ivf path>:<frames>``. Layers are lists of rows of MI values (mi_rows x mi_cols, padding included);
a layer with array elements (referenceFrame, motionVectors) is split into one layer per element:
``referenceFrame[0]``, ``motionVectors[0]`` (= mv[0].col) … ``motionVectors[3]`` (= mv[1].row). Each layer is
stored flattened row-major and run-length encoded as ``[value, count, value, count, …]``.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path
from typing import Any

LAYERS = {
    "blockSize": "-bs",
    "mode": "-m",
    "uv_mode": "-uvm",
    "skip": "-s",
    "transformSize": "-ts",
    "referenceFrame": "-r",
    "motionVectors": "-mv",
    "delta_q": "-dq",
}


def rle(values: list[int]) -> list[int]:
    out: list[int] = []
    for v in values:
        if out and out[-2] == v:
            out[-1] += 1
        else:
            out += [v, 1]
    return out


def inspect_frames(inspect: str, ivf: Path, limit: int) -> list[dict[str, Any]]:
    args = [inspect, str(ivf)] + list(LAYERS.values())
    text = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    frames = [f for f in json.loads(text) if f is not None]
    out = []
    for f in frames[:limit]:
        layers: dict[str, list[int]] = {}
        for name in LAYERS:
            rows = f[name]
            first = rows[0][0]
            if isinstance(first, list):
                for k in range(len(first)):
                    layers[f"{name}[{k}]"] = rle([v[k] for row in rows for v in row])
            else:
                layers[name] = rle([v for row in rows for v in row])
        out.append(
            {
                "frame_type": f["frameType"],
                "show_frame": f["showFrame"],
                "mi_rows": len(f["mode"]),
                "mi_cols": len(f["mode"][0]),
                "layers": layers,
            }
        )
    return out


def ref_frame_idx(ffmpeg: str, ivf: Path) -> list[list[int]]:
    """ref_frame_idx[0..6] of every frame header with show_existing_frame = 0, in decode order ([] for intra)."""
    args = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "trace",
        "-i",
        str(ivf),
        "-c",
        "copy",
        "-bsf:v",
        "trace_headers",
        "-f",
        "null",
        "-",
    ]
    log = subprocess.run(args, check=True, capture_output=True, text=True).stderr
    headers: list[list[int]] = []
    current: list[int] | None = None
    for line in log.splitlines():
        m = re.search(r"\]\s+\d+\s+(\S+)\s+\S+ = (-?\d+)$", line)
        if not m:
            continue
        name, value = m.group(1), int(m.group(2))
        if name == "show_existing_frame":
            if current is not None:
                headers.append(current)
            current = None if value == 1 else []
        elif name.startswith("ref_frame_idx[") and current is not None:
            current.append(value)
    if current is not None:
        headers.append(current)
    return headers


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inspect", required=True)
    ap.add_argument("--ffmpeg", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("streams", nargs="+", help="<corpus id>=<ivf path>:<frames>")
    a = ap.parse_args()
    doc: dict[str, Any] = {
        "generator": "plugins/ovt-plugin-aom/tests/data/make_inspect_reference.py",
        "oracle": "libaom v3.12.1 examples/inspect (CONFIG_INSPECTION=1); FFmpeg trace_headers for ref_frame_idx",
        "streams": {},
    }
    for spec in a.streams:
        corpus, rest = spec.split("=", 1)
        path, limit = rest.rsplit(":", 1)
        frames = inspect_frames(a.inspect, Path(path), int(limit))
        refs = ref_frame_idx(a.ffmpeg, Path(path))
        for f, r in zip(frames, refs):
            f["ref_frame_idx"] = r
        doc["streams"][corpus] = frames
    a.out.write_text(json.dumps(doc, separators=(",", ":"), sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
