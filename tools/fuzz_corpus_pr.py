#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Carry the nightly fuzz lane's new corpus entries into a pull request (chapter 14 §14.7.2; WI-0.033).

    fuzz_corpus_pr.py stage --out DIR [--corpus core/tests/fuzz/corpus]
    fuzz_corpus_pr.py apply --from DIR [--corpus core/tests/fuzz/corpus] [--summary FILE]
    fuzz_corpus_pr.py pr    --summary FILE [--date YYYY-MM-DD] [--dry-run]

`stage` runs in a fuzz shard after `ovt_fuzz_<t> -merge=1 core/tests/fuzz/corpus/<t> corpus_cache/<t>` has
copied the inputs that add coverage into the checkout. It copies the untracked files of the corpus tree to
DIR/<target>/<name>, within the limits below. `apply` runs in the job that opens the pull request: it copies
the staged files of every shard into the corpus tree, again within the limits, and writes a Markdown summary.
`pr` commits the change on a new branch (`git commit -s`), pushes it and opens the pull request with `gh`
(GH_TOKEN in the environment); with --dry-run it only prints what it would do.

Limits (the second is §14.7.2's; the others keep a review small and the 60 s smoke run reaching the parsers):
a file is at most 64 KiB, a target's corpus directory at most 2 MiB after the change and a run adds at most
100 files per target. Smaller files are taken first; the rest are dropped and listed. Only targets that
already have a corpus directory are touched, and only plain 1..255-character file names without a leading
dot are accepted (libFuzzer names its units by SHA-1).

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import re
import shutil
import subprocess
import sys
from pathlib import Path

MAX_FILE_BYTES = 64 * 1024
MAX_TARGET_BYTES = 2 * 1024 * 1024
MAX_FILES_PER_TARGET = 100
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$")
DEFAULT_CORPUS = "core/tests/fuzz/corpus"


def dir_bytes(path: Path) -> int:
    return sum(f.stat().st_size for f in path.iterdir() if f.is_file())


def select(candidates: dict[str, int], existing_bytes: int) -> tuple[list[str], dict[str, str]]:
    """Choose the files to take from {name: size}; returns (kept, {dropped name: reason}), kept sorted."""
    kept: list[str] = []
    dropped: dict[str, str] = {}
    total = existing_bytes
    for name, size in sorted(candidates.items(), key=lambda kv: (kv[1], kv[0])):
        if not NAME_RE.match(name):
            dropped[name] = "unsafe name"
        elif size == 0:
            dropped[name] = "empty"
        elif size > MAX_FILE_BYTES:
            dropped[name] = f"{size} bytes > {MAX_FILE_BYTES}"
        elif len(kept) >= MAX_FILES_PER_TARGET:
            dropped[name] = f"more than {MAX_FILES_PER_TARGET} files per target"
        elif total + size > MAX_TARGET_BYTES:
            dropped[name] = f"target would exceed {MAX_TARGET_BYTES} bytes"
        else:
            kept.append(name)
            total += size
    return sorted(kept), dropped


def untracked(corpus: Path) -> dict[str, list[str]]:
    out = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z", "--", str(corpus)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    found: dict[str, list[str]] = {}
    for rel in filter(None, out.split("\0")):
        parts = Path(rel).relative_to(corpus).parts
        if len(parts) == 2:  # <target>/<file>; deeper trees are not corpus entries
            found.setdefault(parts[0], []).append(parts[1])
    return found


def copy_selected(
    src_dir: Path, dst_dir: Path, names: dict[str, int], existing: int
) -> tuple[list[str], dict[str, str]]:
    kept, dropped = select(names, existing)
    dst_dir.mkdir(parents=True, exist_ok=True)
    for name in kept:
        shutil.copyfile(src_dir / name, dst_dir / name)
    return kept, dropped


def cmd_stage(args: argparse.Namespace) -> int:
    corpus, out = Path(args.corpus), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    for target, files in sorted(untracked(corpus).items()):
        sizes = {n: (corpus / target / n).stat().st_size for n in files}
        kept, dropped = copy_selected(corpus / target, out / target, sizes, dir_bytes_committed(corpus / target, files))
        print(f"{target}: staged {len(kept)}, dropped {len(dropped)}")
    return 0


def dir_bytes_committed(target_dir: Path, new_files: list[str]) -> int:
    skip = set(new_files)
    return sum(f.stat().st_size for f in target_dir.iterdir() if f.is_file() and f.name not in skip)


def cmd_apply(args: argparse.Namespace) -> int:
    corpus, src = Path(args.corpus), Path(args.src)
    lines = ["# Nightly fuzz corpus update", ""]
    added = 0
    for tdir in sorted(p for p in src.iterdir() if p.is_dir()) if src.is_dir() else []:
        target = tdir.name
        dest = corpus / target
        if not dest.is_dir():
            lines.append(f"- `{target}`: skipped, no corpus directory")
            continue
        names = {f.name: f.stat().st_size for f in tdir.iterdir() if f.is_file() and not (dest / f.name).exists()}
        kept, dropped = copy_selected(tdir, dest, names, dir_bytes(dest))
        added += len(kept)
        lines.append(f"- `{target}`: {len(kept)} new, {dir_bytes(dest)} bytes in total")
        lines.extend(f"  - dropped `{n}`: {why}" for n, why in sorted(dropped.items()))
    lines += ["", f"{added} new corpus entries."]
    text = "\n".join(lines) + "\n"
    if args.summary:
        Path(args.summary).write_text(text)
    print(text, end="")
    return 0 if added else 3  # 3: nothing new, the workflow stops here without an error


def run(cmd: list[str], dry: bool) -> None:
    print("+ " + " ".join(cmd))
    if not dry:
        subprocess.run(cmd, check=True)


def cmd_pr(args: argparse.Namespace) -> int:
    date = args.date or datetime.datetime.now(datetime.timezone.utc).date().isoformat()
    branch, title = f"fuzz/corpus-{date}", f"WI-0.033: add nightly fuzz corpus entries ({date})"
    body = (
        "Risk: Low. Binary fuzz seeds only; the `fuzz-smoke` and `fuzz-regress` runs replay them.\n"
        "Review focus: nothing to review by hand; CI replays every entry.\n\n"
    ) + Path(args.summary).read_text()
    if (
        subprocess.run(
            ["git", "status", "--porcelain", "--", DEFAULT_CORPUS], check=True, capture_output=True, text=True
        ).stdout
        == ""
    ):
        print("nothing to commit")
        return 0
    dry = args.dry_run
    run(["git", "checkout", "-b", branch], dry)
    run(["git", "add", "--", DEFAULT_CORPUS], dry)
    run(
        [
            "git",
            "commit",
            "-s",
            "-m",
            title,
            "-m",
            "Found by the nightly fuzz lane (`-merge=1` over the CI corpus cache).",
        ],
        dry,
    )
    run(["git", "push", "origin", branch], dry)
    run(["gh", "pr", "create", "--base", "main", "--head", branch, "--title", title, "--body", body], dry)
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("--out", required=True)
    s.add_argument("--corpus", default=DEFAULT_CORPUS)
    a = sub.add_parser("apply")
    a.add_argument("--from", dest="src", required=True)
    a.add_argument("--corpus", default=DEFAULT_CORPUS)
    a.add_argument("--summary")
    p = sub.add_parser("pr")
    p.add_argument("--summary", required=True)
    p.add_argument("--date")
    p.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    return {"stage": cmd_stage, "apply": cmd_apply, "pr": cmd_pr}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
