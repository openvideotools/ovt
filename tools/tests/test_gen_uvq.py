# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/goldens/gen_uvq.py with a stand-in for google/uvq's ``uvq_inference.py`` (chapter 14 §14.4.14; WI-0.565b).

The real script and its PyTorch checkpoints only exist in the oracle image; the stand-in writes the result dictionaries
of UVQ 1.5 and UVQ 1.0 so that the command line, the normaliser, the gzip and the index run for real.
"""

from __future__ import annotations

import gzip
import json
import sys
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "goldens"))
sys.path.insert(0, str(REPO / "tests" / "oracles" / "normalize"))

import canon  # noqa: E402
import gen_uvq as gen  # noqa: E402

STUB = """
    import argparse, json
    p = argparse.ArgumentParser()
    p.add_argument("input"); p.add_argument("--model_version"); p.add_argument("--device"); p.add_argument("--threads")
    p.add_argument("--fps"); p.add_argument("--output")
    a = p.parse_args()
    assert a.device == "cpu" and a.threads == "1" and a.fps == "1", a
    if a.model_version == "1.5":
        res = {"uvq1p5_score": 3.5, "per_frame_scores": [3.25, 3.75], "frame_indices": [0, 25], "video_name": a.input}
    else:
        res = {"compression_content_distortion": 3.125, "compression": 3.0, "content": 3.25, "distortion": 3.5,
               "video_name": a.input}
    json.dump(res, open(a.output, "w"))
"""


@pytest.fixture
def setup(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    script = tmp_path / "uvq_inference.py"
    script.write_text(textwrap.dedent(STUB), encoding="utf-8")
    monkeypatch.setenv("OVT_ORACLE_UVQ_INFERENCE", str(script))
    corpora = tmp_path / "corpora"
    for cid in gen.CLIPS:
        d = corpora / cid
        d.mkdir(parents=True)
        (d / "clip.mp4").write_bytes(cid.encode())
        (d / ".ok").write_text(json.dumps({"primary": "clip.mp4"}), encoding="utf-8")
    return corpora


def test_plan_lists_every_clip_and_version(capsys: pytest.CaptureFixture[str]) -> None:
    assert gen.main(["--list"]) == 0
    out = capsys.readouterr().out.splitlines()
    assert out == [f"{c} uvq-1.5 uvq-1.0" for c in gen.CLIPS]


def test_goldens_are_valid_and_reproducible(setup: Path, tmp_path: Path) -> None:
    outs = []
    for n in (1, 2):
        out = tmp_path / f"out{n}"
        assert gen.main(["--out", str(out), "--corpora-dir", str(setup)]) == 0
        outs.append(out)
    names = sorted(p.name for p in outs[0].iterdir())
    assert names == sorted([f"{c}.uvq-{v}.json.gz" for c in gen.CLIPS for v in gen.VERSIONS] + ["uvq-index.json"])
    for name in names:
        assert (outs[0] / name).read_bytes() == (outs[1] / name).read_bytes()
    doc = json.loads(gzip.decompress((outs[0] / "ytugc_clip01.uvq-1.5.json.gz").read_bytes()))
    assert canon.validate(doc, "scores") == []
    assert doc["pooled"]["uvq"]["mean"] == 3.5 and [f["i"] for f in doc["frames"]] == [
        0,
        25,
    ]
    doc10 = json.loads(gzip.decompress((outs[0] / "ytugc_clip01.uvq-1.0.json.gz").read_bytes()))
    assert doc10["pooled"]["uvq"]["compression_content_distortion"] == 3.125


def test_a_failing_oracle_is_exit_1(setup: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    bad = tmp_path / "bad.py"
    bad.write_text("import sys; sys.exit(3)\n", encoding="utf-8")
    monkeypatch.setenv("OVT_ORACLE_UVQ_INFERENCE", str(bad))
    assert (
        gen.main(
            [
                "--out",
                str(tmp_path / "o"),
                "--corpora-dir",
                str(setup),
                "--only",
                "ytugc_clip01",
            ]
        )
        == 1
    )


def test_a_missing_oracle_is_exit_4(setup: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("OVT_ORACLE_UVQ_INFERENCE", str(tmp_path / "nope.py"))
    assert gen.main(["--out", str(tmp_path / "o"), "--corpora-dir", str(setup)]) == 4


def test_a_missing_corpus_is_exit_4(tmp_path: Path, setup: Path) -> None:
    assert gen.main(["--out", str(tmp_path / "o"), "--corpora-dir", str(tmp_path / "empty")]) == 4
