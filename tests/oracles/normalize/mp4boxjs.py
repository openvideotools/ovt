#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""``mp4boxjs_dump.mjs`` JSON -> ``box-tree`` (chapter 14 §14.4.9, WI-0.555).

Input: the array ``[{path, start, size, hdr_size, type, uuid?, fields}]`` that ``tests/oracles/mp4boxjs_dump.mjs``
(WI-0.564) writes from mp4box.js's parsed boxes, in file order. Each entry becomes ``{path, offset: start,
size, header_length, fields}``. The path is rebuilt from the depth of the dump's path and ``type`` so that
the ``[n]`` rule is the same as for every other oracle.

``header_length``: mp4box.js adds the 4 FullBox version/flags bytes to ``hdr_size``
(``parseFullHeader``), OVT's ``BoxNode.header_length`` does not (chapter 8 §8.2.4), so 4 is subtracted
for every box whose fields hold ``version`` and ``flags``. ``uuid`` goes to ``fields.usertype`` as
lowercase hex. Field names go through ``mp4boxjs_field_map.json``; unmapped ones are dropped.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

INDEX_RE = re.compile(r"\[\d+\]$")


def convert(args: argparse.Namespace) -> list[dict[str, Any]]:
    doc = canon.read_json(args.input)
    if not isinstance(doc, list):
        raise canon.NormalizeError("expected a JSON array of boxes")
    maps = canon.load_map("mp4boxjs_field_map")
    boxes: list[tuple[int, str, dict[str, Any]]] = []
    for n, box in enumerate(doc):
        try:
            path, fourcc = box["path"], box["type"]
            start, size, hdr_size = (int(box[k]) for k in ("start", "size", "hdr_size"))
        except (KeyError, TypeError, ValueError) as exc:
            raise canon.NormalizeError(f"box {n}: missing or bad key {exc}") from exc
        if INDEX_RE.sub("", path.rsplit("/", 1)[-1]) != fourcc:
            raise canon.NormalizeError(f"box {n}: path {path!r} does not end in type {fourcc!r}")
        raw = box.get("fields", {})
        # SPEC-ASSUMPTION(#134): header_length excludes FullBox version/flags; mp4box.js hdr_size includes them.
        full = "version" in raw and "flags" in raw
        record: dict[str, Any] = {"offset": start, "size": size, "header_length": hdr_size - 4 * full}
        fields = canon.map_fields(
            ((k, str(v)) for k, v in raw.items()), canon.box_map(maps, fourcc), args.keep_unmapped
        )
        if box.get("uuid"):
            fields["usertype"] = str(box["uuid"]).replace("-", "").lower()
        if fields:
            record["fields"] = fields
        boxes.append((path.count("/"), fourcc, record))
    return canon.box_paths(boxes)


def add_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--keep-unmapped", action="store_true", help="also emit unmapped names as ?<name>")


if __name__ == "__main__":
    sys.exit(canon.run("box-tree", __doc__.splitlines()[0], convert, add_args))
