# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tests/corpora/synthetic/generate.py, recipes.json and tools/check_fast_subset.py (WI-0.553).

The encoders are replaced by fake executables on PATH that print the pinned (or a wrong) version and write a
deterministic output file, so these tests need no oracle image.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
REPO = TESTS.parents[1]
SYNTH = REPO / "tests" / "corpora" / "synthetic"
sys.path.insert(0, str(SYNTH))
sys.path.insert(0, str(REPO / "tools"))

import check_fast_subset  # noqa: E402
import generate  # noqa: E402

FAKE = """#!{python}
import hashlib, os, sys
name = os.path.basename(sys.argv[0])
args = sys.argv[1:]
versions = {versions!r}
if set(args) & {{"--version", "-version", "--help"}}:
    print(versions.get(name, "unknown"))
    sys.exit(0)
out = None
for flag in ("-o", "--output", "-b", "-out"):
    if flag in args and args.index(flag) + 1 < len(args):
        out = args[args.index(flag) + 1]
if out is None:
    out = args[-1]
body = hashlib.sha256(" ".join([name] + args).encode()).hexdigest().encode() + b"\\n"
if out == "-":
    sys.stdout.buffer.write(body)
else:
    with open(out, "wb") as fh:
        fh.write(body + sys.stdin.buffer.read() if not sys.stdin.isatty() else body)
"""

PINNED = {
    "ffmpeg": "ffmpeg version n7.1.1 Copyright (c) 2000-2025",
    "x264": "x264 0.164.3191 4613ac3",
    "x265": "x265 [info]: HEVC encoder version 4.1+1",
    "aomenc": "Usage: aomenc ... av1 - AOMedia Project AV1 Encoder 3.12.1",
    "MP4Box": "MP4Box - GPAC version 2.4-rev0",
}


def fake_tools(tmp: Path, versions: dict[str, str]) -> Path:
    bin_dir = tmp / "bin"
    bin_dir.mkdir()
    for name in versions:
        path = bin_dir / name
        path.write_text(FAKE.format(python=sys.executable, versions=versions), encoding="utf-8")
        path.chmod(0o755)
    return bin_dir


@pytest.fixture
def env(tmp_path, monkeypatch):
    def setup(versions: dict[str, str] = PINNED) -> Path:
        bin_dir = fake_tools(tmp_path, versions)
        monkeypatch.setenv("PATH", f"{bin_dir}{os.pathsep}{os.environ['PATH']}")
        monkeypatch.setenv("OVT_CORPORA_DIR", str(tmp_path / "corpora"))
        manifest = tmp_path / "manifest.json"
        shutil.copyfile(generate.DEFAULT_MANIFEST, manifest)
        return manifest

    return setup


def test_recipes_load_and_cover_every_numbered_recipe():
    doc = generate.load_recipes(generate.DEFAULT_RECIPES)
    numbers = {r["recipe"] for r in doc["recipes"]}
    assert numbers == {str(n) for n in range(1, 77)} | {"15a"}
    for rec in doc["recipes"]:
        assert generate.closure(doc, [rec["id"]])[-1]["id"] == rec["id"]


