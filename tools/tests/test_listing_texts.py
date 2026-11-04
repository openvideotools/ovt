# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Schema and length checks of tools/release/listing_texts.toml (chapter 15 §15.8, WI-0.608).

Standalone: needs only the standard library and pytest. The metainfo test also needs `appstreamcli` and
skips without it. tools/release/collect.py --check-listings (WI-0.036) repeats these checks.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tomllib
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
LISTINGS = REPO / "tools" / "release" / "listing_texts.toml"
GEN_METAINFO = REPO / "tools" / "release" / "gen_metainfo.py"

# Channels marked "Yes" (or "Primary", for the cask) in §15.8.
CHANNELS = ("play", "fdroid", "flathub", "homebrew", "pypi", "github")
KEYS = {"short", "long", "licence_line", "patent_notice"}
SHORT_MAX = 80  # Play: 80 characters; F-Droid summary: 80 bytes. The stricter (bytes) applies everywhere.
LONG_MAX = 4000  # Play long description
LICENCE = "GPL-3.0-or-later"
SOURCE_TAG = "https://github.com/openvideotools/ovt/tree/v{version}"
PATENT_WORDS = re.compile(r"\bpatent", re.IGNORECASE)


@pytest.fixture(scope="module")
def listings() -> dict:
    with LISTINGS.open("rb") as f:
        return tomllib.load(f)


def test_product_licence(listings: dict) -> None:
    assert listings["product"]["project_license"] == LICENCE
    assert listings["product"]["source_url"] == SOURCE_TAG


@pytest.mark.parametrize("channel", CHANNELS)
def test_channel_present_with_schema(listings: dict, channel: str) -> None:
    entry = listings["channels"].get(channel)
    assert entry is not None, f"[channels.{channel}] missing"
    assert set(entry) == KEYS, f"[channels.{channel}] keys {sorted(entry)}"
    assert isinstance(entry["patent_notice"], bool)
    for key in ("short", "long", "licence_line"):
        assert isinstance(entry[key], str) and entry[key].strip(), f"{channel}.{key} empty"


def test_no_unknown_channels(listings: dict) -> None:
    assert set(listings["channels"]) == set(CHANNELS)


@pytest.mark.parametrize("channel", CHANNELS)
def test_lengths(listings: dict, channel: str) -> None:
    entry = listings["channels"][channel]
    short = entry["short"]
    assert "\n" not in short
    assert len(short.encode("utf-8")) <= SHORT_MAX, f"{channel}.short is {len(short.encode())} bytes"
    assert len(entry["long"].strip()) <= LONG_MAX, f"{channel}.long is {len(entry['long'].strip())} chars"


@pytest.mark.parametrize("channel", CHANNELS)
def test_licence_line_states_licence_and_source_tag(listings: dict, channel: str) -> None:
    line = listings["channels"][channel]["licence_line"]
    assert LICENCE in line
    assert SOURCE_TAG in line


def test_patent_notice_only_where_codec_pack_is_listed(listings: dict) -> None:
    # The default downloads carry no AVC/HEVC decoder (POL-PAT-2); only GitHub Releases list the codec pack.
    flags = {c: listings["channels"][c]["patent_notice"] for c in CHANNELS}
    assert flags == {c: c == "github" for c in CHANNELS}


@pytest.mark.parametrize("channel", ("homebrew",))
def test_cask_desc_style(listings: dict, channel: str) -> None:
    # Homebrew `brew audit`: no product name and no final full stop in `desc`.
    short = listings["channels"][channel]["short"]
    assert not short.endswith(".")
    assert "OVT" not in short


def test_short_texts_have_no_patent_sentence(listings: dict) -> None:
    for channel in CHANNELS:
        assert not PATENT_WORDS.search(listings["channels"][channel]["short"]), channel


def test_metainfo_project_license(tmp_path: Path) -> None:
    out = tmp_path / "org.openvideotools.Studio.metainfo.xml"
    subprocess.run(
        [sys.executable, "-I", str(GEN_METAINFO), "--out", str(out), "--version", "1.0.0", "--date", "2026-11-04"],
        check=True,
    )
    root = ET.parse(out).getroot()
    assert root.findtext("project_license") == LICENCE
    assert root.findtext("id") == "org.openvideotools.Studio"
    first = out.read_bytes()
    subprocess.run(
        [sys.executable, "-I", str(GEN_METAINFO), "--out", str(out), "--version", "1.0.0", "--date", "2026-11-04"],
        check=True,
    )
    assert out.read_bytes() == first, "metainfo output is not deterministic"
    if shutil.which("appstreamcli") is None:
        pytest.skip("appstreamcli not installed")
    result = subprocess.run(["appstreamcli", "validate", "--no-net", str(out)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
