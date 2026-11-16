#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-2-Clause
"""Writes inspect_ext_reference.json, the oracle data of test_aom_map_mi_ext.cpp (WI-2.159, chapter 07 §7.6.1).

For each stream: the extension layers of libaom's ``examples/inspect`` built with extension patch 0001
(plugins/ovt-plugin-aom/patches, libaom v3.12.1, CONFIG_INSPECTION=1; ``-DOVT_BUILD_AOM_INSPECT=ON`` installs it
as ``<OVT_THIRD_PARTY_PREFIX>/libaom/bin/inspect``) for every frame with tile data, run-length encoded.

    make_inspect_ext_reference.py --inspect <inspect> --out inspect_ext_reference.json \\
        aom_av1_1_b8_23_film_grain_50=<path> aom_av1_1_b8_05_mv=<path>

Layers (inspect flag): ``mode`` (-m), ``angle_delta[0..1]`` (-ad), ``motion_mode`` (-mm), ``wm_type`` and
``wm_params[0..5]`` (-wm), ``mvd[0..3]`` (-mvd: ref 0 row, col, ref 1 row, col), ``transformType`` (-tt) and
``cdef_idx`` (-cdi). A layer of arrays is split into one layer per element. Each layer is stored flattened
row-major (mi_rows x mi_cols, padding included) and run-length encoded as ``[value, count, value, count, …]``.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any

FLAGS = ["-m", "-ad", "-mm", "-wm", "-mvd", "-tt", "-cdi"]
LAYERS = ["mode", "angle_delta", "motion_mode", "wm_type", "wm_params", "mvd", "transformType", "cdef_idx"]


def rle(values: list[int]) -> list[int]:
    out: list[int] = []
    for v in values:
        if out and out[-2] == v:
            out[-1] += 1
        else:
            out += [v, 1]
    return out


def inspect_frames(inspect: str, ivf: Path) -> list[dict[str, Any]]:
    text = subprocess.run([inspect, str(ivf), *FLAGS], check=True, capture_output=True, text=True).stdout
    out = []
    for f in (f for f in json.loads(text) if f is not None):
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inspect", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("streams", nargs="+", help="<corpus id>=<ivf path>")
    a = ap.parse_args()
    doc: dict[str, Any] = {
        "generator": "plugins/ovt-plugin-aom/tests/data/make_inspect_ext_reference.py",
        "oracle": "libaom v3.12.1 examples/inspect with plugins/ovt-plugin-aom/patches/0001 (CONFIG_INSPECTION=1)",
        "streams": {},
    }
    for spec in a.streams:
        corpus, path = spec.split("=", 1)
        doc["streams"][corpus] = inspect_frames(a.inspect, Path(path))
    a.out.write_text(json.dumps(doc, separators=(",", ":"), sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