@pytest.mark.parametrize(
    ("argv", "problem"),
    [
        (["x264", "--crf", "20", "-o", "a.264", "s.y4m"], "x264 needs --threads"),
        (["x265", "--frame-threads", "1", "--input", "s.y4m"], "x265 needs"),
        (["aomenc", "--cpu-used=4", "-o", "a.ivf", "s.y4m"], "aomenc needs"),
        (["ffmpeg", "-i", "s.y4m", "-c:v", "libx264", "a.264"], "libx264 needs -threads"),
        (["ffmpeg", "-i", "s.y4m", "-c:v", "libx265", "-x265-params", "pools=1", "a.265"], "libx265 needs"),
        (["ffmpeg", "-i", "s.y4m", "-c:v", "libaom-av1", "a.mp4"], "libaom-av1 needs -threads"),
        (["MP4Box", "-add", "a.264", "-new", "a.mp4"], "MP4Box needs -for-test"),
        (["mkvmerge", "--no-date", "-o", "a.mkv", "a.mp4"], "mkvmerge needs"),
        (["ffmpeg", "-threads", "1", "-i", "s.y4m", "-c:v", "libx264", "a.264"], "libx264 needs -threads N after"),
        (["ffmpeg", "-f", "lavfi", "-i", "life=s=64x64:mold=10", "a.y4m"], "lavfi life needs seed"),
        (["ffmpeg", "-f", "lavfi", "-i", "cellauto=s=64x64:rule=110,format=yuv420p", "a.y4m"], "lavfi cellauto"),
        (["ffmpeg", "-f", "lavfi", "-i", "life", "a.y4m"], "lavfi life needs seed"),
        (["ffmpeg", "-f", "lavfi", "-i", "cellauto,format=yuv420p", "a.y4m"], "lavfi cellauto"),
        (["mp4encrypt", "--method", "MPEG-CENC", "--key", "1:00112233:random", "f.mp4", "o.mp4"], "fixed IV"),
    ],
)
def test_unpinned_threads_and_dates_are_rejected(tmp_path, argv, problem):
    """Thread counts change x264/x265/libaom output and muxers stamp dates, so recipes must pin both (#119)."""
    assert problem in (generate.determinism_problem(argv) or "")
    doc = json.loads(generate.DEFAULT_RECIPES.read_text())
    doc["recipes"][0]["steps"] = [{"cmd": argv}]
    path = tmp_path / "recipes.json"
    path.write_text(json.dumps(doc))
    with pytest.raises(generate.GenError, match="not reproducible") as err:
        generate.load_recipes(path)
    assert err.value.code == 4


def test_seeded_lavfi_and_fixed_iv_pass():
    assert generate.determinism_problem(["ffmpeg", "-f", "lavfi", "-i", "life=s=64x64:seed=1:mold=10", "a.y4m"]) is None
    assert generate.determinism_problem(["ffmpeg", "-i", "cellauto=rule=110:seed=1", "a.y4m"]) is None
    assert generate.determinism_problem(["mp4encrypt", "--key", "1:0011:0a610676cb88f302", "f.mp4", "o"]) is None


def test_av1_resize_times_superres_over_2x_is_rejected(tmp_path):
    """aomenc fails mid-encode when resize x superres exceeds AV1's 2x reference scaling (recipe 19, R=S=12)."""
    base = ["aomenc", "--threads=1", "--superres-mode=1", "--superres-denominator=12", "--resize-mode=1"]
    assert "exceeds" in generate.av1_scale_problem([*base, "--resize-denominator=12", "-o", "a.ivf", "s.y4m"])
    assert generate.av1_scale_problem([*base, "--resize-denominator=9", "-o", "a.ivf", "s.y4m"]) is None
    assert generate.av1_scale_problem(["aomenc", "--superres-mode=0", "--superres-denominator=16"]) is None
    doc = json.loads(generate.DEFAULT_RECIPES.read_text())
    doc["recipes"][0]["steps"] = [{"cmd": [*base, "--resize-denominator=12", "-o", "a.ivf", "s.y4m"]}]
    path = tmp_path / "recipes.json"
    path.write_text(json.dumps(doc))
    with pytest.raises(generate.GenError, match="reference scaling") as err:
        generate.load_recipes(path)
    assert err.value.code == 4


def test_every_encoder_and_muxer_step_is_pinned():
    doc = generate.load_recipes(generate.DEFAULT_RECIPES)  # raises on any unpinned step
    steps = [s for item in [*doc["sources"], *doc["recipes"]] for s in item["steps"]]
    argvs = [a for s in steps for a in generate.step_argvs(s)]
    assert all(generate.determinism_problem(a) is None for a in argvs)
    assert {"x264", "x265", "aomenc", "MP4Box", "mkvmerge"} <= {a[0] for a in argvs}


