# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/check_slow_labels.py: tests that cite slow corpus streams carry the [slow] tag (WI-0.552c)."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_slow_labels.py"

MANIFEST = {
    "entries": [
        {"id": "big_stream", "tags": ["slow"]},
        {"id": "small_stream", "tags": ["fast"]},
        {"id": "both_stream", "tags": ["fast", "slow"]},
    ]
}


def run(tmp_path: Path, source: str) -> subprocess.CompletedProcess[str]:
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps(MANIFEST), encoding="utf-8")
    root = tmp_path / "tests"
    root.mkdir()
    (root / "t.cpp").write_text(source, encoding="utf-8")
    return subprocess.run(
        [sys.executable, "-I", str(TOOL), "--manifest", str(manifest), str(root)],
        capture_output=True,
        text=True,
        check=False,
    )


def test_slow_stream_in_slow_case_passes(tmp_path: Path) -> None:
    src = 'TEST_CASE("a", "[x][slow]") {\n  corpus_path("big_stream");\n}\n'
    assert run(tmp_path, src).returncode == 0


def test_slow_stream_in_fast_case_fails(tmp_path: Path) -> None:
    src = 'TEST_CASE("a", "[x][fast]") {\n  corpus_path("big_stream");\n}\n'
    out = run(tmp_path, src)
    assert out.returncode == 1
    assert "t.cpp:1" in out.stdout and "big_stream" in out.stdout


def test_stream_tagged_fast_and_slow_is_fast_enough(tmp_path: Path) -> None:
    src = 'TEST_CASE("a", "[fast]") {\n  corpus_path("both_stream");\n  corpus_path("small_stream");\n}\n'
    assert run(tmp_path, src).returncode == 0


def test_second_case_is_judged_on_its_own(tmp_path: Path) -> None:
    src = (
        'TEST_CASE("a", "[slow]") {\n  corpus_path("big_stream");\n}\n'
        'TEST_CASE("b", "[fast]") {\n  corpus_path("big_stream");\n}\n'
    )
    out = run(tmp_path, src)
    assert out.returncode == 1
    assert "t.cpp:4" in out.stdout and "t.cpp:1" not in out.stdout


def test_fast_and_slow_together_fails(tmp_path: Path) -> None:
    out = run(tmp_path, 'TEST_CASE("a", "[fast][slow]") {}\n')
    assert out.returncode == 1 and "both" in out.stdout


def test_opt_out_tag(tmp_path: Path) -> None:
    src = 'TEST_CASE("a", "[fast][slow-stream-ok]") {\n  id("big_stream");\n}\n'
    assert run(tmp_path, src).returncode == 0


def test_repository_is_consistent() -> None:
    out = subprocess.run([sys.executable, "-I", str(TOOL)], capture_output=True, text=True, check=False, cwd=REPO)
    assert out.returncode == 0, out.stdout
