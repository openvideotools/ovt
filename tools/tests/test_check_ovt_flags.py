# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/check_ovt_flags.py (WI-0.559 acceptance test 3) on a miniature CMake project."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "check_ovt_flags.py"

pytestmark = pytest.mark.skipif(shutil.which("cmake") is None, reason="cmake not installed")

TOP = """cmake_minimum_required(VERSION 3.25)
project(mini CXX)
add_library(ovt_flags INTERFACE)
target_compile_definitions(ovt_flags INTERFACE OVT_FLAGS_APPLIED=1)
add_library(ovt::flags ALIAS ovt_flags)
add_subdirectory(core)
add_subdirectory(third_party)
"""


def _project(root: Path, core_body: str, top_extra: str = "") -> Path:
    (root / "core").mkdir()
    (root / "third_party").mkdir()
    (root / "CMakeLists.txt").write_text(TOP + top_extra)
    (root / "a.cpp").write_text("int a() { return 0; }\n")
    (root / "core" / "CMakeLists.txt").write_text(core_body)
    # Third-party targets never take the warning set; they must not be reported.
    (root / "third_party" / "CMakeLists.txt").write_text("add_library(vendored STATIC ../a.cpp)\n")
    build = root / "build"
    subprocess.run(["cmake", "-S", str(root), "-B", str(build)], check=True, capture_output=True)
    return build


def _run(root: Path, build: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            sys.executable,
            "-I",
            str(SCRIPT),
            "--build-dir",
            str(build),
            "--source-dir",
            str(root),
        ],
        capture_output=True,
        text=True,
        check=False,
    )


def test_clean_tree_passes(tmp_path: Path) -> None:
    build = _project(
        tmp_path,
        "add_library(good STATIC ../a.cpp)\ntarget_link_libraries(good PRIVATE ovt::flags)\n",
    )
    done = _run(tmp_path, build)
    assert done.returncode == 0, done.stdout + done.stderr


def test_target_without_flags_names_target(tmp_path: Path) -> None:
    build = _project(
        tmp_path,
        "add_library(good STATIC ../a.cpp)\ntarget_link_libraries(good PRIVATE ovt::flags)\n"
        "add_library(rogue STATIC ../a.cpp)\n",
    )
    done = _run(tmp_path, build)
    assert done.returncode == 1
    assert "rogue" in done.stdout
    assert "good" not in done.stdout
    assert "vendored" not in done.stdout


def test_unconfigured_build_dir_is_exit_2(tmp_path: Path) -> None:
    assert _run(tmp_path, tmp_path / "nope").returncode == 2


def test_root_level_target_is_skipped(tmp_path: Path) -> None:
    # The file API reports paths.source == "." for a target of the top-level CMakeLists.txt.
    build = _project(
        tmp_path,
        "add_library(good STATIC ../a.cpp)\ntarget_link_libraries(good PRIVATE ovt::flags)\n",
        top_extra="add_library(rootlib STATIC a.cpp)\n",
    )
    done = _run(tmp_path, build)
    assert done.returncode == 0, done.stdout + done.stderr
