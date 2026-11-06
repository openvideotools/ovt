#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regenerate the ``known`` lists of ``cbs_name_map.json`` from an FFmpeg source tree (WI-0.555).

``trace_headers.py`` refuses a CBS name that is neither ``known`` nor ``rename``d (chapter 14 §14.4.1:
"any unmapped CBS name fails"). The known names are every syntax element name the CBS read templates of
the pinned FFmpeg (``tests/oracles/ORACLE_VERSIONS.md``) can print. Run this when the FFmpeg pin moves:

    python -I tests/oracles/normalize/gen_cbs_names.py <ffmpeg-src> [--check]

``--check`` exits 1 when the committed lists differ from the tree's. ``rename`` entries are kept.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

MAP = Path(__file__).resolve().parent / "cbs_name_map.json"
SOURCES = {
    "h264": ["cbs_h264_syntax_template.c", "cbs_sei_syntax_template.c", "cbs_h2645.c"],
    "h265": ["cbs_h265_syntax_template.c", "cbs_sei_syntax_template.c", "cbs_h2645.c"],
    "av1": ["cbs_av1_syntax_template.c", "cbs_av1.c"],
}
# CBS read macros and the 0-based argument that holds the element name (cbs_h2645.c, cbs_av1.c).
NAME_ARG = dict.fromkeys("flag flags ue ues se ses uvlc increment subexp delta_q leb128 xue xse".split(), 0)
NAME_ARG.update(dict.fromkeys("u ub ubs us fixed fc fcs fb fbs su sus ns xu xsu".split(), 1))
CALL_RE = re.compile(r"\b(" + "|".join(sorted(NAME_ARG, key=len, reverse=True)) + r")\s*\(")
LITERAL_RE = re.compile(r'"([a-z][a-z0-9_]*)"')  # names passed as strings, e.g. "obu_size"
IDENT_RE = re.compile(r"^[a-z][a-z0-9_]*$")


def call_args(text: str, start: int) -> list[str]:
    depth, arg, args = 0, "", []
    for ch in text[start:]:
        if ch == "(":
            depth += 1
            if depth == 1:
                continue
        elif ch == ")":
            depth -= 1
            if depth == 0:
                args.append(arg.strip())
                return args
        elif ch == "," and depth == 1:
            args.append(arg.strip())
            arg = ""
            continue
        arg += ch
    return args


def names_in(text: str) -> set[str]:
    found: set[str] = set()
    for m in CALL_RE.finditer(text):
        args = call_args(text, m.end() - 1)
        n = NAME_ARG[m.group(1)]
        if len(args) > n:
            name = re.sub(r"\[.*?\]", "", args[n])
            if IDENT_RE.match(name):
                found.add(name)
    found.update(m.group(1) for m in LITERAL_RE.finditer(text) if "_" in m.group(1))
    return found


def generate(src: Path) -> dict[str, list[str]]:
    base = src / "libavcodec"
    return {
        codec: sorted(set().union(*(names_in((base / f).read_text(encoding="utf-8")) for f in files)))
        for codec, files in SOURCES.items()
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ffmpeg_src", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    current = json.loads(MAP.read_text(encoding="utf-8"))
    known = generate(args.ffmpeg_src)
    if args.check:
        stale = [c for c in known if current["known"].get(c) != known[c]]
        for codec in stale:
            print(f"cbs_name_map.json: known[{codec}] differs from {args.ffmpeg_src}", file=sys.stderr)
        return 1 if stale else 0
    current["known"] = known
    MAP.write_text(json.dumps(current, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
