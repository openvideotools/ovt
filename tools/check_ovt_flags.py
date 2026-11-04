#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that every OVT target links the ovt::flags interface target (chapter 14 WI-0.559, chapter 13 §13.2.5).

    check_ovt_flags.py [--build-dir build/linux-x64-debug] [--source-dir .]

Reads the CMake file API reply (codemodel-v2) of an already configured build directory. If the directory
has no reply yet, the query file is written and CMake is re-run on the directory to produce one. Every
compiled target (executable, static/shared/module/object library) whose source directory lies under
core/, render/, plugins/, cli/ or app/ must carry the compile definition OVT_FLAGS_APPLIED=1 that ovt::flags adds. Third-party builds are out of scope:
they live outside these directories and never take the warning set (D24).

Exit 0 when clean, 1 naming each offending target, 2 when the build directory or reply is unusable.
Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

OVT_DIRS = ("core", "render", "plugins", "cli", "app")
CHECKED_TYPES = {
    "EXECUTABLE",
    "STATIC_LIBRARY",
    "SHARED_LIBRARY",
    "MODULE_LIBRARY",
    "OBJECT_LIBRARY",
}
# The file API lists no dependency on an INTERFACE library, so cmake/OvtCompilerFlags.cmake gives ovt::flags
# the compile definition below and the check looks for it in the target's compile groups.
# SPEC-ASSUMPTION(#29): marker definition OVT_FLAGS_APPLIED=1 on ovt::flags.
MARKER = "OVT_FLAGS_APPLIED=1"


def _load_reply(build_dir: Path) -> Path:
    api = build_dir / ".cmake" / "api" / "v1"
    if not list((api / "reply").glob("index-*.json")):
        query = api / "query"
        query.mkdir(parents=True, exist_ok=True)
        (query / "codemodel-v2").touch()
        done = subprocess.run(["cmake", str(build_dir)], capture_output=True, text=True, check=False)
        if done.returncode != 0:
            sys.stderr.write(done.stdout + done.stderr)
            raise SystemExit(2)
    return api / "reply"


def violations(build_dir: Path, source_dir: Path) -> list[str]:
    reply = _load_reply(build_dir)
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        print(f"error: no CMake file API reply in {reply}", file=sys.stderr)
        raise SystemExit(2)
    index = json.loads(indexes[-1].read_text())
    ref = next((o for o in index["objects"] if o["kind"] == "codemodel"), None)
    if ref is None:
        print("error: the file API reply has no codemodel", file=sys.stderr)
        raise SystemExit(2)
    model = json.loads((reply / ref["jsonFile"]).read_text())
    config = model["configurations"][0]
    bad: list[str] = []
    for entry in config["targets"]:
        target = json.loads((reply / entry["jsonFile"]).read_text())
        if target["type"] not in CHECKED_TYPES:
            continue
        src = Path(target["paths"]["source"])
        rel = (
            src
            if not src.is_absolute()
            else src.relative_to(source_dir.resolve())
            if src.is_relative_to(source_dir.resolve())
            else None
        )
        if rel is None or not rel.parts or rel.parts[0] not in OVT_DIRS:
            continue
        defines = {d["define"] for g in target.get("compileGroups", []) for d in g.get("defines", [])}
        if MARKER not in defines:
            bad.append(f"{target['name']} ({rel})")
    return sorted(bad)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build-dir", type=Path, default=Path("build/linux-x64-debug"))
    ap.add_argument("--source-dir", type=Path, default=Path("."))
    args = ap.parse_args(argv)
    if not (args.build_dir / "CMakeCache.txt").is_file():
        print(
            f"error: {args.build_dir} is not a configured CMake build directory",
            file=sys.stderr,
        )
        return 2
    bad = violations(args.build_dir, args.source_dir)
    for name in bad:
        print(f"target does not link ovt::flags: {name}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
