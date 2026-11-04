# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tests/oracles/oracle_versions.py, the oracle image's version report and pin check (WI-0.554)."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ORACLES = REPO / "tests" / "oracles"
sys.path.insert(0, str(ORACLES))

import oracle_versions  # noqa: E402

TABLE = f"""intro
{oracle_versions.BEGIN}
| Tool | Version | Source | Licence | Probe | Expect |
|---|---|---|---|---|---|
| alpha | 1.2.3 | somewhere | MIT | `printf 'build 2026-01-01\\nalpha version 1.2.3  (x)\\n'` | `version 1.2.3` |
| beta | 9 | elsewhere | MIT | `echo beta 8` | `beta 9` |
| pipe | 1 | x | MIT | `echo a \\| tr a b` | `b` |
{oracle_versions.END}
"""


def test_parse_table_reads_the_marked_rows() -> None:
    rows = oracle_versions.parse_table(TABLE)
    assert [r.tool for r in rows] == ["alpha", "beta", "pipe"]
    assert rows[0].probe.startswith("printf ")
    assert rows[2].probe == "echo a | tr a b"


def test_report_prints_only_the_matching_line_and_flags_mismatches() -> None:
    text, ok = oracle_versions.report(oracle_versions.parse_table(TABLE))
    assert not ok
    assert "| alpha | 1.2.3 | alpha version 1.2.3 (x) | ok |" in text
    assert "2026-01-01" not in text  # build dates never reach the report (acceptance test 3)
    assert "| beta | 9 | (no line contains beta 9; first line: beta 8) | MISMATCH |" in text
    assert "| pipe | 1 | b | ok |" in text


def test_python_package_probe() -> None:
    assert oracle_versions.run_probe("python-package:pytest") == pytest.__version__
    assert oracle_versions.run_probe("python-package:no-such-dist-ovt") == ""


def test_missing_markers_are_an_error() -> None:
    with pytest.raises(ValueError, match="markers"):
        oracle_versions.parse_table("| Tool |")


def test_committed_table_lists_every_section_14_2_6_tool() -> None:
    rows = oracle_versions.parse_table((ORACLES / "ORACLE_VERSIONS.md").read_text(encoding="utf-8"))
    tools = {r.tool for r in rows}
    assert len(tools) == len(rows)
    for tool in (
        "ffmpeg",
        "x264",
        "x265",
        "aomenc",
        "inspect",
        "dav1d",
        "vmaf",
        "MP4Box",
        "mp4dump",
        "mp4encrypt",
        "mp4fragment",
        "mp4ff-info",
        "mp4box.js",
        "mediainfo",
        "mkvinfo",
        "mkvmerge",
        "tsp",
        "tstables",
        "tsanalyze",
        "gstreamer-codecparsers",
        "ldecod",
        "TAppEncoder",
        "TAppDecoder",
        "TAppDecoder_trace",
        "dump_psnrhvs",
        "dump_ciede2000",
        "mpv",
        "uvq",
        "tensorflow-cpu",
        "tf2onnx",
        "bjontegaard",
        "siti-tools",
        "node",
    ):
        assert tool in tools, tool
    assert all(r.expect for r in rows)


def test_committed_pins_pass_the_check() -> None:
    assert oracle_versions.check_pins(ORACLES) == []


@pytest.fixture
def oracle_copy(tmp_path: Path) -> Path:
    dest = tmp_path / "oracles"
    shutil.copytree(ORACLES, dest, ignore=shutil.ignore_patterns("node_modules"))
    return dest


def _append(path: Path, text: str) -> None:
    path.write_text(path.read_text(encoding="utf-8") + text, encoding="utf-8")


@pytest.mark.parametrize(
    ("file", "addition", "message"),
    [
        ("Dockerfile", "FROM node:20 AS extra\n", "not pinned by digest"),
        ("Dockerfile", "RUN npm install -g mp4box@latest\n", "'latest'"),
        ("build_jm.sh", 'FOO_VERSION="4.5.6"\n', "FOO_VERSION=4.5.6 is not recorded"),
        ("build_jm.sh", 'BAR_SHA256="0000"\n', "BAR_SHA256 is a placeholder"),
        ("build_hm.sh", "git clone https://example.org/x.git\n", "git clone"),
        ("Dockerfile", "RUN go install example.org/tool@main\n", "no @version"),
        ("requirements.txt", "numpy\n", "numpy is not pinned"),
    ],
)
def test_check_pins_rejects_floating_inputs(oracle_copy: Path, file: str, addition: str, message: str) -> None:
    _append(oracle_copy / file, addition)
    problems = oracle_versions.check_pins(oracle_copy)
    assert any(message in p for p in problems), problems


def test_comments_are_ignored_by_the_pin_check(oracle_copy: Path) -> None:
    _append(oracle_copy / "build_jm.sh", "# the newest release is not always the latest one\n")
    assert oracle_versions.check_pins(oracle_copy) == []
