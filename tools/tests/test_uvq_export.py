# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/uvq_export: the preprocessing document check and the committed export outputs (WI-3.314)."""

from __future__ import annotations

import importlib.util
import json
import re
import sys
from pathlib import Path
from types import ModuleType

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools" / "uvq_export"
SHARE = REPO / "core" / "share" / "models" / "uvq"
HEADER = (
    "| model version | input tensor name | shape | dtype | channel order | value range/normalisation | "
    "frames per chunk | resize filter | source file:line |\n|---|---|---|---|---|---|---|---|---|\n"
)


def load(name: str) -> ModuleType:
    spec = importlib.util.spec_from_file_location(f"uvq_{name}", TOOLS / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


check_doc = load("check_doc")
export_uvq = load("export_uvq")  # stdlib imports only at module level; torch/onnx load inside the steps


def test_committed_document_cites_a_source_line_in_every_row() -> None:
    # Acceptance test 3 (the OVT: references are checked against this repository).
    assert check_doc.check(check_doc.DEFAULT_DOC, None) == []


def test_row_without_reference_is_reported(tmp_path: Path) -> None:
    doc = tmp_path / "d.md"
    doc.write_text(
        HEADER + "| 1.5 | a | [1] | float32 | RGB | x | 1 | bicubic | utils/video_reader.py:173 |\n"
        "| 1.5 | b | [1] | float32 | RGB | x | 1 | bicubic | see above |\n",
        encoding="utf-8",
    )
    assert check_doc.check(doc, None) == [f"E-NOREF {doc}:4 table row without a source file:line reference"]


def test_missing_or_short_spec_table_is_reported(tmp_path: Path) -> None:
    doc = tmp_path / "d.md"
    doc.write_text("| step | source file:line |\n|---|---|\n| a | x.py:1 |\n", encoding="utf-8")
    assert check_doc.check(doc, None) == [f"E-NOTABLE {doc} no table with the §9.6.4 header"]
    doc.write_text("| model version | source file:line |\n|---|---|\n| 1.5 | x.py:1 |\n", encoding="utf-8")
    assert check_doc.check(doc, None) == [f"E-COLUMNS {doc}:1 §9.6.4 table has 2 columns, expected 9"]


def test_cited_lines_are_checked_against_the_clone(tmp_path: Path) -> None:
    clone = tmp_path / "uvq"
    (clone / "utils").mkdir(parents=True)
    (clone / "utils" / "video_reader.py").write_text("a\nb\n", encoding="utf-8")
    doc = tmp_path / "d.md"
    doc.write_text(
        HEADER + "| 1.5 | a | [1] | f | RGB | x | 1 | b | utils/video_reader.py:2, utils/video_reader.py:3 |\n",
        encoding="utf-8",
    )
    assert check_doc.check(doc, clone) == [f"E-BADREF {doc}:3 utils/video_reader.py:3 file or line does not exist"]


def test_manifest_lists_every_exported_graph_with_a_hash() -> None:
    manifest = json.loads((SHARE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == 1 and manifest["model"] == "uvq" and manifest["version"] == "1.5"
    assert manifest["commit"] == export_uvq.UVQ_COMMIT and manifest["license"] == "Apache-2.0"
    names = {f["name"] for f in manifest["files"]}
    assert names == {f"{g}_{p}.onnx" for g in export_uvq.GRAPHS for p in ("fp32", "fp16")}
    for f in manifest["files"]:
        assert re.fullmatch(r"[0-9a-f]{64}", f["sha256"]) and f["bytes"] > 0
        assert f["url"].startswith("https://") and f["url"].endswith("/" + f["name"])
        assert f["inputs"] == export_uvq.GRAPHS[f["name"].rsplit("_", 1)[0]][2]


def test_labels_file_holds_the_youtube8m_vocabulary() -> None:
    labels = (SHARE / "labels_yt8m.txt").read_text(encoding="utf-8").splitlines()
    assert len(labels) == 3862 and labels[0] == "Game" and all(label.strip() for label in labels)


def test_report_passes_on_twenty_clips_and_matches_the_manifest() -> None:
    # Acceptance tests 1 and 2 are run by `export_uvq.py all`; this pins the committed record of that run.
    report = json.loads((TOOLS / "report.json").read_text(encoding="utf-8"))
    assert report["uvq_commit"] == export_uvq.UVQ_COMMIT and report["opset"] == 17
    for version in ("1.5", "1.0"):
        clips = report["clips"][version]
        assert sorted(clips) == export_uvq.CLIP_IDS
        assert all(abs(c["fp32"]["delta"]) <= 0.01 and abs(c["fp16"]["delta"]) <= 0.02 for c in clips.values())
    manifest = json.loads((SHARE / "manifest.json").read_text(encoding="utf-8"))
    for f in manifest["files"]:
        graph, precision = f["name"].removesuffix(".onnx").rsplit("_", 1)
        assert report["models"][graph][precision]["sha256"] == f["sha256"]


def test_weight_hash_list_parses() -> None:
    hashes = export_uvq.read_weight_hashes()
    assert len(hashes) == 42 and all(re.fullmatch(r"[0-9a-f]{64}", h) for h in hashes.values())
