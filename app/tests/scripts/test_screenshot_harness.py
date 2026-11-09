# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""The screenshot harness (chapter 14 section 14.10.2; WI-0.568 step 3): size, determinism and the themes."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "tools"))
import imgdiff


@pytest.fixture(scope="module")
def harness(request: pytest.FixtureRequest) -> str:
    path = request.config.getoption("--ovt-studio-screenshots")
    if not path or not Path(path).exists():
        pytest.skip("ovt_studio_screenshots not given (--ovt-studio-screenshots)")
    return path


def render(harness: str, out: Path, theme: str, scene: str = "main_window") -> subprocess.CompletedProcess:
    return subprocess.run(
        [harness, "--scene", scene, "--theme", theme, "--out", str(out)],
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )


def imgdiff_ui(a: Path, b: Path) -> int:
    return subprocess.run(
        [sys.executable, "-I", str(REPO / "tools/imgdiff.py"), "--mode", "ui", str(a), str(b)],
        capture_output=True,
        text=True,
        check=False,
    ).returncode


def test_list_scenes(harness: str) -> None:
    res = subprocess.run([harness, "--list-scenes"], capture_output=True, text=True, timeout=30, check=False)
    assert res.returncode == 0 and "main_window" in res.stdout.split()


def test_bad_arguments(harness: str, tmp_path: Path) -> None:
    assert render(harness, tmp_path, "light", scene="no_such_scene").returncode == 2
    assert render(harness, tmp_path, "purple").returncode == 2


def test_fixed_size_and_deterministic(harness: str, tmp_path: Path) -> None:
    """14.10.2: 1600x1000; two runs of the same scene and theme are byte-identical (spec section 0.7.5)."""
    for run in ("one", "two"):
        res = render(harness, tmp_path / run, "light")
        assert res.returncode == 0, res.stderr
    first, second = tmp_path / "one/main_window_light.png", tmp_path / "two/main_window_light.png"
    pix, depth = imgdiff.read_png(first)
    assert depth == 8 and pix.shape == (1000, 1600, 3)
    assert first.read_bytes() == second.read_bytes()
    assert imgdiff_ui(first, second) == 0


def test_themes_differ(harness: str, tmp_path: Path) -> None:
    for theme in ("light", "dark"):
        assert render(harness, tmp_path, theme).returncode == 0
    assert imgdiff_ui(tmp_path / "main_window_light.png", tmp_path / "main_window_dark.png") == 1
