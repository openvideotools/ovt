#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""``mp4ff-info -l all:1`` text -> ``box-tree`` (chapter 14 §14.4.7, WI-0.555).

mp4ff prints one ``[<fourcc>] size=<n>[ version=<v> flags=<hex>]`` line per box, indented two spaces per
level, followed by ``- <name>: <value>`` lines for its fields. There are no offsets. Field lines take
three shapes:

* ``- name: value``; a repeated name collects into a list when the map says ``name[]``. Values in quotes
  lose them; ``flags``-style values print hex followed by a decoded ``(...)``, which ``hex`` drops.
* ``- Width: 64.0, Height: 64.0``: several pairs on one line are split at ``", "`` when every part is a
  pair. ``split`` in the map handles ``- hSpacing:vSpacing: 1:1``.
* ``- sample[1]: size=2314 compositionTimeOffset=512`` and ``- 1: time=512 moofOffset=797``: array rows,
  mapped through ``entries`` (per fourcc) into ``fields[<array>]`` objects.

Names go through ``mp4ff_field_map.json`` (``box`` per fourcc, ``*`` for all); unmapped names are dropped.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

BOX_RE = re.compile(r"^( *)\[(.{4})\] size=(\d+)(?: version=(\d+) flags=([0-9a-fA-F]+))?")
FIELD_RE = re.compile(r"^ *- (.*)$")
BARE_RE = re.compile(r"^ +([A-Za-z]\w*: .*)$")
ROW_RE = re.compile(r"^(?:\w+\[\d+\]|\d+)$")
PAIR_RE = re.compile(r"^[A-Za-z][\w ]*: ")
HEX_RE = re.compile(r"^ +(?:[0-9a-fA-F]{2})+$")
KV_RE = re.compile(r"(\w+)=(\S+)")


def unquote(value: str) -> str:
    return value[1:-1] if len(value) >= 2 and value[0] == value[-1] == '"' else value


def convert(args: argparse.Namespace) -> list[dict[str, Any]]:
    maps = canon.load_map("mp4ff_field_map")
    boxes: list[tuple[int, str, dict[str, Any]]] = []
    raw: list[list[tuple[str, str]]] = []
    rows: list[dict[str, Any]] = []
    for n, line in enumerate(canon.read_text(args.input).splitlines(), start=1):
        m = BOX_RE.match(line)
        if m:
            indent, fourcc = len(m.group(1)), m.group(2)
            if indent % 2:
                raise canon.NormalizeError(f"line {n}: odd indentation")
            pairs = [] if m.group(4) is None else [("version", m.group(4)), ("flags", "0x" + m.group(5))]
            boxes.append((indent // 2, fourcc, {"size": int(m.group(3))}))
            raw.append(pairs)
            rows.append({})
            continue
        f = FIELD_RE.match(line) or BARE_RE.match(line)  # sgpd prints `groupingType: seig` without the dash
        if not f:
            # An MPEG-4 descriptor header (`Descriptor "tag=3 ES" size=2+44` in esds); its `- name: value` lines
            # that follow are attached to the esds box and dropped when unmapped.
            if line.lstrip().startswith('Descriptor "'):
                continue
            # A bare hex dump line: the bytes of a parameter-set NAL unit under its `- SPS_33 complete: 1` line.
            if HEX_RE.match(line):
                continue
            if line.strip():
                raise canon.NormalizeError(f"line {n}: neither a box nor a field line: {line!r}")
            continue
        if not boxes:
            raise canon.NormalizeError(f"line {n}: field before the first box")
        name, _, value = f.group(1).partition(": ")
        fourcc = boxes[-1][1]
        if ROW_RE.match(name):
            spec = maps["entries"].get(fourcc)
            if spec:
                row = canon.map_fields(KV_RE.findall(value), spec["fields"], args.keep_unmapped)
                rows[-1].setdefault(spec["array"], []).append(row)
            continue
        split = maps["split"].get(fourcc, {}).get(name)
        if split:
            raw[-1].extend(zip(split, value.split(":")))
            continue
        parts = f.group(1).split(", ")
        if len(parts) > 1 and all(PAIR_RE.match(p) for p in parts):
            raw[-1].extend(tuple(p.split(": ", 1)) for p in parts)  # type: ignore[misc]
        else:
            raw[-1].append((name, unquote(value)))
    out = []
    for (depth, fourcc, record), pairs, arrays in zip(boxes, raw, rows):
        fields = {**canon.map_fields(pairs, canon.box_map(maps["box"], fourcc), args.keep_unmapped), **arrays}
        if fields:
            record["fields"] = fields
        out.append((depth, fourcc, record))
    return canon.box_paths(out)


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--keep-unmapped", action="store_true", help="also emit unmapped names as ?<name>")


if __name__ == "__main__":
    sys.exit(canon.run("box-tree", __doc__.splitlines()[0], convert, add_args))
