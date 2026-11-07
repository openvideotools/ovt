#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Cross-oracle self-check of the H.26x header goldens (chapter 14 §14.4.1 / §14.4.2, WI-0.567 step 5).

    h26x_selfcheck.py --codec h264|h265 <id>.trace_headers.json <id>.gstparse.json [--known tests/golden/KNOWN_DIFFS.md]

Both goldens describe the same units. FFmpeg's CBS (``trace_headers``) lists every syntax element it read, by spec
name and subscript; GStreamer's codecparsers (``gstparse``) lists the structure members it filled, also by spec
name (``tests/oracles/gst_fields.h``), nested structures flattened to their leaf name and arrays to ``name[i]``.
For every unit the two are matched by index (their NAL unit types must agree) and, for every member that both
oracles name, the values must be equal. Members only one oracle names are not shared fields and are only counted.
A name that occurs more than once in a unit on either side (the same element in two nested structures) cannot be
paired and is counted as ambiguous.

A disagreement is documented when ``KNOWN_DIFFS.md`` has a table row ``| <codec>.<structure>.<member> | <reason> |``
(``fnmatch`` patterns allowed; the member name has its subscripts removed; an optional third column limits the row to
corpus ids matching an ``fnmatch`` pattern). Exit status: 0 no undocumented disagreement, 1 otherwise, 2 bad input.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

ROW_RE = re.compile(
    r"^\|\s*`?((?:h264|h265)\.[A-Za-z0-9_.*?\[\]]+)`?\s*\|\s*([^|]+?)\s*\|(?:\s*`?([^|`]*?)`?\s*\|)?\s*$"
)
SUBSCRIPT_RE = re.compile(r"\[\d+\]")


def load_known(path: Path | None) -> list[tuple[str, str]]:
    """Rows of KNOWN_DIFFS.md as ``(member pattern, corpus id pattern)``."""
    rows: list[tuple[str, str]] = []
    if path is None or not path.is_file():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        m = ROW_RE.match(line)
        if m:
            rows.append((m.group(1), (m.group(3) or "*").strip() or "*"))
    return rows


def is_known(known: list[tuple[str, str]], member: str, corpus_id: str) -> bool:
    return any(fnmatch.fnmatchcase(member, pat) and fnmatch.fnmatchcase(corpus_id, ids) for pat, ids in known)


def cbs_units(elements: list[dict[str, Any]]) -> dict[int, dict[str, list[int | str]]]:
    units: dict[int, dict[str, list[int | str]]] = defaultdict(lambda: defaultdict(list))
    for e in elements:
        units[e["unit"]][e["name"]].append(e["value"])
    return units


def flatten(node: Any, key: str, out: list[tuple[str, Any]]) -> None:
    """Leaves of a gstparse structure: a leaf's name is its member name plus array subscripts."""
    if isinstance(node, dict):
        for k, v in node.items():
            flatten(v, k, out)
    elif isinstance(node, list):
        for i, v in enumerate(node):
            if isinstance(v, (list, dict)):
                flatten_indexed(v, f"{key}[{i}]", out)
            else:
                out.append((f"{key}[{i}]", v))
    else:
        out.append((key, node))


def flatten_indexed(node: Any, key: str, out: list[tuple[str, Any]]) -> None:
    if isinstance(node, list):
        for i, v in enumerate(node):
            flatten_indexed(v, f"{key}[{i}]", out)
    elif isinstance(node, dict):
        for k, v in node.items():
            flatten_indexed(v, f"{key}.{k}", out)
    else:
        out.append((key, node))


def check(
    codec: str,
    trace: list[dict[str, Any]],
    gst: dict[str, Any],
    corpus_id: str,
    known: list[tuple[str, str]],
) -> dict[str, Any]:
    cbs = cbs_units(trace)
    report: dict[str, Any] = {
        "compared": 0,
        "documented": 0,
        "ambiguous": 0,
        "gst_only": 0,
        "problems": [],
    }
    seen: Counter[str] = Counter()
    for key in sorted(gst):
        unit = gst[key]
        index = int(key)
        if "error" in unit:
            report["problems"].append(f"unit {index}: GStreamer could not parse it ({unit['error']})")
            continue
        elements = cbs.get(index)
        if elements is None:
            report["problems"].append(f"unit {index}: no trace_headers unit")
            continue
        cbs_type = elements.get("nal_unit_type", [None])[0]
        if cbs_type != unit["unit"]["type"]:
            report["problems"].append(
                f"unit {index}: nal_unit_type {cbs_type} (trace_headers) vs {unit['unit']['type']} (gst)"
            )
            continue
        for structure, body in unit.items():
            if structure == "unit":
                continue
            leaves: list[tuple[str, Any]] = []
            flatten(body, structure, leaves)
            counts = Counter(name for name, _ in leaves)
            for name, value in leaves:
                if counts[name] > 1 or len(elements.get(name, [])) > 1:
                    report["ambiguous"] += 1
                    continue
                if name not in elements:
                    report["gst_only"] += 1
                    continue
                report["compared"] += 1
                if elements[name][0] == value:
                    continue
                member = f"{codec}.{structure}.{SUBSCRIPT_RE.sub('', name)}"
                if is_known(known, member, corpus_id):
                    report["documented"] += 1
                else:
                    seen[member] += 1
                    if seen[member] <= 3:
                        report["problems"].append(
                            f"unit {index} {member}: trace_headers {elements[name][0]} vs gstparse {value}"
                        )
    report["undocumented"] = dict(seen)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--codec", required=True, choices=("h264", "h265"))
    parser.add_argument("--known", type=Path, help="KNOWN_DIFFS.md")
    parser.add_argument("--id", default="*", help="corpus id (for the per-id rows of KNOWN_DIFFS.md)")
    parser.add_argument("trace_headers")
    parser.add_argument("gstparse")
    args = parser.parse_args(argv)
    try:
        trace = json.loads(Path(args.trace_headers).read_text(encoding="utf-8"))
        gst = json.loads(Path(args.gstparse).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    report = check(args.codec, trace, gst, args.id, load_known(args.known))
    print(json.dumps({k: v for k, v in report.items() if k != "problems"}, sort_keys=True))
    for line in report["problems"]:
        print(f"disagreement: {line}", file=sys.stderr)
    return 1 if report["problems"] else 0


if __name__ == "__main__":
    sys.exit(main())
