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
    ("mp4box_diso", "mp4box_diso_tiny.xml", [], "box-tree"),
    ("mp4box_diso", "mp4box_diso_frag.xml", [], "box-tree"),
    ("mp4box_dts", "mp4box_dts.txt", [], "sample-table"),
    ("mp4ff_info", "mp4ff_info_frag.txt", [], "box-tree"),
    ("mediainfo_trace", "mediainfo_trace_frag.xml", [], "box-tree"),
    ("mediainfo_trace", "mediainfo_hdr10.json", ["--hdr"], "header-struct"),
    ("mp4boxjs", "mp4boxjs_tiny.json", [], "box-tree"),
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


def test_jm_pcm_macroblock_keeps_the_running_qp(tmp_path: Path) -> None:
    """I_PCM has QP_Y = QP_Y,PRED; the MBs after it keep accumulating from there, not from 0."""
    lines = (FIX / "jm_trace_skip.txt").read_text(encoding="utf-8").splitlines()
    out, mb = [], -1
    for line in lines:
        if line.startswith("*********** POC: 0 "):
            mb += 1
        if mb == 2 and "intra4x4_pred_mode" in line:
            continue  # MB 2 of the I frame becomes an I_PCM macroblock
        if mb == 2 and "mb_qp_delta" in line:
            line = "@200   pcm_sample_luma                                                1 ( 77)"
        out.append(line)
    trace = tmp_path / "pcm.txt"
    trace.write_text("\n".join(out) + "\n", encoding="utf-8")
    mbs = normalise("jm_trace", str(trace))["frames"][0]["mbs"]
    assert [m["qp"] for m in mbs] == [24, 23, 23, 23]
    assert mbs[2]["pcm"] == 1 and "mb_qp_delta" not in mbs[2]


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


# -- ISOBMFF oracles (§14.4.6-§14.4.9) ---------------------------------------------------------------------
# tiny.mp4: 3 frames of 64x64 libx264, moov at the end; frag.mp4: the same remuxed with
# -movflags frag_keyframe+empty_moov; hdr.mp4: 2 frames of x265 Main 10 with HDR10 SEI. Captured with GPAC
# 2.2.1, mp4ff-info v0.48.0, MediaInfo 24.01 and, for mp4boxjs_tiny.json, a stand-in for WI-0.564's
# mp4boxjs_dump.mjs written to §14.4.9 (mp4box 1.x). The MediaInfo trace drops its "Second pass" NAL block
# and the MediaInfo JSON drops Encoded_Library_Settings; the rest is byte-exact.


