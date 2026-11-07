#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The tools the synthetic corpus recipes need from the oracle image (WI-0.554, WI-0.553 c2).

    recipe_tools.py --write [--recipes FILE] [--out FILE]   (in the repository) derive recipe_tools.json
    recipe_tools.py --check [--list FILE]                   (in the oracle image) verify every tool is there

``--write`` reads tests/corpora/synthetic/recipes.json and lists every executable the recipe and source steps call
(pipes and the MP4 wrapping step included), every executable a pin probes, the version pins themselves, the FFmpeg
encoders, bitstream filters, filters, lavfi sources, muxers and the lavfi input device the ffmpeg steps name, and the third-party Python
modules the ``{repo}/tools/*.py`` helpers import. tools/tests/test_recipe_tools.py fails when the committed list
differs from what --write derives, so a new recipe cannot silently need a tool the image lacks.

``--check`` runs inside the image (``oracle-recipe-tools``; oracle-smoke-test calls it) and prints one ``ok``/``FAIL``
line per item: the executable is on PATH, the pin's probe or version stamp shows the pinned version, FFmpeg lists the
component, the Python module imports. Exit 0 when everything is present, 1 otherwise, 2 on usage errors.
"""

from __future__ import annotations

import argparse
import ast
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
DEFAULT_LIST = HERE / "recipe_tools.json"
STAMPS = os.environ.get("OVT_ORACLES_STAMPS", "/opt/ovt-oracles/share/ovt-oracles/stamps")

# Not provided by the oracle build: the interpreter running generate.py, and `ovt` (the tool under test).
NOT_IMAGE_TOOLS = {"{python}", "ovt"}
# ffmpeg chooses the muxer from the output name when no -f is given.
MUXER_BY_SUFFIX = {
    ".mp4": "mp4",
    ".m4v": "ipod",  # FFmpeg's ipod muxer owns m4v, m4a and m4b
    ".m4a": "ipod",
    ".m4b": "ipod",
    ".mov": "mov",
    ".mkv": "matroska",
    ".webm": "webm",
    ".ts": "mpegts",
    ".y4m": "yuv4mpegpipe",
    ".264": "h264",
    ".h264": "h264",
    ".265": "hevc",
    ".h265": "hevc",
    ".ivf": "ivf",
    ".obu": "obu",
    ".avif": "avif",
    ".yuv": "rawvideo",
    ".wav": "wav",
    ".opus": "opus",
}
FILTER_OPTS = {"-vf", "-af", "-filter:v", "-filter:a", "-lavfi", "-filter_complex"}
CODEC_OPT = re.compile(r"-(?:c|codec)(?::[vas](?::\d+)?)?|-[vas]codec")


# ------------------------------------------------------------------------------------------------ derive


def split_graph(graph: str) -> list[str]:
    """Split a filtergraph on , and ; outside single quotes."""
    parts, cur, quoted = [], [], False
    for ch in graph:
        if ch == "'":
            quoted = not quoted
        if ch in ",;" and not quoted:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    return parts


def filter_names(graph: str) -> set[str]:
    names = set()
    for part in split_graph(graph):
        part = re.sub(r"^(\s*\[[^\]]*\])*", "", part).strip()  # leading [in] labels
        name = re.match(r"[A-Za-z0-9_]+", part)
        if name:
            names.add(name.group(0))
    return names


def ffmpeg_needs(argv: list[str], need: dict[str, set[str]]) -> None:
    args = argv[1:]
    last_input = max((i for i, a in enumerate(args) if a == "-i"), default=-1)
    explicit_muxer = False
    for i, arg in enumerate(args[:-1]):
        val = args[i + 1]
        if CODEC_OPT.fullmatch(arg) and val != "copy":
            need["encoders"].add(val)
        elif arg.startswith("-bsf"):
            need["bsfs"].update(p.split("=")[0] for p in val.split(",") if p)
        elif arg in FILTER_OPTS:
            need["filters"].update(filter_names(val))
        elif arg == "-f" and val == "lavfi":
            need["devices"].add("lavfi")  # the libavdevice input device of the lavfi sources
        elif arg == "-f":
            # `ffmpeg -formats` lists demuxers and muxers alike. Only a -f after the last -i names the muxer, so only
            # that one replaces the muxer implied by the output name.
            need["formats"].add(val)
            explicit_muxer = explicit_muxer or i > last_input
        elif arg == "-i" and i > 0 and args[i - 1] == "lavfi":
            need["filters"].update(filter_names(val))  # lavfi sources and their format= chains are filters
    out = Path(args[-1]) if args else None
    if not explicit_muxer and out is not None and out.suffix.lower() in MUXER_BY_SUFFIX:
        need["formats"].add(MUXER_BY_SUFFIX[out.suffix.lower()])


def helper_modules(script: Path, tools_dir: Path) -> set[str]:
    """Top-level modules a helper imports that are neither stdlib nor another tools/ module."""
    tree = ast.parse(script.read_text(encoding="utf-8"))
    mods = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            mods.update(a.name.split(".")[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
            mods.add(node.module.split(".")[0])
    return {m for m in mods if m not in sys.stdlib_module_names and not (tools_dir / f"{m}.py").exists()}


def derive(recipes_path: Path) -> dict[str, Any]:
    repo = recipes_path.resolve().parents[3]
    sys.path.insert(0, str(recipes_path.resolve().parent))
    import generate  # tests/corpora/synthetic/generate.py

    doc = json.loads(recipes_path.read_text(encoding="utf-8"))
    need: dict[str, set[str]] = {
        k: set() for k in ("executables", "encoders", "bsfs", "filters", "formats", "devices", "python")
    }
    for item in [*doc["sources"], *doc["recipes"]]:
        steps = list(item["steps"])
        if item.get("mp4"):
            steps.append(generate.wrap_step(item["mp4"]))
        for step in steps:
            for argv in generate.step_argvs(step):
                need["executables"].add(argv[0])
                if argv[0] == "ffmpeg":
                    ffmpeg_needs(argv, need)
                for arg in argv:
                    if arg.startswith("{repo}/tools/") and arg.endswith(".py"):
                        script = repo / arg[len("{repo}/") :]
                        need["python"].update(helper_modules(script, script.parent))
    pins = {}
    for tool, pin in sorted(doc["pins"].items()):
        pins[tool] = pin
        if "probe" in pin:
            need["executables"].add(pin["probe"][0])
    return {
        "_comment": "Generated by tests/oracles/recipe_tools.py --write from tests/corpora/synthetic/recipes.json.",
        "executables": sorted(need["executables"] - NOT_IMAGE_TOOLS),
        "pins": pins,
        "ffmpeg": {k: sorted(need[k]) for k in ("encoders", "bsfs", "filters", "formats", "devices")},
        "python_modules": sorted(need["python"]),
    }


def dumps(doc: dict[str, Any]) -> str:
    return json.dumps(doc, indent=2) + "\n"


# ------------------------------------------------------------------------------------------------- check


def run(argv: list[str]) -> str:
    try:
        res = subprocess.run(argv, capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return res.stdout + res.stderr


def pin_text(pin: dict[str, Any]) -> str:
    if "stamp" in pin:
        try:
            return (Path(STAMPS) / f"{pin['stamp']}.version").read_text(encoding="utf-8")
        except OSError:
            return ""
    return run(pin["probe"])


def ffmpeg_names(kind: str) -> set[str]:
    """Component names from `ffmpeg -encoders|-bsfs|-filters|-formats|-devices`."""
    text = run(["ffmpeg", "-hide_banner", f"-{kind}"])
    names = set()
    for line in text.splitlines():
        cols = line.split()
        if kind == "bsfs":
            if len(cols) == 1 and re.fullmatch(r"[a-z0-9_]+", cols[0]):
                names.add(cols[0])
        elif len(cols) >= 2 and re.fullmatch(r"[A-Z.|<>]+", cols[0]) and cols[0] != "--":
            names.update(cols[1].split(","))  # -formats lists "matroska,webm" style aliases
    return names


def check(list_path: Path) -> int:
    doc = json.loads(list_path.read_text(encoding="utf-8"))
    failures = 0

    def report(ok: bool, what: str, detail: str = "") -> None:
        nonlocal failures
        failures += 0 if ok else 1
        print(f"{'ok  ' if ok else 'FAIL'}  {what}{('  ' + detail) if detail else ''}")

    for exe in doc["executables"]:
        path = shutil.which(exe)
        report(path is not None, f"executable {exe}", path or "not on PATH")
    for tool, pin in doc["pins"].items():
        text = pin_text(pin)
        line = next((ln.strip() for ln in text.splitlines() if pin["expect"] in ln), "")
        report(bool(line), f"pin {tool} = {pin['expect']!r}", line or "version not found")
    for kind, wanted in doc["ffmpeg"].items():
        have = ffmpeg_names(kind)
        for name in wanted:
            report(name in have, f"ffmpeg {kind[:-1]} {name}")
    for mod in doc["python_modules"]:
        report(importlib.util.find_spec(mod) is not None, f"python module {mod}", sys.executable)
    print(f"{failures} missing" if failures else "every recipe tool is present")
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    ap.add_argument("--recipes", type=Path, default=HERE.parent / "corpora" / "synthetic" / "recipes.json")
    ap.add_argument("--out", type=Path, default=DEFAULT_LIST)
    ap.add_argument("--list", type=Path, default=DEFAULT_LIST)
    args = ap.parse_args()
    if args.write:
        args.out.write_text(dumps(derive(args.recipes)), encoding="utf-8")
        return 0
    return check(args.list)


if __name__ == "__main__":
    sys.exit(main())
