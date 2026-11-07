#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""GPAC ``MP4Box -diso -std`` XML -> ``box-tree`` (chapter 14 §14.4.6, WI-0.555).

Every XML element with ``Size`` and ``Type`` attributes is a box; nesting gives the path. ``-diso`` prints no
file offsets, so entries carry ``path``, ``size`` and ``fields`` only. ``Type`` is GPAC's
``gf_4cc_to_str_safe`` form: a byte outside 0x20..0x7E is written as two hex digits (``A9too`` = ``©too``).

Fields go through ``gpac_field_map.json``:

* ``box``: per fourcc (``*`` for all boxes), attribute name -> spec name or ``[spec name, conversion]``
  (conversions in ``canon.field_value``). Attributes the map does not name are dropped.
* ``records``: child elements that are not boxes but whose attributes belong to the box
  (``AVCDecoderConfigurationRecord``); their attributes go through the same box map.
* ``entries``: child elements that are array members (``TimeToSampleEntry``): ``{"array": name,
  "fields": {...}}`` appends an object, ``{"array": name, "value": attribute}`` appends a scalar.

``UnknownBox`` elements keep ``path`` and ``size`` only (§14.4.6: compared on fourcc + size).
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

SKIP_ATTRS = {"Size", "Type", "Specification", "Container"}


def local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def gpac_fourcc(text: str) -> str:
    """Undo gf_4cc_to_str_safe: 4 bytes, each printable ASCII or two uppercase hex digits."""

    def walk(rest: str, got: bytes) -> bytes | None:
        if len(got) == 4:
            return got if not rest else None
        if rest and 0x20 <= ord(rest[0]) <= 0x7E:
            found = walk(rest[1:], got + rest[0].encode("latin-1"))
            if found is not None:
                return found
        if len(rest) >= 2 and all(c in "0123456789ABCDEF" for c in rest[:2]):
            byte = int(rest[:2], 16)
            if not 0x20 <= byte <= 0x7E:
                return walk(rest[2:], got + bytes([byte]))
        return None

    raw = text.encode("latin-1") if len(text) == 4 else walk(text, b"")
    if raw is None:
        raise canon.NormalizeError(f"box Type {text!r} is not a GPAC fourcc")
    return raw.decode("latin-1")


def convert(args: argparse.Namespace) -> list[dict[str, Any]]:
    try:
        root = ET.fromstring(canon.read_text(args.input))
    except ET.ParseError as exc:
        raise canon.NormalizeError(f"not XML: {exc}") from exc
    maps = canon.load_map("gpac_field_map")
    boxes: list[tuple[int, str, dict[str, Any]]] = []

    def collect(elem: ET.Element, raw: list[tuple[str, str]], fields: dict[str, Any], fourcc: str, depth: int) -> None:
        """Walk the non-box children of a box: records, entries, and boxes nested at the next depth."""
        for child in elem:
            name = local(child.tag)
            if "Type" in child.attrib and "Size" in child.attrib:
                visit(child, depth + 1)
            elif name in maps["records"]:
                raw.extend(child.attrib.items())
                collect(child, raw, fields, fourcc, depth)
            elif name in maps["entries"]:
                spec = maps["entries"][name]
                if "value" in spec:
                    value = canon.field_value(child.attrib.get(spec["value"], ""), spec.get("conv", "auto"))
                else:
                    value = canon.map_fields(child.attrib.items(), spec["fields"], args.keep_unmapped)
                fields.setdefault(spec["array"], []).append(value)
            else:
                collect(child, raw, fields, fourcc, depth)  # wrappers GPAC adds around boxes

    def visit(elem: ET.Element, depth: int) -> None:
        fourcc = gpac_fourcc(elem.attrib["Type"])
        record: dict[str, Any] = {"size": canon.to_int(elem.attrib["Size"], f"{fourcc} Size")}
        boxes.append((depth, fourcc, record))
        raw = [(k, v) for k, v in elem.attrib.items() if k not in SKIP_ATTRS]
        entries: dict[str, Any] = {}
        collect(elem, raw, entries, fourcc, depth)
        if local(elem.tag) == "UnknownBox":
            return
        fields = {**canon.map_fields(raw, canon.box_map(maps["box"], fourcc), args.keep_unmapped), **entries}
        if fields:
            record["fields"] = fields

    if local(root.tag) != "IsoMediaFile":
        raise canon.NormalizeError(f"root element is {local(root.tag)!r}, expected IsoMediaFile")
    for child in root:
        if "Type" in child.attrib and "Size" in child.attrib:
            visit(child, 0)
    return canon.box_paths(boxes)


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--keep-unmapped", action="store_true", help="also emit unmapped attributes as ?<name>")


if __name__ == "__main__":
    sys.exit(canon.run("box-tree", __doc__.splitlines()[0], convert, add_args))
