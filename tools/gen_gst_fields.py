#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate tests/oracles/gst_fields.h from the GStreamer codecparsers headers (chapter 14 §14.4.2, WI-0.562).

The header lists, for every parsed structure that ``gst_parse_oracle`` prints (H.264 SPS/PPS/slice header, HEVC
VPS/SPS/PPS/slice header, AV1 OBU header/sequence header/frame header/tile group), every member that carries a
number: its JSON name, its C offset, its integer kind and its array shape. The C file walks these tables; it
names no member itself, so a GStreamer upgrade is a regeneration, not an edit.

Rules (one regex-free pass over the headers, comments stripped, braces matched):
  * scalar integer / gboolean / enum members become leaves; nested structs are entered and become nested JSON
    objects (``vui_parameters.sar_width``);
  * fixed arrays of any rank become nested JSON arrays; an array of structs becomes one array per leaf member
    (a "struct of arrays"), because the normaliser has to re-key them to the spec names anyway;
  * a union contributes its first member under the union's own name (the other members share its bytes);
  * pointers, GArray and other non-numeric members are skipped and listed on stderr with --verbose;
  * ``LIMITS`` trims the first dimension of an array to the count GStreamer stores next to it (JSON arrays then
    hold only the parsed entries); ``RENAMES`` is the hand-maintained map from GStreamer member names to spec
    names for members that differ. Every other name is the GStreamer member name, which is the spec name in
    nearly all cases.

``tests/oracles/normalize/gst_ignore.txt`` lists the members that GStreamer derives instead of parsing; this tool
checks that every line of it names a member that exists, so a rename in GStreamer cannot leave a stale entry.

Usage:
  gen_gst_fields.py [--include-dir DIR] [--output FILE] [--ignore FILE] [--check] [--verbose]
``--check`` regenerates in memory and exits 1 when the committed file differs.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = REPO / "tests" / "oracles" / "gst_fields.h"
DEFAULT_IGNORE = REPO / "tests" / "oracles" / "normalize" / "gst_ignore.txt"

# (codec, key printed by the oracle, C struct) in output order.
ROOTS = [
    ("h264", "sps", "GstH264SPS", "gsth264parser.h"),
    ("h264", "pps", "GstH264PPS", "gsth264parser.h"),
    ("h264", "slice_header", "GstH264SliceHdr", "gsth264parser.h"),
    ("h265", "vps", "GstH265VPS", "gsth265parser.h"),
    ("h265", "sps", "GstH265SPS", "gsth265parser.h"),
    ("h265", "pps", "GstH265PPS", "gsth265parser.h"),
    ("h265", "slice_header", "GstH265SliceHdr", "gsth265parser.h"),
    ("av1", "obu_header", "GstAV1OBUHeader", "gstav1parser.h"),
    ("av1", "sequence_header", "GstAV1SequenceHeaderOBU", "gstav1parser.h"),
    ("av1", "frame_header", "GstAV1FrameHeaderOBU", "gstav1parser.h"),
    ("av1", "tile_group", "GstAV1TileGroupOBU", "gstav1parser.h"),
]

KINDS = {
    "guint8": "GF_U8", "gint8": "GF_I8", "guint16": "GF_U16", "gint16": "GF_I16",
    "guint32": "GF_U32", "gint32": "GF_I32", "guint": "GF_U32", "gint": "GF_I32",
    "guint64": "GF_U64", "gint64": "GF_I64", "gboolean": "GF_I32", "uint8_t": "GF_U8",
}  # fmt: skip

# JSON names that differ from the GStreamer member names: (root key as above, JSON path) -> spec name.
# Only the leaf name changes; the path to it is kept.
RENAMES = {
    ("h264", "sps", "id"): "seq_parameter_set_id",
    ("h264", "sps", "num_ref_frames"): "max_num_ref_frames",
    ("h264", "pps", "id"): "pic_parameter_set_id",
    ("h264", "pps", "pic_order_present_flag"): "bottom_field_pic_order_in_frame_present_flag",
    ("h264", "slice_header", "type"): "slice_type",
    ("h264", "slice_header", "qp_delta"): "slice_qp_delta",
    ("h265", "slice_header", "type"): "slice_type",
    ("h265", "slice_header", "qp_delta"): "slice_qp_delta",
    ("h265", "slice_header", "cb_qp_offset"): "slice_cb_qp_offset",
    ("h265", "slice_header", "cr_qp_offset"): "slice_cr_qp_offset",
    ("h265", "slice_header", "temporal_mvp_enabled_flag"): "slice_temporal_mvp_enabled_flag",
    ("h265", "slice_header", "sao_luma_flag"): "slice_sao_luma_flag",
    ("h265", "slice_header", "sao_chroma_flag"): "slice_sao_chroma_flag",
    ("h265", "slice_header", "segment_address"): "slice_segment_address",
    ("h265", "slice_header", "beta_offset_div2"): "slice_beta_offset_div2",
    ("h265", "slice_header", "tc_offset_div2"): "slice_tc_offset_div2",
}  # fmt: skip

