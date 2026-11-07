#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Self-check of the libaom ``inspect`` goldens against ``aomdec --summary`` (chapter 14 §14.4.5, WI-0.566 step 5).

A golden covers the first ``K`` temporal units of a stream. ``aomdec --summary`` on the same ``K`` temporal units
(the IVF prefix the golden was made from) prints ``D decoded frames/S showed frames``. ``inspect`` writes one object
per decoded frame, hidden frames included and ``show_existing_frame = 1`` outputs absent (§14.4.5). So:

* ``D == K``: aomdec decoded every temporal unit of the prefix;
* ``E = S - (frames with show_frame = 1)`` is the number of ``show_existing_frame`` outputs and must be ``>= 0``;
* every shown frame has accounting symbols and at least one of them carries bits (the block-grid ``symbols``
  sums are non-zero).

``check(...)`` returns the list of problems (empty when the golden agrees).
"""

from __future__ import annotations

import re
from typing import Any

SUMMARY_RE = re.compile(r"(\d+) decoded frames/(\d+) showed frames")


def parse_summary(text: str) -> tuple[int, int]:
    """``(decoded, showed)`` from the ``aomdec --summary`` line."""
    m = SUMMARY_RE.search(text)
    if not m:
        raise ValueError("no 'N decoded frames/M showed frames' line in the aomdec output")
    return int(m.group(1)), int(m.group(2))


def check(doc: dict[str, Any], temporal_units: int, decoded: int, showed: int) -> dict[str, Any]:
    """Compare the block-grid document of one golden with the aomdec counts of its stream prefix."""
    frames = doc["frames"]
    problems: list[str] = []
    shown = [f for f in frames if f["header"]["show_frame"] == 1]
    hidden = [f for f in frames if f["header"]["show_frame"] == 0]
    if len(shown) + len(hidden) != len(frames):
        problems.append("a frame has a show_frame value other than 0 or 1")
    existing = showed - len(shown)
    if decoded != temporal_units:
        problems.append(f"aomdec decoded {decoded} temporal units, the prefix has {temporal_units}")
    if existing < 0:
        problems.append(f"inspect lists {len(shown)} shown frames but aomdec shows only {showed}")
    for f in shown:
        syms = f.get("symbols") or []
        if not any(s["bits_x8"] > 0 for s in syms):
            problems.append(f"shown frame {f['frame']} has no accounting symbol with bits")
    return {
        "frames": len(frames),
        "shown": len(shown),
        "hidden": len(hidden),
        "show_existing": max(existing, 0),
        "aomdec_decoded": decoded,
        "aomdec_showed": showed,
        "problems": problems,
    }
