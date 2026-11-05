# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the oracle normalisers in tests/oracles/normalize/ (chapter 14 §14.4, WI-0.555).

Fixtures under fixtures/oracles/ are oracle output: captured where the oracle runs in a plain container
(FFmpeg trace_headers, extract_mvs), otherwise written from the oracle's own printf formats (each fixture's
first line says which). Every normaliser runs as `python -I <script>` the way the oracle image calls it.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

REPO = Path(__file__).resolve().parents[2]
NORM = REPO / "tests" / "oracles" / "normalize"
FIX = Path(__file__).resolve().parent / "fixtures" / "oracles"
sys.path.insert(0, str(NORM))

import canon  # noqa: E402
import gen_cbs_names  # noqa: E402

# (script, fixture, extra args, family): every normaliser of this WI with every fixture (acceptance test 3).
CASES = [
    ("trace_headers", "trace_headers_av1.txt", ["--codec", "av1"], "syntax-elements"),
    ("trace_headers", "trace_headers_h264.txt", ["--codec", "h264"], "syntax-elements"),
    ("trace_headers", "trace_headers_h265.txt", ["--codec", "hevc"], "syntax-elements"),
    ("jm_trace", "jm_trace_skip.txt", [], "block-grid"),
    ("ffmpeg_mvs", "ffmpeg_mvs_h264.csv", [], "block-grid"),
    ("ffmpeg_mvs", "ffmpeg_qp_10bit.csv", ["--qp", "--bit-depth", "10"], "block-grid"),
    ("hm_grid", "hm_cu_dump_16x8.json", [], "block-grid"),
    ("aom_inspect", "aom_inspect_8x8.json", [], "block-grid"),
]


def run(script: str, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-I", str(NORM / f"{script}.py"), *args], capture_output=True, text=True, check=False
    )


def normalise(script: str, fixture: str, *args: str) -> Any:
    proc = run(script, str(FIX / fixture), *args)
    assert proc.returncode == 0, proc.stderr
    return json.loads(proc.stdout)


@pytest.mark.parametrize(("script", "fixture", "args", "family"), CASES, ids=[f"{c[0]}-{c[1]}" for c in CASES])
def test_output_matches_family_schema_and_is_deterministic(
    script: str, fixture: str, args: list[str], family: str
) -> None:
    first = run(script, str(FIX / fixture), *args)
    assert first.returncode == 0, first.stderr
    assert canon.validate(json.loads(first.stdout), family) == []
    assert run(script, str(FIX / fixture), *args).stdout == first.stdout  # §0.7.5 determinism


def test_every_family_has_a_schema() -> None:
    for family in canon.FAMILIES:
        assert canon.load_schema(family)["type"] in ("array", "object")


# -- trace_headers (§14.4.1) -------------------------------------------------------------------------------


def test_av1_seq_profile_is_at_payload_bit_0() -> None:
    """WI-0.555 acceptance test 1: OBU header and obu_size bits are subtracted."""
    doc = normalise("trace_headers", "trace_headers_av1.txt", "--codec", "av1")
    profiles = [e for e in doc if e["name"] == "seq_profile"]
    assert profiles and all(e["bit_offset"] == 0 for e in profiles)
    header = {e["name"]: e["bit_offset"] for e in doc if e["unit"] == profiles[0]["unit"] and e["bit_offset"] < 0}
    assert header["obu_forbidden_bit"] == -16 and header["obu_size"] == -8


def test_h26x_offsets_count_from_the_nal_header() -> None:
    doc = normalise("trace_headers", "trace_headers_h264.txt", "--codec", "h264")
    first = [e for e in doc if e["unit"] == 0]
    assert [(e["name"], e["bit_offset"], e["bit_length"]) for e in first[:4]] == [
        ("forbidden_zero_bit", 0, 1),
        ("nal_ref_idc", 1, 2),
        ("nal_unit_type", 3, 5),
        ("profile_idc", 8, 8),
    ]
    assert doc == sorted(doc, key=lambda e: (e["unit"], e["bit_offset"]))
    assert not any(e["name"] in ("rbsp_stop_one_bit", "rbsp_alignment_zero_bit") for e in doc)
    hevc = normalise("trace_headers", "trace_headers_h265.txt", "--codec", "h265")
    assert next(e for e in hevc if e["name"] == "vps_video_parameter_set_id")["bit_offset"] == 16


