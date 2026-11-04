# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the third-party tooling of WI-0.010: tools/versions_to_cmake.py, tools/apply_patches.py,
tools/third_party_offline.py and the configure-time checks of cmake/OvtThirdPartyCommon.cmake."""

from __future__ import annotations

import hashlib
import json
import re
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
import third_party_offline  # noqa: E402
import versions_to_cmake as vtc  # noqa: E402

HEADER = "| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |\n|---|---|---|---|---|---|---|---|\n"


def table(*rows: str) -> str:
    return "# pins\n\n" + HEADER + "".join(r + "\n" for r in rows)


def make_archive(tmp: Path, name: str = "libfixture-1.2.3.tar.gz") -> tuple[Path, str]:
    src = tmp / "src" / "libfixture-1.2.3"
    src.mkdir(parents=True)
    (src / "PATENTS").write_text("patent grant\n")
    (src / "README").write_text("fixture\n")
    archive = tmp / "upstream" / name
    archive.parent.mkdir()
    with tarfile.open(archive, "w:gz") as t:
        t.add(src, arcname="libfixture-1.2.3")
    return archive, hashlib.sha256(archive.read_bytes()).hexdigest()


# ---- versions_to_cmake.py -------------------------------------------------------------------------------


def test_repository_table_emits_the_tp_spelling_only(tmp_path: Path) -> None:
    out = tmp_path / "versions.cmake"
    assert vtc.main(["--out", str(out)]) == 0
    text = out.read_text()
    for var in ("OVT_TP_libaom_VERSION", "OVT_TP_libaom_URL", "OVT_TP_libaom_SHA256", "OVT_TP_ffmpeg_URL"):
        assert f"set({var} " in text
    assert 'set(OVT_TP_libaom_VERSION "3.12.1")' in text
    assert not re.search(r"\bOVT_(?!TP_)[A-Z0-9]+_(VERSION|URL|SHA256)\b", text)


def test_dep_ids() -> None:
    assert vtc.dep_id("libaom") == "libaom"
    assert vtc.dep_id("FFmpeg") == "ffmpeg"
    assert vtc.dep_id("vcpkg baseline") == "vcpkg_baseline"
    assert vtc.dep_id("Gradle / AGP") == "gradle_agp"


def test_url_with_another_version_is_rejected() -> None:
    text = table("| libfoo | 1.2.4 | MIT | https://example.org/libfoo-1.2.3.tar.gz | – | `<sha>` | x | |")
    with pytest.raises(vtc.VersionsError, match=r"row 'libfoo'.*1\.2\.3.*1\.2\.4"):
        vtc.parse(text)


def test_url_with_a_version_prefix_is_accepted() -> None:
    rows = vtc.parse(table("| Qt | 6.8.3 | LGPL-3.0-only | https://download.qt.io/qt/6.8/ | – | – | x | |"))
    assert rows[0].url == "https://download.qt.io/qt/6.8/"


def test_angle_bracket_autolink_url_is_unwrapped() -> None:
    # markdownlint MD034 wants bare URLs written as <url>; the brackets are not part of the URL.
    rows = vtc.parse(table("| Qt | 6.8.3 | LGPL-3.0-only | <https://download.qt.io/qt/6.8/> | – | – | x | |"))
    assert rows[0].url == "https://download.qt.io/qt/6.8/"


def test_duplicate_ids_and_bad_hashes_are_rejected() -> None:
    with pytest.raises(vtc.VersionsError, match="already used"):
        vtc.parse(table("| Foo | 1 | MIT | – | – | – | x | |", "| foo | 1 | MIT | – | – | – | x | |"))
    with pytest.raises(vtc.VersionsError, match="not 64 lower-case hex"):
        vtc.parse(table("| Foo | 1 | MIT | – | – | `abcdef0123` | x | |"))


def test_placeholders_become_empty_hashes() -> None:
    row = vtc.parse(table("| Foo | 1.0 | MIT | https://e.org/foo-1.0.tar.gz | – | `<sha>` | x | |"))[0]
    assert row.sha256 == "" and row.sha_placeholder and row.is_archive


def test_update_hashes_fills_sha_and_patents(tmp_path: Path) -> None:
    archive, sha = make_archive(tmp_path)
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(
        table(f"| libaom | 1.2.3 | BSD-2-Clause | {archive.as_uri()} | – | `<sha>` | x | `PATENTS sha256=<sha>` |")
    )
    assert vtc.main(["--versions", str(versions), "--update-hashes", "--downloads", str(tmp_path / "dl")]) == 0
    text = versions.read_text()
    patents = hashlib.sha256(b"patent grant\n").hexdigest()
    assert f"`{sha}`" in text and f"PATENTS sha256={patents}" in text and "<sha>" not in text


def test_check_flags_a_script_that_spells_its_version(tmp_path: Path) -> None:
    (tmp_path / "third_party").mkdir()
    (tmp_path / "third_party" / "foo.cmake").write_text("set(URL https://e.org/foo-1.2.3.tar.gz)\n")
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(
        table("| foo | 1.2.3 | MIT | https://e.org/foo-1.2.3.tar.gz | – | `<sha>` | third_party/foo.cmake | |")
    )
    assert vtc.main(["--versions", str(versions), "--check", "--source-dir", str(tmp_path)]) == 1
    (tmp_path / "third_party" / "foo.cmake").write_text("URL ${OVT_TP_foo_URL} SHA ${OVT_TP_foo_SHA256}\n")
    assert vtc.main(["--versions", str(versions), "--check", "--source-dir", str(tmp_path)]) == 0


def test_template_passes_the_script_check() -> None:
    rows = vtc.parse(table("| mydep | 1.0 | MIT | – | – | – | third_party/_template.cmake | |"))
    assert vtc.check_scripts(rows, REPO) == []


# ---- configure-time download check (acceptance test 2) --------------------------------------------------

CMAKE = shutil.which("cmake")


def configure_fixture(tmp_path: Path, versions: Path) -> subprocess.CompletedProcess[str]:
    project = tmp_path / "project"
    project.mkdir(exist_ok=True)
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.25)\nproject(tp_fixture NONE)\n"
        f'list(APPEND CMAKE_MODULE_PATH "{(REPO / "cmake").as_posix()}")\n'
        "include(OvtThirdPartyCommon)\novt_tp_declare(libfixture)\n"
        'message(STATUS "archive: ${OVT_TP_libfixture_ARCHIVE}")\n'
    )
    return subprocess.run(
        [
            str(CMAKE),
            "-S",
            str(project),
            "-B",
            str(tmp_path / "build"),
            f"-DOVT_TP_VERSIONS_MD={versions}",
            f"-DOVT_TP_DOWNLOADS={tmp_path / 'dl'}",
            f"-DPython3_EXECUTABLE={sys.executable}",
        ],
        capture_output=True,
        text=True,
        check=False,
    )


@pytest.mark.skipif(CMAKE is None, reason="cmake not installed")
def test_configure_fails_naming_the_row_on_sha_mismatch(tmp_path: Path) -> None:
    archive, _ = make_archive(tmp_path)
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(table(f"| libfixture | 1.2.3 | MIT | {archive.as_uri()} | – | `{'0' * 64}` | x | |"))
    result = configure_fixture(tmp_path, versions)
    assert result.returncode != 0
    assert "SHA-256 mismatch" in result.stderr and "row 'libfixture'" in result.stderr


@pytest.mark.skipif(CMAKE is None, reason="cmake not installed")
def test_configure_downloads_and_verifies(tmp_path: Path) -> None:
    archive, sha = make_archive(tmp_path)
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(table(f"| libfixture | 1.2.3 | MIT | {archive.as_uri()} | – | `{sha}` | x | |"))
    result = configure_fixture(tmp_path, versions)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "dl" / archive.name).read_bytes() == archive.read_bytes()
    generated = (tmp_path / "build" / "third_party" / "versions.cmake").read_text()
    assert f'set(OVT_TP_libfixture_SHA256 "{sha}")' in generated


@pytest.mark.skipif(CMAKE is None, reason="cmake not installed")
def test_configure_fails_on_a_row_without_hash(tmp_path: Path) -> None:
    archive, _ = make_archive(tmp_path)
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(table(f"| libfixture | 1.2.3 | MIT | {archive.as_uri()} | – | `<sha>` | x | |"))
    result = configure_fixture(tmp_path, versions)
    assert result.returncode != 0 and "has no SHA-256" in result.stderr


# ---- apply_patches.py ------------------------------------------------------------------------------------

PATCH = """From: OVT
Subject: change greeting
Upstream-Status: Pending

