# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/gen_gst_fields.py (chapter 14 §14.4.2; WI-0.562)."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

import gen_gst_fields as gen  # noqa: E402

HEADER = """
#ifndef X
#define X
/* a comment { with a brace */
struct _Inner { guint8 a; gint32 b[3]; };
struct _Outer
{
  gint id;                 // trailing comment
  guint8 flags[2][4];
  Inner nested;
  Inner list[5];
  guint8 *ptr;
  union { guint32 first; guint16 second; } value;
  struct { guint8 x, y; } pair;
  GstSomeEnum mode;
  guint8 count;
  guint16 dims[GST_MAX_THING + 1];
#ifndef GST_REMOVE_DEPRECATED
  void *priv;
#endif
};
#endif
"""


def _structs(tmp_path: Path) -> dict:
    (tmp_path / "gstfake_parser.h").write_text(HEADER, encoding="utf-8")
    return gen.collect_structs(tmp_path)


def test_parse_structs_strips_comments_and_preprocessor(tmp_path: Path) -> None:
    structs = _structs(tmp_path)
    assert set(structs) == {"Inner", "Outer"}
    names = [m.name for m in structs["Outer"]]
    assert names[:3] == ["id", "flags", "nested"]
    assert "x" not in names  # inline struct members stay inside the pair member
    assert next(m for m in structs["Outer"] if m.name == "flags").dims == ["2", "4"]
    assert next(m for m in structs["Outer"] if m.name == "dims").dims == ["GST_MAX_THING + 1"]


def test_flatten_shapes(tmp_path: Path) -> None:
    structs = _structs(tmp_path)
    skipped: list[str] = []
    leaves = {".".join(le.json_path): le for le in gen.flatten(structs, "Outer", skipped)}
    assert leaves["id"].kind == "GF_I32" and leaves["id"].counts == []
    flags = leaves["flags"]
    assert flags.c_path == "flags[0][0]" and len(flags.counts) == 2
    assert flags.counts[1] == "GF_N(Outer, flags[0])" and flags.strides[0] == "GF_SZ(Outer, flags[0])"
    assert leaves["nested.b"].c_path == "nested.b[0]"
    # An array of structs becomes one array per member, with the struct stride first.
    assert leaves["list.a"].c_path == "list[0].a" and leaves["list.a"].strides == ["GF_SZ(Outer, list[0])"]
    assert leaves["list.b"].counts[1] == "GF_N(Outer, list[0].b)"
    # A union stands for its first member; a pointer is skipped; an inline struct is entered; an enum is an int.
    assert leaves["value"].c_path == "value.first" and leaves["value"].kind == "GF_U32"
    assert "ptr (pointer)" in skipped
    assert {"pair.x", "pair.y"} <= set(leaves)
    assert leaves["mode"].kind == "GF_I32"
    assert "priv (pointer)" in skipped


def test_limits_resolve_to_the_count_member() -> None:
    assert gen.find_limit("h264", "sps", ["offset_for_ref_frame"]) == (
        ["num_ref_frames_in_pic_order_cnt_cycle"],
        0,
    )
    assert gen.find_limit(
        "h264",
        "slice_header",
        [
            "dec_ref_pic_marking",
            "ref_pic_marking",
            "memory_management_control_operation",
        ],
    ) == (["dec_ref_pic_marking", "n_ref_pic_marking"], 0)
    # HRD arrays are trimmed by the count next to them, wherever the HRD structure sits.
    assert gen.find_limit("h264", "sps", ["vui_parameters", "nal_hrd_parameters", "cbr_flag"]) == (
        ["vui_parameters", "nal_hrd_parameters", "cpb_cnt_minus1"],
        1,
    )
    assert gen.find_limit("h264", "sps", ["profile_idc"]) is None


def test_ignore_file_names_existing_members(tmp_path: Path) -> None:
    names = {("h264", "sps"): ["width", "vui_parameters.par_n"]}
    good = tmp_path / "good.txt"
    good.write_text(
        "# c\nh264.sps.width  # trailing\nh264.sps.vui_parameters.par_n\n",
        encoding="utf-8",
    )
    assert gen.check_ignore(good, names) == []
    bad = tmp_path / "bad.txt"
    bad.write_text("h264.sps.nope\nh999.sps.width\n", encoding="utf-8")
    assert len(gen.check_ignore(bad, names)) == 2


def test_committed_header_is_current() -> None:
    """With the GStreamer headers installed the committed gst_fields.h must equal a fresh generation."""
    include = gen.default_include_dir()
    if not (include / "gsth264parser.h").is_file():
        pytest.skip("gstreamer codecparsers headers not installed")
    version = gen.gst_version(include)
    committed = gen.DEFAULT_OUTPUT.read_text(encoding="utf-8")
    if f"GStreamer {version}." not in committed:
        pytest.skip(f"gst_fields.h was generated from another GStreamer than {version}")
    assert gen.main(["--check"]) == 0
