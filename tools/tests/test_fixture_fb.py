# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of gen_fixture_fb and gen_dense_fa (WI-0.553 part c1): the buffers are read back with a schema-driven
reader (independent of the writer's field ordering) and checked against the chapter 6 §6.9 V-rules they can
violate. Small picture sizes keep the run short; the generators take any size.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import Any

import numpy as np
import pytest
from flatbuffers.table import Table

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import gen_dense_fa  # noqa: E402
import gen_fixture_fb as gf  # noqa: E402

S = gf.load_schema()


class Reader:
    """Reads one table of a buffer by field name, using the slot order of the schema."""

    def __init__(self, buf: bytearray, pos: int, name: str) -> None:
        self.buf, self.name = buf, name
        self.t = Table(buf, pos)
        self.fields = {f: (slot, t, d) for slot, (f, t, d) in enumerate(S.tables[name])}

    def __getitem__(self, field: str) -> Any:
        slot, t, default = self.fields[field]
        off = self.t.Offset(4 + 2 * slot)
        scalar = S.scalar(t)
        if scalar:
            return (
                struct.unpack_from("<" + gf.SCALARS[scalar], self.buf, self.t.Pos + off)[0]
                if off
                else S.default(t, default)
            )
        if not off:
            return None
        pos = self.t.Pos + off
        if t in S.structs:
            return struct.unpack_from(S.struct_layout(t)[0], self.buf, pos)
        if t == "string":
            return self.t.String(pos).decode()
        if t.startswith("["):
            elem, start, n = t[1:-1], self.t.Vector(off), self.t.VectorLen(off)
            if S.scalar(elem):
                return np.frombuffer(self.buf, gf.NUMPY[S.scalar(elem)], n, start)
            return [Reader(self.buf, self.t.Indirect(start + 4 * i), elem) for i in range(n)]
        return Reader(self.buf, self.t.Indirect(pos), t)


def root(buf: bytes) -> Reader:
    assert buf[4:8] == b"OVTA"
    return Reader(bytearray(buf), struct.unpack_from("<I", buf)[0], "FrameAnalysis")


def check_tree(fa: Reader, leaf_kind: str) -> list[Reader]:
    """V-004..V-012 for the block tree and the cell grid; returns the blocks."""
    w, h = fa["frame"]["coded_size"][2:]
    grid, blocks = fa["grid"], fa["blocks"]
    cw, ch = grid["cells_w"], grid["cells_h"]
    assert (cw, ch) == (-(-w // 4), -(-h // 4))
    assert [b["id"] for b in blocks] == list(range(len(blocks)))
    assert blocks[0]["parent"] == -1 and blocks[0]["rect"] == (0, 0, w, h) and blocks[0]["level"] == 0
    kind = {v: k for k, v in S.enums["BlockKind"][1].items()}
    owned = np.zeros(len(blocks), bool)
    bid = grid["block_id"].reshape(ch, cw)
    for b in blocks[1:]:
        parent = blocks[b["parent"]]
        assert 0 <= b["parent"] < b["id"] and list(parent["children"]).count(b["id"]) == 1
        assert b["level"] == parent["level"] + 1
        x, y, bw, bh = b["rect"]
        assert bw > 0 and bh > 0 and x % 4 == y % 4 == bw % 4 == bh % 4 == 0
        if b["level"] >= 2:
            px, py, pw, ph = parent["rect"]
            assert px <= x and py <= y and x + bw <= px + pw and y + bh <= py + ph
        if kind[b["kind"]] == leaf_kind:
            assert b["children"] is None
            cells = bid[y // 4 : (y + bh) // 4, x // 4 : (x + bw) // 4]
            assert (cells == b["id"]).all()  # V-012: the leaf owns exactly its cells
            owned[b["id"]] = True
    assert owned[bid.ravel()].all()  # V-010/V-011: every cell names a leaf
    return blocks


@pytest.mark.parametrize("size", [(1920, 1080), (200, 120)])
def test_grid_tree_is_valid(size: tuple[int, int]) -> None:
    fa = root(gf.build_grid(S, *size))
    assert fa["schema_version"] == 1 and fa["plugin_id"] and fa["plugin_version"]
    blocks = check_tree(fa, "CU")
    sizes = {b["rect"][2] for b in blocks if b["level"] == 2}
    assert sizes == {64, 32, 16, 8}  # every CU size appears
    assert fa["grid"]["pred"].tolist() == [S.enums["PredKind"][1]["Intra"]] * len(fa["grid"]["pred"])


def test_mv_frame_is_valid() -> None:
    fa = root(gf.build_mv(S, 200, 120))
    blocks = check_tree(fa, "Block")
    grid, refs, mvs = fa["grid"], fa["refs"], fa["mvs"]
    n = grid["cells_w"] * grid["cells_h"]
    assert [r["index"] for r in refs] == list(range(8)) and refs[0]["name"] == "INTRA"  # V-014
    assert [r["slot"] for r in refs] == [-1, 0, 1, 2, 3, 4, 5, 6]
    assert refs[1]["usage_cells"] == n and sum(r["usage_cells"] for r in refs) == n  # V-016
    assert (grid["ref0"] == 1).all() and (grid["ref1"] == -1).all()  # V-026
    assert len(mvs) == n and [m["block_id"] for m in mvs] == sorted(m["block_id"] for m in mvs)  # V-025
    cw = grid["cells_w"]
    for m in mvs[:: max(1, n // 200)]:  # V-015 on a sample
        x, y = m["rect"][:2]
        assert blocks[m["block_id"]]["rect"] == m["rect"]
        i = (y // 4) * cw + x // 4
        assert (grid["mv0_x"][i], grid["mv0_y"][i]) == (m["mv_x"], m["mv_y"]) and m["ref"] == 1
    assert np.abs(grid["mv0_x"]).max() <= 512 and len(set(grid["mv0_y"].tolist())) > 10


def test_dense_adds_qp_ramp(tmp_path: Path) -> None:
    fa = root(gf.build_mv(S, 256, 64, dense=True))
    qp = fa["grid"]["qp"]
    assert qp.min() == 0 and qp.max() == 252 and fa["qp_hist"].sum() == len(qp)  # V-028
    for b in fa["blocks"][2:40]:
        x, y = b["rect"][:2]
        assert b["qp"] == qp[(y // 4) * fa["grid"]["cells_w"] + x // 4]
    out = tmp_path / "fa.bin"
    assert gen_dense_fa.main(["--size", "1080p", "--out", str(out)]) == 0
    assert root(out.read_bytes())["grid"]["cells_w"] == 480


def test_output_is_deterministic(tmp_path: Path) -> None:
    a, b = tmp_path / "a.fb", tmp_path / "b.fb"
    for path in (a, b):
        assert gf.main(["--kind", "grid", "--size", "1080p", "--out", str(path)]) == 0
    assert a.read_bytes() == b.read_bytes()
    assert gf.build_mv(S, 64, 64) == gf.build_mv(S, 64, 64)


def test_schema_reader_matches_declarations() -> None:
    assert S.struct_layout("BitSpan") == ("<QIxxxx", 8)  # ulong + uint, padded to 16
    assert S.default("Block", None) == 0 and S.enums["Av1RefFrame"][1]["ALTREF_FRAME"] == 7
    assert [f for f, _, _ in S.tables["RefSlot"]] == ["index", "frame", "name", "is_long_term", "usage_cells", "slot"]
