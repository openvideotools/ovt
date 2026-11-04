# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/check_wi_refs.py, tools/check_versions_md.py and the WI-0.007 hook configuration."""

from __future__ import annotations

import json
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import check_versions_md  # noqa: E402
import check_wi_refs  # noqa: E402

# ---- a miniature specification ---------------------------------------------------------------------------

CH16 = """# 16 Roadmap

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **M0.1** | Scaffold | w1 | WI-0.000, WI-0.001 |
| **M0.2** | Core | w2 | WI-0.002 |
| **MA.0** | Android shell (track A, runs with M0.2) | w2 | WI-0.050 |

## 16.4 Resolved dependency table

| WI | Chapter §ref | Title | Size | Phase | Milestone | Depends on (concrete ids only) | FRs (chapter 4 §4.17) |
|---|---|---|---|---|---|---|---|
| WI-0.000 | 13 §13.10 | Skeleton | M | 0 | M0.1 | none | FR-GEN-001 |
| WI-0.001 | 13 §13.10 | Presets | S | 0 | {m001} | {d001} | FR-GEN-002 |
| WI-0.002 | 13 §13.10 | Manifest | S | 0 | M0.2 | WI-0.001 | — |
| WI-0.050 | 13 §13.10 | Android | S | A | MA.0 | WI-0.002 | — |

## 16.5 Next
"""

CH13 = """# 13 Platforms

### WI-0.000 — Skeleton
**Depends on:** none.
### WI-0.001 — Presets
**Depends on:** {b001}.
### WI-0.002 — Manifest
**Depends on:** WI-0.001 (the vcpkg part arrives with WI-0.050).
### WI-0.050 — Android
**Depends on:** WI-0.002.
"""

CH04 = """# 4 Requirements

## 4.17 FR → WI traceability

| Area | Requirement | Phase | Priority | WIs |
|---|---|---|---|---|
| GEN | FR-GEN-001 | 0 | must | WI-0.000 |
| GEN | FR-GEN-002 | 0 | must | WI-0.001 |
"""


def write_spec(tmp: Path, *, m001: str = "M0.1", d001: str = "WI-0.000", b001: str | None = None) -> Path:
    spec = tmp / "docs" / "spec"
    spec.mkdir(parents=True)
    (spec / "16-roadmap-and-work-items.md").write_text(CH16.format(m001=m001, d001=d001))
    (spec / "13-platforms-build-ci.md").write_text(CH13.format(b001=b001 or d001))
    (spec / "04-requirements.md").write_text(CH04)
    return spec / "16-roadmap-and-work-items.md"


def run_table(ch16: Path, capsys: pytest.CaptureFixture[str]) -> tuple[int, str]:
    rc = check_wi_refs.main(["--table", str(ch16), "--root", str(ch16.parent)])
    out = capsys.readouterr()
    return rc, out.out + out.err


