# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/gen_sdf_atlas.py (WI-2.433): the distance transform and the byte encoding, without a font."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import gen_sdf_atlas as g


def disc(size: int = 40, radius: float = 10.0) -> np.ndarray:
    yy, xx = np.mgrid[0:size, 0:size]
    return np.where((xx - size / 2 + 0.5) ** 2 + (yy - size / 2 + 0.5) ** 2 <= radius**2, 255, 0).astype(np.uint8)


def test_edt_1d_matches_brute_force() -> None:
    f = np.array([g.INF, 0.0, g.INF, g.INF, 4.0, g.INF, g.INF, 0.0])
    expected = [min((p - q) ** 2 + f[q] for q in range(len(f))) for p in range(len(f))]
    assert np.allclose(g.edt_1d(f), expected)


def test_sdf_encodes_inside_edge_and_outside() -> None:
    sdf = g.sdf_bytes(disc())
    assert sdf[20, 20] == 255  # 10 px deep, beyond the 8 px spread
    assert sdf[0, 0] == 0  # far outside
    row = sdf[20].astype(int)
    edge = [x for x in range(39) if (row[x] > 127) != (row[x + 1] > 127)]
    assert edge == [9, 29]  # the outline of a radius-10 disc centred between pixels 19 and 20
    # A fully covered (binary) raster: pixel 8 is 2 px from the nearest inside pixel (10).
    assert sdf[20, 8] == int(np.floor(127.5 - 2 * 127.5 / g.SPREAD + 0.5))


def test_empty_glyph_is_all_outside() -> None:
    assert not g.sdf_bytes(np.zeros((20, 20), dtype=np.uint8)).any()


def test_layout_fits_the_atlas() -> None:
    cell_w, cell_h, columns, rows, height = g.layout(29, 57)
    assert (cell_w, cell_h) == (45, 73)
    assert columns * cell_w <= g.ATLAS_WIDTH
    assert columns * rows >= len(g.GLYPHS) == 101
    assert height % 4 == 0 and height >= rows * cell_h


def test_header_is_deterministic() -> None:
    atlas = np.arange(g.ATLAS_WIDTH * 4, dtype=np.uint32).astype(np.uint8).reshape(4, g.ATLAS_WIDTH)
    meta = {"sha256": "0" * 64, "pillow": "x", "freetype": "y", "ascent": 45, "descent": 12, "advance": 29}
    meta.update({"cell_w": 45, "cell_h": 73, "columns": 22, "height": 4})
    first = g.header(atlas, meta)
    assert first == g.header(atlas.copy(), dict(meta))
    # REUSE-IgnoreStart
    assert "SPDX-License-Identifier: Bitstream-Vera" in first
    # REUSE-IgnoreEnd
    assert f"std::array<uint8_t, {4 * g.ATLAS_WIDTH}> kSdfAtlas" in first
