# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/goldens/gen_h26x.py, h26x_selfcheck.py and the WI-0.567 normaliser changes (chapter 14 §14.4)."""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
GOLDENS = REPO / "tools" / "goldens"
NORM = REPO / "tests" / "oracles" / "normalize"
sys.path.insert(0, str(GOLDENS))
sys.path.insert(0, str(NORM))

import gen_h26x as gen  # noqa: E402
import gstparse  # noqa: E402
import h26x_selfcheck as sc  # noqa: E402
import trace_headers  # noqa: E402

SC = b"\x00\x00\x00\x01"


def h264_stream(pictures: int) -> bytes:
    """AUD, SPS, PPS, then ``pictures`` x (SEI, one slice with first_mb_in_slice 0, one slice with first_mb 1)."""
    out = SC + b"\x09\xf0" + SC + b"\x67\x42\x00\x1e" + SC + b"\x68\xce\x38\x80"
    for _ in range(pictures):
        out += SC + b"\x06\x05\x01\xff" + SC + b"\x41\x9a\x22" + SC + b"\x41\x40\x22"
    return out


def h265_stream(pictures: int) -> bytes:
    out = SC + b"\x40\x01\x0c" + SC + b"\x42\x01\x01" + SC + b"\x44\x01\xc0"
    for _ in range(pictures):
        out += SC + b"\x02\x01\xa8" + SC + b"\x02\x01\x28"  # TRAIL_R, first_slice_segment_in_pic_flag 1 then 0
    return out


def count_pictures(data: bytes, codec: str) -> int:
    return sum(1 for _b, nal, _e in gen.nal_units(data) if gen.first_slice_of_picture(data, codec, nal) is True)


# ---- truncation -------------------------------------------------------------------------------------------------


@pytest.mark.parametrize(("codec", "make"), [("h264", h264_stream), ("h265", h265_stream)])
def test_truncate_keeps_whole_pictures(codec: str, make) -> None:
    data = make(5)
    cut, kept = gen.truncate(data, codec, 2)
    assert kept == 2
    assert count_pictures(cut, codec) == 2
    assert data.startswith(cut)
    assert cut.endswith(b"\x40\x22") or codec == "h265"  # the second slice of picture 2 is kept
    assert gen.truncate(data, codec, 9) == (data, 5)


def test_truncate_moves_the_cut_before_the_next_access_unit_prefix() -> None:
    data = h264_stream(3)
    cut, _ = gen.truncate(data, "h264", 1)
    assert cut.endswith(b"\x41\x40\x22")  # the SEI of picture 2 (type 6) is not left behind
    assert not cut.endswith(b"\xff")


def test_nal_units_handle_three_and_four_byte_start_codes() -> None:
    data = b"\x00\x00\x01\x09\xf0\x00\x00\x00\x01\x67\x42"
    assert gen.nal_units(data) == [(0, 3, 5), (5, 9, 11)]
    assert [data[n] for _b, n, _e in gen.nal_units(data)] == [0x09, 0x67]


def test_sniff() -> None:
    assert gen.sniff(b"\x00\x00\x00\x20ftypisom") == "isobmff"
    assert gen.sniff(b"\x1a\x45\xdf\xa3....") == "matroska"
    assert gen.sniff(b"\x00\x00\x00\x01\x67") == "annexb"
    assert gen.sniff(b"\x47" + b"\x00" * 187 + b"\x47" + b"\x00" * 10) == "mpegts"
    with pytest.raises(gen.StepError):
        gen.sniff(b"RIFF....")


# ---- selection --------------------------------------------------------------------------------------------------