def test_pins_match_oracle_versions_table():
    sys.path.insert(0, str(REPO / "tests" / "oracles"))
    import oracle_versions

    rows = {r.tool: r for r in oracle_versions.parse_table((REPO / "tests/oracles/ORACLE_VERSIONS.md").read_text())}
    doc = generate.load_recipes(generate.DEFAULT_RECIPES)
    for tool, pin in doc["pins"].items():
        row = rows["aomenc" if tool == "scalable_encoder" else tool]
        assert pin["expect"] == row.expect, tool


def test_version_mismatch_exits_5_and_writes_nothing(env, tmp_path, capsys):
    """Acceptance test 2: x265 --version differs from the pin -> exit 5, no file written."""
    env({**PINNED, "x265": "x265 [info]: HEVC encoder version 4.0"})
    out = tmp_path / "out"
    assert generate.main(["--only", "syn_h265_amp_sao_on", "--out", str(out)]) == 5
    assert not out.exists()
    assert "encoder version 4.1" in capsys.readouterr().err


def test_generate_check_and_update_manifest(env, tmp_path):
    manifest = env()
    out = tmp_path / "out"
    argv = ["--only", "syn_h265_amp_sao_on", "--out", str(out), "--manifest", str(manifest)]
    assert generate.main([*argv, "--check"]) == 5  # hash not recorded yet
    assert generate.main([*argv, "--update-manifest"]) == 0
    record = json.loads((out / "syn_h265_amp_sao_on" / ".ok").read_text())
    assert record["primary"] == "syn_h265_amp_sao_on.265"
    entry = next(e for e in json.loads(manifest.read_text())["entries"] if e["id"] == "syn_h265_amp_sao_on")
    assert entry["sha256"] == record["sha256"] and "unverified" not in entry
    assert generate.main([*argv, "--check"]) == 0  # deterministic: the second run reproduces the hash
    entry["sha256"] = "0" * 64
    data = json.loads(manifest.read_text())
    data["entries"] = [entry if e["id"] == entry["id"] else e for e in data["entries"]]
    manifest.write_text(json.dumps(data))
    assert generate.main([*argv, "--check"]) == 5


def test_mp4_wrap_makes_the_mp4_primary(env, tmp_path):
    env()
    out = tmp_path / "out"
    assert generate.main(["--only", "syn_h264_gop60_1080p", "--out", str(out)]) == 0
    record = json.loads((out / "syn_h264_gop60_1080p" / ".ok").read_text())
    assert record["primary"] == "syn_h264_gop60_1080p.mp4"
    assert set(record["files"]) == {"syn_h264_gop60_1080p.mp4", "syn_h264_gop60_1080p.264"}


def test_pipes_globs_and_stdout(env, tmp_path):
    env()
    recipes = {
        "schema_version": 1,
        "pins": {"ffmpeg": {"probe": ["ffmpeg", "-version"], "expect": "n7.1.1"}},
        "sources": [{"file": "a.y4m", "steps": [{"cmd": ["ffmpeg", "a.y4m"]}]}],
        "recipes": [
            {
                "recipe": "1",
                "id": "syn_t_pipe",
                "features": [],
                "needs": ["a.y4m"],
                "files": ["p.out"],
                "steps": [{"pipe": [["cat", "a.y4m"], ["ffmpeg", "-i", "-", "p.out"]]}],
            },
            {
                "recipe": "2",
                "id": "syn_t_glob",
                "features": [],
                "needs": ["a.y4m"],
                "files": ["seg_*.m4s", "all.bin"],
                "steps": [{"cmd": ["ffmpeg", f"seg_{n}.m4s"]} for n in (1, 2, 10)]
                + [{"cmd": ["cat", "{glob:seg_*.m4s}"], "stdout": "all.bin"}],
            },
        ],
        "fixtures": [],
    }
    path = tmp_path / "recipes.json"
    path.write_text(json.dumps(recipes))
    out = tmp_path / "out"
    assert generate.main(["--recipes", str(path), "--out", str(out)]) == 0
    pipe = (out / "syn_t_pipe" / "p.out").read_bytes()
    assert pipe.endswith((tmp_path / "out" / ".work" / "a.y4m").read_bytes())  # stdin reached the second process
    files = json.loads((out / "syn_t_glob" / ".ok").read_text())["files"]
    assert set(files) == {"seg_1.m4s", "seg_2.m4s", "seg_10.m4s", "all.bin"}
    seg = b"".join((out / "syn_t_glob" / f"seg_{n}.m4s").read_bytes() for n in (1, 2, 10))
    assert (out / "syn_t_glob" / "all.bin").read_bytes() == seg  # {glob:} expands in natural order