def test_extradata_is_skipped_unless_asked() -> None:
    plain = normalise("trace_headers", "trace_headers_h264.txt", "--codec", "h264")
    full = normalise("trace_headers", "trace_headers_h264.txt", "--codec", "h264", "--include-extradata")
    assert max(e["unit"] for e in full) == max(e["unit"] for e in plain) + 2  # SPS + PPS of the avcC


def test_inferred_elements_are_dropped() -> None:
    doc = normalise("trace_headers", "trace_headers_av1.txt", "--codec", "av1")
    assert all(e["bit_length"] > 0 for e in doc)
    assert "tile_cols_log2" not in {e["name"] for e in doc}  # printed with an empty bit string


def test_unmapped_cbs_name_fails(tmp_path: Path) -> None:
    trace = tmp_path / "t.txt"
    trace.write_text(
        "[trace_headers @ 0x1] 0           forbidden_zero_bit       0 = 0\n"
        "[trace_headers @ 0x1] 1           not_a_cbs_name          11 = 3\n",
        encoding="utf-8",
    )
    proc = run("trace_headers", str(trace), "--codec", "h264")
    assert proc.returncode == canon.EXIT_INPUT and "not_a_cbs_name" in proc.stderr


def test_cbs_known_names_come_from_the_read_macros() -> None:
    src = (
        "flag(constraint_set3_flag); u(8, level_idc, 0, 255); ues(offset_for_ref_frame[i], 0, 1, 1, i);"
        ' fc(4, obu_type, 0, 15); cbs_av1_read_leb128(ctx, rw, "obu_size", &v);'
    )
    assert gen_cbs_names.names_in(src) == {
        "constraint_set3_flag",
        "level_idc",
        "offset_for_ref_frame",
        "obu_type",
        "obu_size",
    }


# -- JM trace (§14.4.3) ------------------------------------------------------------------------------------


def test_jm_skipped_macroblocks_become_p_skip() -> None:
    """WI-0.555 acceptance test 2: every MB without mb_type and every address gap is P_Skip."""
    frames = normalise("jm_trace", "jm_trace_skip.txt")["frames"]
    p_frame = frames[1]
    assert [m["mb_addr"] for m in p_frame["mbs"]] == [0, 1, 2, 3]
    assert [m["mb_type"] for m in p_frame["mbs"]] == ["P_Skip", "P_Skip", 0, "P_Skip"]
    assert p_frame["mbs"][2]["mvd_l0"] == [[-1, 1]] and p_frame["mbs"][2]["ref_idx_l0"] == [0]
    assert p_frame["slices"] == [
        {"first_mb_in_slice": 0, "slice_type": 5, "slice_qp_delta": 0, "num_ref_idx_active": [1, 0]}
    ]


def test_jm_qp_accumulates_mb_qp_delta() -> None:
    i_frame = normalise("jm_trace", "jm_trace_skip.txt")["frames"][0]
    assert [m["qp"] for m in i_frame["mbs"]] == [24, 23, 23, 23]  # 26 + 0 - 2, then -1
    assert i_frame["mbs"][2]["intra4x4_pred_mode"] == [-1] * 16


# -- ffmpeg_mvs (§14.4.3a) ---------------------------------------------------------------------------------


def test_ffmpeg_mvs_rect_and_units() -> None:
    frames = normalise("ffmpeg_mvs", "ffmpeg_mvs_h264.csv")["frames"]
    assert [f["frame"] for f in frames] == [1, 2]  # framenum counts from 1; the I frame has no rows
    mv = next(m for m in frames[0]["mvs"] if m["x"] == 16)
    assert mv == {"list": 0, "x": 16, "y": 0, "w": 16, "h": 16, "mv_x": 48, "mv_y": 16}  # 12/4 px = 48/16


def test_ffmpeg_qp_subtracts_qp_bd_offset() -> None:
    frames = normalise("ffmpeg_mvs", "ffmpeg_qp_10bit.csv", "--qp", "--bit-depth", "10")["frames"]
    assert [b["qp"] for b in frames[0]["qp_blocks"]] == [12, 14]
    assert frames[1]["qp_blocks"][0]["qp"] == 24


# -- HM grid (§14.4.4) -------------------------------------------------------------------------------------


