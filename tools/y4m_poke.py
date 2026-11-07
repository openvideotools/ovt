#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Change one sample of a Y4M file (chapter 14 §14.3.3 recipe 52; WI-0.553).

    y4m_poke.py --frame N --plane P --x X --y Y --delta D in.y4m out.y4m

Copies ``in.y4m`` to ``out.y4m`` with sample (x, y) of plane P (0 = Y, 1 = Cb, 2 = Cr) of frame N changed by D,
and prints ``{"frame", "plane", "x", "y", "a", "b"}`` (old and new value) as JSON on stdout for the
``compare_expected.json`` sidecar. 8- and 16-bit containers (``C420p10`` etc.) are supported. A result outside
the sample range is an error rather than a clamp, so ``b == a + delta`` always holds. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import re
import json
import sys
from pathlib import Path

# chroma subsampling (horizontal, vertical shift) and plane count per Y4M colour space prefix
LAYOUTS = {"420": (1, 1, 3), "422": (1, 0, 3), "444": (0, 0, 3), "mono": (0, 0, 1)}


def plane_geometry(header: str) -> tuple[list[tuple[int, int]], int, int]:
    """([(width, height)] per plane, bytes per sample, max value)."""
    tags = {t[0]: t[1:] for t in header.split()[1:]}
    w, h = int(tags["W"]), int(tags["H"])
    cs = tags.get("C", "420jpeg")
    m = re.search(r"(?:p|mono)(\d+)$", cs)  # 420p10, 444p16, mono10, mono16
    depth = int(m.group(1)) if m else 8
    layout = next((v for k, v in LAYOUTS.items() if cs.startswith(k)), None)
    if layout is None:
        raise ValueError(f"unsupported colour space C{cs}")
    sx, sy, n = layout
    cw, ch = (w + sx) >> sx, (h + sy) >> sy
    return [(w, h), (cw, ch), (cw, ch)][:n], 2 if depth > 8 else 1, (1 << depth) - 1


def poke(data: bytes, frame: int, plane: int, x: int, y: int, delta: int) -> tuple[bytes, int, int]:
    eol = data.index(b"\n")
    planes, bps, maxval = plane_geometry(data[:eol].decode("ascii"))
    if not 0 <= plane < len(planes) or not (0 <= x < planes[plane][0] and 0 <= y < planes[plane][1]):
        raise ValueError(f"sample ({x}, {y}) of plane {plane} is outside the picture")
    frame_bytes = sum(pw * ph for pw, ph in planes) * bps
    pos = eol + 1
    for _ in range(frame):
        pos = data.index(b"\n", pos) + 1 + frame_bytes
    if not data.startswith(b"FRAME", pos):
        raise ValueError(f"frame {frame} not found")
    pos = data.index(b"\n", pos) + 1
    pos += sum(pw * ph for pw, ph in planes[:plane]) * bps + (y * planes[plane][0] + x) * bps
    if pos + bps > len(data):
        raise ValueError(f"frame {frame} is truncated")
    a = int.from_bytes(data[pos : pos + bps], "little")
    b = a + delta
    if not 0 <= b <= maxval:
        raise ValueError(f"{a} {delta:+d} is outside 0..{maxval}")
    return data[:pos] + b.to_bytes(bps, "little") + data[pos + bps :], a, b


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    for name in ("--frame", "--plane", "--x", "--y", "--delta"):
        ap.add_argument(name, type=int, required=True)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    try:
        out, a, b = poke(args.input.read_bytes(), args.frame, args.plane, args.x, args.y, args.delta)
    except (ValueError, OSError) as e:
        print(f"y4m_poke: {e}", file=sys.stderr)
        return 2
    args.output.write_bytes(out)
    print(json.dumps({"frame": args.frame, "plane": args.plane, "x": args.x, "y": args.y, "a": a, "b": b}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