# Arrays whose first dimension is trimmed to a stored count: (root key, JSON path of the array or of a struct
# array containing it) -> (JSON path of the count member, constant added to the count).
LIMITS = {
    ("h264", "sps", "offset_for_ref_frame"): ("num_ref_frames_in_pic_order_cnt_cycle", 0),
    ("h264", "pps", "run_length_minus1"): ("num_slice_groups_minus1", 1),
    ("h264", "pps", "top_left"): ("num_slice_groups_minus1", 1),
    ("h264", "pps", "bottom_right"): ("num_slice_groups_minus1", 1),
    ("h264", "slice_header", "ref_pic_list_modification_l0"): ("n_ref_pic_list_modification_l0", 0),
    ("h264", "slice_header", "ref_pic_list_modification_l1"): ("n_ref_pic_list_modification_l1", 0),
    ("h264", "slice_header", "dec_ref_pic_marking.ref_pic_marking"): ("dec_ref_pic_marking.n_ref_pic_marking", 0),
    ("h265", "slice_header", "lt_idx_sps"): ("num_long_term_sps", 0),
    ("h265", "pps", "column_width_minus1"): ("num_tile_columns_minus1", 0),
    ("h265", "pps", "row_height_minus1"): ("num_tile_rows_minus1", 0),
    ("av1", "sequence_header", "operating_points"): ("operating_points_cnt_minus_1", 1),
    ("av1", "frame_header", "tile_info.width_in_sbs_minus_1"): ("tile_info.tile_cols", 0),
    ("av1", "frame_header", "tile_info.height_in_sbs_minus_1"): ("tile_info.tile_rows", 0),
    ("av1", "frame_header", "tile_info.mi_col_starts"): ("tile_info.tile_cols", 1),
    ("av1", "frame_header", "tile_info.mi_row_starts"): ("tile_info.tile_rows", 1),
    ("av1", "tile_group", "entry"): ("num_tiles", 0),
}  # fmt: skip
# HRD and prediction-weight arrays repeat under several parents; matched by the last path components.
SUFFIX_LIMITS = {
    ("h264", "bit_rate_value_minus1"): ("cpb_cnt_minus1", 1),
    ("h264", "cpb_size_value_minus1"): ("cpb_cnt_minus1", 1),
    ("h264", "cbr_flag"): ("cpb_cnt_minus1", 1),
}  # fmt: skip
for _list in ("0", "1"):
    for _name in ("luma_weight", "luma_offset", "chroma_weight", "chroma_offset"):
        SUFFIX_LIMITS[("h264", f"pred_weight_table.{_name}_l{_list}")] = (
            f"num_ref_idx_l{_list}_active_minus1",
            1,
        )


@dataclass
class Member:
    """One declarator of a struct body."""

    name: str
    ctype: str  # type name, or "struct"/"union" for an inline definition
    dims: list[str] = field(
        default_factory=list
    )  # array dimensions, outermost first (text only)
    pointer: bool = False
    body: list[Member] | None = None  # inline struct/union members


