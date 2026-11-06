# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the libde265 patch step (WI-0.014, chapter 13 §13.3.6): third_party/libde265.cmake running
tools/apply_patches.py --series through ExternalProject, and the repository's own series file.

The build tests use a stand-in libde265-1.0.15 tarball (a one-file static library) served from a file:// URL, so
they need cmake and a C++ compiler but no network. The unit tests of apply_patches.py itself are in
test_third_party_tools.py (WI-0.010)."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import apply_patches  # noqa: E402
import check_patches  # noqa: E402

SERIES = REPO / "plugins" / "ovt-plugin-de265" / "patches" / "series"
CMAKE = shutil.which("cmake")
CXX = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
needs_build = pytest.mark.skipif(CMAKE is None or CXX is None, reason="cmake or a C++ compiler is not installed")

HEADER = "| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |\n|---|---|---|---|---|---|---|---|\n"

UPSTREAM_CC = """\
#include "de265.h"

// line one
const char* de265_get_version(void) { return "1.0.15"; }
"""

UPSTREAM_CMAKE = """\
cmake_minimum_required(VERSION 3.5)
project(libde265 CXX)
add_library(de265 STATIC libde265/de265.cc)
target_include_directories(de265 PRIVATE libde265)
include(GNUInstallDirs)
install(TARGETS de265 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES libde265/de265.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libde265)
"""


def patch(name: str, old: str, new: str, status: str = "Inappropriate [test fixture]") -> tuple[str, str]:
    body = (
        f"From: OVT\nSubject: [PATCH] {name}\nUpstream-Status: {status}\n\n"
        "--- a/libde265/de265.cc\n+++ b/libde265/de265.cc\n"
        f'@@ -1,4 +1,4 @@\n #include "de265.h"\n \n-{old}\n+{new}\n'
        ' const char* de265_get_version(void) { return "1.0.15"; }\n'
    )
    return name, body


GOOD = patch("0001-comment.patch", "// line one", "// line one, patched")
BAD = patch("0002-broken.patch", "// a line that is not there", "// never")


def fixture_archive(tmp: Path) -> tuple[Path, str]:
    src = tmp / "upstream-src" / "libde265-1.0.15"
    (src / "libde265").mkdir(parents=True)
    (src / "CMakeLists.txt").write_text(UPSTREAM_CMAKE)
    (src / "COPYING").write_text("LGPL-3.0-or-later\n")
    (src / "libde265" / "de265.h").write_text('#pragma once\nextern "C" const char* de265_get_version(void);\n')
    (src / "libde265" / "de265.cc").write_text(UPSTREAM_CC)
    archive = tmp / "upstream" / "libde265-1.0.15.tar.gz"
    archive.parent.mkdir()
    with tarfile.open(archive, "w:gz") as t:
        t.add(src, arcname="libde265-1.0.15")
    return archive, hashlib.sha256(archive.read_bytes()).hexdigest()


def write_series(tmp: Path, *patches: tuple[str, str]) -> Path:
    d = tmp / "patches"
    d.mkdir(exist_ok=True)
    for old in d.iterdir():
        old.unlink()
    for name, body in patches:
        (d / name).write_text(body)
    series = d / "series"
    series.write_text("# fixture series\n" + "".join(f"{name}\n" for name, _ in patches))
    return series


class Build:
    """A project that includes the real third_party/libde265.cmake against the fixture tarball."""

    def __init__(self, tmp: Path, series: Path) -> None:
        archive, sha = fixture_archive(tmp)
        versions = tmp / "VERSIONS.md"
        versions.write_text(
            "# pins\n\n" + HEADER + f"| libde265 | 1.0.15 | LGPL-3.0-or-later | {archive.as_uri()} | "
            f"plugins/ovt-plugin-de265/patches/0001-de265-internals-api.patch | `{sha}` | third_party/libde265.cmake | |\n"
        )
        project = tmp / "project"
        project.mkdir()
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\nproject(de265_fixture C CXX)\n"
            f'list(APPEND CMAKE_MODULE_PATH "{(REPO / "cmake").as_posix()}")\n'
            "include(OvtLicenseTags)\n"
            f'include("{(REPO / "third_party" / "libde265.cmake").as_posix()}")\n'
        )
        self.tmp = tmp
        self.prefix = tmp / "prefix" / "libde265"
        self.source = self.prefix / "src" / "tp_libde265"
        self.configure = [
            str(CMAKE),
            "-S",
            str(project),
            "-B",
            str(tmp / "build"),
            f"-DOVT_TP_VERSIONS_MD={versions}",
            f"-DOVT_TP_DOWNLOADS={tmp / 'dl'}",
            f"-DOVT_THIRD_PARTY_PREFIX={tmp / 'prefix'}",
            f"-DOVT_DE265_PATCH_SERIES={series}",
            f"-DPython3_EXECUTABLE={sys.executable}",
        ]

    def run(self) -> subprocess.CompletedProcess[str]:
        configured = subprocess.run(self.configure, capture_output=True, text=True, check=False)
        assert configured.returncode == 0, configured.stdout + configured.stderr
        return subprocess.run(
            [str(CMAKE), "--build", str(self.tmp / "build"), "--target", "tp_libde265"],
            capture_output=True,
            text=True,
            check=False,
        )

    def recorded(self) -> list[str]:
        data = json.loads((self.prefix / "share" / "ovt" / "patches.json").read_text())
        return [p["name"] for p in data["patches"]]

    def patched_source(self) -> str:
        return (self.source / "libde265" / "de265.cc").read_text()


