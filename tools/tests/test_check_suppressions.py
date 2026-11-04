# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/check_suppressions.py (WI-0.558, acceptance test 2)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS / "check_suppressions.py"


def run(tmp_path: Path, name: str, text: str) -> subprocess.CompletedProcess[str]:
    path = tmp_path / name
    path.write_text(text)
    return subprocess.run([sys.executable, "-I", str(SCRIPT), str(path)], capture_output=True, text=True, check=False)


def test_committed_files_pass() -> None:
    result = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stdout


def test_entry_without_url_fails(tmp_path: Path) -> None:
    result = run(tmp_path, "tsan.supp", "race:libfoo_worker\n")
    assert result.returncode == 1
    assert "E-NOURL" in result.stdout and "tsan.supp:1" in result.stdout


def test_entry_with_url_passes(tmp_path: Path) -> None:
    text = "# https://github.com/example/libfoo/issues/12\nrace:libfoo_worker\nrace:libfoo_other\n"
    assert run(tmp_path, "tsan.supp", text).returncode == 0


def test_blank_line_ends_the_block(tmp_path: Path) -> None:
    text = "# https://github.com/example/libfoo/issues/12\nleak:a\n\nleak:b\n"
    result = run(tmp_path, "asan.supp", text)
    assert result.returncode == 1 and "asan.supp:4" in result.stdout


def test_bare_host_is_not_an_issue_link(tmp_path: Path) -> None:
    assert run(tmp_path, "asan.supp", "# https://github.com\nleak:a\n").returncode == 1


def test_unknown_kind_and_empty_pattern(tmp_path: Path) -> None:
    result = run(tmp_path, "ubsan.supp", "# https://example.org/bug/1\nrace:foo\nshift:\n")
    assert result.returncode == 1
    assert "E-KIND" in result.stdout and "E-EMPTY" in result.stdout


def test_missing_file_exits_2(tmp_path: Path) -> None:
    result = subprocess.run(
        [sys.executable, "-I", str(SCRIPT), str(tmp_path / "nope.supp")], capture_output=True, text=True, check=False
    )
    assert result.returncode == 2
