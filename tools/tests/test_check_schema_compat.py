# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/check_schema_compat.py (WI-0.003, acceptance test 2)."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS / "check_schema_compat.py"
sys.path.insert(0, str(TOOLS))

import check_schema_compat as csc

OLD = """
// a comment with table Fake { x:int; }
namespace ovt.fb;
struct S { a:int; }
table T {
  a:int;
  b:string;
  old:int (deprecated);
}
root_type T;
"""

needs_flatc = pytest.mark.skipif(shutil.which("flatc") is None, reason="flatc not installed")


def run(tmp_path: Path, new_text: str, old_text: str = OLD) -> subprocess.CompletedProcess[str]:
    base = tmp_path / "base"
    base.mkdir()
    (base / "m.fbs").write_text(old_text)
    new = tmp_path / "m.fbs"
    new.write_text(new_text)
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT), "--baseline-dir", str(base), str(new)],
        text=True,
        capture_output=True,
        check=False,
    )


def test_parse_fields_ignores_comments_and_keeps_namespace() -> None:
    fields = csc.parse_fields(OLD)
    assert fields == {"ovt.fb.S": {"a"}, "ovt.fb.T": {"a", "b", "old"}}


def test_removed_fields_names_table_and_field() -> None:
    assert csc.removed_fields(OLD, OLD.replace("  b:string;\n", "")) == ["ovt.fb.T.b"]
    assert csc.removed_fields(OLD, OLD.replace("struct S { a:int; }", "")) == ["ovt.fb.S.a"]
    assert csc.removed_fields(OLD, OLD.replace("b:string;", "b:string; c:int;")) == []


@needs_flatc
def test_unchanged_and_additive_schema_conform(tmp_path: Path) -> None:
    r = run(
        tmp_path, OLD.replace("b:string;", "b:string;").replace("old:int (deprecated);", "old:int (deprecated); d:int;")
    )
    assert r.returncode == 0, r.stdout + r.stderr


@needs_flatc
def test_removed_field_exits_1_and_names_it(tmp_path: Path) -> None:
    r = run(tmp_path, OLD.replace("  b:string;\n", ""))
    assert r.returncode == 1
    assert "E-REMOVED m.fbs ovt.fb.T.b" in r.stdout


@needs_flatc
def test_retyped_field_exits_1(tmp_path: Path) -> None:
    r = run(tmp_path, OLD.replace("a:int;\n  b", "a:long;\n  b"))
    assert r.returncode == 1
    assert "E-CONFORM" in r.stdout and "a" in r.stdout
