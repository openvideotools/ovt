#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check the headers of third-party decoder patches (chapter 15 §15.5.5, WI-0.607).

    check_patches.py [--max-pending-days 180] [paths...]

Without paths it checks every `*.patch` under `plugins/*/patches/` and `third_party/patches/`. A path may be a
patch file or a directory (searched recursively for `*.patch`). The header is everything before the diff: the
first line that starts with `diff --git`, `--- `, `Index: ` or is exactly `---` (the `git format-patch`
separator before the diffstat) ends it. Every patch must carry, in its header:

    REUSE-IgnoreStart
    From: <author>
    Subject: [PATCH] <summary>
    Upstream-Status: Submitted [<URL>] | Accepted [<version>] | Pending | Inappropriate [<reason>]
    SPDX-License-Identifier: <licence of the patched project>
    Rationale: <why no public API suffices>
    REUSE-IgnoreEnd

A `Pending` patch must also carry `Date:` (`git format-patch` writes the RFC 2822 form; hand-written patches
may use `YYYY-MM-DD`). Days are counted from `Date:` to today (UTC), never from the file's mtime. A `Pending`
patch older than --max-pending-days without an `Issue:` line holding a URL or `#<number>` fails; every other
`Pending` patch is reported as a warning.

Messages, one per finding, on stderr; exit 1 when there is any `E-` finding, else 0:
    E-PATCH-HEADER <file> missing <field>
    E-PATCH-HEADER <file> invalid <field>
    E-PATCH-STALE <file> <days>
    W-PATCH-PENDING <file> <days>

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import datetime as dt
import email.utils
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
# SPEC-ASSUMPTION(#66): `Date:` is required only for `Pending`; the issue link is an `Issue:` line with a URL or
# `#<n>`; a malformed Upstream-Status or Date is `E-PATCH-HEADER <file> invalid <field>`.
REQUIRED = ("From", "Subject", "Upstream-Status", "SPDX-License-Identifier", "Rationale")
FIELD_RE = re.compile(r"^([A-Za-z][A-Za-z0-9-]*):[ \t]*(.*?)\s*$")
# §15.5.5 forms; the bracket around the argument is optional (VERSIONS.md and gen_notices use the bare form).
STATUS_RE = re.compile(r"^(Submitted|Accepted|Inappropriate)\s+(\[\s*\S.*\]|[^\[\s].*)$|^Pending(\s.*)?$")
ISSUE_RE = re.compile(r"https?://\S+|#\d+")
ISO_DATE_RE = re.compile(r"^(\d{4})-(\d{2})-(\d{2})(?:[T ].*)?$")


def header_lines(text: str) -> list[str]:
    lines = []
    for line in text.splitlines():
        if line.startswith(("diff --git", "--- ", "Index: ")) or line.rstrip() == "---":
            break
        lines.append(line)
    return lines


def parse_header(text: str) -> dict[str, str]:
    """First occurrence of each `Field: value` line before the diff; folded lines extend the previous field."""
    fields: dict[str, str] = {}
    last = None
    for line in header_lines(text):
        if last is not None and line[:1] in (" ", "\t") and line.strip():
            if last in fields:
                fields[last] = f"{fields[last]} {line.strip()}".strip()
            continue
        m = FIELD_RE.match(line)
        if m is None:
            last = None
            continue
        name, value = m.group(1), m.group(2)
        key = next((f for f in (*REQUIRED, "Date", "Issue") if f.lower() == name.lower()), name)
        last = key if key not in fields else None
        fields.setdefault(key, value)
    return fields


def parse_date(value: str) -> dt.date | None:
    m = ISO_DATE_RE.match(value.strip())
    try:
        if m:
            return dt.date(int(m.group(1)), int(m.group(2)), int(m.group(3)))
        return email.utils.parsedate_to_datetime(value).astimezone(dt.timezone.utc).date()
    except (TypeError, ValueError, IndexError, OverflowError):
        return None


def check_patch(path: Path, label: str, max_days: int, today: dt.date) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    fields = parse_header(path.read_text(encoding="utf-8", errors="replace"))
    for name in REQUIRED:
        if not fields.get(name):
            errors.append(f"E-PATCH-HEADER {label} missing {name}")
    status = fields.get("Upstream-Status", "")
    if status and not STATUS_RE.match(status):
        errors.append(f"E-PATCH-HEADER {label} invalid Upstream-Status")
    if not status.startswith("Pending") or not STATUS_RE.match(status):
        return errors, warnings
    if not fields.get("Date"):
        errors.append(f"E-PATCH-HEADER {label} missing Date")
        return errors, warnings
    date = parse_date(fields["Date"])
    if date is None:
        errors.append(f"E-PATCH-HEADER {label} invalid Date")
        return errors, warnings
    days = max(0, (today - date).days)
    has_issue = bool(ISSUE_RE.search(fields.get("Issue", "")))
    if days > max_days and not has_issue:
        errors.append(f"E-PATCH-STALE {label} {days}")
    else:
        warnings.append(f"W-PATCH-PENDING {label} {days}")
    return errors, warnings


def default_paths(root: Path) -> list[Path]:
    dirs = sorted(root.glob("plugins/*/patches")) + [root / "third_party" / "patches"]
    return [d for d in dirs if d.is_dir()]


def expand(paths: list[Path]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        out.extend(sorted(p.rglob("*.patch")) if p.is_dir() else [p])
    return out


def display(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return str(path)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    ap.add_argument("paths", nargs="*", type=Path, help="patch files or directories (default: the patch dirs)")
    ap.add_argument("--max-pending-days", type=int, default=180, metavar="N")
    ap.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root (default patch dirs)")
    args = ap.parse_args(argv)
    root = args.root.resolve()
    patches = expand(args.paths or default_paths(root))
    today = dt.datetime.now(dt.timezone.utc).date()
    failed = False
    for patch in patches:
        label = display(patch, root)
        if not patch.is_file():
            print(f"E-PATCH-HEADER {label} missing file", file=sys.stderr)
            failed = True
            continue
        errors, warnings = check_patch(patch, label, args.max_pending_days, today)
        for msg in errors + warnings:
            print(msg, file=sys.stderr)
        failed = failed or bool(errors)
    if not failed:
        print(f"check_patches: {len(patches)} patch(es) OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
