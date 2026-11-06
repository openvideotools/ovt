#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Corpora manifest helper (chapter 14 §14.2.4, §14.3.1; WI-0.552).

    manifest_add.py add --id ID --source SRC --url URL [--licence TEXT --spdx ID --redistributable]
                        [--codec C]... [--container C]... [--tag T]... [--extract-member M] [--manifest FILE]
    manifest_add.py fill [--ids A,B] [--sources S,T] [--tags T] [--jobs N] [--manifest FILE]
    manifest_add.py from-git DIR --repo ORG/NAME --source SRC [--glob GLOB]... [--licence-preset NAME] [--manifest FILE]
    manifest_add.py list-dir URL --source SRC --suffix .zip [--prefix ID_PREFIX]

``add`` downloads one URL and upserts an entry with ``sha256`` and ``size_bytes``. ``fill`` downloads every entry
that still carries ``"unverified": true`` and a ``url`` and records the hash (run it on a machine that can reach
the origin; some hosts, e.g. www.itu.int, are not reachable from every CI container). ``from-git`` turns a sparse
git checkout into entries without downloading: Git LFS pointer files already carry the SHA-256 and size of the
real content, other files are hashed from the checkout; the URL is pinned to the commit. ``list-dir`` prints
candidate entries (``unverified``) for the ``.zip`` links of an HTML directory listing (ITU-T ``wftp3``).

Downloads are untrusted data: nothing is executed or extracted; each file is streamed to a temporary file and
hashed. The manifest is rewritten with the same layout as ``tests/corpora/synthetic/generate.py`` uses (indent 2,
key order of the written entry, trailing newline), so a diff only shows the changed entries.

Exit codes: 0 ok; 2 download or hash failure for at least one entry; 3 network failure; 4 manifest invalid; 5 usage.
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import html.parser
import json
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = REPO / "tests" / "corpora" / "manifest.json"
LFS_MAGIC = b"version https://git-lfs.github.com/spec/v1"
ID_BAD = re.compile(r"[^A-Za-z0-9_]+")
CHUNK = 1 << 20

# Licence presets: (licence sentence, SPDX id, redistributable).
PRESETS: dict[str, tuple[str, str, bool]] = {
    "itu": (
        "ITU-T conformance bitstream; free to download; redistribution not permitted — do not mirror",
        "LicenseRef-ITU-Conformance",
        False,
    ),
    "aom": (
        "AOM test data: BSD-2-Clause + AOM Patent License 1.0 (same as libaom)",
        "BSD-2-Clause-Patent",
        True,
    ),
    "mpegff": (
        "BSD-3-Clause-Clear repository; MPEG conformance media, not redistributable outside the repo",
        "LicenseRef-MPEG-Conformance",
        False,
    ),
    "gpac": (
        "GPAC test suite media; per-file notices in media/README",
        "LicenseRef-GPAC-Testsuite-Media",
        False,
    ),
    "bento4": (
        "Bento4 test media, freely downloadable; treated as non-redistributable",
        "LicenseRef-Bento4-Test-Media",
        False,
    ),
    "mkv": (
        "Matroska test files, public test material without a licence file; treated as not redistributable",
        "LicenseRef-Matroska-Test-Files",
        False,
    ),
    "heif-nokia": (
        "Nokia HEIF conformance files: free for testing, not redistributable",
        "LicenseRef-Nokia-HEIF-Conformance",
        False,
    ),
    "avif": ("av1-avif test files: BSD-2-Clause (AOMedia)", "BSD-2-Clause", True),
    "tsduck": ("tsduck-test input files: BSD-2-Clause", "BSD-2-Clause", True),
    "dovi": (
        "dovi_tool test assets: MIT repository; treated as not redistributable (third-party picture data)",
        "LicenseRef-DoviTool-Assets",
        False,
    ),
}


class ManifestError(Exception):
    pass


def load(path: Path) -> dict[str, Any]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ManifestError(f"cannot read {path}: {exc}") from exc
    if not isinstance(doc, dict) or not isinstance(doc.get("entries"), list):
        raise ManifestError(f"{path}: not a corpora manifest")
    return doc


def save(path: Path, doc: dict[str, Any]) -> None:
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def upsert(doc: dict[str, Any], entry: dict[str, Any]) -> None:
    """Replace the entry with the same id in place, else append (keeps manifest order stable)."""
    for index, old in enumerate(doc["entries"]):
        if old["id"] == entry["id"]:
            doc["entries"][index] = entry
            return
    doc["entries"].append(entry)


def make_id(source: str, stem: str, taken: set[str] | None = None) -> str:
    """``<source>_<stem>`` with every run of characters outside ``[A-Za-z0-9_]`` collapsed to ``_`` (§14.3.1)."""
    base = f"{source}_{ID_BAD.sub('_', stem.split('/')[-1]).strip('_')}"[:64]
    if taken is None or base not in taken:
        return base
    n = 2
    while f"{base[: 60 - len(str(n))]}_{n}" in taken:
        n += 1
    return f"{base[: 60 - len(str(n))]}_{n}"


