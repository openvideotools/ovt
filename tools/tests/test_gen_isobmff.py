# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/goldens/gen_isobmff.py and tests/oracles/mp4boxjs_dump.mjs (chapter 14 §14.4.9; WI-0.564)."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "goldens"))
sys.path.insert(0, str(REPO / "tests" / "oracles" / "normalize"))

import canon  # noqa: E402
import gen_isobmff as gen  # noqa: E402

GOLDEN = REPO / "tests" / "golden" / "isobmff"
DUMP = REPO / "tests" / "oracles" / "mp4boxjs_dump.mjs"
TINY = REPO / "tests" / "golden" / "isobmff" / "ovt_min_structural.mp4"


def copy_goldens(tmp_path: Path) -> Path:
    out = tmp_path / "isobmff"
    out.mkdir()
    for p in GOLDEN.glob("*.json"):
        shutil.copy(p, out / p.name)
    shutil.copy(GOLDEN / "KNOWN_DIFFS.md", out / "KNOWN_DIFFS.md")
    return out


def test_committed_goldens_are_self_consistent(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """WI-0.564 acceptance test 1: the five oracles agree, apart from the documented differences."""
    assert gen.main(["check", "--out", str(GOLDEN)]) == 0
    assert "0 problem(s)" in capsys.readouterr().out


def test_every_golden_matches_its_family_schema() -> None:
    for path in sorted(GOLDEN.glob("*.json")):
        if path.name == "ORACLES.json" or path.name.endswith(".pybox.json"):
            continue
        family = "sample-table" if path.name.endswith(".mp4box-dts.json") else "box-tree"
        assert canon.validate(json.loads(path.read_text(encoding="utf-8")), family) == [], path.name


def test_an_undocumented_disagreement_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out = copy_goldens(tmp_path)
    target = out / "mpegff_a4_tone_fragmented.mp4ff-info.json"
    doc = json.loads(target.read_text(encoding="utf-8"))
    doc[0]["size"] += 1
    target.write_text(json.dumps(doc), encoding="utf-8")
    assert gen.main(["check", "--out", str(out)]) == 1
    assert "undocumented: mpegff_a4_tone_fragmented size ftyp" in capsys.readouterr().out


def test_a_sample_outside_every_mdat_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out = copy_goldens(tmp_path)
    target = out / "mpegff_a4_tone_fragmented.mp4box-dts.json"
    doc = json.loads(target.read_text(encoding="utf-8"))
    doc[0]["offset"] = 3
    target.write_text(json.dumps(doc), encoding="utf-8")
    assert gen.main(["check", "--out", str(out)]) == 1
    assert "sample-outside-mdat" in capsys.readouterr().out


def test_a_stale_known_diffs_row_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out = copy_goldens(tmp_path)
    with (out / "KNOWN_DIFFS.md").open("a", encoding="utf-8") as fh:
        fh.write("| `mpegff_a4_tone_fragmented` | `size` | `moov` | stale |\n")
    assert gen.main(["check", "--out", str(out)]) == 1
    assert "stale KNOWN_DIFFS row" in capsys.readouterr().out


def test_known_diffs_table_parses() -> None:
    rows = gen.known_diffs(GOLDEN / "KNOWN_DIFFS.md")
    assert ("*", "header_length", "*/stsd/????") in rows and all(len(r) == 3 for r in rows)


def mp4box_js_available() -> bool:
    if shutil.which("node") is None:
        return False
    env = dict(os.environ)
    probe = subprocess.run(
        ["node", "-e", "require('node:module').createRequire('/')('mp4box')"],
        capture_output=True,
        env=env,
        check=False,
    )
    return probe.returncode == 0


def box(fourcc: bytes, payload: bytes = b"") -> bytes:
    return (8 + len(payload)).to_bytes(4, "big") + fourcc + payload


def full(fourcc: bytes, payload: bytes = b"", version: int = 0, flags: int = 0) -> bytes:
    return box(fourcc, bytes([version]) + flags.to_bytes(3, "big") + payload)


def stsd_dref_file() -> bytes:
    """A hand-made moov with dinf/dref/url and stsd/mp4a, the two places where mp4box.js keeps children in `entries`."""
    entry = box(
        b"mp4a",
        bytes(6)
        + (1).to_bytes(2, "big")
        + bytes(8)
        + (2).to_bytes(2, "big")
        + (16).to_bytes(2, "big")
        + bytes(4)
        + (48000 << 16).to_bytes(4, "big"),
    )
    stbl = box(b"stbl", full(b"stsd", (1).to_bytes(4, "big") + entry))
    dinf = box(b"dinf", full(b"dref", (1).to_bytes(4, "big") + full(b"url ", b"", flags=1)))
    return box(b"ftyp", b"isom" + bytes(4) + b"isom") + box(
        b"moov", box(b"trak", box(b"mdia", box(b"minf", dinf + stbl)))
    )


@pytest.mark.skipif(
    not mp4box_js_available(),
    reason="node with mp4box.js on NODE_PATH (the oracle image)",
)
def test_dump_script_emits_version_and_flags_for_every_fullbox_and_recurses_into_entries(
    tmp_path: Path,
) -> None:
    """Issue #139: FullBoxes carry both keys, and stsd / dref children appear with continuing paths."""
    missing = subprocess.run(
        ["node", str(DUMP), str(tmp_path / "absent.mp4")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert missing.returncode == 3 and "error:" in missing.stderr
    src = tmp_path / "t.mp4"
    src.write_bytes(stsd_dref_file())
    first = subprocess.run(["node", str(DUMP), str(src)], capture_output=True, text=True, check=True).stdout
    again = subprocess.run(["node", str(DUMP), str(src)], capture_output=True, text=True, check=True).stdout
    assert first == again  # deterministic
    boxes = {b["path"]: b for b in json.loads(first)}
    base = "moov/trak/mdia/minf/"
    assert base + "dinf/dref/url " in boxes and base + "stbl/stsd/mp4a" in boxes
    for path in (base + "dinf/dref", base + "dinf/dref/url ", base + "stbl/stsd"):
        assert "version" in boxes[path]["fields"] and "flags" in boxes[path]["fields"], path
    assert boxes[base + "dinf/dref/url "]["fields"]["flags"] == 1
