# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the lane dispatcher tools/ctest.sh (chapter 13 §13.2.4, WI-0.001)."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

SCRIPT = Path(__file__).resolve().parents[1] / "ctest.sh"


def dispatch(*args: str, host: str | None = None) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ, OVT_CTEST_DRY_RUN="1")
    if host is not None:
        env["OVT_CTEST_HOST"] = host
    return subprocess.run(
        ["bash", str(SCRIPT), *args], env=env, capture_output=True, text=True, check=False
    )


@pytest.mark.parametrize(
    ("host", "lane", "expected"),
    [
        ("Linux", "pr", "pr"),
        ("Linux", "nightly", "nightly"),
        ("Linux", "sanitizers", "sanitizers"),
        ("Linux", "perf", "perf"),
        ("Darwin", "pr", "pr-macos"),
        ("Darwin", "nightly", "nightly-macos"),
        ("Darwin", "sanitizers", "sanitizers-macos"),
        ("Darwin", "perf", "perf-macos"),
    ],
)
def test_host_picks_lane_preset(host: str, lane: str, expected: str) -> None:
    result = dispatch(lane, host=host)
    assert result.returncode == 0, result.stderr
    assert result.stderr.strip() == f"+ ctest --preset {expected}"


@pytest.mark.parametrize(
    ("configure_preset", "expected"),
    [
        ("macos-arm64-release", "pr-macos"),
        ("linux-x64-debug", "pr"),
        ("android-x86_64-debug", "pr-android"),
    ],
)
def test_configure_preset_overrides_host(configure_preset: str, expected: str) -> None:
    result = dispatch("pr", configure_preset, host="Linux")
    assert result.returncode == 0, result.stderr
    assert result.stderr.strip() == f"+ ctest --preset {expected}"


def test_extra_arguments_are_passed_on() -> None:
    result = dispatch("pr", "--", "-R", "version", host="Linux")
    assert result.stderr.strip() == "+ ctest --preset pr -R version"


@pytest.mark.parametrize(
    "args",
    [
        ("bogus",),
        ("nightly", "android-arm64-debug"),
        ("pr", "windows-x64-debug"),
        (),
    ],
)
def test_invalid_input_exits_2(args: tuple[str, ...]) -> None:
    assert dispatch(*args, host="Linux").returncode == 2


def test_unknown_host_exits_2() -> None:
    assert dispatch("pr", host="Windows_NT").returncode == 2
