# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/goldens/gen_units.py, the generator of the ``<id>.ovt-units.json`` goldens (chapter 14 WI-1.570)."""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "goldens"))

import gen_units as gen

LOG = """\
[trace_headers @ 0x1] Extradata
[trace_headers @ 0x1] 3           nal_unit_type                                           00111 = 7
[trace_headers @ 0x1] 3           nal_unit_type                                           01000 = 8
[hevc @ 0x2] 3           nal_unit_type                                           00001 = 1
[trace_headers @ 0x1] Packet: 2288 bytes, key frame, no pts, dts -96000, duration 120000.
[trace_headers @ 0x1] 3           nal_unit_type                                           00111 = 7
[trace_headers @ 0x1] 3           nal_unit_type                                           00101 = 5
[trace_headers @ 0x1] 3           nal_unit_type                                           00101 = 5
size=       0kB time=00:00:00.00 bitrate=N/A speed=N/A    [trace_headers @ 0x1] Packet: 35 bytes, no pts, dts 1.
[trace_headers @ 0x1] 3           nal_unit_type                                           00001 = 1
[extract_extradata @ 0x3] obu_type: 2, temporal_id: 0
"""


def test_parse_trace_groups_nal_types_by_packet() -> None:
    assert gen.parse_trace(LOG) == [(2288, [7, 5, 5]), (35, [1])]


def test_render_and_classify() -> None:
    doc = {"id": "x", "codec": "h264", "framing": "annexb", "packets": [{"pos": 0, "size": 9, "types": [7]}]}
    assert gen.render(doc) == gen.render(doc)
    assert json.loads(gen.render(doc))["packets"] == doc["packets"]
    assert gen.classify({"containers": ["ivf"], "codecs": ["av1"]}) == ("av1", "ivf")
    assert gen.classify({"containers": ["isobmff"], "codecs": ["h264"]}) is None
    with pytest.raises(gen.StepError):
        gen.render({**doc, "packets": [{"pos": i, "size": i} for i in range(120000)]})


@pytest.mark.skipif(not (shutil.which("ffmpeg") and shutil.which("ffprobe")), reason="needs ffmpeg and ffprobe")
def test_live_h264_multislice(tmp_path: Path) -> None:
    src = tmp_path / "a.264"
    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-y",
        "-f",
        "lavfi",
        "-i",
        "testsrc=size=64x64:rate=10",
        "-frames:v",
        "6",
        "-c:v",
        "libx264",
        "-x264-params",
        "keyint=3:slices=2",
        str(src),
    ]
    if subprocess.run(cmd, check=False).returncode != 0:
        pytest.skip("no libx264")
    doc = gen.build(src, "x", "h264", "annexb")
    assert len(doc["packets"]) == 6
    assert doc["packets"][0]["types"][-2:] == [5, 5]  # two IDR slices: not collapsed by the log's repeat filter
    assert sum(p["size"] for p in doc["packets"]) == src.stat().st_size
