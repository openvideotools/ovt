# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/gen_notices.py (chapter 15 §15.6; WI-0.603 acceptance tests 1 and 2, bundle content of test 3)."""

from __future__ import annotations

import contextlib
import io
import json
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import gen_notices as gn  # noqa: E402

VERSIONS = """\
| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |
|---|---|---|---|---|---|---|---|
| vcpkg baseline | `abc` | MIT (tool) | <https://github.com/microsoft/vcpkg> | – | – | manifest | |
| libaom | 3.12.1 | BSD-2-Clause | <https://example.org/libaom.tar.gz> | – | `x` | third_party/libaom.cmake | |
| libde265 | 1.0.15 | LGPL-3.0-or-later | <https://example.org/libde265.tar.gz> | plugins/p/patches/0001-api.patch | `x` | third_party/libde265.cmake | |
| ONNX Runtime | 1.20.1 | MIT | vcpkg port / <https://example.org/onnx> | – | – | vcpkg | |
| VMAF models | in libvmaf | BSD-2-Clause-Patent | – | – | – | built-in | |
"""


def entry(name: str, version: str, lic: str, targets: list[str] | None = None) -> dict:
    src = "versions" if targets is None else "third_party"
    return {
        "component": name,
        "version": version,
        "license": lic,
        "chosen": lic,
        "source": src,
        "targets": targets or [],
    }


@pytest.fixture
def tree(tmp_path: Path) -> dict:
    report = {
        "inventory": [
            entry("vcpkg baseline", "abc", "MIT"),
            entry("libde265", "1.0.15", "LGPL-3.0-or-later", ["ovt_plugin_de265"]),
            entry("libaom", "3.12.1", "BSD-2-Clause", ["ovt_plugin_aom"]),
            entry("ONNX Runtime", "1.20.1", "MIT", ["ovt_core"]),
            entry("HM", "HM-18.0", "BSD-3-Clause"),  # in VERSIONS.md but linked by no target
            entry("VMAF models", "in libvmaf", "BSD-2-Clause-Patent"),
        ]
    }
    (tmp_path / "report.json").write_text(json.dumps(report))
    (tmp_path / "VERSIONS.md").write_text(VERSIONS)
    (tmp_path / "VERSION").write_text("0.1.0\n")
    lic = tmp_path / "licenses"
    for dep, text in (
        ("libaom", "libaom BSD text\r\n"),
        ("libde265", "LGPL text\n\n\n"),
        ("onnxruntime", "MIT text\n"),
    ):
        (lic / dep).mkdir(parents=True)
        (lic / dep / "LICENSE").write_text(text, newline="")
    (lic / "libaom" / "PATENTS").write_text("AOM patents\n")
    patch = tmp_path / "plugins" / "p" / "patches"
    patch.mkdir(parents=True)
    (patch / "0001-api.patch").write_text("From: x\nUpstream-Status: Submitted https://example.org/pr/1\n---\n")
    return {"root": tmp_path, "lic": lic}


def run(tree: dict, out: Path, *extra: str) -> tuple[int, str]:
    err = io.StringIO()
    root = tree["root"]
    with contextlib.redirect_stderr(err):
        code = gn.main(
            [
                "--report", str(root / "report.json"),
                "--versions", str(root / "VERSIONS.md"),
                "--licenses-dir", str(tree["lic"]),
                "--artifact", "macos-app",
                "--out", str(out),
                "--root", str(root),
                *extra,
            ]
        )  # fmt: skip
    return code, err.getvalue()


def test_two_runs_are_byte_identical(tree: dict, tmp_path: Path) -> None:
    a, b = tmp_path / "a" / "N.md", tmp_path / "b" / "N.md"
    assert run(tree, a)[0] == 0
    assert run(tree, b)[0] == 0
    assert a.read_bytes() == b.read_bytes()
    assert b"\r" not in a.read_bytes()


def test_missing_licence_text_fails_with_code(tree: dict, tmp_path: Path) -> None:
    (tree["lic"] / "onnxruntime" / "LICENSE").unlink()
    out = tmp_path / "N.md"
    code, err = run(tree, out)
    assert code == 1
    assert "E-NOTICE-MISSING ONNX Runtime" in err
    assert not out.exists()