MANIFEST = {
    "entries": [
        {
            "id": "jvt_A",
            "source": "jvt",
            "codecs": ["h264"],
            "containers": ["annexb"],
            "tags": ["fast"],
        },
        {
            "id": "jvt_slow",
            "source": "jvt",
            "codecs": ["h264"],
            "containers": ["annexb"],
            "tags": [],
        },
        {
            "id": "jctvc_B",
            "source": "jctvc",
            "codecs": ["h265"],
            "containers": ["annexb"],
            "tags": ["fast"],
        },
        {
            "id": "heif_C",
            "source": "heif",
            "codecs": ["h265"],
            "containers": ["heif"],
            "tags": ["fast"],
        },
        {
            "id": "mpegff_video_cenc_128iv",
            "source": "mpegff",
            "codecs": ["h264"],
            "tags": ["fast"],
        },
        {"id": "aom_x", "source": "aom", "codecs": ["av1"], "tags": ["fast"]},
        {"id": "syn_h265_amp_sao_on", "source": "syn", "tags": ["fast"]},
        {"id": "syn_h264_epb_dense", "source": "syn"},
        {"id": "syn_h264_mbaff", "source": "syn"},
    ]
}


def test_select_takes_fast_h26x_entries_and_the_epb_stream() -> None:
    chosen, skipped = gen.select(MANIFEST, None)
    assert [e["id"] for e in chosen] == [
        "jvt_A",
        "jctvc_B",
        "syn_h265_amp_sao_on",
        "syn_h264_epb_dense",
    ]
    assert set(skipped) == {"heif_C", "mpegff_video_cenc_128iv"}


def test_select_only_names_any_entry() -> None:
    chosen, _ = gen.select(MANIFEST, ["syn_h264_mbaff", "jvt_slow"])
    assert [e["id"] for e in chosen] == ["jvt_slow", "syn_h264_mbaff"]
    with pytest.raises(gen.StepError):
        gen.select(MANIFEST, ["aom_x"])


def test_oracles_per_entry() -> None:
    by_id = {e["id"]: e for e in MANIFEST["entries"]}
    everything = set(gen.ALL_ORACLES)
    assert gen.oracles_for(by_id["jvt_A"], "h264", everything) == [
        "trace_headers",
        "gstparse",
        "jm",
        "mvs",
    ]
    assert gen.oracles_for(by_id["jctvc_B"], "h265", everything) == [
        "trace_headers",
        "gstparse",
        "hm",
    ]
    assert gen.oracles_for(by_id["jctvc_B"], "h265", {"hm"}) == ["hm"]


# ---- normaliser additions ---------------------------------------------------------------------------------------


def test_gstparse_keys_units_and_drops_ignored_members() -> None:
    text = "\n".join(
        [
            json.dumps(
                {
                    "unit": {"index": 0, "offset": 4, "type": 7},
                    "sps": {"width": 16, "level_idc": 30},
                }
            ),
            json.dumps(
                {
                    "unit": {"index": 1, "offset": 20, "type": 8},
                    "pps": {"valid": 1, "pic_parameter_set_id": 0},
                }
            ),
        ]
    )
    doc = gstparse.convert_text(text, "h264")
    assert list(doc) == ["000000", "000001"]
    assert doc["000000"]["sps"] == {"level_idc": 30}  # h264.sps.width is in gst_ignore.txt
    assert doc["000001"]["pps"] == {"pic_parameter_set_id": 0}


def test_gstparse_rejects_non_units() -> None:
    with pytest.raises(gstparse.canon.NormalizeError):
        gstparse.convert_text('{"sps": {}}', "h264")
    with pytest.raises(gstparse.canon.NormalizeError):
        gstparse.convert_text("", "h264")


def test_epb_offsets_skip_emulation_prevention_bytes() -> None:
    # NAL header 0x67, then 00 00 03 01 (EPB at raw index 3), then 80
    stream = SC + b"\x67\x00\x00\x03\x01\x80" + SC + b"\x68\xce"
    units = trace_headers.annexb_units(stream)
    assert units == [(4, [3]), (14, [])]
    elements = [
        {
            "unit": 0,
            "name": "a",
            "bit_offset": 8 * 3 + 2,
            "bit_length": 1,
            "value": 0,
        },  # RBSP byte 3 = raw byte 4
        {
            "unit": 0,
            "name": "b",
            "bit_offset": 8 * 2,
            "bit_length": 1,
            "value": 0,
        },  # RBSP byte 2 = raw byte 2
        {"unit": 1, "name": "c", "bit_offset": 8, "bit_length": 8, "value": 0},
    ]
    trace_headers.add_file_offsets(elements, stream)
    assert [(e["file_byte"], e["file_bit"]) for e in elements] == [
        (4 + 4, 2),
        (4 + 2, 0),
        (14 + 1, 0),
    ]


