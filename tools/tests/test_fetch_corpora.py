# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/fetch_corpora.py and tools/tests/fixture_server.py (WI-0.551)."""

from __future__ import annotations

import hashlib
import io
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zlib
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
TOOLS = TESTS.parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TESTS))

import fetch_corpora  # noqa: E402
from fixture_server import serve  # noqa: E402

FIXTURE_MANIFEST = TESTS / "fixtures" / "fixture.json"
PLACEHOLDER = "http://127.0.0.1:8080/"
PAYLOAD = bytes(range(256)) * 64  # 16 KiB


def stored_gzip(data: bytes) -> bytes:
    """A gzip stream with stored (uncompressed) deflate blocks: identical bytes on every zlib and Python version."""
    comp = zlib.compressobj(0, zlib.DEFLATED, -15)
    body = comp.compress(data) + comp.flush()
    header = b"\x1f\x8b\x08\x00" + b"\x00\x00\x00\x00" + b"\x00\xff"
    return header + body + struct.pack("<II", zlib.crc32(data), len(data) & 0xFFFFFFFF)


def build_files(root: Path) -> None:
    """Deterministically write the archives served by the tests (hashes are pinned in fixture.json)."""
    root.mkdir(parents=True, exist_ok=True)
    (root / "plain.bin").write_bytes(PAYLOAD)
    with zipfile.ZipFile(root / "nested.zip", "w") as zf:
        for name, data in (("dir/stream.264", PAYLOAD), ("dir/stream_yuv.md5", b"d41d8cd98f00b204e9800998ecf8427e\n")):
            zf.writestr(zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0)), data)
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w") as tf:
        info = tarfile.TarInfo("stream.obu")
        info.size, info.mtime = len(PAYLOAD), 0
        tf.addfile(info, io.BytesIO(PAYLOAD))
    (root / "archive.tar.gz").write_bytes(stored_gzip(raw.getvalue()))
    (root / "corrupt.bin").write_bytes(PAYLOAD)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entry(entry_id: str, root: Path, filename: str, **extra: object) -> dict[str, object]:
    base: dict[str, object] = {
        "id": entry_id,
        "source": "misc",
        "url": PLACEHOLDER + filename,
        "sha256": sha(root / filename),
        "size_bytes": (root / filename).stat().st_size,
        "licence": "test fixture",
        "licence_spdx": "CC0-1.0",
        "redistributable": True,
    }
    base.update(extra)
    return base


@pytest.fixture
def files(tmp_path: Path) -> Path:
    root = tmp_path / "serve"
    build_files(root)
    return root


def write_manifest(path: Path, entries: list[dict[str, object]], base: str) -> Path:
    text = json.dumps({"schema_version": 1, "entries": entries}).replace(PLACEHOLDER, base)
    path.write_text(text, encoding="utf-8")
    return path


def run(manifest: Path, cache: Path, *extra: str) -> int:
    return fetch_corpora.main(["--manifest", str(manifest), "--cache-dir", str(cache), "--retries", "1", *extra])


def test_committed_fixture_manifest_matches_generated_archives(files: Path) -> None:
    """fixture.json hashes are reproducible: the archives are built byte-for-byte deterministically."""
    committed = json.loads(FIXTURE_MANIFEST.read_text(encoding="utf-8"))
    for e in committed["entries"]:
        name = e["url"].rsplit("/", 1)[1]
        assert sha(files / name) == e["sha256"], name
        assert (files / name).stat().st_size == e["size_bytes"]


def test_acceptance_1_fixture_manifest_fetches_everything(files: Path, tmp_path: Path) -> None:
    """Given the fixture server, When fetching fixture.json, Then all files are present with .ok and exit 0."""
    text = FIXTURE_MANIFEST.read_text(encoding="utf-8")
    with serve(files) as base:
        manifest = tmp_path / "m.json"
        manifest.write_text(text.replace(PLACEHOLDER, base), encoding="utf-8")
        cache = tmp_path / "cache"
        assert run(manifest, cache) == 0
    for entry_id in [e["id"] for e in json.loads(text)["entries"]]:
        assert (cache / entry_id / ".ok").is_file()
    assert (cache / "fx_plain" / "plain.bin").read_bytes() == PAYLOAD
    assert (cache / "fx_zip" / "stream.264").read_bytes() == PAYLOAD
    assert (cache / "fx_zip" / "stream_yuv.md5").is_file()
    assert (cache / "fx_targz" / "stream.obu").read_bytes() == PAYLOAD
    assert (cache / "alias" / "plain_alias.bin").resolve() == (cache / "fx_plain" / "plain.bin").resolve()


def test_acceptance_2_wrong_sha256_exits_2_without_ok(files: Path, tmp_path: Path) -> None:
    with serve(files) as base:
        e = entry("bad", files, "plain.bin", sha256="0" * 64)
        manifest = write_manifest(tmp_path / "m.json", [e], base)
        assert run(manifest, tmp_path / "cache") == 2
    assert not (tmp_path / "cache" / "bad" / ".ok").exists()


