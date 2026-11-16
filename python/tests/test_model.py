# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""WI-0.095 acceptance tests: python/ovt/model.py against the WI-0.093 golden fixtures.

CTest (`python_model_pytest`) puts the staged package (generated ovt/fb + ovt/model.py) on PYTHONPATH and sets
OVT_GOLDEN_DIR to core/tests/golden.
"""

from __future__ import annotations

import json
import os
import struct
from pathlib import Path

import numpy as np
import pytest

zstandard = pytest.importorskip("zstandard")

GOLDEN = Path(os.environ.get("OVT_GOLDEN_DIR", Path(__file__).resolve().parents[2] / "core" / "tests" / "golden"))

import ovt.fb.FrameAnalysis  # noqa: E402,F401  (acceptance test 1: the generated package imports)
from ovt import model  # noqa: E402


def _fa(name: str):
    return model.load_frame_analysis(GOLDEN / name)


def test_import_generated_namespace_package():
    from ovt.fb.CellGrid import CellGrid
    from ovt.fb.FrameAnalysis import FrameAnalysis

    assert FrameAnalysis.FrameAnalysisBufferHasIdentifier(
        (GOLDEN / "tiny16_hevc.fa").read_bytes(), 0, size_prefixed=False
    ) or FrameAnalysis.FrameAnalysisBufferHasIdentifier(
        (GOLDEN / "tiny16_hevc.fa").read_bytes(), 0, size_prefixed=True
    )
    assert CellGrid is not None


def test_at1_hevc_qp_cell():
    qp = model.grid_as_numpy(_fa("tiny16_hevc.fa"))["qp"]
    assert qp.shape == (4, 4)
    assert qp.reshape(4, 4)[2, 0] == 30


def test_at2_av1_mv_dtype_and_shape():
    grid = model.grid_as_numpy(_fa("tiny64_av1.fa"))
    assert grid["mv0_x"].dtype == np.dtype("int16")
    assert grid["mv0_x"].shape == (16, 16)


def test_grid_is_zero_copy_view():
    data = bytearray((GOLDEN / "tiny16_hevc.fa").read_bytes())
    qp = model.grid_as_numpy(model.parse_frame_analysis(data))["qp"]
    assert np.shares_memory(qp, np.frombuffer(data, dtype=np.uint8))
    assert not qp.flags.owndata


def test_empty_vectors_have_right_dtype():
    grid = model.grid_as_numpy(_fa("tiny16_hevc.fa"))  # HEVC fixture provides no mv0_x
    empty = [k for k, v in grid.items() if v.size == 0]
    assert "mv0_x" in empty
    assert grid["mv0_x"].dtype == np.dtype("int16")
    assert grid["bits_x8"].dtype == np.dtype("uint32")


def test_size_prefixed_and_zstd(tmp_path):
    raw = (GOLDEN / "tiny16_hevc.fa").read_bytes()
    prefixed = struct.pack("<I", len(raw)) + raw
    p = tmp_path / "00000000.fa.zst"
    p.write_bytes(zstandard.ZstdCompressor(level=3).compress(prefixed))
    a = model.load_frame_analysis(p)
    assert model.grid_as_numpy(a)["qp"].reshape(4, 4)[2, 0] == 30
    q = tmp_path / "plain.fa.zst"
    q.write_bytes(zstandard.ZstdCompressor(level=3).compress(raw))
    assert model.load_frame_analysis(q).PluginId() == a.PluginId()
    r = tmp_path / "prefixed.fa"
    r.write_bytes(prefixed)
    assert model.load_frame_analysis(r).PluginId() == a.PluginId()


def test_rejects_non_frame_analysis(tmp_path):
    p = tmp_path / "junk.fa"
    p.write_bytes(b"\x00" * 64)
    with pytest.raises(ValueError):
        model.load_frame_analysis(p)
    p.write_bytes(b"abc")
    with pytest.raises(ValueError):
        model.load_frame_analysis(p)


def test_mv_to_luma_samples():
    assert model.mv_to_luma_samples(32) == 2.0
    out = model.mv_to_luma_samples(np.array([-8, 0, 16], dtype=np.int16))
    assert out.tolist() == [-0.5, 0.0, 1.0]


def test_values_match_cxx_json_export():
    """Twenty-plus fields read through Python equal the C++ JSON exporter's golden (tiny16_hevc.json)."""
    j = json.loads((GOLDEN / "tiny16_hevc.json").read_text())
    fa = _fa("tiny16_hevc.fa")
    frame, grid = fa.Frame(), fa.Grid()
    cs = frame.CodedSize()
    checks = [
        (fa.SchemaVersion(), j["schema_version"]),
        (fa.PluginId().decode(), j["plugin_id"]),
        (fa.PluginVersion().decode(), j["plugin_version"]),
        (frame.Id().DecodeIndex(), j["frame"]["id"]["decode_index"]),
        (frame.Id().DisplayIndex(), j["frame"]["id"]["display_index"]),
        (bool(frame.Shown()), j["frame"]["shown"]),
        (bool(frame.IsRap()), j["frame"]["is_rap"]),
        (frame.Poc(), j["frame"]["poc"]),
        (frame.TimebaseNum(), j["frame"]["timebase_num"]),
        (frame.TimebaseDen(), j["frame"]["timebase_den"]),
        (frame.SizeBytes(), j["frame"]["size_bytes"]),
        ((cs.X(), cs.Y(), cs.W(), cs.H()), tuple(j["frame"]["coded_size"][k] for k in "xywh")),
        (grid.CellsW(), j["grid"]["cells_w"]),
        (grid.CellsH(), j["grid"]["cells_h"]),
        (fa.BlocksLength(), len(j["blocks"])),
        (fa.PartitionsLength(), len(j["partitions"])),
        (fa.FilterParamsType().decode(), j["filter_params_type"]),
        (fa.QpHistLength(), len(j["qp_hist"])),
    ]
    for got, want in checks:
        assert got == want
    g = model.grid_as_numpy(fa)
    for name in ("block_id", "partition_id", "pred", "qp", "intra_mode_y", "ref0", "tx_type", "cbf", "bits_x8",
                 "deblock_bs_v", "filter_idx"):
        assert g[name].reshape(-1).tolist() == j["grid"][name], name
    for i, jb in enumerate(j["blocks"]):
        b = fa.Blocks(i)
        r = b.Rect()
        assert (b.Id(), b.Parent(), b.Level(), b.Qp(), b.BitsX8()) == (
            jb["id"], jb["parent"], jb["level"], jb["qp"], jb["bits_x8"])
        assert (r.X(), r.Y(), r.W(), r.H()) == tuple(jb["rect"][k] for k in "xywh")
        assert b.ChildrenLength() == len(jb.get("children", []))


def test_blocks_as_dataframe():
    pd = pytest.importorskip("pandas")
    j = json.loads((GOLDEN / "tiny16_hevc.json").read_text())
    df = model.blocks_as_dataframe(_fa("tiny16_hevc.fa"))
    assert isinstance(df, pd.DataFrame)
    assert len(df) == len(j["blocks"])
    assert df["kind"].tolist() == [b["kind"] for b in j["blocks"]]
    assert df["pred"].tolist() == [b["pred"] for b in j["blocks"]]
    assert df["ext_type"].tolist() == [b.get("ext_type", "") for b in j["blocks"]]
    assert df["w"].tolist() == [b["rect"]["w"] for b in j["blocks"]]
