#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""MediaInfo trace XML -> ``box-tree`` offsets; ``--hdr``: MediaInfo JSON -> ``header-struct`` (§14.4.8).

Box mode reads ``mediainfo --Details=1 --Output=XML``. MediaInfo names a ``<block>`` descriptively
("Movie header"); the fourcc is in its first child, ``<block name="Header">`` holding ``<data
name="Name">``. Every block with such a header is a box: ``{path, offset, size, header_length}`` with
``header_length`` = the Header block's size (8, 16 with largesize; FullBox version/flags are outside it,
as in OVT's ``BoxNode``). Blocks without one (the "Second pass" NAL dump, "Matrix structure") are not
boxes; boxes inside them are not expected and are searched anyway. Offsets in the XML are decimal.
MediaInfo writes a non-ASCII fourcc byte as ``?`` (``?too`` for ``©too``); the normaliser keeps it.

``--hdr`` reads ``mediainfo --Output=JSON`` and prints ``{"hdr": {...}}`` for the first video track (or
``--track <ID>``) in ``ovt_ColorDescription`` units (backbone §0.5.6): ``primaries``/``transfer``/``matrix``
as H.273 codes and ``full_range`` via ``mediainfo_hdr_map.json``; ``mdcv_primaries`` [[x, y]] in R, G, B
order and ``mdcv_white_point`` in 0.00002 units (named sets such as "Display P3" from the map);
``mdcv_max/min_luminance`` in 0.0001 cd/m²; ``max_cll``/``max_fall`` in cd/m²; ``hdr_format``,
``hdr_format_profile`` and ``hdr_format_compatibility`` as MediaInfo prints them. Absent keys are left out.
"""

from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from decimal import Decimal
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

XY_RE = re.compile(r"^(R|G|B|White point): x=([\d.]+) y=([\d.]+)$")
LUM_RE = re.compile(r"^min: ([\d.]+) cd/m2, max: ([\d.]+) cd/m2$")
CDM2_RE = re.compile(r"^(\d+) cd/m2$")


def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def box_header(block: ET.Element) -> tuple[str, int] | None:
    """(fourcc, header length) when the block's first child is a box header."""
    first = next(iter(block), None)
    if first is None or local(first.tag) != "block" or first.get("name") != "Header":
        return None
    for data in first:
        if local(data.tag) == "data" and data.get("name") == "Name" and data.text and len(data.text) == 4:
            return data.text, canon.to_int(first.get("size", ""), "Header size")
    return None


def boxes(args: argparse.Namespace) -> list[dict[str, Any]]:
    try:
        root = ET.fromstring(canon.read_text(args.input))
    except ET.ParseError as exc:
        raise canon.NormalizeError(f"not XML: {exc}") from exc
    found: list[tuple[int, str, dict[str, Any]]] = []

    def walk(elem: ET.Element, depth: int) -> None:
        for block in elem:
            if local(block.tag) != "block":
                continue
            head = box_header(block)
            if head is None:
                walk(block, depth)
                continue
            record = {
                "offset": canon.to_int(block.get("offset", ""), "block offset"),
                "size": canon.to_int(block.get("size", ""), "block size"),
                "header_length": head[1],
            }
            found.append((depth, head[0], record))
            walk(block, depth + 1)

    media = [e for e in root if local(e.tag) == "media"]
    if local(root.tag) != "MediaTrace" or len(media) != 1:
        raise canon.NormalizeError("expected a MediaTrace document with one <media> element")
    walk(media[0], 0)
    return canon.box_paths(found)


def units(value: str, scale: int) -> int:
    return int((Decimal(value) * scale).to_integral_value())


def lookup(table: dict[str, int], value: str, what: str) -> int:
    if value not in table:
        raise canon.NormalizeError(f"{what} {value!r} is not in mediainfo_hdr_map.json")
    return table[value]


def hdr(args: argparse.Namespace) -> dict[str, Any]:
    maps = canon.load_map("mediainfo_hdr_map")
    tracks = canon.read_json(args.input).get("media", {}).get("track", [])
    video = [t for t in tracks if t.get("@type") == "Video" and (args.track is None or t.get("ID") == str(args.track))]
    if not video:
        raise canon.NormalizeError("no video track" + (f" with ID {args.track}" if args.track else ""))
    t = video[0]
    out: dict[str, Any] = {}
    for key, name in (
        ("colour_primaries", "primaries"),
        ("transfer_characteristics", "transfer"),
        ("matrix_coefficients", "matrix"),
        ("colour_range", "full_range"),
    ):
        if key in t:
            out[name] = lookup(maps[name], t[key], key)
    if "MasteringDisplay_ColorPrimaries" in t:
        text = t["MasteringDisplay_ColorPrimaries"]
        if text in maps["mdcv_named"]:
            named = maps["mdcv_named"][text]
            out["mdcv_primaries"], out["mdcv_white_point"] = named[:3], named[3]
        else:
            xy = {}
            for part in text.split(", "):
                m = XY_RE.match(part)
                if not m:
                    raise canon.NormalizeError(f"MasteringDisplay_ColorPrimaries {text!r} not understood")
                xy[m.group(1)] = [units(m.group(2), 50000), units(m.group(3), 50000)]
            if set(xy) != {"R", "G", "B", "White point"}:
                raise canon.NormalizeError(f"MasteringDisplay_ColorPrimaries {text!r} lacks a colour")
            out["mdcv_primaries"], out["mdcv_white_point"] = [xy["R"], xy["G"], xy["B"]], xy["White point"]
    if "MasteringDisplay_Luminance" in t:
        m = LUM_RE.match(t["MasteringDisplay_Luminance"])
        if not m:
            raise canon.NormalizeError(f"MasteringDisplay_Luminance {t['MasteringDisplay_Luminance']!r}")
        out["mdcv_min_luminance"], out["mdcv_max_luminance"] = units(m.group(1), 10000), units(m.group(2), 10000)
    for key, name in (("MaxCLL", "max_cll"), ("MaxFALL", "max_fall")):
        if key in t:
            m = CDM2_RE.match(t[key])
            if not m:
                raise canon.NormalizeError(f"{key} {t[key]!r} not understood")
            out[name] = int(m.group(1))
    for key in ("HDR_Format", "HDR_Format_Profile", "HDR_Format_Compatibility"):
        if key in t:
            out[key.lower()] = t[key]
    return {"hdr": out}


def convert(args: argparse.Namespace) -> Any:
    return hdr(args) if args.hdr else boxes(args)


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--hdr", action="store_true", help="the input is `mediainfo --Output=JSON` (HDR summary)")
    parser.add_argument("--track", type=int, help="--hdr: the video track ID (default: the first video track)")


if __name__ == "__main__":
    args = sys.argv[1:]
    sys.exit(canon.run("header-struct" if "--hdr" in args else "box-tree", __doc__.splitlines()[0], convert, add_args))
