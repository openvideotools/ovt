#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""libaom ``inspect -A -a`` JSON -> ``block-grid`` plus accounting symbols (chapter 14 §14.4.5, WI-0.555).

Framing as ``examples/inspect.c`` writes it: ``[``, one object per decoded frame (hidden frames included,
``show_existing_frame`` outputs absent) each followed by ``,``, then ``null`` and ``]`` (chapter 6 §6.8.3).
Each frame becomes ``{frame, w4, h4, header, fields, symbols}``:

* ``fields``: every per-MI layer flattened row-major (MI unit = 4x4 = OVT cell); a layer with array elements
  is split into several fields; names and scales from ``aom_inspect_names.json``.
* ``header``: the frame-level keys (``frameType`` -> ``frame_type`` …); ``config``/``configString`` dropped.
* ``symbols``: the accounting stream (``[x, y]`` context markers, ``[id, bits, samples]`` symbols) summed per
  ``(x4, y4, name)`` into ``{x4, y4, name, bits_x8, count}`` (bits are 1/8-bit units, ``count`` = samples).

A frame whose key set differs from the pinned one (``-A`` layers, their maps, the frame keys, optionally the
accounting keys) is refused with exit 3.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402


# SPEC-ASSUMPTION(#93): accounting symbols sit in the block-grid frame as "symbols".
def symbols(frame: dict[str, Any], index: int) -> list[dict[str, Any]]:
    names = frame["symbolsMap"]
    totals: dict[tuple[int, int, str], list[int]] = {}
    ctx: tuple[int, int] | None = None
    for entry in frame["symbols"]:
        if len(entry) == 2:
            ctx = (entry[0], entry[1])
        elif len(entry) == 3:
            if ctx is None:
                raise canon.NormalizeError(f"frame {index}: symbol before the first [x, y] context")
            sym_id, bits, samples = entry
            if not 0 <= sym_id < len(names):
                raise canon.NormalizeError(f"frame {index}: symbol id {sym_id} outside symbolsMap")
            acc = totals.setdefault((ctx[0], ctx[1], names[sym_id]), [0, 0])
            acc[0] += bits
            acc[1] += samples
        else:
            raise canon.NormalizeError(f"frame {index}: symbols entry {entry!r} is neither [x,y] nor [id,bits,n]")
    return [
        {"x4": x, "y4": y, "name": name, "bits_x8": v[0], "count": v[1]}
        for (x, y, name), v in sorted(totals.items(), key=lambda kv: (kv[0][1], kv[0][0], kv[0][2]))
    ]


def convert_frame(index: int, frame: dict[str, Any], names: dict[str, Any]) -> dict[str, Any]:
    required = set(names["layers"]) | set(names["maps"]) | set(names["frame_keys"]) | set(names["ignored_keys"])
    optional = set(names["accounting_keys"])
    keys = set(frame)
    if not required <= keys or keys - required - optional:
        raise canon.NormalizeError(
            f"frame {index}: key set differs from aom_inspect_names.json "
            f"(missing {sorted(required - keys)}, unknown {sorted(keys - required - optional)})"
        )
    rows = frame["blockSize"]
    h4, w4 = len(rows), len(rows[0]) if rows else 0
    fields: dict[str, list[int]] = {}
    for layer, outs in names["layers"].items():
        grid = frame[layer]
        if len(grid) != h4 or any(len(r) != w4 for r in grid):
            raise canon.NormalizeError(f"frame {index}: layer {layer} is not {h4}x{w4} (RLE output, inspect -x?)")
        scale = names["scale"].get(layer, 1)
        cells = [c if isinstance(c, list) else [c] for r in grid for c in r]
        if any(len(c) != len(outs) for c in cells):
            raise canon.NormalizeError(f"frame {index}: layer {layer} elements are not {len(outs)} wide")
        for k, out in enumerate(outs):
            fields[out] = [c[k] * scale for c in cells]
    header = {out: frame[key] for key, out in names["frame_keys"].items()}
    result: dict[str, Any] = {"frame": index, "w4": w4, "h4": h4, "header": header, "fields": fields}
    if "symbols" in frame:
        result["symbols"] = symbols(frame, index)
    return result


def convert(args: argparse.Namespace) -> dict[str, Any]:
    doc = canon.read_json(args.input)
    if not isinstance(doc, list) or not doc or doc[-1] is not None:
        raise canon.NormalizeError("expected inspect framing: [ {frame}, ..., null ]")
    names = canon.load_map("aom_inspect_names")
    frames = doc[:-1]
    if not all(isinstance(f, dict) for f in frames):
        raise canon.NormalizeError("a frame entry is not an object")
    return {"frames": [convert_frame(i, f, names) for i, f in enumerate(frames)]}


if __name__ == "__main__":
    sys.exit(canon.run("block-grid", __doc__.splitlines()[0], convert))
