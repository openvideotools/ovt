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


def entry(name: str, version: str, lic: str) -> dict:
    return {
        "component": name,
        "version": version,
        "license": lic,
        "chosen": lic,
        "source": "versions",
        "targets": [],
    }


@pytest.fixture
def tree(tmp_path: Path) -> dict:
    report = {
        "inventory": [
            entry("vcpkg baseline", "abc", "MIT"),
            entry("libde265", "1.0.15", "LGPL-3.0-or-later"),
            entry("libaom", "3.12.1", "BSD-2-Clause"),
            entry("ONNX Runtime", "1.20.1", "MIT"),
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
        ("onnx-runtime", "MIT text\n"),
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
    (tree["lic"] / "onnx-runtime" / "LICENSE").unlink()
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
    ]  # sorted; build-only and "in libvmaf" rows dropped
    assert "**yes** — patches: 0001-api.patch (Upstream-Status: Submitted https://example.org/pr/1)" in text
    assert "**PATENTS**" in text and "libaom BSD text\n" in text
    assert "None." in text
    assert (tmp_path / "PATENT_NOTICE.md").read_text().count("H.264/AVC") == 1


def test_no_patent_notice_by_default(tree: dict, tmp_path: Path) -> None:
    out = tmp_path / "N.md"
    assert run(tree, out)[0] == 0
    assert gn.PATENT_NOTICE not in out.read_text()
    assert not (tmp_path / "PATENT_NOTICE.md").exists()


def test_vcpkg_and_qt_fallbacks(tree: dict, tmp_path: Path) -> None:
    (tree["lic"] / "onnx-runtime" / "LICENSE").unlink()
    share = tmp_path / "share" / "onnx-runtime"
    share.mkdir(parents=True)
    (share / "copyright").write_text("vcpkg MIT\n")
    report = json.loads((tree["root"] / "report.json").read_text())
    report["inventory"].append(entry("Qt", "6.8.3", "LGPL-3.0-only"))
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
