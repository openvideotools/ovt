# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The committed libaom ``inspect`` goldens agree with the ``aomdec --summary`` counts recorded beside them
(chapter 14 §14.4.5, WI-0.566 step 5 and acceptance test 1)."""

from __future__ import annotations

import gzip
import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
GOLDEN = REPO / "tests" / "golden"
sys.path.insert(0, str(REPO / "tools" / "goldens"))

import av1_selfcheck as sc  # noqa: E402

INDEX = (
    json.loads((GOLDEN / "av1-index.json").read_text(encoding="utf-8")) if (GOLDEN / "av1-index.json").is_file() else {}
)
ENTRIES = {cid: e["goldens"]["aom-inspect"] for cid, e in INDEX.get("entries", {}).items() if e["goldens"]}

pytestmark = pytest.mark.skipif(not ENTRIES, reason="no AV1 goldens committed")


def load(cid: str) -> dict:
    with gzip.open(GOLDEN / ENTRIES[cid]["file"], "rt", encoding="utf-8") as f:
        return json.load(f)


@pytest.mark.parametrize("cid", sorted(ENTRIES))
def test_frame_counts_match_aomdec(cid: str) -> None:
    rec = ENTRIES[cid]
    doc = load(cid)
    rep = sc.check(doc, rec["temporal_units"], rec["aomdec_decoded"], rec["aomdec_showed"])
    known = INDEX["entries"][cid].get("selfcheck_known", {}).get("problems", [])
    assert [p for p in rep["problems"] if p not in known] == []
    assert rep["frames"] == rec["frames"]
    assert rep["hidden"] == rec["hidden"]


@pytest.mark.parametrize("cid", sorted(ENTRIES))
def test_golden_is_under_the_size_cap(cid: str) -> None:
    with gzip.open(GOLDEN / ENTRIES[cid]["file"], "rb") as f:
        assert len(f.read()) <= INDEX["max_bytes"]


def test_hidden_altref_showexisting_has_hidden_frames() -> None:
    cid = "syn_av1_hidden_altref_showexisting"
    if cid not in ENTRIES:
        pytest.skip("no golden for it: every temporal unit exceeds the size cap (see av1-index.json)")
    rec = ENTRIES[cid]
    assert rec["hidden"] >= 1
    assert all(f["header"]["show_frame"] in (0, 1) for f in load(cid)["frames"])