def download_hash(url: str, retries: int = 3) -> tuple[str, int]:
    """Stream ``url`` to a temporary file and return ``(sha256, size)``; raises urllib errors after the retries."""
    last: Exception | None = None
    for _ in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ovt-manifest-add/1"})
            digest, size = hashlib.sha256(), 0
            with (
                urllib.request.urlopen(req, timeout=60) as resp,
                tempfile.TemporaryFile() as tmp,
            ):
                while chunk := resp.read(CHUNK):
                    digest.update(chunk)
                    tmp.write(chunk)
                    size += len(chunk)
            return digest.hexdigest(), size
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            last = exc
    assert last is not None
    raise last


def lfs_pointer(data: bytes) -> tuple[str, int] | None:
    """Return ``(sha256, size)`` when ``data`` is a Git LFS pointer file, else None."""
    if not data.startswith(LFS_MAGIC) or len(data) > 1024:
        return None
    oid = re.search(rb"^oid sha256:([0-9a-f]{64})$", data, re.MULTILINE)
    size = re.search(rb"^size ([0-9]+)$", data, re.MULTILINE)
    return (oid.group(1).decode(), int(size.group(1))) if oid and size else None


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def git_rev(directory: Path) -> str:
    out = subprocess.run(
        ["git", "-C", str(directory), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=True,
    )
    return out.stdout.strip()


def file_url(repo: str, rev: str, rel: str, lfs: bool = False) -> str:
    host = "media.githubusercontent.com/media" if lfs else "raw.githubusercontent.com"
    return f"https://{host}/{repo}/{rev}/{urllib.parse.quote(rel)}"


def entries_from_git(
    directory: Path,
    repo: str,
    source: str,
    globs: list[str],
    preset: str,
    taken: set[str],
    rev: str | None = None,
) -> list[dict[str, Any]]:
    """One entry per tracked file matching a glob (relative to ``directory``), sorted by path."""
    licence, spdx, redistributable = PRESETS[preset]
    rev = rev or git_rev(directory)
    files = sorted(p for p in directory.rglob("*") if p.is_file() and ".git" not in p.relative_to(directory).parts)
    out = []
    for path in files:
        rel = path.relative_to(directory).as_posix()
        if not any(fnmatch.fnmatchcase(rel, g) for g in globs):
            continue
        pointer = lfs_pointer(path.read_bytes()) if path.stat().st_size <= 1024 else None
        sha, size = pointer or (file_sha256(path), path.stat().st_size)
        entry_id = make_id(source, path.stem, taken)
        taken.add(entry_id)
        out.append(
            {
                "id": entry_id,
                "source": source,
                "licence": licence,
                "licence_spdx": spdx,
                "redistributable": redistributable,
                **(
                    {
                        "vcs": {
                            "git": f"https://github.com/{repo}",
                            "lfs_path": rel,
                            "rev": rev,
                        }
                    }
                    if pointer
                    else {"url": file_url(repo, rev, rel, False)}
                ),
                "sha256": sha,
                "size_bytes": size,
                "upstream_path": rel,
            }
        )
    return out


class _Links(html.parser.HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.hrefs: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        if tag == "a":
            self.hrefs.extend(v for k, v in attrs if k == "href" and v)


def parse_listing(page: str, base_url: str, suffix: str) -> list[str]:
    """Absolute URLs of the links in an HTML directory listing that end with ``suffix`` (query strings dropped)."""
    parser = _Links()
    parser.feed(page)
    seen: dict[str, None] = {}
    for href in parser.hrefs:
        full = urllib.parse.urljoin(base_url, href.split("?")[0].split("#")[0])
        if full.lower().endswith(suffix.lower()) and full.startswith(base_url.rsplit("/", 1)[0] + "/"):
            seen[full] = None
    return sorted(seen)


def cmd_add(args: argparse.Namespace) -> int:
    doc = load(args.manifest)
    licence, spdx, redistributable = (
        PRESETS.get(args.licence_preset, ("", "", False)) if args.licence_preset else ("", "", False)
    )
    entry: dict[str, Any] = {
        "id": args.id,
        "source": args.source,
        "licence": args.licence or licence,
        "licence_spdx": args.spdx or spdx,
        "redistributable": args.redistributable or redistributable,
        "url": args.url,
    }
    if not entry["licence"] or not entry["licence_spdx"]:
        print(
            "error: --licence and --spdx (or --licence-preset) are required",
            file=sys.stderr,
        )
        return 5
    try:
        entry["sha256"], entry["size_bytes"] = download_hash(args.url)
    except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
        print(f"error: {args.url}: {exc}", file=sys.stderr)
        return 3
    if args.codec:
        entry["codecs"] = args.codec
    if args.container:
        entry["containers"] = args.container
    if args.extract_member:
        entry["extract"] = {"archive": "auto", "member": args.extract_member}
    if args.tag:
        entry["tags"] = args.tag
    upsert(doc, entry)
    save(args.manifest, doc)
    print(f"added {entry['id']} {entry['sha256']} {entry['size_bytes']}")
    return 0


def cmd_fill(args: argparse.Namespace) -> int:
    doc = load(args.manifest)
    ids = set(filter(None, args.ids.split(",")))
    sources = set(filter(None, args.sources.split(",")))
    tags = set(filter(None, args.tags.split(",")))
    todo = [
        e
        for e in doc["entries"]
        if e.get("unverified")
        and "url" in e
        and (not ids or e["id"] in ids)
        and (not sources or e["source"] in sources)
        and (not tags or tags & set(e.get("tags", [])))
    ]

    def work(entry: dict[str, Any]) -> tuple[str, tuple[str, int] | str]:
        try:
            return entry["id"], download_hash(entry["url"])
        except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
            return entry["id"], str(exc)

    worst = 0
    with ThreadPoolExecutor(max_workers=max(args.jobs, 1)) as pool:
        results = dict(pool.map(work, todo))
    for entry in doc["entries"]:
        result = results.get(entry["id"])
        if isinstance(result, tuple):
            entry["sha256"], entry["size_bytes"] = result
            entry.pop("unverified", None)
            print(f"filled {entry['id']} {result[0]} {result[1]}")
        elif result is not None:
            print(f"failed {entry['id']}: {result}", file=sys.stderr)
            worst = 3
    save(args.manifest, doc)
    print(
        f"{sum(isinstance(r, tuple) for r in results.values())} of {len(todo)} filled",
        file=sys.stderr,
    )
    return worst


def cmd_from_git(args: argparse.Namespace) -> int:
    doc = load(args.manifest)
    taken = {e["id"] for e in doc["entries"]}
    for entry in entries_from_git(
        args.directory,
        args.repo,
        args.source,
        args.glob,
        args.licence_preset,
        taken,
        args.rev,
    ):
        upsert(doc, entry)
        print(f"{entry['id']} {entry['size_bytes']}")
    save(args.manifest, doc)
    return 0


def cmd_list_dir(args: argparse.Namespace) -> int:
    licence, spdx, redistributable = PRESETS[args.licence_preset]
    try:
        with urllib.request.urlopen(
            urllib.request.Request(args.url, headers={"User-Agent": "ovt-manifest-add/1"}),
            timeout=60,
        ) as r:
            page = r.read().decode("utf-8", "replace")
    except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
        print(f"error: {args.url}: {exc}", file=sys.stderr)
        return 3
    for url in parse_listing(page, args.url, args.suffix):
        stem = Path(urllib.parse.urlparse(url).path).stem
        entry = {
            "id": make_id(args.source, stem),
            "source": args.source,
            "licence": licence,
            "licence_spdx": spdx,
            "redistributable": redistributable,
            "url": url,
            "unverified": True,
        }
        print(json.dumps(entry, ensure_ascii=False))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp: argparse.ArgumentParser) -> None:
        sp.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)

    a = sub.add_parser("add")
    common(a)
    a.add_argument("--id", required=True)
    a.add_argument("--source", required=True)
    a.add_argument("--url", required=True)
    a.add_argument("--licence")
    a.add_argument("--spdx")
    a.add_argument("--redistributable", action="store_true")
    a.add_argument("--licence-preset", choices=sorted(PRESETS))
    a.add_argument("--codec", action="append")
    a.add_argument("--container", action="append")
    a.add_argument("--tag", action="append")
    a.add_argument("--extract-member")
    a.set_defaults(func=cmd_add)

    f = sub.add_parser("fill")
    common(f)
    f.add_argument("--ids", default="")
    f.add_argument("--sources", default="")
    f.add_argument("--tags", default="")
    f.add_argument("--jobs", type=int, default=4)
    f.set_defaults(func=cmd_fill)

    g = sub.add_parser("from-git")
    common(g)
    g.add_argument("directory", type=Path)
    g.add_argument("--repo", required=True)
    g.add_argument("--rev")
    g.add_argument("--source", required=True)
    g.add_argument("--glob", action="append", required=True)
    g.add_argument("--licence-preset", required=True, choices=sorted(PRESETS))
    g.set_defaults(func=cmd_from_git)

    ld = sub.add_parser("list-dir")
    ld.add_argument("url")
    ld.add_argument("--source", required=True)
    ld.add_argument("--suffix", default=".zip")
    ld.add_argument("--licence-preset", default="itu", choices=sorted(PRESETS))
    ld.set_defaults(func=cmd_list_dir)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except ManifestError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 4


if __name__ == "__main__":
    sys.exit(main())
