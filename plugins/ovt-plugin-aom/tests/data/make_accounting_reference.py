#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-2-Clause
"""Writes accounting_reference.json, the inspect oracle data of test_aom_accounting.cpp (WI-2.157, chapter 07 §7.6.1).

For each stream: the ``symbolsMap`` and ``symbols`` arrays that libaom's ``examples/inspect -a`` (v3.12.1, built with
CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1 by tests/oracles/build_libaom_inspect.sh or OVT_BUILD_AOM_INSPECT=ON) prints
for the first frames with tile data, verbatim: ``symbols`` mixes context markers ``[x, y]`` and records
``[id, bits, samples]``. inspect.c prints the marker *instead of* the first record at each new context, so the test
rebuilds this array from its own full record list before it compares totals (spec question #408).

    make_accounting_reference.py --inspect <inspect> --out accounting_reference.json \\
        aom_av1_1_b8_01_size_16x16=<path>:8 aom_av1_1_b8_01_size_196x196=<path>:2 aom_av1_1_b8_22_svc_L1T2=<path>:2

``<corpus id>=<ivf path>:<frames>``.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


def inspect_frames(inspect: str, ivf: Path, limit: int) -> list[dict[str, Any]]:
    text = subprocess.run([inspect, str(ivf), "-a"], check=True, capture_output=True, text=True).stdout
    frames = [f for f in json.loads(text) if f is not None]
    return [
        {
            "frame_type": f["frameType"],
            "show_frame": f["showFrame"],
            "symbolsMap": f["symbolsMap"],
            "symbols": f["symbols"],
        }
        for f in frames[:limit]
    ]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inspect", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("streams", nargs="+", help="<corpus id>=<ivf path>:<frames>")
    a = ap.parse_args()
    doc: dict[str, Any] = {
        "generator": "plugins/ovt-plugin-aom/tests/data/make_accounting_reference.py",
        "oracle": "libaom v3.12.1 examples/inspect -a (CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1)",
        "streams": {},
    }
    for spec in a.streams:
        corpus, rest = spec.split("=", 1)
        path, limit = rest.rsplit(":", 1)
        doc["streams"][corpus] = inspect_frames(a.inspect, Path(path), int(limit))
    a.out.write_text(json.dumps(doc, separators=(",", ":"), sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
