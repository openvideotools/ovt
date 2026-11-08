# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Texts of code that a dependency compiles in besides its own (third_party/licenses/<dep>/, WI-0.017 follow-up).

The texts are staged into THIRD_PARTY_NOTICES.md by cmake/OvtNotices.cmake, which is only reached through the
LICENSE_FILES of the dependency's script. This test keeps the three in step: every file under
third_party/licenses/<dep>/ is named in third_party/<dep>.cmake, every named file exists, its name ends in the SPDX
id of the REUSE annotation that covers it, and the file is not empty.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import tomllib

REPO = Path(__file__).resolve().parents[2]
LICENCES = REPO / "third_party" / "licenses"
DEPS = sorted(p.name for p in LICENCES.iterdir() if p.is_dir())


def listed(dep: str) -> set[str]:
    text = (REPO / "third_party" / f"{dep}.cmake").read_text(encoding="utf-8")
    return set(re.findall(rf"licenses/{dep}/([A-Za-z0-9_.+-]+\.txt)", text)) | set(
        re.findall(r"\$\{_ovt_\w+_licdir\}/([A-Za-z0-9_.+-]+\.txt)", text)
    )


def reuse_annotations() -> dict[str, str]:
    data = tomllib.loads((REPO / "REUSE.toml").read_text(encoding="utf-8"))
    out: dict[str, str] = {}
    for a in data["annotations"]:
        paths = [a["path"]] if isinstance(a["path"], str) else a["path"]
        for p in paths:
            out[p] = a["SPDX-License-Identifier"]
    return out


def test_dependencies_with_bundled_texts() -> None:
    assert DEPS == ["dav1d", "libde265", "libplacebo", "libvmaf"]


@pytest.mark.parametrize("dep", DEPS)
def test_every_text_is_staged_and_every_staged_text_exists(dep: str) -> None:
    on_disk = {p.name for p in (LICENCES / dep).iterdir()}
    assert listed(dep) == on_disk


@pytest.mark.parametrize("dep", DEPS)
def test_text_names_and_annotations_agree(dep: str) -> None:
    ann = reuse_annotations()
    for p in sorted((LICENCES / dep).iterdir()):
        assert p.stat().st_size > 200, f"{p.name} is too short to be a licence text"
        spdx = ann.get(f"third_party/licenses/{dep}/{p.name}")
        assert spdx, f"{p.name}: no REUSE annotation"
        assert p.stem.endswith(spdx), f"{p.name}: file name does not end in its SPDX id {spdx}"
        assert (REPO / "LICENSES" / f"{spdx}.txt").is_file()