# ---- self-check -------------------------------------------------------------------------------------------------


def cbs(*units: list[tuple[str, int]]) -> list[dict]:
    return [
        {"unit": i, "name": n, "bit_offset": 0, "bit_length": 1, "value": v}
        for i, unit in enumerate(units)
        for n, v in unit
    ]


GST = {
    "000000": {
        "unit": {"index": 0, "offset": 4, "type": 7},
        "sps": {
            "level_idc": 30,
            "offset_for_ref_frame": [4, 5],
            "vui_parameters": {"sar_width": 1},
        },
    }
}
TRACE = cbs(
    [
        ("nal_unit_type", 7),
        ("level_idc", 30),
        ("offset_for_ref_frame[0]", 4),
        ("offset_for_ref_frame[1]", 5),
        ("sar_width", 1),
    ]
)


def test_selfcheck_accepts_agreement_and_counts_shared_fields() -> None:
    report = sc.check("h264", TRACE, GST, "x", [])
    assert report["problems"] == []
    assert report["compared"] == 4


def test_selfcheck_reports_an_undocumented_disagreement_once_per_member() -> None:
    trace = cbs([("nal_unit_type", 7), ("level_idc", 31), ("sar_width", 1)])
    report = sc.check("h264", trace, GST, "x", [])
    assert report["undocumented"] == {"h264.sps.level_idc": 1}
    assert "level_idc" in report["problems"][0]


def test_selfcheck_known_rows_document_a_disagreement(tmp_path: Path) -> None:
    known = tmp_path / "KNOWN_DIFFS.md"
    known.write_text("| `h264.sps.level_*` | GStreamer maps level 1b | jvt_* |\n", encoding="utf-8")
    trace = cbs([("nal_unit_type", 7), ("level_idc", 31)])
    assert sc.check("h264", trace, GST, "jvt_A", sc.load_known(known))["problems"] == []
    assert sc.check("h264", trace, GST, "jctvc_B", sc.load_known(known))["problems"]


def test_selfcheck_flags_unit_type_mismatch_and_gst_errors() -> None:
    trace = cbs([("nal_unit_type", 8)])
    assert "nal_unit_type" in sc.check("h264", trace, GST, "x", [])["problems"][0]
    broken = {"000000": {"unit": {"index": 0, "offset": 4, "type": 7}, "error": {"code": 3}}}
    assert "could not parse" in sc.check("h264", TRACE, broken, "x", [])["problems"][0]


def test_selfcheck_skips_ambiguous_names() -> None:
    gst = {
        "000000": {
            "unit": {"index": 0, "offset": 0, "type": 7},
            "sps": {"a": {"cpb_cnt": 1}, "b": {"cpb_cnt": 2}},
        }
    }
    report = sc.check("h264", cbs([("nal_unit_type", 7), ("cpb_cnt", 9)]), gst, "x", [])
    assert report["problems"] == [] and report["ambiguous"] == 2


# ---- the generator end to end with fake oracles -----------------------------------------------------------------


