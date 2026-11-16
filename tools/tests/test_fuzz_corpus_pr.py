# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/fuzz_corpus_pr.py: staging and applying the nightly fuzz corpus entries (WI-0.033)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import fuzz_corpus_pr as fcp  # noqa: E402


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True, text=True).stdout


def make_repo(tmp_path: Path) -> Path:
    git(tmp_path, "init", "-q")
    seed = tmp_path / "corpus" / "bitreader" / "seed"
    seed.parent.mkdir(parents=True)
    seed.write_bytes(b"abc")
    git(tmp_path, "add", ".")
    git(tmp_path, "-c", "user.name=t", "-c", "user.email=t@example.org", "commit", "-q", "-m", "seed")
    return tmp_path


def test_select_takes_small_files_first_and_reports_drops():
    sizes = {"a" * 40: 10, "b" * 40: fcp.MAX_FILE_BYTES + 1, "c" * 40: 0, ".hidden": 5, "../x": 5, "d" * 40: 5}
    kept, dropped = fcp.select(sizes, 0)
    assert kept == sorted(["a" * 40, "d" * 40])
    assert dropped["b" * 40].endswith(f"> {fcp.MAX_FILE_BYTES}")
    assert dropped["c" * 40] == "empty"
    assert dropped[".hidden"] == "unsafe name" and dropped["../x"] == "unsafe name"


def test_select_enforces_per_target_total_and_file_count():
    kept, dropped = fcp.select({"x": 100, "y": 200}, fcp.MAX_TARGET_BYTES - 150)
    assert kept == ["x"] and "exceed" in dropped["y"]
    many = {f"f{i:03}": 1 for i in range(fcp.MAX_FILES_PER_TARGET + 5)}
    kept, dropped = fcp.select(many, 0)
    assert len(kept) == fcp.MAX_FILES_PER_TARGET and len(dropped) == 5


def test_stage_then_apply_round_trip(tmp_path, monkeypatch):
    repo = make_repo(tmp_path)
    monkeypatch.chdir(repo)
    new = repo / "corpus" / "bitreader"
    (new / ("1" * 40)).write_bytes(b"new entry")
    (new / ("2" * 40)).write_bytes(bytes(fcp.MAX_FILE_BYTES + 1))
    assert fcp.main(["stage", "--out", "staged", "--corpus", "corpus"]) == 0
    assert sorted(p.name for p in (repo / "staged" / "bitreader").iterdir()) == ["1" * 40]

    git(repo, "clean", "-fdq", "--", "corpus")
    assert not (new / ("1" * 40)).exists()
    (repo / "staged" / "unknown_target").mkdir()
    (repo / "staged" / "unknown_target" / "f").write_bytes(b"x")
    assert fcp.main(["apply", "--from", "staged", "--corpus", "corpus", "--summary", "s.md"]) == 0
    assert (new / ("1" * 40)).read_bytes() == b"new entry"
    assert not (repo / "corpus" / "unknown_target").exists()
    summary = (repo / "s.md").read_text()
    assert "`bitreader`: 1 new" in summary and "no corpus directory" in summary


def test_apply_with_nothing_new_exits_3(tmp_path, monkeypatch):
    repo = make_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "staged" / "bitreader").mkdir(parents=True)
    (repo / "staged" / "bitreader" / "seed").write_bytes(b"abc")  # already committed under that name
    assert fcp.main(["apply", "--from", "staged", "--corpus", "corpus"]) == 3


def test_pr_dry_run_prints_the_git_and_gh_commands(tmp_path, monkeypatch, capsys):
    repo = make_repo(tmp_path)
    monkeypatch.chdir(repo)
    (repo / "summary.md").write_text("# x\n")
    monkeypatch.setattr(fcp, "DEFAULT_CORPUS", "corpus")
    (repo / "corpus" / "bitreader" / ("3" * 40)).write_bytes(b"z")
    assert fcp.main(["pr", "--summary", "summary.md", "--date", "2026-11-08", "--dry-run"]) == 0
    out = capsys.readouterr().out
    assert "git checkout -b fuzz/corpus-2026-11-08" in out
    assert "git commit -s" in out and "gh pr create" in out
    assert git(repo, "branch", "--show-current").strip() != "fuzz/corpus-2026-11-08"
