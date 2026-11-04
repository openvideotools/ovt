# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Checks of the licence texts of WI-0.600 (chapter 15 §15.1)."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
LICENSES = REPO / "LICENSES"
AOM = LICENSES / "LicenseRef-AOM-Patent-License-1.0.txt"
EXPECTED = {
    "LGPL-2.1-or-later.txt",
    "GPL-3.0-or-later.txt",
    "LGPL-3.0-or-later.txt",
    "BSD-2-Clause.txt",
    "BSD-3-Clause.txt",
    "CC-BY-SA-4.0.txt",
    "CC-BY-4.0.txt",  # docs/spec/ (#20)
    "LicenseRef-AOM-Patent-License-1.0.txt",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def test_licence_directory_holds_the_expected_texts() -> None:
    assert {p.name for p in LICENSES.iterdir()} == EXPECTED


def test_aom_patent_text_matches_the_versions_md_pin() -> None:
    # Acceptance test 2: the SHA-256 equals the `PATENTS sha256=` note of the libaom row (WI-0.010).
    versions = REPO / "third_party" / "VERSIONS.md"
    if not versions.exists():
        pytest.skip("third_party/VERSIONS.md arrives with WI-0.010")
    row = next(line for line in versions.read_text(encoding="utf-8").splitlines() if line.startswith("| libaom |"))
    m = re.search(r"PATENTS sha256=([0-9a-f]{64})", row)
    assert m, "libaom row has no PATENTS sha256= note yet (tools/versions_to_cmake.py --update-hashes)"
    assert sha256(AOM) == m.group(1)


def test_aom_plugin_ships_the_same_patents_file() -> None:
    assert (REPO / "plugins" / "ovt-plugin-aom" / "PATENTS").read_bytes() == AOM.read_bytes()


def test_license_is_preamble_plus_gpl3() -> None:
    text = (REPO / "LICENSE").read_text(encoding="utf-8")
    preamble, _, rest = text.partition("\n\n")
    assert preamble.startswith("Open Video Tools is distributed as a whole under GPL-3.0-or-later.")
    assert rest == (LICENSES / "GPL-3.0-or-later.txt").read_text(encoding="utf-8")