def fake_tool(path: Path, body: str) -> Path:
    path.write_text("#!/bin/sh\n" + body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


@pytest.fixture
def lab(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> dict:
    corpora = tmp_path / "corpora"
    for cid, name, data in (
        ("jvt_A", "a.264", h264_stream(8)),
        ("jctvc_B", "b.bit", h265_stream(8)),
    ):
        (corpora / cid).mkdir(parents=True)
        (corpora / cid / name).write_bytes(data)
        (corpora / cid / ".ok").write_text(json.dumps({"primary": name}), encoding="utf-8")
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"entries": MANIFEST["entries"][:3]}), encoding="utf-8")
    # ffmpeg: prints one trace unit per NAL start code it can see; the others are fixed.
    tools = tmp_path / "bin"
    tools.mkdir()
    trace = "[trace_headers @ 0x1] Packet: 10 bytes\\n[trace_headers @ 0x1] 0 forbidden_zero_bit 0 = 0\\n"
    monkeypatch.setenv("OVT_ORACLE_FFMPEG", str(fake_tool(tools / "ffmpeg", f'printf "{trace}" >&2\n')))
    gst = '{"unit":{"index":0,"offset":4,"type":0},"slice_header":{"first_mb_in_slice":0}}'
    monkeypatch.setenv("OVT_ORACLE_GST_PARSE_ORACLE", str(fake_tool(tools / "gst", f"echo '{gst}'\n")))
    monkeypatch.setenv("OVT_ORACLE_LDECOD", str(fake_tool(tools / "ldecod", "exit 3\n")))
    monkeypatch.setenv(
        "OVT_ORACLE_HM_CU_DUMP",
        str(fake_tool(tools / "hm", "echo '{\"frames\":[]}'\n")),
    )
    monkeypatch.setenv("OVT_ORACLE_FFMPEG_MVS", str(fake_tool(tools / "mvs", "echo framenum\n")))
    return {"manifest": manifest, "corpora": corpora, "out": tmp_path / "out"}


def test_main_writes_goldens_and_index(lab: dict, capsys: pytest.CaptureFixture) -> None:
    rc = gen.main(["--manifest", str(lab["manifest"]), "--out", str(lab["out"]), "--corpora-dir", str(lab["corpora"]),
                   "--oracles", "trace_headers,gstparse,hm", "--jobs", "1"])  # fmt: skip
    err = capsys.readouterr().err
    out: Path = lab["out"]
    assert (out / "h264" / "jvt_A.trace_headers.json").is_file(), err
    assert (out / "h264" / "jvt_A.gstparse.json").is_file()
    assert (out / "h265" / "jctvc_B.hm-grid.json").is_file()
    index = json.loads((out / "h26x-index.json").read_text(encoding="utf-8"))
    assert set(index["entries"]) == {"jvt_A", "jctvc_B"}
    assert index["entries"]["jvt_A"]["goldens"]["trace_headers"]["pictures"] == gen.HEADER_PICTURES
    assert "heif" not in json.dumps(index["entries"])
    assert rc in (0, 1)  # the fake trace has no nal_unit_type, so the self-check may complain; goldens still exist


def test_failed_oracle_is_reported_and_a_listed_skip_is_not(lab: dict, tmp_path: Path) -> None:
    base = ["--manifest", str(lab["manifest"]), "--out", str(lab["out"]), "--corpora-dir", str(lab["corpora"]),
            "--oracles", "jm", "--jobs", "1"]  # fmt: skip
    assert gen.main(base) == gen.EXIT_STEP
    index = json.loads((lab["out"] / "h26x-index.json").read_text(encoding="utf-8"))
    assert "ldecod exited 3" in index["entries"]["jvt_A"]["failed"]["jm"]
    skips = tmp_path / "skips.json"
    skips.write_text(json.dumps({"jvt_A:jm": "JM cannot decode the fake stream"}), encoding="utf-8")
    assert gen.main([*base, "--skips", str(skips)]) == gen.EXIT_OK
    index = json.loads((lab["out"] / "h26x-index.json").read_text(encoding="utf-8"))
    assert index["entries"]["jvt_A"]["skipped"]["jm"].startswith("JM cannot")


def test_missing_tools_exit_4(lab: dict, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("OVT_ORACLE_LDECOD", "")
    monkeypatch.setenv("PATH", "/nonexistent")
    rc = gen.main(["--manifest", str(lab["manifest"]), "--out", str(lab["out"]), "--corpora-dir", str(lab["corpora"]),
                   "--oracles", "jm"])  # fmt: skip
    assert rc == gen.EXIT_MISSING


def test_script_runs_in_isolated_mode() -> None:
    res = subprocess.run([sys.executable, "-I", str(GOLDENS / "gen_h26x.py"), "--help"], capture_output=True, text=True,
                         check=False, env={**os.environ})  # fmt: skip
    assert res.returncode == 0 and "--manifest" in res.stdout