def test_table_passes_on_a_consistent_spec(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    rc, out = run_table(write_spec(tmp_path), capsys)
    assert rc == 0, out
    assert "0 error(s)" in out


def test_dependency_on_a_later_milestone_names_both(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    # Acceptance test 3: WI-0.001 (M0.1) depends on WI-0.002 (M0.2).
    rc, out = run_table(write_spec(tmp_path, d001="WI-0.002"), capsys)
    assert rc == 1
    assert re.search(r"WI-0\.001 \(M0\.1\) depends on WI-0\.002 \(M0\.2\)", out)


def test_track_a_milestone_takes_its_core_ordinal(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    # WI-0.001 in M0.2 may depend on WI-0.050 of MA.0 ("runs with M0.2"), but WI-0.050 depends on WI-0.002 too.
    rc, out = run_table(write_spec(tmp_path, m001="M0.2", d001="WI-0.050"), capsys)
    assert "later milestone" not in out


def test_depends_on_line_must_match_the_row(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    rc, out = run_table(write_spec(tmp_path, b001="none"), capsys)
    assert rc == 1 and "WI-0.001: Depends on differs" in out


def test_unknown_citation_and_missing_block(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    ch16 = write_spec(tmp_path)
    ch13 = ch16.parent / "13-platforms-build-ci.md"
    ch13.write_text(
        ch13.read_text().replace("### WI-0.050 — Android\n**Depends on:** WI-0.002.\n", "")
        + "\nSee WI-0.999; ids such as e.g. WI-2.014 and WI-0.097 reserved are fine.\n"
    )
    rc, out = run_table(ch16, capsys)
    assert rc == 1
    assert "cites WI-0.999" in out and "WI-0.050: expected exactly one" in out
    assert "WI-2.014" not in out and "WI-0.097" not in out


def test_frs_column_must_be_the_inverse(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    ch16 = write_spec(tmp_path)
    ch16.write_text(ch16.read_text().replace("| none | FR-GEN-001 |", "| none | — |"))
    rc, out = run_table(ch16, capsys)
    assert rc == 1 and "WI-0.000: FRs column" in out


# ---- --make-issues (acceptance test 5) ---------------------------------------------------------------------


def fake_gh(tmp: Path) -> dict[str, str]:
    bin_dir = tmp / "bin"
    bin_dir.mkdir()
    gh = bin_dir / "gh"
    gh.write_text(
        f"#!{sys.executable}\nimport sys\nopen({str(tmp / 'gh.log')!r}, 'a').write(' '.join(sys.argv[1:]) + '\\n')\n"
    )
    gh.chmod(gh.stat().st_mode | stat.S_IEXEC)
    return {"PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}"}


def test_make_issues_is_idempotent(
    tmp_path: Path, capsys: pytest.CaptureFixture[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    ch16 = write_spec(tmp_path)
    issues = tmp_path / "issues.json"
    issues.write_text(json.dumps([{"title": "WI-0.000: Skeleton"}, {"title": "spec: unrelated"}]))
    monkeypatch.setenv("PATH", fake_gh(tmp_path)["PATH"])
    args = ["--make-issues", "--issues-json", str(issues), "--root", str(ch16.parents[2])]
    # first real run creates the three missing issues through gh
    assert check_wi_refs.main(args) == 0
    assert "3 issue(s) to create" in capsys.readouterr().out
    assert (tmp_path / "gh.log").read_text().count("issue create --title") == 3
    # a second dry run reports nothing left to create
    assert check_wi_refs.main([*args, "--dry-run"]) == 0
    assert "0 issue(s) to create" in capsys.readouterr().out


# ---- --spec-version (acceptance test 4) ----------------------------------------------------------------------


def git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-c", "user.name=t", "-c", "user.email=t@e", *args], cwd=repo, check=True, capture_output=True
    )


@pytest.mark.skipif(shutil.which("git") is None, reason="git not installed")
def test_spec_change_without_version_bump_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    spec = tmp_path / "docs" / "spec"
    spec.mkdir(parents=True)
    (spec / "00-front-matter.md").write_text("| **Version** | 1.0-rc1 (2026-11-04) |\n")
    (spec / "CHANGELOG.md").write_text("# Changelog\n\n## 1.0.0-rc1 — 2026-11-04\n\n- initial\n")
    (spec / "04-requirements.md").write_text("FR-GEN-001 must hold.\n")
    git(tmp_path, "init", "-q", "-b", "main")
    git(tmp_path, "add", ".")
    git(tmp_path, "commit", "-q", "-m", "spec")
    git(tmp_path, "checkout", "-q", "-b", "pr")
    (spec / "04-requirements.md").write_text("FR-GEN-001 must hold, changed.\n")
    git(tmp_path, "commit", "-q", "-am", "edit")
    args = ["--spec-version", "--base", "main", "--root", str(tmp_path)]
    assert check_wi_refs.main(args) == 1
    assert "still '1.0-rc1 (2026-11-04)'" in capsys.readouterr().err

    (spec / "00-front-matter.md").write_text("| **Version** | 1.0.1-rc1 (2026-11-05) |\n")
    (spec / "CHANGELOG.md").write_text(
        "# Changelog\n\n## 1.0.1-rc1 — 2026-11-05\n\n- 4 — edit — FR-GEN-001\n\n"
        "## 1.0.0-rc1 — 2026-11-04\n\n- initial\n"
    )
    git(tmp_path, "commit", "-q", "-am", "bump")
    assert check_wi_refs.main(args) == 0


# ---- --commit-msg and the DCO hook (acceptance test 2) -------------------------------------------------------


@pytest.mark.parametrize(
    ("message", "ok"),
    [
        ("WI-0.007: add pre-commit hooks\n\nWhy.\n", True),
        ("Fix typo in README\n\nSee #15.\n", True),
        ("Fix typo in README\n", False),
        ("Merge branch 'main' into wi/0.007-pre-commit\n", True),
        ("fixup! WI-0.007: add hooks\n", True),
    ],
)
def test_commit_msg_needs_a_reference(message: str, ok: bool) -> None:
    assert (check_wi_refs.check_commit_msg(message) is None) == ok


def dco_regex() -> re.Pattern[str]:
    config = (REPO / ".pre-commit-config.yaml").read_text()
    m = re.search(r"- id: dco-signoff\n\s+name: \"([^\"]+)\"\n\s+entry: \"([^\"]+)\"", config)
    assert m, "dco-signoff hook not found"
    assert "git commit -s" in m.group(1)
    return re.compile(m.group(2), re.M)


def test_dco_hook_pattern() -> None:
    rx = dco_regex()
    assert rx.search("WI-0.007: x\n\nSigned-off-by: Ada Lovelace <ada@example.org>\n")
    assert not rx.search("WI-0.007: x\n\nNo trailer here.\n")


# ---- clang-format (acceptance test 1, the formatter half) ----------------------------------------------------


@pytest.mark.skipif(shutil.which("clang-format") is None, reason="clang-format not installed")
def test_clang_format_config_reformats(tmp_path: Path) -> None:
    shutil.copy(REPO / ".clang-format", tmp_path / ".clang-format")
    src = tmp_path / "bad.cpp"
    src.write_text("int  main( ){int *p=nullptr;return p==nullptr?0:1;}\n")
    out = subprocess.run(["clang-format", "--style=file", str(src)], capture_output=True, text=True, check=True)
    assert out.stdout != src.read_text()
    assert "int* p = nullptr;" in out.stdout  # PointerAlignment: Left


# ---- check_versions_md.py --------------------------------------------------------------------------------


def test_versions_md_without_table_passes(tmp_path: Path) -> None:
    assert check_versions_md.main(["--root", str(tmp_path)]) == 0


def test_script_without_row_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    (tmp_path / "third_party").mkdir()
    (tmp_path / "third_party" / "VERSIONS.md").write_text(
        "| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |\n|---|---|---|---|---|---|---|---|\n"
        "| libaom | 3.12.1 | BSD-2-Clause | https://e.org/libaom-3.12.1.tar.gz | – | `<sha>` | third_party/libaom.cmake | |\n"
    )
    (tmp_path / "third_party" / "_template.cmake").write_text("# template\n")
    (tmp_path / "third_party" / "dav1d.cmake").write_text("# no row\n")
    assert check_versions_md.main(["--root", str(tmp_path)]) == 1
    captured = capsys.readouterr()
    assert "third_party/dav1d.cmake has no third_party/VERSIONS.md row" in captured.err
    assert "_template" not in captured.err
    assert "third_party/libaom.cmake (libaom)" in captured.out  # pending, not an error
