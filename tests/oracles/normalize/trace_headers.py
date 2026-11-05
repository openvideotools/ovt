#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""FFmpeg ``trace_headers`` log -> ``syntax-elements`` (chapter 14 §14.4.1, WI-0.555).

Input: the stderr of ``ffmpeg -i <in> -map 0:v:0 -c:v copy -bsf:v trace_headers -f null -``. CBS
(``libavcodec/cbs.c`` ``ff_cbs_trace_read_log``) prints ``<position> <name>[<idx>] <bits> = <value>`` with
the position counted from the start of the unit after emulation-prevention removal. A new unit starts at
its first header field (``forbidden_zero_bit`` for H.26x, ``obu_forbidden_bit`` for AV1); unit indices
count the units CBS decomposes, in log order.

* H.264/HEVC: positions already count from the NAL header's first bit, as OVT's ``BitSpan`` does
  (chapter 7 ``Unit.rbsp`` = NAL header + payload), so they are kept.
* AV1: OVT counts from the first OBU payload bit (chapter 0 §0.5.1), so the OBU header and
  ``obu_size`` bits are subtracted (``8 × (1 + obu_extension_flag)`` + the leb128 bits). The header
  fields themselves keep their (negative) offsets so that nothing read is lost.
* The ``Extradata`` block (parameter sets from the container's codec configuration) and ``Side data``
  blocks are skipped by default: their units repeat in the first packet. ``--include-extradata`` keeps them.
* Elements with an empty bit string (inferred, not read) are dropped, as are the names in
  ``trace_headers_ignore.txt``. Names go through ``cbs_name_map.json``: ``rename`` maps CBS names to spec
  names, and every base name (subscripts removed) must be in ``known`` or ``rename``, otherwise exit 3.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

PREFIX_RE = re.compile(r"\[trace_headers @ 0x[0-9a-fA-F]+\] (.*)$")
ELEMENT_RE = re.compile(r"^\s*(\d+)\s+(\S+)\s+([01]*)\s*=\s*(-?\d+)\s*$")
SUBSCRIPT_RE = re.compile(r"\[\d+\]")
UNIT_START = {"h264": "forbidden_zero_bit", "h265": "forbidden_zero_bit", "av1": "obu_forbidden_bit"}
CODEC_ALIASES = {"avc": "h264", "hevc": "h265", "264": "h264", "265": "h265"}


def base_name(name: str) -> str:
    return SUBSCRIPT_RE.sub("", name)


def parse(text: str, include_extradata: bool) -> list[list[tuple[int, str, str, int]]]:
    """Split the log into units, each a list of (position, name, bits, value) in log order."""
    units: list[list[tuple[int, str, str, int]]] = []
    skipping = False
    for raw in text.splitlines():
        m = PREFIX_RE.search(raw)
        if not m:
            continue
        line = m.group(1).rstrip()
        if line == "Extradata" or line == "Side data:":
            skipping = not include_extradata
            continue
        if line.startswith("Packet: ") or line == "Payload:":
            skipping = False
            continue
        e = ELEMENT_RE.match(line)
        if not e or skipping:
            continue  # structure headers ("Sequence Parameter Set", "OBU header", ...) and other log lines
        pos, name, bits, value = int(e.group(1)), e.group(2), e.group(3), int(e.group(4))
        if name in UNIT_START.values() and pos == 0:
            units.append([])
        if not units:
            raise canon.NormalizeError(f"syntax element {name!r} before the first unit header")
        units[-1].append((pos, name, bits, value))
    return units


# SPEC-ASSUMPTION(#94): AV1 offsets count from the first OBU payload bit; OBU header fields stay negative.
def av1_payload_start(unit: list[tuple[int, str, str, int]]) -> int:
    """Bit position of the first OBU payload bit: after the OBU header and, if present, obu_size."""
    fields = {name: (pos, bits, value) for pos, name, bits, value in unit}
    if "obu_extension_flag" not in fields:
        raise canon.NormalizeError("AV1 unit without obu_extension_flag")
    start = 8 * (1 + fields["obu_extension_flag"][2])
    if fields.get("obu_has_size_field", (0, "", 0))[2]:
        pos, bits, _ = fields["obu_size"]
        start = pos + len(bits)
    return start


def convert(args: argparse.Namespace) -> list[dict[str, Any]]:
    codec = CODEC_ALIASES.get(args.codec, args.codec)
    names = canon.load_map("cbs_name_map")
    rename: dict[str, str] = names["rename"].get(codec, {})
    known = set(names["known"].get(codec, [])) | set(rename)
    ignore = canon.load_list("trace_headers_ignore.txt")
    out: list[dict[str, Any]] = []
    unknown: set[str] = set()
    for index, unit in enumerate(parse(canon.read_text(args.input), args.include_extradata)):
        shift = av1_payload_start(unit) if codec == "av1" else 0
        for pos, name, bits, value in unit:
            base = base_name(name)
            if not bits or base in ignore:
                continue
            if base not in known:
                unknown.add(base)
                continue
            spec = rename.get(base, base) + name[len(base) :]
            out.append(
                {"unit": index, "name": spec, "bit_offset": pos - shift, "bit_length": len(bits), "value": value}
            )
    if unknown:
        raise canon.NormalizeError(f"CBS names not in cbs_name_map.json ({codec}): {', '.join(sorted(unknown))}")
    out.sort(key=lambda e: (e["unit"], e["bit_offset"]))
    return out


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--codec", required=True, choices=sorted(UNIT_START) + sorted(CODEC_ALIASES))
    parser.add_argument(
        "--include-extradata", action="store_true", help="also emit the units of the Extradata / Side data blocks"
    )


if __name__ == "__main__":
    sys.exit(canon.run("syntax-elements", __doc__.splitlines()[0], convert, add_args))
