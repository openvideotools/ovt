#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Download, verify and cache test corpora (chapter 14 section 14.2.4; WI-0.551).

    fetch_corpora.py [--manifest tests/corpora/manifest.json] [--cache-dir DIR]
                     [--ids ID[,ID...]] [--sources SRC[,SRC...]] [--tags TAG[,TAG...]]
                     [--max-total-bytes N] [--verify-only] [--jobs N] [--retries 3]

Cache layout: ``<cache>/<id>/<member-or-filename>`` plus ``<cache>/<id>/.ok``. ``.ok`` is written only after
SHA-256 verification; it is a JSON record of the archive hash and of the hash of every file kept, so
``--verify-only`` can re-hash without network access. ``<cache>/alias/<name>`` symlinks come from the
``aliases`` array of an entry (section 14.3.5).

Exit codes: 0 all present and verified; 2 hash mismatch; 3 network failure (including a Content-Length
mismatch); 4 manifest invalid or an unsafe archive member. Downloads are untrusted: nothing is executed
except the optional ``post`` steps, which run the oracle ``ffmpeg`` on a file whose hash was already verified.
"""

from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path, PurePosixPath
from typing import Any

EXIT_OK = 0
EXIT_HASH = 2
EXIT_NETWORK = 3
EXIT_MANIFEST = 4

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO_ROOT / "tests" / "corpora" / "manifest.json"
SCHEMA_NAME = "manifest.schema.json"
CHUNK = 1 << 20
OK_NAME = ".ok"


class FetchError(Exception):
    """A failure that maps to an exit code."""

    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


class HashMismatch(FetchError):
    def __init__(self, message: str) -> None:
        super().__init__(EXIT_HASH, message)


class NetworkError(FetchError):
    def __init__(self, message: str) -> None:
        super().__init__(EXIT_NETWORK, message)


class ManifestError(FetchError):
    def __init__(self, message: str) -> None:
        super().__init__(EXIT_MANIFEST, message)


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------------------------- manifest


def load_manifest(path: Path) -> dict[str, Any]:
    """Load and validate a manifest; raise ManifestError (exit 4) on any problem.

    SPEC-ASSUMPTION(#41): the optional ``unverified`` flag marks entries whose hash is not yet recorded.
    """
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}") from exc
    schema_path = REPO_ROOT / "tests" / "corpora" / SCHEMA_NAME
    try:
        import jsonschema
    except ImportError as exc:
        raise ManifestError("python package 'jsonschema' is required (tools/requirements.txt)") from exc
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(manifest), key=lambda e: list(e.absolute_path))
    if errors:
        first = errors[0]
        where = "/".join(str(p) for p in first.absolute_path) or "<root>"
        raise ManifestError(f"manifest invalid at {where}: {first.message}")
    seen: set[str] = set()
    aliases: dict[str, str] = {}
    for entry in manifest["entries"]:
        if entry["id"] in seen:
            raise ManifestError(f"duplicate id {entry['id']}")
        seen.add(entry["id"])
        for alias in entry.get("aliases", []):
            if alias in aliases or alias in (".", ".."):
                raise ManifestError(f"duplicate or invalid alias {alias}")
            aliases[alias] = entry["id"]
        if "extract" in entry:
            safe_member(entry["extract"]["member"])
            if "md5_member" in entry["extract"]:
                safe_member(entry["extract"]["md5_member"])
    return manifest


def entry_url(entry: dict[str, Any]) -> str | None:
    """The download URL of an entry: ``url``, or for a ``vcs`` entry (WI-0.552) the Git LFS media URL.

    SPEC-ASSUMPTION(#41): ``vcs.rev`` (a commit id) is added so the URL is pinned; only github.com origins resolve.
    """
    if "url" in entry:
        return entry["url"]
    vcs = entry.get("vcs")
    if not vcs or "rev" not in vcs:
        return None
    m = re.fullmatch(r"https://github\.com/([^/]+/[^/]+?)(?:\.git)?/?", vcs["git"])
    if m is None:
        return None
    return f"https://media.githubusercontent.com/media/{m.group(1)}/{vcs['rev']}/{urllib.parse.quote(vcs['lfs_path'])}"


def safe_member(name: str) -> str:
    """Return ``name`` if it is a relative path without ``..``; raise exit 4 ``unsafe member name`` otherwise."""
    posix = PurePosixPath(name.replace("\\", "/"))
    if not name or posix.is_absolute() or ".." in posix.parts or (len(name) > 1 and name[1] == ":"):
        raise ManifestError(f"unsafe member name: {name!r}")
    return str(posix)


def select_entries(
    manifest: dict[str, Any], ids: list[str], sources: list[str], tags: list[str]
) -> list[dict[str, Any]]:
    entries = manifest["entries"]
    known = {e["id"] for e in entries}
    for wanted in ids:
        if wanted not in known:
            raise ManifestError(f"unknown id {wanted}")
    out = []
    for entry in entries:
        if ids and entry["id"] not in ids:
            continue
        if sources and entry["source"] not in sources:
            continue
        if tags and not set(tags) & set(entry.get("tags", [])):
            continue
        out.append(entry)
    return out


def cache_dir_for(arg: str | None) -> Path:
    """Cache dir precedence: --cache-dir > $OVT_CORPORA_DIR > <platform cache root>/corpora (D32)."""
    if arg:
        return Path(arg)
    env = os.environ.get("OVT_CORPORA_DIR")
    if env:
        return Path(env)
    if sys.platform == "darwin":
        root = Path.home() / "Library" / "Caches" / "org.openvideotools.ovt"
    else:
        xdg = os.environ.get("XDG_CACHE_HOME")
        root = (Path(xdg) if xdg else Path.home() / ".cache") / "ovt"
    return root / "corpora"


# ------------------------------------------------------------------------------------------- hashing


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        while chunk := fh.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, dest: Path, max_bytes: int | None = None) -> str:
    """Stream ``url`` into ``dest`` and return its SHA-256. Raises NetworkError.

    Stops with NetworkError once more than ``max_bytes`` (the manifest ``size_bytes``) arrived, which caps what a
    hijacked URL can write.
    """
    if url.startswith("gs://"):
        url = "https://storage.googleapis.com/" + url[len("gs://") :]
    request = urllib.request.Request(url, headers={"User-Agent": "ovt-fetch-corpora/1"})
    digest = hashlib.sha256()
    received = 0
    try:
        with urllib.request.urlopen(request, timeout=60) as resp, dest.open("wb") as out:
            expected = resp.headers.get("Content-Length")
            while chunk := resp.read(CHUNK):
                digest.update(chunk)
                out.write(chunk)
                received += len(chunk)
                if max_bytes is not None and received > max_bytes:
                    raise NetworkError(f"{url}: more than the manifest size_bytes ({max_bytes}) received")
    except urllib.error.HTTPError as exc:
        raise NetworkError(f"{url}: HTTP {exc.code}") from exc
    except (urllib.error.URLError, http.client.HTTPException, OSError) as exc:
        raise NetworkError(f"{url}: {exc}") from exc
    if expected is not None and int(expected) != received:
        raise NetworkError(f"{url}: Content-Length {expected} but received {received} bytes")
    return digest.hexdigest()


# ------------------------------------------------------------------------------------------ extraction


def detect_archive(path: Path, declared: str) -> str:
    """Decide the archive type from magic bytes (some ITU .zip files are really 7z)."""
    with path.open("rb") as fh:
        head = fh.read(6)
    if head.startswith(b"7z\xbc\xaf\x27\x1c"):
        kind = "7z"
    elif head.startswith(b"PK"):
        kind = "zip"
    elif head.startswith(b"\x1f\x8b"):
        kind = "tar.gz"
    else:
        raise ManifestError(f"{path.name}: unrecognised archive format (declared {declared})")
    return kind


def extract_member(archive: Path, declared: str, member: str, dest_dir: Path) -> Path:
    """Extract exactly ``member`` into ``dest_dir`` (basename only) and return the written path."""
    member = safe_member(member)
    kind = detect_archive(archive, declared)
    target = dest_dir / PurePosixPath(member).name
    if kind == "zip":
        with zipfile.ZipFile(archive) as zf:
            names = {n.replace("\\", "/"): n for n in zf.namelist()}
            if member not in names:
                raise ManifestError(f"member {member!r} not found in {archive.name}")
            with zf.open(names[member]) as src, target.open("wb") as out:
                shutil.copyfileobj(src, out, CHUNK)
    elif kind == "tar.gz":
        with tarfile.open(archive, "r:gz") as tf:
            try:
                info = tf.getmember(member)
            except KeyError as exc:
                raise ManifestError(f"member {member!r} not found in {archive.name}") from exc
            fobj = tf.extractfile(info) if info.isfile() else None
            if fobj is None:
                raise ManifestError(f"member {member!r} is not a regular file")
            with fobj, target.open("wb") as out:
                shutil.copyfileobj(fobj, out, CHUNK)
    else:
        try:
            import py7zr
        except ImportError as exc:
            raise ManifestError(f"{archive.name} is a 7z archive; install py7zr") from exc
        with py7zr.SevenZipFile(archive) as zf:
            if member not in zf.getnames():
                raise ManifestError(f"member {member!r} not found in {archive.name}")
            zf.extract(path=dest_dir / ".7z", targets=[member])
        shutil.move(str(dest_dir / ".7z" / member), target)
        shutil.rmtree(dest_dir / ".7z", ignore_errors=True)
    return target


def run_post(entry: dict[str, Any], src: Path, dest_dir: Path) -> Path | None:
    """Run the ``post`` steps. Returns the produced file or None when ffmpeg is unavailable."""
    post = entry["post"]
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        log(f"warning: {entry['id']}: ffmpeg not found, post step skipped; entry unavailable")
        return None
    if "ffmpeg_trim_seconds" in post:
        out = dest_dir / (src.stem + ".trim.mp4")
        cmd = [ffmpeg, "-y", "-v", "error", "-i", str(src), "-t", str(post["ffmpeg_trim_seconds"]),
               "-c", "copy", "-map", "0:v:0", str(out)]  # fmt: skip
    else:
        w = post["wrap_y4m"]
        out = dest_dir / (src.stem + ".y4m")
        cmd = [ffmpeg, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", w["pix_fmt"],
               "-s", f"{w['width']}x{w['height']}", "-r", w["fps"], "-i", str(src)]  # fmt: skip
        if "frames" in w:
            cmd += ["-frames:v", str(w["frames"])]
        cmd += ["-f", "yuv4mpegpipe", str(out)]
    subprocess.run(cmd, check=True, timeout=600)
    return out


# ---------------------------------------------------------------------------------------------- cache


def read_ok(entry_dir: Path) -> dict[str, Any] | None:
    try:
        return json.loads((entry_dir / OK_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def verify_entry(entry: dict[str, Any], entry_dir: Path) -> bool:
    """Re-hash the cached files against ``.ok`` (no network). True when everything matches."""
    record = read_ok(entry_dir)
    if record is None or record.get("sha256") != entry.get("sha256"):
        return False
    for name, digest in record.get("files", {}).items():
        path = entry_dir / name
        if not path.is_file() or sha256_file(path) != digest:
            return False
    return True


def write_aliases(entry: dict[str, Any], entry_dir: Path, cache: Path) -> None:
    record = read_ok(entry_dir) or {}
    main = record.get("primary")
    if not main:
        return
    alias_dir = cache / "alias"
    for alias in entry.get("aliases", []):
        alias_dir.mkdir(parents=True, exist_ok=True)
        link = alias_dir / alias
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(os.path.relpath(entry_dir / main, alias_dir))


def fetch_entry(entry: dict[str, Any], cache: Path, retries: int, verify_only: bool) -> str:
    """Fetch one entry. Returns 'ok', 'cached' or 'unavailable'; raises FetchError."""
    entry_dir = cache / entry["id"]
    if verify_entry(entry, entry_dir):
        write_aliases(entry, entry_dir, cache)
        return "cached"
    if verify_only:
        raise HashMismatch(f"{entry['id']}: cache entry missing or does not match its recorded hashes")
    # A stale or damaged entry is rebuilt from scratch.
    shutil.rmtree(entry_dir, ignore_errors=True)
    entry_dir.mkdir(parents=True, exist_ok=True)
    last: FetchError | None = None
    with tempfile.TemporaryDirectory(prefix=".fetch-", dir=cache) as tmp_name:
        tmp = Path(tmp_name)
        for attempt in range(1, max(retries, 1) + 1):
            part = tmp / "download.part"
            try:
                digest = download(entry_url(entry) or "", part, entry["size_bytes"])
                if digest != entry["sha256"]:
                    raise HashMismatch(f"{entry['id']}: sha256 {digest} != expected {entry['sha256']}")
                last = None
                break
            except FetchError as exc:
                last = exc
                log(f"{entry['id']}: attempt {attempt} failed: {exc}")
                part.unlink(missing_ok=True)
                if attempt < retries:
                    time.sleep(min(2 ** (attempt - 1) * 0.5, 8))
        if last is not None:
            shutil.rmtree(entry_dir, ignore_errors=True)
            raise last
        files: dict[str, str] = {}
        if "extract" in entry:
            ex = entry["extract"]
            members = [ex["member"]] + ([ex["md5_member"]] if "md5_member" in ex else [])
            for member in members:
                path = extract_member(part, ex["archive"], member, entry_dir)
                files[path.name] = sha256_file(path)
            primary = PurePosixPath(ex["member"]).name
        else:
            name = PurePosixPath((entry_url(entry) or "").split("?", 1)[0]).name or entry["id"]
            primary = safe_member(name)
            shutil.move(str(part), entry_dir / primary)
            files[primary] = entry["sha256"]
        record: dict[str, Any] = {"sha256": entry["sha256"], "primary": primary, "files": files}
        if "post" in entry:
            produced = run_post(entry, entry_dir / primary, entry_dir)
            if produced is None:
                shutil.rmtree(entry_dir, ignore_errors=True)
                return "unavailable"
            post_digest = sha256_file(produced)
            if "sha256_post" in entry and post_digest != entry["sha256_post"]:
                shutil.rmtree(entry_dir, ignore_errors=True)
                raise HashMismatch(f"{entry['id']}: sha256_post {post_digest} != expected {entry['sha256_post']}")
            record["files"][produced.name] = post_digest
            record["primary"] = produced.name
        (entry_dir / OK_NAME).write_text(json.dumps(record, sort_keys=True), encoding="utf-8")
    write_aliases(entry, entry_dir, cache)
    return "ok"


# ------------------------------------------------------------------------------------------------ main


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Download, verify and cache OVT test corpora.")
    p.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    p.add_argument("--cache-dir")
    p.add_argument("--ids", default="")
    p.add_argument("--sources", default="")
    p.add_argument("--tags", default="")
    p.add_argument("--max-total-bytes", type=int)
    p.add_argument("--verify-only", action="store_true")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--retries", type=int, default=3)
    return p.parse_args(argv)


def split(value: str) -> list[str]:
    return [v for v in value.split(",") if v]


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        ids, sources, tags = split(args.ids), split(args.sources), split(args.tags)
        entries = select_entries(manifest, ids, sources, tags)
        todo = []
        for entry in entries:
            if entry_url(entry) is None or entry.get("unverified") or entry.get("manual_download"):
                if entry["id"] in ids:
                    raise ManifestError(f"{entry['id']}: not fetchable (no url, manual download or unverified hash)")
                log(f"skip {entry['id']}: not fetchable by URL (manual download, vcs or hash pending)")
                continue
            todo.append(entry)
    except ManifestError as exc:
        log(f"error: {exc}")
        return exc.code

    cache = cache_dir_for(args.cache_dir)
    cache.mkdir(parents=True, exist_ok=True)

    budget = args.max_total_bytes
    planned, total = [], 0
    for entry in todo:
        cached = verify_entry(entry, cache / entry["id"])
        need = 0 if cached else entry["size_bytes"]
        if budget is not None and total + need > budget:
            log(f"skip {entry['id']}: {need} bytes would exceed --max-total-bytes {budget}")
            continue
        total += need
        planned.append(entry)

    worst = EXIT_OK

    def work(entry: dict[str, Any]) -> tuple[str, str, int]:
        try:
            return entry["id"], fetch_entry(entry, cache, args.retries, args.verify_only), EXIT_OK
        except FetchError as exc:
            log(f"error: {entry['id']}: {exc}")
            return entry["id"], "failed", exc.code

    with ThreadPoolExecutor(max_workers=max(args.jobs, 1)) as pool:
        for entry_id, status, code in pool.map(work, planned):
            print(f"{status:11} {entry_id}")
            if code == EXIT_MANIFEST:
                worst = EXIT_MANIFEST
            elif code and worst != EXIT_MANIFEST:
                worst = EXIT_HASH if EXIT_HASH in (code, worst) else code
    return worst


if __name__ == "__main__":
    sys.exit(main())
