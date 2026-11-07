# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tests/oracles/recipe_tools.json lists every tool the synthetic corpus recipes need from the oracle image (WI-0.554).

The image's `oracle-recipe-tools --check` (run by oracle-smoke-test) verifies that list, so it must stay in step with
tests/corpora/synthetic/recipes.json: regenerate it with `python3 tests/oracles/recipe_tools.py --write`.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tests" / "oracles" / "recipe_tools.py"


def load():
    spec = importlib.util.spec_from_file_location("recipe_tools", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_committed_list_matches_recipes():
    mod = load()
    derived = mod.dumps(mod.derive(REPO / "tests" / "corpora" / "synthetic" / "recipes.json"))
    committed = (REPO / "tests" / "oracles" / "recipe_tools.json").read_text(encoding="utf-8")
    assert committed == derived, "tests/oracles/recipe_tools.json is stale: run tests/oracles/recipe_tools.py --write"


def test_list_names_the_known_recipe_tools():
    doc = json.loads((REPO / "tests" / "oracles" / "recipe_tools.json").read_text(encoding="utf-8"))
    for exe in ("ffmpeg", "x264", "x265", "aomenc", "scalable_encoder", "TAppEncoder", "lencod", "MP4Box"):
        assert exe in doc["executables"]
    assert "ovt" not in doc["executables"]
    assert "libx265" in doc["ffmpeg"]["encoders"]


def test_filter_names_respect_quotes_and_labels():
    mod = load()
    assert mod.filter_names("select='gte(n,7)',setpts=N/30/TB") == {"select", "setpts"}
    assert mod.filter_names("[0]trim=end_frame=20,format=yuv420p[a];[a][b]concat=n=2:v=1") == {
        "trim",
        "format",
        "concat",
    }


def empty_need():
    return {k: set() for k in ("encoders", "bsfs", "filters", "formats", "devices")}


def test_muxer_from_output_name():
    mod = load()
    need = empty_need()
    mod.ffmpeg_needs(["ffmpeg", "-i", "a.265", "-c", "copy", "out.mkv"], need)
    assert need["formats"] == {"matroska"}
    assert need["encoders"] == set()
    need = empty_need()
    mod.ffmpeg_needs(["ffmpeg", "-i", "a.mp4", "-c", "copy", "out.m4a"], need)
    assert need["formats"] == {"ipod"}


def test_input_format_does_not_hide_the_output_muxer():
    mod = load()
    need = empty_need()
    mod.ffmpeg_needs(["ffmpeg", "-f", "rawvideo", "-s", "64x64", "-i", "x.yuv", "-c:v", "libx264", "out.mp4"], need)
    assert need["formats"] == {"rawvideo", "mp4"}
    need = empty_need()
    mod.ffmpeg_needs(["ffmpeg", "-i", "x.y4m", "-f", "hevc", "out.bin"], need)
    assert need["formats"] == {"hevc"}


def test_lavfi_input_needs_the_lavfi_device():
    mod = load()
    need = empty_need()
    mod.ffmpeg_needs(["ffmpeg", "-f", "lavfi", "-i", "testsrc2=size=64x64", "out.y4m"], need)
    assert need["devices"] == {"lavfi"}
    assert need["filters"] == {"testsrc2"}
    assert need["formats"] == {"yuv4mpegpipe"}
