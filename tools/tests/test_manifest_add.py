# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/manifest_add.py, the populated corpora manifest and the named-stream check (WI-0.552)."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import jsonschema
import pytest

TESTS = Path(__file__).resolve().parent
TOOLS = TESTS.parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TESTS))

import check_fast_subset  # noqa: E402
import fetch_corpora  # noqa: E402
import manifest_add  # noqa: E402
from fixture_server import serve  # noqa: E402

MANIFEST = REPO / "tests" / "corpora" / "manifest.json"
FAST_BUDGET = 314_572_800


def entries() -> list[dict]:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))["entries"]


# ------------------------------------------------------------------------------------- the manifest itself


def test_manifest_validates_and_ids_are_unique():
    manifest = fetch_corpora.load_manifest(MANIFEST)  # schema + duplicate ids/aliases
    assert len(manifest["entries"]) > 400


def test_every_corpus_source_of_the_catalogue_has_entries():
    sources = {e["source"] for e in entries()}
    # argon is manual-download only (hashes come from the maintainer's AOM account), ts has the tsduck captures
    assert set(check_fast_subset.PUBLIC) - {"argon"} <= sources


def test_hash_is_recorded_unless_pending_or_manual():
    for e in entries():
        if e.get("unverified") or e.get("manual_download"):
            assert "sha256" not in e or e.get("manual_download"), e["id"]
        elif "url" in e or "vcs" in e:
            assert len(e["sha256"]) == 64 and e["size_bytes"] > 0, e["id"]


def test_fast_subset_stays_within_the_ci_budget():
    """Acceptance 1 (size half): the verified fast entries fit `--max-total-bytes 314572800`."""
    total = sum(
        e["size_bytes"] for e in entries() if "fast" in e.get("tags", []) and "sha256" in e and e["source"] != "syn"
    )
    assert 0 < total <= FAST_BUDGET


def test_redistributable_flag_matches_origin_terms():
    for e in entries():
        if e["source"] in (
            "jvt",
            "jctvc",
            "mpegff",
            "gpac",
            "bento4",
            "dashif",
            "cta",
            "apple",
            "mkv",
        ):
            assert e["redistributable"] is False, e["id"]
        if e["source"] == "aom":
            assert e["redistributable"] is True, e["id"]


def test_tag_sets_named_in_the_catalogue_have_the_documented_sizes():
    by_tag: dict[str, list[str]] = {}
    for e in entries():
        for tag in e.get("tags", []):
            by_tag.setdefault(tag, []).append(e["id"])
    assert len(by_tag["h264-ps-golden"]) == 20
    assert len(by_tag["h264-slice-golden"]) == 40  # 38 fast streams + MVCNV-1 + SVCBST-1
    assert len(by_tag["h264-analysis-smoke"]) == 10
    assert len(by_tag["h265-cu-golden"]) == 10
    assert all(e["codecs"] == ["h265"] for e in entries() if "rext" in e.get("tags", []))


def test_lfs_backed_entries_use_vcs_with_a_pinned_commit():
    vcs = [e for e in entries() if "vcs" in e]
    assert len(vcs) > 100
    for e in vcs:
        assert "url" not in e and len(e["vcs"]["rev"]) == 40 and e["vcs"]["git"].startswith("https://github.com/")


# ------------------------------------------------------------------------------------- acceptance 3


def test_check_fast_subset_passes_on_the_repository():
    assert check_fast_subset.main([]) == 0


def test_named_stream_without_entry_is_reported(tmp_path):
    ch14 = (REPO / "docs" / "spec" / "14-testing.md").read_text(encoding="utf-8")
    manifest = {"entries": [e for e in entries() if e["id"] != "jvt_CAMA1_Sony_C"]}
    problems = check_fast_subset.check_named(manifest, ch14)
    assert problems == ["manifest.json: jvt_CAMA1_Sony_C (§14.3.2 bullet) has no entry"]


def test_named_ids_cover_stems_ranges_and_size_suffixes():
    ids = check_fast_subset.named_public_ids((REPO / "docs" / "spec" / "14-testing.md").read_text(encoding="utf-8"))
    for want in ("jvt_HPCV_BRCM_A", "jvt_ALPHACONFORMANCEG", "jctvc_Main_422_10_A_RExt_Sony_2", "aom_av1_1_b8_01_size_16x18",
                 "mkv_test1", "mkv_test8", "ytugc_clip03", "mpegff_a1_foreman_qcif"):  # fmt: skip
        assert want in ids, want
    assert "jvt_FRExt1_Panasonic_C" not in ids  # the older revision named as a wrong spelling


# ------------------------------------------------------------------------------------- the helper


def test_lfs_pointer_parsing():
    sha = "ab" * 32
    pointer = f"version https://git-lfs.github.com/spec/v1\noid sha256:{sha}\nsize 908\n".encode()
    assert manifest_add.lfs_pointer(pointer) == (sha, 908)
    assert manifest_add.lfs_pointer(b"\x00\x00\x00 ftypisom") is None
    assert manifest_add.lfs_pointer(pointer + b"x" * 2000) is None


def test_make_id_collapses_punctuation_and_avoids_collisions():
    assert manifest_add.make_id("mkv", "test-1.final") == "mkv_test_1_final"
    taken = {"jvt_a"}
    assert manifest_add.make_id("jvt", "a", taken) == "jvt_a_2"
    assert len(manifest_add.make_id("ts", "x" * 200)) == 64


