#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""``hm_cu_dump`` JSON -> ``block-grid`` (chapter 14 §14.4.4, WI-0.555).

``hm_cu_dump`` (WI-0.563) walks every CTU's ``TComDataCU`` after each picture and prints, per 4x4 unit in
raster order, the raw HM getter values: ``{"frames": [{"poc", "w4", "h4", "log2_ctu_size",
"cells": {"<getter>": [int per cell]}}]}`` (getter keys as in ``hm_grid_map.json``, one list per
component/list, e.g. ``getMv_L0_x``, ``getCbf_Cb``). This script applies the §14.4.4 mapping to the OVT
``CellGrid`` fields ``pred, qp, intra_mode_y, intra_mode_c, mv0_x, mv0_y, mv1_x, mv1_y, ref0, ref1,
tx_log2_w, tx_log2_h, cbf``:

* ``pred`` (``PredKind``): ``getIPCMFlag`` -> PCM 5, ``getSkipFlag`` -> Skip 3, ``MODE_INTRA`` (1) ->
  Intra 1, ``MODE_INTER`` (0) -> Inter 2.
* ``intra_mode_y/c`` = ``getIntraDir`` (identity) for intra cells, -1 otherwise.
* MVs ×4 (1/4 -> 1/16 luma sample), 0 where the list is unused; ``ref0/ref1`` = HM ``refIdx`` (-1 none).
  The comparer maps OVT's ``ref0`` through ``FrameAnalysis.refs[i].name == "L0[refIdx]"``.
* ``tx_log2_w = tx_log2_h = log2_ctu_size − getDepth − getTransformIdx`` (square HEVC transforms).
* ``cbf`` = ``getCbf_Y | getCbf_Cb << 1 | getCbf_Cr << 2`` (each 0/1 at the TU leaf).

Frames keep the dump's order (decode order); ``frame`` is the 0-based position.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

# SPEC-ASSUMPTION(#93): hm_cu_dump prints raw getter values; the HM -> CellGrid mapping lives here.
MODE_INTRA = 1


def convert_frame(index: int, src: dict[str, Any], hm_map: dict[str, Any]) -> dict[str, Any]:
    try:
        w4, h4, log2_ctu, cells = src["w4"], src["h4"], src["log2_ctu_size"], src["cells"]
    except KeyError as exc:
        raise canon.NormalizeError(f"frame {index}: missing key {exc}") from exc
    allowed = set(hm_map["copy"]) | set(hm_map["mv_quarter"]) | set(hm_map["derived_from"]) | set(hm_map["ignored"])
    unknown = set(cells) - allowed
    if unknown:
        raise canon.NormalizeError(f"frame {index}: getters not in hm_grid_map.json: {sorted(unknown)}")
    missing = (set(allowed) - set(hm_map["ignored"])) - set(cells)
    if missing:
        raise canon.NormalizeError(f"frame {index}: missing getters {sorted(missing)}")
    n = w4 * h4
    for key, values in cells.items():
        if len(values) != n:
            raise canon.NormalizeError(f"frame {index}: {key} has {len(values)} cells, expected {n}")

    def col(key: str) -> list[int]:
        return [int(v) for v in cells[key]]

    fields: dict[str, list[int]] = {out: col(key) for key, out in hm_map["copy"].items()}
    for key, (out, ref) in hm_map["mv_quarter"].items():
        fields[out] = [4 * v if r >= 0 else 0 for v, r in zip(col(key), col(ref))]
    pred = []
    for mode, skip, pcm in zip(col("getPredictionMode"), col("getSkipFlag"), col("getIPCMFlag")):
        pred.append(5 if pcm else 3 if skip else 1 if mode == MODE_INTRA else 2)
    fields["pred"] = pred
    for key, out in (("getIntraDir_luma", "intra_mode_y"), ("getIntraDir_chroma", "intra_mode_c")):
        fields[out] = [v if p == 1 else -1 for v, p in zip(col(key), pred)]
    tx = [log2_ctu - d - t for d, t in zip(col("getDepth"), col("getTransformIdx"))]
    fields["tx_log2_w"] = tx
    fields["tx_log2_h"] = list(tx)
    fields["cbf"] = [y | cb << 1 | cr << 2 for y, cb, cr in zip(col("getCbf_Y"), col("getCbf_Cb"), col("getCbf_Cr"))]
    result: dict[str, Any] = {"frame": index, "w4": w4, "h4": h4, "fields": fields}
    if "poc" in src:
        result["poc"] = src["poc"]
    return result


def convert(args: argparse.Namespace) -> dict[str, Any]:
    doc = canon.read_json(args.input)
    if not isinstance(doc, dict) or not isinstance(doc.get("frames"), list):
        raise canon.NormalizeError('expected {"frames": [...]} from hm_cu_dump')
    hm_map = canon.load_map("hm_grid_map")
    return {"frames": [convert_frame(i, f, hm_map) for i, f in enumerate(doc["frames"])]}


if __name__ == "__main__":
    sys.exit(canon.run("block-grid", __doc__.splitlines()[0], convert))
