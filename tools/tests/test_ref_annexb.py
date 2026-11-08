# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/ref_annexb.py (WI-1.226)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import ref_annexb  # noqa: E402


def test_boundary_rule() -> None:
    data = b"\x00\x00\x00\x01\x67\xaa\x00\x00\x03\x01\x00\x00\x01\x68\xbb\x00\x00\x00\x00\x01\x65\xcc\x00\x00"
    assert ref_annexb.split(data) == [(4, 6, 0x67), (13, 2, 0x68), (20, 2, 0x65)]


def test_empty_nal_is_skipped_and_garbage_ignored() -> None:
    assert ref_annexb.split(b"\x11\x22\x00\x00\x01\x00\x00\x01\x65\x80") == [
        (8, 2, 0x65)
    ]
    assert ref_annexb.split(b"\x01\x02\x03") == []


def test_trailing_zero_run_is_not_part_of_the_nal() -> None:
    data = b"\x00\x00\x01\x41\x9a" + b"\x00" * 1000
    assert ref_annexb.split(data) == [(3, 2, 0x41)]


def test_cli_prints_types_per_codec(tmp_path: Path) -> None:
    f = tmp_path / "x.265"
    f.write_bytes(b"\x00\x00\x00\x01\x40\x01\xaa\x00\x00\x01\x42\x01\xbb")
    run = subprocess.run(
        [sys.executable, str(TOOLS / "ref_annexb.py"), str(f)],
        capture_output=True,
        text=True,
    )
    assert run.returncode == 0
    assert run.stdout.splitlines() == ["4,3,32", "10,3,33"]
    run = subprocess.run(
        [sys.executable, str(TOOLS / "ref_annexb.py"), "--codec", "h264", str(f)],
        capture_output=True,
        text=True,
    )
    assert run.stdout.splitlines() == ["4,3,0", "10,3,2"]