--- a/hello.txt
+++ b/hello.txt
@@ -1 +1 @@
-hello
+hello, patched
"""


def source_tree(tmp_path: Path) -> Path:
    src = tmp_path / "src"
    src.mkdir()
    (src / "hello.txt").write_text("hello\n")
    return src


def patches_dir(tmp_path: Path, *patches: tuple[str, str]) -> Path:
    d = tmp_path / "patches"
    d.mkdir()
    for name, body in patches:
        (d / name).write_text(body)
    return d


def test_patches_apply_once_and_are_recorded(tmp_path: Path) -> None:
    src = source_tree(tmp_path)
    d = patches_dir(tmp_path, ("0001-greeting.patch", PATCH))
    record = tmp_path / "prefix" / "share" / "ovt" / "patches.json"
    args = ["--check", "--dir", str(d), "--source", str(src), "--record", str(record)]
    assert apply_patches.main(args) == 0
    assert apply_patches.main(args) == 0  # second run: marker says done, no re-application
    assert (src / "hello.txt").read_text() == "hello, patched\n"
    assert (src / apply_patches.MARKER).exists()
    data = json.loads(record.read_text())
    assert [p["name"] for p in data["patches"]] == ["0001-greeting.patch"]
    assert data["patches"][0]["upstream_status"] == "Pending"


def test_failing_patch_is_named_and_nothing_is_applied(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    src = source_tree(tmp_path)
    bad = PATCH.replace("-hello\n", "-goodbye\n")
    d = patches_dir(tmp_path, ("0001-greeting.patch", PATCH), ("0002-broken.patch", bad))
    assert apply_patches.main(["--check", "--dir", str(d), "--source", str(src)]) == 1
    assert "0002-broken.patch" in capsys.readouterr().err
    assert (src / "hello.txt").read_text() == "hello\n"
    assert not (src / apply_patches.MARKER).exists()


def test_series_order_and_missing_header(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    src = source_tree(tmp_path)
    d = patches_dir(tmp_path, ("a.patch", PATCH.replace("Upstream-Status: Pending\n", "")))
    (d / "series").write_text("# patches\na.patch\n")
    assert apply_patches.main(["--check", "--series", str(d / "series"), "--source", str(src)]) == 1
    assert "Upstream-Status" in capsys.readouterr().err


def test_another_series_on_a_patched_tree_fails(tmp_path: Path) -> None:
    src = source_tree(tmp_path)
    d = patches_dir(tmp_path, ("0001-greeting.patch", PATCH))
    assert apply_patches.main(["--check", "--dir", str(d), "--source", str(src)]) == 0
    (d / "0001-greeting.patch").write_text(PATCH.replace("Pending", "Inappropriate [OVT only]"))
    assert apply_patches.main(["--check", "--dir", str(d), "--source", str(src)]) == 1


def test_empty_or_missing_dir_is_a_no_op(tmp_path: Path) -> None:
    src = source_tree(tmp_path)
    assert apply_patches.main(["--check", "--dir", str(tmp_path / "none"), "--source", str(src)]) == 0
    assert not (src / apply_patches.MARKER).exists()


def test_source_inside_another_checkout_is_patched(tmp_path: Path) -> None:
    if shutil.which("git") is None:
        pytest.skip("git not installed")
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    src = source_tree(tmp_path)  # tmp_path/src sits inside the outer repository, like build/ in OVT
    d = patches_dir(tmp_path, ("0001-greeting.patch", PATCH))
    assert apply_patches.main(["--check", "--dir", str(d), "--source", str(src)]) == 0
    assert (src / "hello.txt").read_text() == "hello, patched\n"


# ---- third_party_offline.py (acceptance test 3, with file:// URLs) ---------------------------------------


def test_offline_fetch_verifies_every_archive(tmp_path: Path) -> None:
    archive, sha = make_archive(tmp_path)
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(
        table(
            f"| libfixture | 1.2.3 | MIT | {archive.as_uri()} | – | `{sha}` | x | |",
            "| tool | 1 | MIT | https://example.org/tool | – | – | x | |",
        )
    )
    out = tmp_path / "dl"
    assert third_party_offline.main(["--versions", str(versions), "--out", str(out)]) == 0
    assert third_party_offline.main(["--versions", str(versions), "--out", str(out)]) == 0  # cached
    assert (out / archive.name).exists()


def test_offline_fetch_fails_on_mismatch_and_placeholder(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    archive, _ = make_archive(tmp_path)
    versions = tmp_path / "VERSIONS.md"
    versions.write_text(
        table(
            f"| libfixture | 1.2.3 | MIT | {archive.as_uri()} | – | `{'1' * 64}` | x | |",
            "| other | 2.0 | MIT | https://example.org/other-2.0.tar.gz | – | `<sha>` | x | |",
        )
    )
    assert third_party_offline.main(["--versions", str(versions), "--out", str(tmp_path / "dl")]) == 1
    err = capsys.readouterr().err
    assert "row 'libfixture'" in err and "mismatch" in err and "row 'other'" in err
    assert not (tmp_path / "dl" / archive.name).exists()