def test_hm_grid_mapping() -> None:
    f = normalise("hm_grid", "hm_cu_dump_16x8.json")["frames"][0]
    fields = f["fields"]
    assert (f["w4"], f["h4"], f["poc"]) == (4, 2, 4)
    assert fields["pred"][:4] == [1, 1, 3, 3]
    assert fields["mv0_x"][2] == -24 and fields["mv0_y"][2] == 12  # quarter-pel x4
    assert fields["intra_mode_y"][:4] == [26, 26, -1, -1]
    assert fields["cbf"][0] == 0b101 and fields["tx_log2_w"][0] == 3


def test_hm_grid_refuses_unknown_getter(tmp_path: Path) -> None:
    doc = json.loads((FIX / "hm_cu_dump_16x8.json").read_text(encoding="utf-8"))
    doc["frames"][0]["cells"]["getSomethingNew"] = [0] * 8
    path = tmp_path / "x.json"
    path.write_text(json.dumps(doc), encoding="utf-8")
    assert run("hm_grid", str(path)).returncode == canon.EXIT_INPUT


# -- libaom inspect (§14.4.5) ------------------------------------------------------------------------------


def test_aom_inspect_layers_and_symbols() -> None:
    frames = normalise("aom_inspect", "aom_inspect_8x8.json")["frames"]
    inter = frames[1]
    assert (inter["w4"], inter["h4"]) == (2, 2)
    assert inter["fields"]["mv0_x"] == [-12] * 4 and inter["fields"]["mv0_y"] == [4] * 4  # 1/8 -> 1/16
    assert inter["fields"]["RefFrame0"] == [1] * 4 and inter["fields"]["qp"] == [120] * 4
    assert inter["header"]["frame_type"] == 1
    # [0,0],[1,24,1],[3,9,2],[1,1],[2,30,1]: sums per (x4, y4, name)
    assert inter["symbols"] == [
        {"x4": 0, "y4": 0, "name": "read_intra_frame_y_mode", "bits_x8": 24, "count": 1},
        {"x4": 0, "y4": 0, "name": "read_skip_txfm", "bits_x8": 9, "count": 2},
        {"x4": 1, "y4": 1, "name": "read_mv", "bits_x8": 30, "count": 1},
    ]
    intra = frames[0]["symbols"]
    assert intra[0] == {"x4": 0, "y4": 0, "name": "read_intra_frame_y_mode", "bits_x8": 56, "count": 2}


def test_aom_inspect_refuses_a_changed_key_set(tmp_path: Path) -> None:
    text = (FIX / "aom_inspect_8x8.json").read_text(encoding="utf-8")
    path = tmp_path / "x.json"
    path.write_text(text.replace('"seg_id"', '"segment_id"'), encoding="utf-8")
    proc = run("aom_inspect", str(path))
    assert proc.returncode == canon.EXIT_INPUT and "segment_id" in proc.stderr


# -- canon (§14.4.0) ---------------------------------------------------------------------------------------


def test_dumps_is_sorted_and_rounds_floats() -> None:
    text = canon.dumps({"b": [1, 2], "a": {"y": 0.1234567, "x": float("inf")}, "c": [{"k": 1}]})
    assert (
        text == '{\n "a": {\n  "x": "inf",\n  "y": 0.123457\n },\n "b": [1,2],\n "c": [\n  {\n   "k": 1\n  }\n ]\n}\n'
    )


def test_validator_reports_violations() -> None:
    bad = [{"unit": -1, "name": "x", "bit_offset": 0, "bit_length": 0, "value": 1.5, "extra": 1}]
    errors = canon.validate(bad, "syntax-elements")
    assert any("minimum" in e for e in errors) and any("extra" in e for e in errors)
    assert any("integer|string" in e for e in errors)
    assert canon.validate({"frames": [{"i": 0, "psnr_y": "inf"}]}, "scores") == []
    assert canon.validate({"frames": [{"i": 0, "psnr_y": "big"}]}, "scores") != []


def test_validate_cli(tmp_path: Path) -> None:
    good = tmp_path / "good.json"
    good.write_text('[{"i": 0, "dts": 0, "cts": 0, "size": 10, "offset": 40, "sync": 1}]', encoding="utf-8")
    cli = [sys.executable, "-I", str(NORM / "canon.py"), "--validate", "sample-table"]
    assert subprocess.run([*cli, str(good)], check=False).returncode == 0
    good.write_text('[{"i": 0}]', encoding="utf-8")
    assert subprocess.run([*cli, str(good)], capture_output=True, check=False).returncode == canon.EXIT_SCHEMA
