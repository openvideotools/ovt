# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Checks of the container entries of tests/corpora/manifest.json (chapter 8 section 8.9.3; WI-1.237)."""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ENTRIES = json.loads((REPO / "tests/corpora/manifest.json").read_text(encoding="utf-8"))["entries"]
MIB = 1 << 20


def tagged(tag: str) -> list[dict]:
    return [e for e in ENTRIES if tag in e.get("tags", [])]


def test_container_tags_are_present() -> None:
    for tag in ("container-smoke", "container-full", "isobmff-broken"):
        assert tagged(tag), tag


def test_smoke_set_is_verified_fast_and_within_300_mib() -> None:
    smoke = tagged("container-smoke")
    assert sum(e["size_bytes"] for e in smoke) <= 300 * MIB
    for e in smoke:
        assert e.get("sha256") and not e.get("unverified"), e["id"]
        assert "fast" in e["tags"], e["id"]
        assert "container-full" not in e["tags"], e["id"]


def test_smoke_set_has_one_file_per_content() -> None:
    digests = [e["sha256"] for e in tagged("container-smoke")]
    assert len(digests) == len(set(digests))


def test_smoke_set_covers_every_phase_one_format() -> None:
    formats = {e["expected"]["format"] for e in tagged("container-smoke")}
    assert {"isobmff", "ivf", "annexb"} <= formats


@pytest.mark.parametrize("tag", ["container-smoke", "container-full", "isobmff-broken"])
def test_every_tagged_entry_has_an_expected_block(tag: str) -> None:
    for e in tagged(tag):
        assert e.get("expected", {}).get("format"), e["id"]


def test_broken_entries_name_diagnostics() -> None:
    for e in tagged("isobmff-broken"):
        codes = e["expected"]["diagnostics"]
        assert codes and all(re.fullmatch(r"CNT-[0-9]{4}", c) for c in codes), e["id"]


def test_full_set_is_verified() -> None:
    for e in tagged("container-full"):
        assert e.get("sha256"), e["id"]