def test_local_only_selection():
    doc = generate.load_recipes(generate.DEFAULT_RECIPES)
    default = generate.select(doc, [], False)
    local = generate.select(doc, [], True)
    assert "syn_h264_1h_1080p" in local and "syn_h264_1h_1080p" not in default
    assert set(local) == {
        "syn_h265_10min_1080p",
        "syn_av1_10min_1080p",
        "syn_h264_1h_1080p",
        "syn_av1_4k_2min",
        "syn_mp4_frag_3600",
        "syn_h265_4k_main10",
    }
    assert generate.main(["--only", "syn_does_not_exist", "--list"]) == 4


def test_missing_manual_download_corpus_skips_its_recipe(env, tmp_path, capsys):
    """syn_mkv_dv_p5 remuxes hdr_dovi_p5, a manual_download corpus with no URL: skipped with a reason until present."""
    manifest = env()
    out = tmp_path / "out"
    base = ["--out", str(out), "--manifest", str(manifest), "--list"]
    assert generate.main(base) == 0
    listed, err = capsys.readouterr()
    assert "syn_mkv_dv_p5" not in listed.split() and "syn_mkv_h264" in listed.split()
    assert "skip syn_mkv_dv_p5: needs hdr_dovi_p5, a manual_download corpus" in err
    assert generate.main([*base, "--only", "syn_mkv_dv_p5"]) == 0  # named explicitly: selected, fails at run time
    assert capsys.readouterr().out.split() == ["syn_mkv_dv_p5"]
    placed = tmp_path / "corpora" / "hdr_dovi_p5"  # a maintainer placed the clip in the cache
    placed.mkdir(parents=True)
    (placed / "hdr_dovi_p5.mp4").write_bytes(b"x")
    (placed / ".ok").write_text(json.dumps({"primary": "hdr_dovi_p5.mp4"}))
    assert generate.main(base) == 0
    listed, err = capsys.readouterr()
    assert "syn_mkv_dv_p5" in listed.split() and "skip" not in err


def test_missing_helper_is_reported_before_running(env, tmp_path, monkeypatch):
    env()
    monkeypatch.setattr(generate, "REPO", tmp_path)  # a checkout without tools/*.py
    assert generate.main(["--only", "syn_h265_multi_pps", "--out", str(tmp_path / "out")]) == 4
    assert not (tmp_path / "out").exists()


def test_manifest_has_every_recipe_and_validates():
    import fetch_corpora

    manifest = fetch_corpora.load_manifest(generate.DEFAULT_MANIFEST)
    syn = {e["id"]: e for e in manifest["entries"] if e["source"] == "syn"}
    doc = generate.load_recipes(generate.DEFAULT_RECIPES)
    for rec in doc["recipes"]:
        if "kind" not in syn[rec["id"]]:
            assert rec["recipe"] == "73", rec["id"]  # WI-1.413 owns the pattern entries
            continue
        expected = generate.manifest_entry(rec)
        got = {k: v for k, v in syn[rec["id"]].items() if k not in ("sha256", "size_bytes", "files")}
        if "sha256" in syn[rec["id"]]:
            expected.pop("unverified")
        assert got == expected, rec["id"]


