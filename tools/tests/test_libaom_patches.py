# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the libaom extension-patch hook (WI-2.154, chapter 07 §7.6.1): third_party/libaom.cmake applying
OVT_AOM_PATCH_DIR/*.patch through tools/apply_patches.py --check, and the LIBAOM_SOURCE_DIR export.

The builds use a stand-in libaom-3.12.1 tarball (a one-file static library that ignores libaom's options) served
from a file:// URL, so they need cmake and a C compiler but no network. The real libaom and the inspection-callback
smoke program (acceptance test 1) are core/tests/test_aom_inspection_smoke.cpp."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import apply_patches  # noqa: E402

CMAKE = shutil.which("cmake")
CC = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
needs_build = pytest.mark.skipif(CMAKE is None or CC is None, reason="cmake or a C compiler is not installed")

HEADER = "| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |\n|---|---|---|---|---|---|---|---|\n"

INSPECTION_C = """\
/* line one */
int ifd_stub(void) { return 1; }
"""

UPSTREAM_CMAKE = """\
cmake_minimum_required(VERSION 3.5)
project(AOM C)
add_library(aom STATIC av1/decoder/inspection.c)
include(GNUInstallDirs)
install(TARGETS aom ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES aom/aom_decoder.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aom)
"""


def patch(name: str, old: str, new: str) -> tuple[str, str]:
    body = (
        f"From: OVT\nSubject: [PATCH] {name}\nUpstream-Status: Inappropriate [test fixture]\n\n"
        "--- a/av1/decoder/inspection.c\n+++ b/av1/decoder/inspection.c\n"
        f"@@ -1,2 +1,2 @@\n-{old}\n+{new}\n"
        " int ifd_stub(void) { return 1; }\n"
    )
    return name, body


GOOD = patch("0001-inspection-extend-mi-data.patch", "/* line one */", "/* line one, patched */")
BAD = patch(
    "0002-inspection-frame-snapshots.patch",
    "/* a line that is not there */",
    "/* never */",
)


def fixture_archive(tmp: Path) -> tuple[Path, str]:
    src = tmp / "upstream-src" / "libaom-3.12.1"
    (src / "av1" / "decoder").mkdir(parents=True)
    (src / "aom").mkdir()
    (src / "CMakeLists.txt").write_text(UPSTREAM_CMAKE)
    (src / "LICENSE").write_text("BSD-2-Clause\n")
    (src / "PATENTS").write_text("AOM Patent License 1.0\n")
    (src / "aom" / "aom_decoder.h").write_text("#pragma once\n")
    (src / "av1" / "decoder" / "inspection.c").write_text(INSPECTION_C)
    (src / "av1" / "decoder" / "inspection.h").write_text("#pragma once\nint ifd_stub(void);\n")
    archive = tmp / "upstream" / "libaom-3.12.1.tar.gz"
    archive.parent.mkdir()
    with tarfile.open(archive, "w:gz") as t:
        t.add(src, arcname="libaom-3.12.1")
    return archive, hashlib.sha256(archive.read_bytes()).hexdigest()


def write_patches(tmp: Path, *patches: tuple[str, str]) -> Path:
    d = tmp / "patches"
    d.mkdir(exist_ok=True)
    for name, body in patches:
        (d / name).write_text(body)
    return d


class Build:
    """A project that includes the real third_party/libaom.cmake against the fixture tarball."""

    def __init__(self, tmp: Path, patch_dir: Path) -> None:
        archive, sha = fixture_archive(tmp)
        versions = tmp / "VERSIONS.md"
        versions.write_text(
            "# pins\n\n" + HEADER + f"| libaom | 3.12.1 | BSD-2-Clause AND LicenseRef-AOM-Patent-License-1.0 | "
            f"{archive.as_uri()} | – | `{sha}` | third_party/libaom.cmake | |\n"
        )
        project = tmp / "project"
        project.mkdir()
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\nproject(aom_fixture C CXX)\n"
            f'list(APPEND CMAKE_MODULE_PATH "{(REPO / "cmake").as_posix()}")\n'
            "include(OvtLicenseTags)\n"
            f'include("{(REPO / "third_party" / "libaom.cmake").as_posix()}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/libaom_dirs.txt" "${LIBAOM_SOURCE_DIR}\\n${LIBAOM_BINARY_DIR}\\n")\n'
            "get_target_property(_inc tp::aom_inspection INTERFACE_INCLUDE_DIRECTORIES)\n"
            'file(APPEND "${CMAKE_BINARY_DIR}/libaom_dirs.txt" "${_inc}\\n")\n'
        )
        self.tmp = tmp
        self.prefix = tmp / "prefix" / "libaom"
        self.source = self.prefix / "src" / "tp_libaom"
        self.configure = [
            str(CMAKE),
            "-S",
            str(project),
            "-B",
            str(tmp / "build"),
            f"-DOVT_TP_VERSIONS_MD={versions}",
            f"-DOVT_TP_DOWNLOADS={tmp / 'dl'}",
            f"-DOVT_THIRD_PARTY_PREFIX={tmp / 'prefix'}",
            f"-DOVT_AOM_PATCH_DIR={patch_dir}",
            f"-DPython3_EXECUTABLE={sys.executable}",
        ]

    def run(self) -> subprocess.CompletedProcess[str]:
        configured = subprocess.run(self.configure, capture_output=True, text=True, check=False)
        assert configured.returncode == 0, configured.stdout + configured.stderr
        return subprocess.run(
            [str(CMAKE), "--build", str(self.tmp / "build"), "--target", "tp_libaom"],
            capture_output=True,
            text=True,
            check=False,
        )

    def recorded(self) -> list[str]:
        data = json.loads((self.prefix / "share" / "ovt" / "patches.json").read_text())
        return [p["name"] for p in data["patches"]]

    def patched_source(self) -> str:
        return (self.source / "av1" / "decoder" / "inspection.c").read_text()


@needs_build
def test_patches_apply_in_order_and_are_recorded(tmp_path: Path) -> None:
    build = Build(tmp_path, write_patches(tmp_path, GOOD))
    result = build.run()
    assert result.returncode == 0, result.stdout + result.stderr
    assert "apply_patches: applied 0001-inspection-extend-mi-data.patch" in result.stdout
    assert (build.prefix / "lib" / "libaom.a").is_file()
    assert build.recorded() == ["0001-inspection-extend-mi-data.patch"]
    assert build.patched_source().count("patched") == 1


@needs_build
def test_broken_patch_fails_the_build_naming_it(tmp_path: Path) -> None:
    """Acceptance test 2 of WI-2.154: a deliberately broken patch fails the build with its name in the error."""
    build = Build(tmp_path, write_patches(tmp_path, GOOD, BAD))
    result = build.run()
    assert result.returncode != 0
    assert "0002-inspection-frame-snapshots.patch does not apply" in result.stdout + result.stderr
    # --check dry-runs the whole series first: nothing is applied, so the tree stays pristine.
    assert "patched" not in build.patched_source()
    assert not (build.source / apply_patches.MARKER).exists()
    assert not (build.prefix / "lib" / "libaom.a").exists()


@needs_build
def test_missing_patch_dir_builds_upstream_and_exports_source_dir(
    tmp_path: Path,
) -> None:
    build = Build(tmp_path, tmp_path / "no-such-dir")
    result = build.run()
    assert result.returncode == 0, result.stdout + result.stderr
    assert build.patched_source() == INSPECTION_C
    source_dir, binary_dir, includes = (tmp_path / "build" / "libaom_dirs.txt").read_text().splitlines()
    assert Path(source_dir) == build.source
    assert (Path(source_dir) / "av1" / "decoder" / "inspection.h").is_file()
    assert Path(binary_dir) == build.prefix / "src" / "tp_libaom-build"
    assert includes.split(";") == [source_dir, binary_dir]


def test_default_patch_dir_is_the_plugins_patches() -> None:
    text = (REPO / "third_party" / "libaom.cmake").read_text()
    assert 'set(OVT_AOM_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/../plugins/ovt-plugin-aom/patches"' in text
