# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/ref_mpd.py: the committed expectation of core/tests/data/dash matches a fresh expansion (WI-4.208)."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "ref_mpd.py"
DATA = REPO / "core" / "tests" / "data" / "dash"


def run(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "-I", str(TOOL), *args], capture_output=True, text=True, check=False)


def test_committed_expectation_is_current():
    result = run(str(DATA / "bbb_30fps_like.mpd"), "--url", "https://example.invalid/dash/bbb_30fps_like.mpd")
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == json.loads((DATA / "bbb_30fps_like.expected.json").read_text())


def test_timeline_repeat_fills_to_period_end(tmp_path):
    mpd = tmp_path / "t.mpd"
    mpd.write_text(
        '<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" mediaPresentationDuration="PT20S"><Period>'
        '<AdaptationSet><Representation id="v"><SegmentTemplate timescale="1000" media="$Time$.m4s">'
        '<SegmentTimeline><S t="0" d="2000"/><S d="3000" r="-1"/></SegmentTimeline></SegmentTemplate>'
        "</Representation></AdaptationSet></Period></MPD>"
    )
    segs = json.loads(run(str(mpd)).stdout)["periods"][0]["adaptation_sets"][0]["representations"][0]["segments"]
    assert len(segs) == 7
    assert segs[-1]["start_time"] + segs[-1]["duration"] == 20000