@needs_build
def test_clean_build_twice_applies_once_and_records(tmp_path: Path) -> None:
    """Acceptance test 1: patches apply once, patches.json lists them, the rebuild does not re-apply."""
    build = Build(tmp_path, write_series(tmp_path, GOOD))
    first = build.run()
    assert first.returncode == 0, first.stdout + first.stderr
    assert "apply_patches: applied 0001-comment.patch" in first.stdout
    assert (build.prefix / "lib" / "libde265.a").is_file()
    assert (build.source / apply_patches.MARKER).is_file()
    assert build.recorded() == ["0001-comment.patch"]
    assert build.patched_source().count("patched") == 1

    second = build.run()
    assert second.returncode == 0, second.stdout + second.stderr
    assert "apply_patches" not in second.stdout
    assert build.patched_source().count("patched") == 1
    assert build.recorded() == ["0001-comment.patch"]


@needs_build
def test_patch_failing_check_fails_the_build_naming_it(tmp_path: Path) -> None:
    """Acceptance test 2: a patch that fails --check fails the build, naming it, and nothing is applied."""
    build = Build(tmp_path, write_series(tmp_path, GOOD, BAD))
    result = build.run()
    assert result.returncode != 0
    assert "0002-broken.patch does not apply" in result.stdout + result.stderr
    assert "patched" not in build.patched_source()
    assert not (build.source / apply_patches.MARKER).exists()


@needs_build
def test_changed_series_re_extracts_and_applies_the_new_one(tmp_path: Path) -> None:
    series = write_series(tmp_path, GOOD)
    build = Build(tmp_path, series)
    assert build.run().returncode == 0
    renamed = patch("0001-other.patch", "// line one", "// line one, patched again")
    changed = write_series(tmp_path, renamed)
    # ExternalProject reruns a step when its inputs are newer than its stamp, and the build tree compares mtimes
    # at the filesystem's granularity. The first build, the rewrite and the second build all fall within it, so
    # make the order certain: the new series is in the future, the first build's stamps are in the past.
    now = time.time()
    for f in changed.parent.iterdir():
        os.utime(f, (now + 60, now + 60))
    for stamp in (tmp_path / "prefix").rglob("*-stamp/*"):
        os.utime(stamp, (now - 60, now - 60))
    result = build.run()
    assert result.returncode == 0, result.stdout + result.stderr
    assert "apply_patches: applied 0001-other.patch" in result.stdout
    assert build.patched_source().count("patched") == 1 and "patched again" in build.patched_source()
    assert build.recorded() == ["0001-other.patch"]


def test_repository_series_lists_existing_patches_with_valid_headers() -> None:
    patches = apply_patches.collect(None, SERIES)
    assert patches[0].name == "0001-de265-internals-api.patch"
    for p in patches:
        assert apply_patches.upstream_status(p)
        errors, _ = check_patches.check_patch(p, p.name, 180, check_patches.dt.date.today())
        assert errors == []


def test_versions_md_row_names_the_series_patches() -> None:
    row = next(
        line
        for line in (REPO / "third_party" / "VERSIONS.md").read_text().splitlines()
        if line.startswith("| libde265 ")
    )
    named = {Path(p.strip()).name for p in row.split("|")[5].split(",")}
    assert {p.name for p in apply_patches.collect(None, SERIES)} <= named
