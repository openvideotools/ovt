# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the link walk of cmake/OvtLicenseTags.cmake (chapter 13 §13.2.3), on a small CMake project.

The Mac Mini's macOS bundle run found NOASSERTION links in license_manifest.json. This covers the ones fixed outside
the walk of WI-0.032 (#202, which handles CMake's `::@` markers and ovt_schemas): the SDK path of a system framework
(Qt's WrapOpenGL) is not a dependency, and a library linked by path keeps the licence its script describes.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

CMAKE_DIR = Path(__file__).resolve().parents[2] / "cmake"

TOP = """\
cmake_minimum_required(VERSION 3.25)
project(t LANGUAGES C)
list(APPEND CMAKE_MODULE_PATH "{cmake_dir}")
include(OvtLicenseTags)
file(WRITE "${{CMAKE_BINARY_DIR}}/a.c" "int a(void) {{ return 0; }}\\n")
add_subdirectory(core)
"""

CORE = """\
add_library(ext_x INTERFACE IMPORTED)
ovt_set_dependency_license(ext_x SOURCE vcpkg SPDX MIT)
ovt_set_dependency_license("/v/lib/liblcms2.a" SOURCE vcpkg SPDX MIT LINKAGE static)
target_link_libraries(ext_x INTERFACE "/v/lib/liblcms2.a" "/v/lib/libundescribed.a")
add_library(core STATIC "${CMAKE_BINARY_DIR}/a.c")
target_link_libraries(core PRIVATE ext_x "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/\
Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/OpenGL.framework" "/System/Library/Frameworks/Metal.framework")
ovt_set_license(core "LGPL-2.1-or-later")
"""


def cmake_version() -> tuple[int, int] | None:
    exe = shutil.which("cmake")
    if not exe:
        return None
    m = re.search(
        r"(\d+)\.(\d+)", subprocess.run([exe, "--version"], capture_output=True, text=True, check=False).stdout
    )
    return (int(m.group(1)), int(m.group(2))) if m else None


@pytest.mark.skipif((cmake_version() or (0, 0)) < (3, 25), reason="needs CMake >= 3.25")
@pytest.mark.skipif(
    not (shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")), reason="needs a C compiler"
)
def test_manifest_lists_only_libraries(tmp_path: Path) -> None:
    src = tmp_path / "src"
    (src / "core").mkdir(parents=True)
    (src / "CMakeLists.txt").write_text(TOP.format(cmake_dir=CMAKE_DIR.as_posix()))
    (src / "core" / "CMakeLists.txt").write_text(CORE)
    r = subprocess.run(
        ["cmake", "-S", str(src), "-B", str(tmp_path / "b")], capture_output=True, text=True, check=False
    )
    assert r.returncode == 0, r.stderr
    manifest = json.loads((tmp_path / "b" / "CMakeFiles" / "ovt_license_manifest.staged.json").read_text())
    (core,) = manifest["targets"]
    links = {lk["name"]: (lk["spdx"], lk["linkage"]) for lk in core["links"]}
    # the frameworks are gone; a path nobody described stays NOASSERTION, so the gate still reports it
    assert links == {
        "ext_x": ("MIT", "header-only"),
        "/v/lib/liblcms2.a": ("MIT", "static"),
        "/v/lib/libundescribed.a": ("NOASSERTION", "shared"),
    }