def test_content(tree: dict, tmp_path: Path) -> None:
    out = tmp_path / "N.md"
    assert run(tree, out, "--patent-notice")[0] == 0
    text = out.read_text()
    assert text.startswith("# Third-party notices for Open Video Tools 0.1.0 (macos-app)\n")
    assert gn.AV1_SENTENCE in text and gn.FFMPEG_SENTENCE in text and gn.PATENT_NOTICE in text
    rows = [ln.split(" | ")[0] for ln in text.splitlines() if ln.startswith("| ") and not ln.startswith("| Component")]
    assert rows == [
        "| libaom",
        "| libde265",
        "| ONNX Runtime",
    ]  # sorted; rows no target links are dropped
    assert "**yes** — patches: 0001-api.patch (Upstream-Status: Submitted https://example.org/pr/1)" in text
    assert "**PATENTS**" in text and "libaom BSD text\n" in text
    assert "None." in text
    assert (tmp_path / "PATENT_NOTICE.md").read_text().count("H.264/AVC") == 1


def test_only_linked_components_need_texts(tree: dict, tmp_path: Path) -> None:
    """HM has no licence text and no target links it: the run still succeeds and HM is not listed."""
    out = tmp_path / "N.md"
    code, err = run(tree, out)
    assert code == 0, err
    assert "HM" not in [ln.split(" | ")[0].lstrip("| ") for ln in out.read_text().splitlines()]


def test_staging_key_matches_dep_id() -> None:
    import versions_to_cmake as v2c

    for name in ("Qt Gradle Plugin", "libaom", "Gradle / AGP"):
        assert gn.key_of(name) == v2c.dep_id(name)


def test_no_patent_notice_by_default(tree: dict, tmp_path: Path) -> None:
    out = tmp_path / "N.md"
    assert run(tree, out)[0] == 0
    assert gn.PATENT_NOTICE not in out.read_text()
    assert not (tmp_path / "PATENT_NOTICE.md").exists()


def test_vcpkg_and_qt_fallbacks(tree: dict, tmp_path: Path) -> None:
    (tree["lic"] / "onnxruntime" / "LICENSE").unlink()
    share = tmp_path / "share" / "onnxruntime"
    share.mkdir(parents=True)
    (share / "copyright").write_text("vcpkg MIT\n")
    report = json.loads((tree["root"] / "report.json").read_text())
    report["inventory"].append(entry("Qt", "6.8.3", "LGPL-3.0-only", ["ovt_studio"]))
    (tree["root"] / "report.json").write_text(json.dumps(report))
    qt = tmp_path / "qt"
    qt.mkdir()
    (qt / "LICENSE.LGPL3").write_text("Qt LGPL\n")
    (qt / "README").write_text("not a licence\n")
    out = tmp_path / "N.md"
    code, err = run(tree, out, "--vcpkg-share", str(tmp_path / "share"), "--qt-licenses", str(qt))
    assert code == 0, err
    text = out.read_text()
    assert "vcpkg MIT" in text and "Qt LGPL" in text and "not a licence" not in text


def test_derived_files_table(tree: dict, tmp_path: Path) -> None:
    derived = tmp_path / "derived.toml"
    derived.write_text(
        '[[files]]\npath = "app/b.cpp"\nupstream = "YUView"\ncommit = "abc"\nlicense = "GPL-3.0-only"\n'
        '[[files]]\npath = "app/a.cpp"\nupstream = "YUView"\ncommit = "abc"\nlicense = "GPL-3.0-only"\n'
    )
    out = tmp_path / "N.md"
    assert run(tree, out, "--derived", str(derived))[0] == 0
    text = out.read_text()
    assert text.index("app/a.cpp") < text.index("app/b.cpp")


def test_bad_derived_file_is_usage_error(tree: dict, tmp_path: Path) -> None:
    derived = tmp_path / "derived.toml"
    derived.write_text('[[files]]\npath = "x"\n')
    assert run(tree, tmp_path / "N.md", "--derived", str(derived))[0] == 2


def test_repository_derived_file_parses() -> None:
    assert gn.read_derived(TOOLS / "derived_files.toml") == []


def test_fence_longer_than_text_backticks() -> None:
    assert gn.fence_for("a ``` b") == "````"