def test_corrupted_download_exits_2(files: Path, tmp_path: Path) -> None:
    with serve(files, corrupt=["corrupt.bin"]) as base:
        manifest = write_manifest(tmp_path / "m.json", [entry("c", files, "corrupt.bin")], base)
        assert run(manifest, tmp_path / "cache") == 2


def test_truncated_download_is_a_network_failure_exit_3(files: Path, tmp_path: Path) -> None:
    with serve(files, corrupt={"plain.bin": "truncate"}) as base:
        manifest = write_manifest(tmp_path / "m.json", [entry("t", files, "plain.bin")], base)
        assert run(manifest, tmp_path / "cache") == 3


def test_missing_file_is_a_network_failure_exit_3(files: Path, tmp_path: Path) -> None:
    with serve(files) as base:
        e = entry("m", files, "plain.bin", url=base + "nope.bin")
        manifest = write_manifest(tmp_path / "m.json", [e], base)
        assert run(manifest, tmp_path / "cache") == 3


def test_acceptance_3_traversal_member_exits_4(files: Path, tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    with zipfile.ZipFile(files / "evil.zip", "w") as zf:
        zf.writestr("../evil", b"x")
    with serve(files) as base:
        e = entry("evil", files, "evil.zip", extract={"archive": "zip", "member": "../evil"})
        manifest = write_manifest(tmp_path / "m.json", [e], base)
        assert run(manifest, tmp_path / "cache") == 4
    assert "unsafe member name" in capsys.readouterr().err
    assert not (tmp_path / "evil").exists()


def test_absolute_member_exits_4(files: Path, tmp_path: Path) -> None:
    e = entry("abs", files, "nested.zip", extract={"archive": "zip", "member": "/etc/passwd"})
    manifest = write_manifest(tmp_path / "m.json", [e], "http://127.0.0.1:1/")
    assert run(manifest, tmp_path / "cache") == 4


def test_acceptance_4_verify_only_offline_and_fast(files: Path, tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    with serve(files) as base:
        e = entry("big", files, "plain.bin")
        manifest = write_manifest(tmp_path / "m.json", [e], base)
        assert run(manifest, cache) == 0
    # server is gone: no network is possible
    start = time.monotonic()
    assert run(manifest, cache, "--verify-only") == 0
    assert time.monotonic() - start < 1.0  # 16 KiB; the streaming hash scales linearly (well below 1 s/GiB)


def test_verify_only_detects_damage_and_missing(files: Path, tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    with serve(files) as base:
        manifest = write_manifest(tmp_path / "m.json", [entry("d", files, "plain.bin")], base)
        assert run(manifest, cache) == 0
    (cache / "d" / "plain.bin").write_bytes(b"damaged")
    assert run(manifest, cache, "--verify-only") == 2
    shutil.rmtree(cache / "d")
    assert run(manifest, cache, "--verify-only") == 2


def test_damaged_cache_is_refetched(files: Path, tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    with serve(files) as base:
        manifest = write_manifest(tmp_path / "m.json", [entry("d", files, "plain.bin")], base)
        assert run(manifest, cache) == 0
        (cache / "d" / "plain.bin").write_bytes(b"damaged")
        assert run(manifest, cache) == 0
    assert (cache / "d" / "plain.bin").read_bytes() == PAYLOAD


@pytest.mark.parametrize(
    "mutate",
    [
        lambda m: m.update(schema_version=2),
        lambda m: m["entries"][0].update(sha256="xyz"),
        lambda m: m["entries"][0].update(url="ftp://example.com/x"),
        lambda m: m["entries"].append(dict(m["entries"][0])),  # duplicate id
        lambda m: m["entries"][0].pop("licence"),
        lambda m: m["entries"][0].update(unexpected=1),
    ],
)
def test_invalid_manifest_exits_4(files: Path, tmp_path: Path, mutate) -> None:
    manifest = {"schema_version": 1, "entries": [entry("a", files, "plain.bin")]}
    mutate(manifest)
    path = tmp_path / "m.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    assert run(path, tmp_path / "cache") == 4


def test_unparsable_manifest_exits_4(tmp_path: Path) -> None:
    path = tmp_path / "m.json"
    path.write_text("{", encoding="utf-8")
    assert run(path, tmp_path / "cache") == 4


def test_selection_by_source_tag_and_max_total_bytes(files: Path, tmp_path: Path) -> None:
    with serve(files) as base:
        a = entry("a", files, "plain.bin", tags=["fast"])
        b = entry("b", files, "nested.zip", source="aom", tags=["slow"])
        manifest = write_manifest(tmp_path / "m.json", [a, b], base)
        cache = tmp_path / "cache"
        assert run(manifest, cache, "--tags", "fast") == 0
        assert (cache / "a" / ".ok").exists() and not (cache / "b").exists()
        assert run(manifest, cache, "--sources", "aom") == 0
        assert (cache / "b" / ".ok").exists()
        shutil.rmtree(cache)
        assert run(manifest, cache, "--max-total-bytes", str(a["size_bytes"])) == 0
        assert (cache / "a" / ".ok").exists() and not (cache / "b" / ".ok").exists()
        assert run(manifest, cache, "--ids", "nope") == 4


def test_unverified_and_manual_entries_are_skipped_unless_named(files: Path, tmp_path: Path) -> None:
    pending = {
        "id": "pending",
        "source": "jvt",
        "url": "https://example.invalid/x.zip",
        "unverified": True,
        "licence": "l",
        "licence_spdx": "LicenseRef-X",
        "redistributable": False,
    }
    manifest = write_manifest(tmp_path / "m.json", [pending], "http://127.0.0.1:1/")
    assert run(manifest, tmp_path / "cache") == 0
    assert run(manifest, tmp_path / "cache", "--ids", "pending") == 4


def test_jobs_fetches_in_parallel(files: Path, tmp_path: Path) -> None:
    with serve(files, delay_ms=300) as base:
        entries = [entry(f"p{i}", files, "plain.bin") for i in range(4)]
        manifest = write_manifest(tmp_path / "m.json", entries, base)
        start = time.monotonic()
        assert run(manifest, tmp_path / "cache", "--jobs", "4") == 0
        assert time.monotonic() - start < 1.1  # serial would take >= 1.2 s


def test_seven_zip_detected_by_magic_bytes(files: Path, tmp_path: Path) -> None:
    py7zr = pytest.importorskip("py7zr")
    src = tmp_path / "src"
    src.mkdir()
    (src / "stream.bit").write_bytes(PAYLOAD)
    with py7zr.SevenZipFile(files / "really7z.zip", "w") as zf:  # misleading extension, as on the ITU server
        zf.write(src / "stream.bit", "stream.bit")
    with serve(files) as base:
        e = entry("sz", files, "really7z.zip", extract={"archive": "zip", "member": "stream.bit"})
        manifest = write_manifest(tmp_path / "m.json", [e], base)
        assert run(manifest, tmp_path / "cache") == 0
    assert (tmp_path / "cache" / "sz" / "stream.bit").read_bytes() == PAYLOAD


def test_post_step_without_ffmpeg_marks_entry_unavailable(
    files: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("PATH", str(tmp_path / "empty"))
    with serve(files) as base:
        e = entry("trim", files, "plain.bin", post={"ffmpeg_trim_seconds": 10}, sha256_post="0" * 64)
        manifest = write_manifest(tmp_path / "m.json", [e], base)
        assert run(manifest, tmp_path / "cache") == 0
    assert not (tmp_path / "cache" / "trim" / ".ok").exists()


def test_cache_dir_precedence(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setenv("OVT_CORPORA_DIR", str(tmp_path / "env"))
    assert fetch_corpora.cache_dir_for("x") == Path("x")
    assert fetch_corpora.cache_dir_for(None) == tmp_path / "env"
    monkeypatch.delenv("OVT_CORPORA_DIR")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "xdg"))
    if sys.platform != "darwin":
        assert fetch_corpora.cache_dir_for(None) == tmp_path / "xdg" / "ovt" / "corpora"


def test_repository_manifest_is_valid() -> None:
    manifest = fetch_corpora.load_manifest(REPO / "tests" / "corpora" / "manifest.json")
    sources = {e["source"] for e in manifest["entries"]}
    assert {"jvt", "jctvc", "aom", "mpegff", "gpac", "bento4", "netflix"} <= sources


def test_fixture_server_range_and_corruption(files: Path) -> None:
    with serve(files, corrupt=["corrupt.bin"]) as base:
        req = urllib.request.Request(base + "plain.bin", headers={"Range": "bytes=10-19"})
        with urllib.request.urlopen(req) as resp:
            assert resp.status == 206
            assert resp.read() == PAYLOAD[10:20]
            assert resp.headers["Content-Range"] == f"bytes 10-19/{len(PAYLOAD)}"
        req = urllib.request.Request(base + "plain.bin", headers={"Range": "bytes=-4"})
        with urllib.request.urlopen(req) as resp:
            assert resp.read() == PAYLOAD[-4:]
        with urllib.request.urlopen(base + "corrupt.bin") as resp:
            data = resp.read()
        assert len(data) == len(PAYLOAD) and data != PAYLOAD
        with pytest.raises(urllib.error.HTTPError) as exc:
            urllib.request.urlopen(base + "missing")
        assert exc.value.code == 404


def test_serve_corpus_cli_prints_url(files: Path) -> None:
    proc = subprocess.Popen(
        [sys.executable, "-I", str(REPO / "tests" / "serve_corpus.py"), "--root", str(files), "--port", "0"],
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        assert proc.stdout is not None
        url = proc.stdout.readline().strip()
        assert url.startswith("http://127.0.0.1:")
        with urllib.request.urlopen(url + "plain.bin") as resp:
            assert resp.read() == PAYLOAD
    finally:
        proc.terminate()
        proc.wait(timeout=5)