def test_parse_listing_keeps_sibling_zip_links_only():
    page = '<a href="?C=N">N</a><a href="/up/">up</a><a href="A.zip">A</a><a href="sub/B.ZIP?x=1">B</a><a href="c.txt">c</a>'
    got = manifest_add.parse_listing(page, "https://h.example/d/", ".zip")
    assert got == ["https://h.example/d/A.zip", "https://h.example/d/sub/B.ZIP"]


def git(cwd: Path, *args: str) -> None:
    subprocess.run(
        [
            "git",
            "-C",
            str(cwd),
            "-c",
            "user.name=t",
            "-c",
            "user.email=t@example.invalid",
            *args,
        ],
        check=True,
        capture_output=True,
    )


def test_from_git_reads_lfs_pointers_without_downloading(tmp_path):
    repo = tmp_path / "r"
    (repo / "data").mkdir(parents=True)
    sha = "cd" * 32
    (repo / "data" / "big.mp4").write_text(
        f"version https://git-lfs.github.com/spec/v1\noid sha256:{sha}\nsize 5000000\n"
    )
    (repo / "data" / "plain.mp4").write_bytes(b"plain bytes")
    (repo / "data" / "skip.json").write_text("{}")
    git(tmp_path, "init", "-q", "r")
    git(repo, "add", ".")
    git(repo, "commit", "-q", "-m", "x")
    rev = manifest_add.git_rev(repo)
    out = manifest_add.entries_from_git(repo, "org/name", "mpegff", ["data/*.mp4"], "mpegff", set())
    by_id = {e["id"]: e for e in out}
    assert set(by_id) == {"mpegff_big", "mpegff_plain"}
    assert by_id["mpegff_big"]["sha256"] == sha and by_id["mpegff_big"]["size_bytes"] == 5_000_000
    assert by_id["mpegff_big"]["vcs"] == {
        "git": "https://github.com/org/name",
        "lfs_path": "data/big.mp4",
        "rev": rev,
    }
    assert by_id["mpegff_plain"]["sha256"] == hashlib.sha256(b"plain bytes").hexdigest()
    assert by_id["mpegff_plain"]["url"] == f"https://raw.githubusercontent.com/org/name/{rev}/data/plain.mp4"


def test_vcs_entry_resolves_to_the_pinned_lfs_media_url():
    entry = {
        "vcs": {
            "git": "https://github.com/o/n",
            "lfs_path": "d/a b.mp4",
            "rev": "1" * 40,
        }
    }
    assert fetch_corpora.entry_url(entry) == f"https://media.githubusercontent.com/media/o/n/{'1' * 40}/d/a%20b.mp4"
    assert (
        fetch_corpora.entry_url(
            {
                "vcs": {
                    "git": "https://example.com/o/n",
                    "lfs_path": "x",
                    "rev": "1" * 40,
                }
            }
        )
        is None
    )
    assert fetch_corpora.entry_url({"url": "https://h/x"}) == "https://h/x"


def test_fill_records_hash_and_size_of_unverified_entries(tmp_path):
    payload = bytes(range(256)) * 4
    root = tmp_path / "www"
    root.mkdir()
    (root / "a.bin").write_bytes(payload)
    manifest = tmp_path / "m.json"
    lic = {"licence": "t", "licence_spdx": "CC0-1.0", "redistributable": True}
    with serve(root) as base:
        doc = {"schema_version": 1, "entries": [
            {"id": "x_a", "source": "misc", **lic, "url": f"{base}a.bin", "unverified": True},
            {"id": "x_b", "source": "misc", **lic, "unverified": True, "note": "no url yet"}]}  # fmt: skip
        manifest.write_text(json.dumps(doc))
        assert manifest_add.main(["fill", "--manifest", str(manifest)]) == 0
    filled = json.loads(manifest.read_text())["entries"]
    assert filled[0]["sha256"] == hashlib.sha256(payload).hexdigest() and filled[0]["size_bytes"] == len(payload)
    assert "unverified" not in filled[0] and filled[1]["unverified"] is True
    jsonschema.validate(
        json.loads(manifest.read_text()),
        json.loads((REPO / "tests/corpora/manifest.schema.json").read_text()),
    )


def test_fill_reports_unreachable_hosts_with_exit_3(tmp_path):
    manifest = tmp_path / "m.json"
    base = {"licence": "t", "licence_spdx": "CC0-1.0", "redistributable": True}
    manifest.write_text(json.dumps({"schema_version": 1, "entries": [
        {"id": "x_a", "source": "misc", **base, "url": "http://127.0.0.1:9/a.bin", "unverified": True}]}))  # fmt: skip
    assert manifest_add.main(["fill", "--manifest", str(manifest)]) == 3
    assert json.loads(manifest.read_text())["entries"][0]["unverified"] is True


@pytest.mark.parametrize("bad", [[], ["add", "--id", "x", "--source", "misc", "--url", "https://h/x"]])
def test_usage_errors_exit_nonzero(bad, tmp_path):
    if not bad:
        with pytest.raises(SystemExit):
            manifest_add.main(bad)
    else:
        manifest = tmp_path / "m.json"
        manifest.write_text('{"schema_version": 1, "entries": []}')
        assert manifest_add.main([*bad, "--manifest", str(manifest)]) == 5  # licence is required