@pytest.mark.parametrize(
    ("name", "key"),
    [
        ("fmt::fmt", "fmt"),
        ("PkgConfig::_ovt_pc_xxHash", "xxhash"),
        ("xxHash::xxhash", "xxhash"),
        ("/usr/lib/x86_64-linux-gnu/libxxhash.so", "xxhash"),
        ("zstd::libzstd_static", "zstd"),
        ("hwy::hwy", "highway"),
        ("CLI11::CLI11", "cli11"),
        # the prebuilt ONNX Runtime: four VERSIONS.md rows, one component (third_party/onnxruntime.cmake)
        ("tp::onnxruntime", "onnxruntime"),
        ("ONNX Runtime", "onnxruntime"),
        ("ONNX Runtime osx-arm64", "onnxruntime"),
        ("ONNX Runtime linux-aarch64", "onnxruntime"),
        ("ONNX Runtime osx-x86_64", "onnxruntime"),
        # names of the macOS bundle's manifest (Mac Mini run of WI-0.063) -> the VERSIONS.md row or vcpkg port
        ("tp::aom", "libaom"),
        ("tp::aom_inspection", "libaom"),
        ("libaom", "libaom"),
        ("tp::de265", "libde265"),
        ("tp::placebo", "libplacebo"),
        ("tp::vmaf", "libvmaf"),
        ("tp::hm_common", "hm"),
        ("tp::ffmpeg_core", "ffmpeg"),
        ("tp::ffmpeg_plugin_avcodec", "ffmpeg"),
        ("FFmpeg", "ffmpeg"),
        ("Qt6::Core", "qt"),
        ("Qt::Gui", "qt"),
        ("WrapOpenGL::WrapOpenGL", "qt"),
        ("Qt", "qt"),
        ("/v/arm64-osx-ovt/lib/libGenericCodeGen.a", "glslang"),
        ("/v/arm64-osx-ovt/lib/libglslang-default-resource-limits.a", "glslang"),
        ("/v/arm64-osx-ovt/lib/libSPIRV.a", "glslang"),
        ("/v/arm64-osx-ovt/lib/libSPIRV-Tools-opt.a", "spirv_tools"),
        ("/v/arm64-osx-ovt/lib/liblcms2.a", "lcms"),
        ("z", "zlib"),
        ("pugixml::static", "pugixml"),
        ("nlohmann_json_schema_validator::validator", "json_schema_validator"),
        ("Catch2::Catch2WithMain", "catch2"),
    ],
)
def test_manifest_names_map_to_one_key(name: str, key: str) -> None:
    assert gn.key_of(name) == key


def test_hyphenated_vcpkg_port_is_found(tree: dict, tmp_path: Path) -> None:
    report = json.loads((tree["root"] / "report.json").read_text())
    report["inventory"].append(entry("nlohmann_json::nlohmann_json", "3.11.3", "MIT", ["ovt_core"]))
    (tree["root"] / "report.json").write_text(json.dumps(report))
    share = tmp_path / "share" / "nlohmann-json"
    share.mkdir(parents=True)
    (share / "copyright").write_text("json MIT\n")
    out = tmp_path / "N.md"
    code, err = run(tree, out, "--vcpkg-share", str(tmp_path / "share"))
    assert code == 0, err
    assert "json MIT" in out.read_text()


def test_system_zlib_and_qt_texts(tree: dict, tmp_path: Path) -> None:
    """`z` (system zlib, FFmpeg's link) takes its text from third_party/system-licenses/ (cmake/OvtNotices.cmake)."""
    report = json.loads((tree["root"] / "report.json").read_text())
    report["inventory"].append(entry("z", "", "Zlib", ["ovt_core"]))
    report["inventory"].append(entry("zstd::libzstd_static", "1.5.6", "BSD-3-Clause OR GPL-2.0-only", ["ovt_core"]))
    report["inventory"].append(entry("Qt6::Core", "6.8.3", "LGPL-3.0-only", ["ovt_studio"]))
    report["inventory"].append(entry("Qt6::Gui", "6.8.3", "LGPL-3.0-only", ["ovt_studio"]))
    (tree["root"] / "report.json").write_text(json.dumps(report))
    share = tmp_path / "share" / "zstd"
    share.mkdir(parents=True)
    (share / "copyright").write_text("zstd BSD\n")
    qt = tmp_path / "qt"
    qt.mkdir()
    (qt / "LGPL-3.0.txt").write_text("GNU LESSER GENERAL PUBLIC LICENSE\n")
    system = TOOLS.parent / "third_party" / "system-licenses"
    out = tmp_path / "N.md"
    code, err = run(
        tree, out, "--licenses-dir", str(system), "--vcpkg-share", str(tmp_path / "share"), "--qt-licenses", str(qt)
    )
    assert code == 0, err
    text = out.read_text()
    assert "Jean-loup Gailly and Mark Adler" in text and "zstd BSD" in text
    assert text.count("### Qt6::") == 1  # one component, one copy of the text
