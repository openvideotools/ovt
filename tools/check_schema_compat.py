#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Fail when a FlatBuffers schema change breaks compatibility with the last release tag (WI-0.003).

    check_schema_compat.py [--flatc PATH] [--schema-dir core/schema] [--baseline-ref TAG | --baseline-dir DIR]
                           [file.fbs ...]

Chapter 6 principle P7: within a schema major version nothing is removed, renamed, retyped or renumbered.
For every schema file the script takes the version at the baseline (the last tag, `git describe --tags
--abbrev=0`, unless --baseline-ref or --baseline-dir says otherwise) and runs `flatc --conform <old> <new>`.
flatc reports a removed field only indirectly ("offsets differ for field: <the next field>"), so the script
also compares the field names of every table and struct and names each removed one as `T.field`.
A field marked `deprecated` stays in the schema, so it never counts as removed.

Messages, one per finding; exit 1 when there is any:
    E-REMOVED <file> <Table>.<field>
    E-CONFORM <file> <flatc message>
No tag and no --baseline-dir means nothing to compare: the script says so and exits 0 (before the first
release). A schema that is new since the baseline is skipped. Exit 2 on usage or tool errors.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
_DECL = re.compile(r"\b(table|struct)\s+(\w+)\s*(?:\([^)]*\))?\s*\{([^}]*)\}", re.DOTALL)
_NAMESPACE = re.compile(r"\bnamespace\s+([\w.]+)\s*;")
_FIELD = re.compile(r"^\s*(\w+)\s*:")


def parse_fields(text: str) -> dict[str, set[str]]:
    """Map `Namespace.Table` to its field names (a lightweight scan; flatc does the real parsing)."""
    text = _COMMENT.sub("", text)
    out: dict[str, set[str]] = {}
    namespace = ""
    pos = 0
    events = sorted(
        [(m.start(), "ns", m) for m in _NAMESPACE.finditer(text)]
        + [(m.start(), "decl", m) for m in _DECL.finditer(text)],
        key=lambda e: e[0],
    )
    for pos, kind, m in events:
        if kind == "ns":
            namespace = m.group(1)
            continue
        name = f"{namespace}.{m.group(2)}" if namespace else m.group(2)
        fields = set()
        for stmt in m.group(3).split(";"):
            fm = _FIELD.match(stmt)
            if fm:
                fields.add(fm.group(1))
        out[name] = fields
    return out


def removed_fields(old_text: str, new_text: str) -> list[str]:
    old, new = parse_fields(old_text), parse_fields(new_text)
    found: list[str] = []
    for decl, fields in old.items():
        if decl not in new:
            found.extend(f"{decl}.{f}" for f in sorted(fields))
            continue
        found.extend(f"{decl}.{f}" for f in sorted(fields - new[decl]))
    return found


def git(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], cwd=REPO, text=True, capture_output=True, check=False)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--flatc", default=shutil.which("flatc") or "flatc")
    ap.add_argument("--schema-dir", default="core/schema", help="repository-relative schema directory")
    ap.add_argument("--baseline-ref", help="git tag or commit holding the baseline (default: the last tag)")
    ap.add_argument("--baseline-dir", type=Path, help="directory holding the baseline .fbs files instead of git")
    ap.add_argument("files", nargs="*", type=Path, help="schemas to check (default: every <schema-dir>/*.fbs)")
    args = ap.parse_args(argv)

    schema_dir = (REPO / args.schema_dir).resolve()
    files = [f.resolve() for f in args.files] or sorted(schema_dir.glob("*.fbs"))
    if not files:
        print(f"no schemas under {schema_dir}; nothing to check")
        return 0

    with tempfile.TemporaryDirectory(prefix="ovt-schema-base-") as tmp:
        base = Path(tmp)
        if args.baseline_dir:
            for f in args.baseline_dir.glob("*.fbs"):
                shutil.copy(f, base / f.name)
            label = str(args.baseline_dir)
        else:
            ref = args.baseline_ref
            if not ref:
                r = git("describe", "--tags", "--abbrev=0")
                if r.returncode != 0:
                    print("no release tag yet; nothing to compare against")
                    return 0
                ref = r.stdout.strip()
            listing = git("ls-tree", "--name-only", f"{ref}:{args.schema_dir}")
            if listing.returncode != 0:
                print(f"{ref} has no {args.schema_dir}; nothing to compare against")
                return 0
            for name in listing.stdout.split():
                if name.endswith(".fbs"):
                    show = git("show", f"{ref}:{args.schema_dir}/{name}")
                    if show.returncode != 0:
                        print(f"git show failed for {name}: {show.stderr.strip()}", file=sys.stderr)
                        return 2
                    (base / name).write_text(show.stdout)
            label = ref

        findings = 0
        for new in files:
            old = base / new.name
            if not old.exists():
                print(f"{new.name}: new since {label}, skipped")
                continue
            for field in removed_fields(old.read_text(), new.read_text()):
                print(f"E-REMOVED {new.name} {field}")
                findings += 1
            try:
                r = subprocess.run(
                    [args.flatc, "--conform", str(old), "-I", str(base), "-I", str(new.parent), str(new)],
                    text=True,
                    capture_output=True,
                    check=False,
                )
            except FileNotFoundError:
                print(f"flatc not found: {args.flatc}", file=sys.stderr)
                return 2
            if r.returncode != 0:
                msg = " ".join((r.stdout + r.stderr).split()) or f"flatc exited {r.returncode}"
                print(f"E-CONFORM {new.name} {msg}")
                findings += 1
        if findings:
            print(f"{findings} incompatible change(s) against {label}", file=sys.stderr)
            return 1
        print(f"schemas conform to {label}")
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