def by_path(doc: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    return {b["path"]: b for b in doc}


def test_diso_paths_index_repeated_siblings_and_map_fields() -> None:
    boxes = by_path(normalise("mp4box_diso", "mp4box_diso_frag.xml"))
    assert "moof[2]/traf/trun" in boxes and "mdat[1]" in boxes and "moov/trak" in boxes
    assert "moov/udta/meta/ilst/\u00a9too" in boxes  # GPAC Type "A9too"
    trun = boxes["moof[1]/traf/trun"]["fields"]
    assert trun["sample_count"] == 2 and trun["data_offset"] == 128
    assert trun["entries"] == [
        {"sample_composition_time_offset": 512, "sample_size": 2314},
        {"sample_composition_time_offset": 512, "sample_size": 972},
    ]
    assert boxes["moov/trak/tkhd"]["fields"]["width"] == 64 << 16  # 16.16 fixed point as the raw integer
    avcc = boxes["moov/trak/mdia/minf/stbl/stsd/avc1/avcC"]["fields"]
    assert avcc["lengthSizeMinusOne"] == 3 and avcc["pictureParameterSetNALUnit"] == ["68ce0f1920"]
    assert all("offset" not in b for b in boxes.values())  # -diso prints no offsets


def test_diso_tiny_entries() -> None:
    boxes = by_path(normalise("mp4box_diso", "mp4box_diso_tiny.xml"))
    stbl = "moov/trak/mdia/minf/stbl/"
    assert boxes["ftyp"]["fields"]["compatible_brands"] == ["isom", "iso2", "avc1", "mp41"]
    assert boxes[stbl + "stsz"]["fields"]["entries"] == [{"entry_size": s} for s in (2314, 972, 2404)]
    assert boxes[stbl + "ctts"]["fields"]["entries"] == [{"sample_count": 3, "sample_offset": 512}]


def test_gpac_fourcc_and_unknown_box(tmp_path: Path) -> None:
    import mp4box_diso

    assert mp4box_diso.gpac_fourcc("A9too") == "\u00a9too" and mp4box_diso.gpac_fourcc("url ") == "url "
    assert mp4box_diso.gpac_fourcc("00000001") == "\x00\x00\x00\x01"
    xml = tmp_path / "u.xml"
    xml.write_text('<IsoMediaFile><UnknownBox Size="16" Type="abcd" Version="0" Foo="1"/></IsoMediaFile>')
    assert normalise("mp4box_diso", str(xml)) == [{"path": "abcd", "size": 16}]


def test_dts_rows_and_track_filter(tmp_path: Path) -> None:
    rows = normalise("mp4box_dts", "mp4box_dts.txt")
    assert rows[1] == {"i": 1, "dts": 512, "cts": 1024, "size": 972, "offset": 2362, "sync": 0}
    assert [r["sync"] for r in rows] == [1, 0, 1]
    two = tmp_path / "two.txt"
    text = (FIX / "mp4box_dts.txt").read_text()
    two.write_text(text + text.replace("track ID 1", "track ID 2"))
    assert {r["track"] for r in normalise("mp4box_dts", str(two))} == {1, 2}
    assert len(normalise("mp4box_dts", str(two), "--track", "2")) == 3
    two.write_text(text.replace("CTS\tSize", "Size\tCTS"))
    assert run("mp4box_dts", str(two)).returncode == canon.EXIT_INPUT


def test_mp4ff_agrees_with_gpac_on_tree_and_shared_fields() -> None:
    gpac = by_path(normalise("mp4box_diso", "mp4box_diso_frag.xml"))
    mp4ff = by_path(normalise("mp4ff_info", "mp4ff_info_frag.txt"))
    assert {(p, b["size"]) for p, b in mp4ff.items()} == {(p, b["size"]) for p, b in gpac.items()}
    for path, box in mp4ff.items():
        theirs = gpac[path].get("fields", {})
        shared = set(box.get("fields", {})) & set(theirs)
        assert {k: box["fields"][k] for k in shared} == {k: theirs[k] for k in shared}, path
    assert mp4ff["moov/trak/mdia/minf/stbl/stsd/avc1/pasp"]["fields"] == {"hSpacing": 1, "vSpacing": 1}
    assert mp4ff["moof[1]/traf/trun"]["fields"]["first_sample_flags"] == 0x02000000
    assert mp4ff["mfra/tfra"]["fields"]["entries"] == gpac["mfra/tfra"]["fields"]["entries"]


def test_mediainfo_offsets_and_header_length() -> None:
    boxes = by_path(normalise("mediainfo_trace", "mediainfo_trace_frag.xml"))
    gpac = by_path(normalise("mp4box_diso", "mp4box_diso_frag.xml"))
    assert {p.replace("?", "\u00a9"): b["size"] for p, b in boxes.items()} == {p: b["size"] for p, b in gpac.items()}
    moofs = [e["moof_offset"] for e in gpac["mfra/tfra"]["fields"]["entries"]]
    assert [boxes["moof[1]"]["offset"], boxes["moof[2]"]["offset"]] == moofs  # two oracles, one offset
    assert boxes["moov/mvhd"] == {"path": "moov/mvhd", "offset": 36, "size": 108, "header_length": 8}


def test_mediainfo_hdr_units(tmp_path: Path) -> None:
    hdr = normalise("mediainfo_trace", "mediainfo_hdr10.json", "--hdr")["hdr"]
    assert (hdr["primaries"], hdr["transfer"], hdr["matrix"], hdr["full_range"]) == (9, 16, 9, 0)
    assert hdr["mdcv_primaries"] == [[34000, 16000], [13250, 34500], [7500, 3000]]  # R, G, B
    assert hdr["mdcv_white_point"] == [15635, 16450]
    assert (hdr["mdcv_max_luminance"], hdr["mdcv_min_luminance"], hdr["max_cll"], hdr["max_fall"]) == (
        10000000,
        1,
        1000,
        400,
    )
    doc = json.loads((FIX / "mediainfo_hdr10.json").read_text())
    video = doc["media"]["track"][1]
    video["MasteringDisplay_ColorPrimaries"] = (
        "R: x=0.680000 y=0.320000, G: x=0.265000 y=0.690000, B: x=0.150000 y=0.060000, White point: x=0.312700 y=0.329000"
    )
    path = tmp_path / "mi.json"
    path.write_text(json.dumps(doc))
    again = normalise("mediainfo_trace", str(path), "--hdr")["hdr"]
    assert again["mdcv_primaries"] == hdr["mdcv_primaries"] and again["mdcv_white_point"] == hdr["mdcv_white_point"]
    video["transfer_characteristics"] = "Something new"
    path.write_text(json.dumps(doc))
    assert run("mediainfo_trace", str(path), "--hdr").returncode == canon.EXIT_INPUT


def test_mp4boxjs_offsets_and_fullbox_header_length() -> None:
    boxes = by_path(normalise("mp4boxjs", "mp4boxjs_tiny.json"))
    gpac = by_path(normalise("mp4box_diso", "mp4box_diso_tiny.xml"))
    assert all(gpac[p]["size"] == b["size"] for p, b in boxes.items())  # mp4box.js lists a subset
    assert boxes["moov/mvhd"]["offset"] == 5746 and boxes["moov/mvhd"]["header_length"] == 8  # hdr_size 12
    assert boxes["moov/trak/mdia/mdhd"]["fields"]["language"] == "und"
    assert boxes["moov/trak/tkhd"]["fields"]["track_ID"] == gpac["moov/trak/tkhd"]["fields"]["track_ID"]


def test_box_paths_rejects_a_depth_jump() -> None:
    with pytest.raises(canon.NormalizeError):
        canon.box_paths([(0, "moov", {}), (2, "mvhd", {})])


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
