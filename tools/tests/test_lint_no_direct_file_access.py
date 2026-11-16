# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/lint_no_direct_file_access.py: the GUI and Python never map the media file (FR-CNT-104, WI-1.234)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "lint_no_direct_file_access.py"


def run(*roots: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-I", str(TOOL), *map(str, roots)], capture_output=True, text=True, check=False
    )


def test_clean_tree_passes(tmp_path: Path) -> None:
    (tmp_path / "ok.cpp").write_text("auto b = ovt_container_bytes(c, 0, 16, &out);\n", encoding="utf-8")
    assert run(tmp_path).returncode == 0


def test_each_spelling_is_found(tmp_path: Path) -> None:
    sources = {
        "a.py": "import mmap\n",
        "b.py": "x = np.memmap(path)\n",
        "c.cpp": "uchar* p = file.map(0, 16); // QFile::map\n",
        "d.cpp": "p = mmap(nullptr, n, PROT_READ, MAP_SHARED, fd, 0);\n",
        "e.kt": "val b: MappedByteBuffer = ch.map(READ_ONLY, 0, n)\n",
    }
    for name, text in sources.items():
        (tmp_path / name).write_text(text, encoding="utf-8")
    out = run(tmp_path)
    assert out.returncode == 1
    for name in sources:
        assert name in out.stdout
    assert "a.py:1" in out.stdout


def test_binary_files_and_build_dirs_are_skipped(tmp_path: Path) -> None:
    (tmp_path / "icon.bin").write_bytes(b"\xff\xfemmap\x00")
    build = tmp_path / "build"
    build.mkdir()
    (build / "gen.cpp").write_text("mmap\n", encoding="utf-8")
    assert run(tmp_path).returncode == 0


def test_repository_app_and_python_are_clean() -> None:
    out = subprocess.run([sys.executable, "-I", str(TOOL)], capture_output=True, text=True, check=False, cwd=REPO)
    assert out.returncode == 0, out.stdout


def test_flag_is_a_usage_error() -> None:
    assert subprocess.run([sys.executable, "-I", str(TOOL), "--bogus"], capture_output=True, check=False).returncode == 2
