#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Apply third-party patches once (chapter 13 §13.3.4 and §13.3.6, WI-0.010).

    apply_patches.py [--check] (--dir DIR | --series FILE) [--source SRC] [--record patches.json]

Runs as the PATCH_COMMAND of a third_party/*.cmake ExternalProject, whose working directory is the
extracted source tree (SRC defaults to the current directory).

  --dir DIR       every DIR/*.patch in lexical order; a missing or empty DIR is a no-op.
  --series FILE   quilt-style list: one patch path per line, relative to FILE's directory; `#` comments.
  --check         dry-run every patch first and abort on the first failure, naming it, before anything is
                  applied. Without it, patches are applied one by one and the first failure aborts.
  --record FILE   write the applied patch names as JSON (`<INSTALL_DIR>/share/ovt/patches.json`), which
                  tools/gen_notices.py lists as "modified; patches: ..." (chapter 15).

Every patch must carry an `Upstream-Status:` header line (OpenEmbedded convention, §13.3.4).

The tool refuses to run twice: after applying it writes SRC/.ovt-patched with the SHA-256 of the series
(names and contents). A second run with the same series is a no-op; a run with a different series fails,
because the tree is no longer pristine.

Patches are checked and applied with `git apply -p1`, with GIT_CEILING_DIRECTORIES set so that a source tree
inside another checkout (build/ under the OVT repository) is not treated as part of it; when git is not
installed, `patch -p1` is used instead.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

MARKER = ".ovt-patched"


class PatchError(Exception):
    pass


def collect(dir_: Path | None, series: Path | None) -> list[Path]:
    if dir_ is not None:
        return sorted(dir_.glob("*.patch")) if dir_.is_dir() else []
    assert series is not None
    if not series.is_file():
        raise PatchError(f"series file {series} does not exist")
    patches = []
    for lineno, line in enumerate(series.read_text(encoding="utf-8").splitlines(), start=1):
        entry = line.split("#", 1)[0].strip()
        if not entry:
            continue
        path = series.parent / entry.split()[0]
        if not path.is_file():
            raise PatchError(f"{series}:{lineno}: patch {entry} does not exist")
        patches.append(path)
    return patches


def upstream_status(patch: Path) -> str:
    for line in patch.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("diff --git") or line.startswith("--- "):
            break
        if line.startswith("Upstream-Status:"):
            return line.split(":", 1)[1].strip()
    raise PatchError(f"{patch.name}: no Upstream-Status: header before the diff (chapter 13 §13.3.4)")


def series_sha256(patches: list[Path]) -> str:
    h = hashlib.sha256()
    for p in patches:
        h.update(p.name.encode() + b"\0" + p.read_bytes() + b"\0")
    return h.hexdigest()


def _run(cmd: list[str], source: Path) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["GIT_CEILING_DIRECTORIES"] = str(source.resolve().parent)
    return subprocess.run(cmd, cwd=source, env=env, capture_output=True, text=True)


def _command(patch: Path, dry_run: bool) -> list[str]:
    if shutil.which("git"):
        return ["git", "apply", "-p1", "--whitespace=nowarn"] + (["--check"] if dry_run else []) + [str(patch)]
    return ["patch", "-p1", "--forward", "--batch", "-s", "-i", str(patch)] + (["--dry-run"] if dry_run else [])


def apply_one(patch: Path, source: Path, dry_run: bool) -> None:
    result = _run(_command(patch, dry_run), source)
    if result.returncode != 0:
        verb = "does not apply" if dry_run else "failed to apply"
        raise PatchError(f"{patch.name} {verb} to {source}:\n{(result.stderr or result.stdout).strip()}")


def record(path: Path, patches: list[Path], digest: str) -> None:
    data = {
        "series_sha256": digest,
        "patches": [{"name": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
                     "upstream_status": upstream_status(p)} for p in patches],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def run(args: argparse.Namespace) -> int:
    source = args.source.resolve()
    patches = collect(args.dir, args.series)
    for p in patches:
        upstream_status(p)
    digest = series_sha256(patches)
    marker = source / MARKER

    if marker.exists():
        previous = marker.read_text(encoding="utf-8").strip()
        if previous != digest:
            raise PatchError(f"{source} was patched with another series (marker {previous}); "
                             "remove the source tree and build again")
        print(f"apply_patches: {source.name} already patched, nothing to do")
    elif patches:
        if args.check:
            for p in patches:
                apply_one(p, source, dry_run=True)
        for p in patches:
            apply_one(p, source, dry_run=False)
            print(f"apply_patches: applied {p.name}")
        marker.write_text(digest + "\n", encoding="utf-8")
    if args.record:
        record(args.record, patches, digest)
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--dir", type=Path, help="directory of NNNN-*.patch files")
    group.add_argument("--series", type=Path, help="quilt-style series file")
    ap.add_argument("--check", action="store_true", help="dry-run every patch before applying any")
    ap.add_argument("--source", type=Path, default=Path.cwd(), help="source tree (default: cwd)")
    ap.add_argument("--record", type=Path, help="write patches.json here")
    args = ap.parse_args(argv)
    try:
        return run(args)
    except (PatchError, OSError) as e:
        print(f"apply_patches: error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
