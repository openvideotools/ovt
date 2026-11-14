# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/ref_m3u8.py: the committed expectations of core/tests/data/hls match a fresh reference parse (WI-4.209)."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "ref_m3u8.py"
DATA = REPO / "core" / "tests" / "data" / "hls"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(TOOL), *args], capture_output=True, text=True, check=False)


@pytest.mark.parametrize("name", ["bipbop_adv_example_hevc_master", "byterange", "media_fmp4"])
def test_committed_expectation_is_current(name):
    result = run(str(DATA / f"{name}.m3u8"), "--url", f"https://example.invalid/hls/{name}.m3u8")
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == json.loads((DATA / f"{name}.expected.json").read_text())