@dataclass
class Leaf:
    json_path: list[str]
    c_path: str  # member path with [0] at every array level
    kind: str
    # per array level: C expressions of element count and element stride
    counts: list[str]
    strides: list[str]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    # Preprocessor lines: both arms of a conditional are kept, which only matters for deprecated members.
    return re.sub(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", " ", text, flags=re.M)


def match_brace(text: str, open_at: int) -> int:
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    raise ValueError("unbalanced braces")


def parse_body(body: str) -> list[Member]:
    """Members of a struct body (the text between the braces)."""
    members: list[Member] = []
    i = 0
    n = len(body)
    while i < n:
        # Declaration = text up to ';' at brace depth 0; an inline struct/union carries its own braces.
        j = i
        depth = 0
        while j < n and not (body[j] == ";" and depth == 0):
            if body[j] == "{":
                depth += 1
            elif body[j] == "}":
                depth -= 1
            j += 1
        decl = body[i:j].strip()
        i = j + 1
        if not decl:
            continue
        inline = re.match(r"^(struct|union)\s*(\w+\s*)?\{", decl)
        if inline:
            open_at = decl.index("{")
            close_at = match_brace(decl, open_at)
            inner = parse_body(decl[open_at + 1 : close_at])
            for declarator in decl[close_at + 1 :].split(","):
                if not declarator.strip():
                    continue  # a tag-only definition declares no member
                name, dims, ptr = parse_declarator(declarator)
                members.append(Member(name, inline.group(1), dims, ptr, inner))
            continue
        m = re.match(r"^((?:const\s+)?[A-Za-z_]\w*)\s+(.*)$", decl, flags=re.S)
        if not m:
            raise ValueError(f"cannot parse declaration: {decl!r}")
        ctype = m.group(1).replace("const ", "")
        for declarator in m.group(2).split(","):
            name, dims, ptr = parse_declarator(declarator)
            members.append(Member(name, ctype, dims, ptr))
    return members


def parse_declarator(text: str) -> tuple[str, list[str], bool]:
    text = text.strip()
    pointer = text.startswith("*") or "*" in text.split("[")[0]
    m = re.match(r"^\**\s*(\w+)\s*((?:\[[^\]]*\]\s*)*)$", text)
    if not m:
        raise ValueError(f"cannot parse declarator: {text!r}")
    dims = re.findall(r"\[([^\]]*)\]", m.group(2))
    return m.group(1), [d.strip() for d in dims], pointer


def collect_structs(include_dir: Path) -> dict[str, list[Member]]:
    """All ``struct _Name {...};`` definitions of the codecparsers headers, keyed by the typedef name."""
    structs: dict[str, list[Member]] = {}
    for header in sorted(include_dir.glob("gst*parser.h")):
        text = strip_comments(header.read_text(encoding="utf-8"))
        for m in re.finditer(r"\bstruct\s+_(\w+)\s*\{", text):
            end = match_brace(text, m.end() - 1)
            structs[m.group(1)] = parse_body(text[m.end() : end])
    return structs


def flatten(
    structs: dict[str, list[Member]], root: str, skipped: list[str]
) -> list[Leaf]:
    leaves: list[Leaf] = []

    def walk(
        members: list[Member],
        json_prefix: list[str],
        c_prefix: str,
        counts: list[str],
        strides: list[str],
    ) -> None:
        for mem in members:
            if mem.pointer:
                skipped.append(".".join([*json_prefix, mem.name]) + " (pointer)")
                continue
            c_here = f"{c_prefix}.{mem.name}" if c_prefix else mem.name
            # Array levels of this member: prefix with every earlier index at 0.
            lvl_counts, lvl_strides = list(counts), list(strides)
            c_expr = c_here
            for depth, _dim in enumerate(mem.dims):
                sub = c_here + "[0]" * depth
                nxt = sub + "[0]"
                lvl_counts.append(f"GF_N({root}, {sub})")
                lvl_strides.append(f"GF_SZ({root}, {nxt})")
                c_expr = nxt
            if mem.dims:
                c_expr = c_here + "[0]" * len(mem.dims)
            c_next = c_expr
            if mem.ctype == "union":
                # The first member stands for the union; the others overlay the same bytes.
                first = mem.body[0] if mem.body else None
                if (
                    first is None
                    or first.ctype not in KINDS
                    or first.dims
                    or first.pointer
                ):
                    skipped.append(".".join([*json_prefix, mem.name]) + " (union)")
                else:
                    leaves.append(Leaf([*json_prefix, mem.name], f"{c_next}.{first.name}", KINDS[first.ctype],
                                       lvl_counts, lvl_strides))  # fmt: skip
            elif mem.ctype == "struct":
                assert mem.body is not None
                walk(
                    mem.body, [*json_prefix, mem.name], c_next, lvl_counts, lvl_strides
                )
            elif mem.ctype in KINDS:
                leaves.append(
                    Leaf(
                        [*json_prefix, mem.name],
                        c_next,
                        KINDS[mem.ctype],
                        lvl_counts,
                        lvl_strides,
                    )
                )
            elif mem.ctype in structs:
                walk(
                    structs[mem.ctype],
                    [*json_prefix, mem.name],
                    c_next,
                    lvl_counts,
                    lvl_strides,
                )
            elif (
                re.match(r"^Gst\w+$", mem.ctype)
                and mem.ctype not in structs
                and mem.ctype != "GArray"
            ):
                # An enum typedef of the same header family (GstAV1FrameType, ...): printed as a signed integer.
                leaves.append(
                    Leaf(
                        [*json_prefix, mem.name],
                        c_next,
                        "GF_I32",
                        lvl_counts,
                        lvl_strides,
                    )
                )
            else:
                skipped.append(".".join([*json_prefix, mem.name]) + f" ({mem.ctype})")

    walk(structs[root], [], "", [], [])
    return leaves


def find_limit(
    codec: str, key: str, json_path: list[str]
) -> tuple[list[str], int] | None:
    """The stored count that trims the first array dimension of the leaf at json_path, if one is declared."""
    for cut in range(len(json_path), 0, -1):
        dotted = ".".join(json_path[:cut])
        hit = LIMITS.get((codec, key, dotted))
        if hit:
            return hit[0].split("."), hit[1]
        for (c, suffix), (count, add) in SUFFIX_LIMITS.items():
            if c == codec and (dotted == suffix or dotted.endswith("." + suffix)):
                parent = dotted[: -len(suffix)].rstrip(".")
                # Prediction-weight counts live in the slice header, HRD counts next to the array.
                path = (
                    count
                    if "." in suffix
                    else (f"{parent}.{count}" if parent else count)
                )
                return path.split("."), add
    return None


def render(
    include_dir: Path, version: str, verbose: bool
) -> tuple[str, dict[tuple[str, str], list[str]]]:
    structs = collect_structs(include_dir)
    out: list[str] = []
    w = out.append
    w("/* SPDX-FileCopyrightText: 2026 The Open Video Tools contributors */")
    w("/* SPDX-License-Identifier: LGPL-2.1-or-later */")
    w("/*")
    w(
        " * GENERATED by tools/gen_gst_fields.py from the GStreamer codecparsers headers; do not edit."
    )
    w(
        f" * GStreamer {version}. Regenerate: python3 tools/gen_gst_fields.py (CI: --check)."
    )
    w(" *")
    w(
        " * One table per parsed structure (chapter 14 §14.4.2): JSON name, C offset, integer kind and array shape of"
    )
    w(" * every numeric member. Arrays of structs are printed as one array per member.")
    w(" */")
    w("/* clang-format off */")
    w("#ifndef OVT_ORACLES_GST_FIELDS_H")
    w("#define OVT_ORACLES_GST_FIELDS_H")
    w("")
    w("#ifndef GST_USE_UNSTABLE_API")
    w(
        "#define GST_USE_UNSTABLE_API  /* the codecparsers API is unstable; the version is pinned (ORACLE_VERSIONS.md) */"
    )
    w("#endif")
    w("")
    w("#include <stddef.h>")
    w("#include <stdint.h>")
    w("")
    w("#include <gst/codecparsers/gsth264parser.h>")
    w("#include <gst/codecparsers/gsth265parser.h>")
    w("#include <gst/codecparsers/gstav1parser.h>")
    w("")
    w(
        "typedef enum { GF_U8, GF_I8, GF_U16, GF_I16, GF_U32, GF_I32, GF_U64, GF_I64 } GfKind;"
    )
    w("")
    w("#define GF_MAX_RANK 4")
    w("#define GF_NO_LIMIT (-1)")
    w("")
    w("typedef struct {")
    w(
        "  const char *name;     /* dotted JSON path; the C file splits it into nested objects */"
    )
    w("  size_t offset;        /* offset of element [0]...[0] */")
    w("  GfKind kind;")
    w("  int rank;             /* number of array dimensions, 0 for a scalar */")
    w("  size_t count[GF_MAX_RANK];   /* element counts, outermost first */")
    w("  size_t stride[GF_MAX_RANK];  /* byte strides, outermost first */")
    w(
        "  int limit;            /* index in the same table of the member that trims count[0], or GF_NO_LIMIT */"
    )
    w("  int limit_add;        /* added to that member's value */")
    w("} GfField;")
    w("")
    w("#define GF_N(T, p) (sizeof(((T *)0)->p) / sizeof(((T *)0)->p[0]))")
    w("#define GF_SZ(T, p) (sizeof(((T *)0)->p))")
    w("")
    names: dict[tuple[str, str], list[str]] = {}
    for codec, key, struct, _header in ROOTS:
        skipped: list[str] = []
        leaves = flatten(structs, struct, skipped)
        # Limits are resolved on the GStreamer names; renaming comes last.
        by_orig = {".".join(le.json_path): i for i, le in enumerate(leaves)}
        limits: list[tuple[int, int]] = []
        for le in leaves:
            found = find_limit(codec, key, le.json_path)
            if found is None:
                limits.append((-1, 0))
                continue
            cpath = ".".join(found[0])
            if cpath not in by_orig:
                raise SystemExit(
                    f"{struct}: count member {cpath} for {'.'.join(le.json_path)} not found"
                )
            if leaves[by_orig[cpath]].counts:
                raise SystemExit(f"{struct}: count member {cpath} is an array")
            limits.append((by_orig[cpath], found[1]))
        for le in leaves:
            renamed = RENAMES.get((codec, key, ".".join(le.json_path)))
            if renamed:
                le.json_path = [*le.json_path[:-1], renamed]
        table = f"gst_{codec}_{key}_fields"
        names[(codec, key)] = [".".join(le.json_path) for le in leaves]
        if len(set(names[(codec, key)])) != len(leaves):
            raise SystemExit(f"{struct}: duplicate JSON names after renaming")
        w(f'/* {struct} -> "{key}" ({codec}) */')
        w(f"static const GfField {table}[] = {{")
        for le, (limit_idx, limit_add) in zip(leaves, limits, strict=True):
            rank = len(le.counts)
            if rank > 4:
                raise SystemExit(
                    f"{struct}: rank {rank} > 4 at {'.'.join(le.json_path)}"
                )
            counts = ", ".join(le.counts + ["0"] * (4 - rank))
            strides = ", ".join(le.strides + ["0"] * (4 - rank))
            w(
                f'  {{"{".".join(le.json_path)}", offsetof({struct}, {le.c_path}), {le.kind}, {rank}, '
                f"{{{counts}}}, {{{strides}}}, {limit_idx}, {limit_add}}},"
            )
        w("};")
        w(
            f"#define GST_{codec.upper()}_{key.upper()}_FIELD_COUNT (sizeof({table}) / sizeof({table}[0]))"
        )
        w("")
        if verbose:
            for sk in skipped:
                print(f"{struct}: skipped {sk}", file=sys.stderr)
    w("/* clang-format on */")
    w("#endif /* OVT_ORACLES_GST_FIELDS_H */")
    return "\n".join(out) + "\n", names


def check_ignore(path: Path, names: dict[tuple[str, str], list[str]]) -> list[str]:
    """Lines of the ignore file must be ``<codec>.<key>.<json path>`` of members that exist."""
    problems: list[str] = []
    if not path.exists():
        return [f"{path}: missing"]
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split(".", 2)
        if (
            len(parts) != 3
            or (parts[0], parts[1]) not in names
            or parts[2] not in names[(parts[0], parts[1])]
        ):
            problems.append(f"{path}:{lineno}: '{line}' names no member")
    return problems


def gst_version(include_dir: Path) -> str:
    try:
        return subprocess.run(  # noqa: S603
            ["pkg-config", "--modversion", "gstreamer-codecparsers-1.0"],  # noqa: S607
            check=True, capture_output=True, text=True,
        ).stdout.strip()  # fmt: skip
    except (OSError, subprocess.CalledProcessError):
        text = (include_dir.parent.parent / "gstversion.h").read_text(encoding="utf-8")
        parts = [
            re.search(rf"GST_VERSION_{p}\s+\((\d+)\)", text)
            for p in ("MAJOR", "MINOR", "MICRO")
        ]
        return ".".join(m.group(1) if m else "?" for m in parts)


def default_include_dir() -> Path:
    try:
        inc = subprocess.run(  # noqa: S603
            ["pkg-config", "--variable=includedir", "gstreamer-codecparsers-1.0"],  # noqa: S607
            check=True, capture_output=True, text=True,
        ).stdout.strip()  # fmt: skip
    except (OSError, subprocess.CalledProcessError):
        inc = "/usr/include"
    return Path(inc) / "gstreamer-1.0" / "gst" / "codecparsers"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "--include-dir",
        type=Path,
        default=None,
        help="directory holding gsth264parser.h",
    )
    ap.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    ap.add_argument("--ignore", type=Path, default=DEFAULT_IGNORE)
    ap.add_argument(
        "--check",
        action="store_true",
        help="exit 1 if --output is not what the headers generate",
    )
    ap.add_argument(
        "--verbose",
        action="store_true",
        help="list the skipped (non-numeric) members on stderr",
    )
    args = ap.parse_args(argv)
    include_dir = args.include_dir or default_include_dir()
    if not (include_dir / "gsth264parser.h").is_file():
        print(f"gen_gst_fields: {include_dir}/gsth264parser.h not found (install libgstreamer-plugins-bad1.0-dev)",
              file=sys.stderr)  # fmt: skip
        return 3
    text, names = render(include_dir, gst_version(include_dir), args.verbose)
    problems = check_ignore(args.ignore, names)
    for p in problems:
        print(f"gen_gst_fields: {p}", file=sys.stderr)
    if problems:
        return 4
    if args.check:
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
            print(
                f"gen_gst_fields: {args.output} is stale; run tools/gen_gst_fields.py",
                file=sys.stderr,
            )
            return 1
        return 0
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
