# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/coverage_gate.py (WI-0.031, acceptance test 3)."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "coverage_gate.py"


def export(root: Path, rows: dict[str, tuple[int, int]]) -> Path:
    files = [
        {"filename": str(root / name), "summary": {"lines": {"count": n, "covered": c}}}
        for name, (n, c) in rows.items()
    ]
    path = root / "export.json"
    path.write_text(json.dumps({"data": [{"files": files}]}))
    return path


def run(root: Path, rows: dict[str, tuple[int, int]]) -> subprocess.CompletedProcess[str]:
    summary = export(root, rows)
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), "check", "--summary", str(summary), "--root", str(root)],
        capture_output=True,
        text=True,
        check=False,
    )


def test_all_above_thresholds_passes(tmp_path: Path) -> None:
    result = run(tmp_path, {"core/src/bitreader/a.cpp": (100, 95), "core/src/model/b.cpp": (100, 85)})
    assert result.returncode == 0, result.stdout
    assert "container/: no source files yet" in result.stdout


def test_low_bitreader_names_directory_and_percentage(tmp_path: Path) -> None:
    result = run(tmp_path, {"core/src/bitreader/a.cpp": (100, 10), "core/src/model/b.cpp": (1000, 1000)})
    assert result.returncode == 1
    assert "bitreader/: 10.0 % < 90 %" in result.stdout


def test_low_overall_fails(tmp_path: Path) -> None:
    result = run(tmp_path, {"core/src/model/b.cpp": (100, 70)})
    assert result.returncode == 1 and "overall: 70.0 %" in result.stdout


def test_gated_directory_with_sources_but_no_lines_fails(tmp_path: Path) -> None:
    (tmp_path / "core/src/syntax").mkdir(parents=True)
    (tmp_path / "core/src/syntax/x.cpp").write_text("")
    result = run(tmp_path, {"core/src/model/b.cpp": (100, 100)})
    assert result.returncode == 1 and "syntax/: 0.0 %" in result.stdout


def test_files_outside_core_src_are_ignored(tmp_path: Path) -> None:
    result = run(tmp_path, {"core/src/model/b.cpp": (100, 100), "third_party/x.cpp": (1000, 0)})
    assert result.returncode == 0, result.stdout


def test_export_without_core_sources_is_unusable(tmp_path: Path) -> None:
    assert run(tmp_path, {"third_party/x.cpp": (10, 10)}).returncode == 2
