#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""``gst_parse_oracle`` JSON Lines -> ``header-struct`` (chapter 14 §14.4.2, WI-0.567).

``gst_parse_oracle`` (WI-0.562) prints one JSON object per input unit, ``{"unit": {"index", "offset", "type"},
"sps": {...}}`` (or ``"error"`` for a unit GStreamer could not parse). This normaliser keeps them all in one
``header-struct`` document keyed by the zero-padded unit index (``"000000"``, ``"000001"``, ... so that sorted keys
stay in stream order), drops the members that GStreamer derives instead of parsing (``gst_ignore.txt``, lines
``<codec>.<structure>.<member path>``) and sorts nothing else: values stay as GStreamer printed them.

SPEC-ASSUMPTION(#153): the golden ``<id>.gstparse.json`` is this document; the spec's "one object per unit" and
"a single <id>.gstparse.json" are reconciled by the unit-index keys.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

CODECS = ("h264", "h265", "av1")
INDEX_WIDTH = 6


def ignore_paths(codec: str) -> list[list[str]]:
    """The ``gst_ignore.txt`` member paths of ``codec`` as ``[structure, member, ...]`` lists."""
    out = []
    for line in sorted(canon.load_list("gst_ignore.txt")):
        parts = line.split(".")
        if parts[0] == codec and len(parts) >= 3:
            out.append(parts[1:])
    return out


def drop(doc: dict[str, Any], path: list[str]) -> None:
    node: Any = doc
    for key in path[:-1]:
        node = node.get(key) if isinstance(node, dict) else None
        if node is None:
            return
    if isinstance(node, dict):
        node.pop(path[-1], None)


def convert_text(text: str, codec: str) -> dict[str, Any]:
    ignored = ignore_paths(codec)
    out: dict[str, Any] = {}
    for n, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        unit = canon.parse_json(line, f"line {n}")
        if not isinstance(unit, dict) or not isinstance(unit.get("unit"), dict) or "index" not in unit["unit"]:
            raise canon.NormalizeError(f"line {n}: not a gst_parse_oracle unit object")
        for path in ignored:
            drop(unit, path)
        out[f"{int(unit['unit']['index']):0{INDEX_WIDTH}d}"] = unit
    if not out:
        raise canon.NormalizeError("no units in the gst_parse_oracle output")
    return out


def convert(args: argparse.Namespace) -> dict[str, Any]:
    return convert_text(canon.read_text(args.input), args.codec)


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--codec", required=True, choices=CODECS)


if __name__ == "__main__":
    sys.exit(canon.run("header-struct", __doc__.splitlines()[0], convert, add_args))