def test_check_fast_subset_registry_is_consistent():
    assert check_fast_subset.main([]) == 0


def test_check_fast_subset_all_chapters_passes():
    """Acceptance test 5: every fixture cited by the spec is a registry id or a §14.3.5 alias."""
    assert check_fast_subset.main(["--all-chapters"]) == 0


def test_check_fast_subset_flags_unknown_names(tmp_path, capsys):
    spec = tmp_path / "spec"
    spec.mkdir()
    shutil.copyfile(REPO / "docs" / "spec" / "14-testing.md", spec / "14-testing.md")
    (spec / "99-test.md").write_text(
        "Uses `syn_h264_mbaff`, `corpus/syn_h265_*`, `corpus/<id>` and `syn_sei_h264_foo`.\n"
        "Then syn_h264_nonexistent, `corpus/jvt_NOT_A_STREAM`, `bogus_grid.fb` and `mystery_clip.mp4`.\n",
        encoding="utf-8",
    )
    assert check_fast_subset.main(["--all-chapters", "--spec-dir", str(spec)]) == 1
    out = capsys.readouterr().out
    found = sorted(re.findall(r"unknown (?:corpus id|fixture file|fixture) (\S+)", out))
    assert found == ["bogus_grid.fb", "corpus/jvt_NOT_A_STREAM", "mystery_clip.mp4", "syn_h264_nonexistent"]
    assert all(line.startswith("99-test.md:2:") for line in out.splitlines())


def test_fixtures_run_hash_and_register(env, tmp_path, monkeypatch):
    """--fixtures writes each fixture to its path, skips ovt-made and local_only ones, and records hashes."""
    manifest = env()
    monkeypatch.setattr(generate, "REPO", tmp_path)
    (tmp_path / "tools").mkdir()
    (tmp_path / "tools" / "gen.py").write_text("import sys; open(sys.argv[1], 'wb').write(b'fx')\n")
    fixtures = [
        {"id": "fixture_t_a", "path": "fx/a.bin", "cmd": ["{python}", "{repo}/tools/gen.py", "{out}"]},
        {
            "id": "fixture_t_perf",
            "path": "fx/p.bin",
            "cmd": ["{python}", "{repo}/tools/gen.py", "{out}"],
            "local_only": True,
        },
        {"id": "fixture_t_ovt", "path": "fx/o.fb", "cmd": ["ovt", "analyze", "x", "-o", "{out}"]},
    ]
    doc = json.loads(generate.DEFAULT_RECIPES.read_text()) | {"fixtures": fixtures}
    path = tmp_path / "recipes.json"
    path.write_text(json.dumps(doc))
    monkeypatch.setattr(generate.shutil, "which", lambda name: None)
    assert generate.main(["--recipes", str(path), "--manifest", str(manifest), "--sync-manifest"]) == 0
    entry = {e["id"]: e for e in json.loads(manifest.read_text())["entries"]}["fixture_t_a"]
    assert entry["kind"] == "fixture" and entry["path"] == "fx/a.bin" and entry["unverified"]
    base = ["--recipes", str(path), "--manifest", str(manifest), "--out", str(tmp_path / "out"), "--fixtures"]
    assert generate.main([*base, "--check"]) == 5  # hash not recorded yet
    assert generate.main([*base, "--update-manifest"]) == 0
    assert (tmp_path / "fx" / "a.bin").read_bytes() == b"fx"
    assert not (tmp_path / "fx" / "p.bin").exists() and not (tmp_path / "fx" / "o.fb").exists()
    entry = {e["id"]: e for e in json.loads(manifest.read_text())["entries"]}["fixture_t_a"]
    assert entry["sha256"] == generate.sha256_file(tmp_path / "fx" / "a.bin") and "unverified" not in entry
    assert generate.main([*base, "--check"]) == 0
    doc["fixtures"].append(dict(fixtures[0]))
    path.write_text(json.dumps(doc))
    assert generate.main(["--recipes", str(path), "--list"]) == 4  # duplicate fixture id
