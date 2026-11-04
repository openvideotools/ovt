#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""VERSIONS.md <-> third_party script consistency (chapter 13 §13.3.8 and §13.9.2, WI-0.007).

    check_versions_md.py [--root DIR] [files...]

Fails when a third_party/VERSIONS.md row and the script that consumes it disagree:

  * every third_party/<dep>.cmake script (except _template.cmake) is named in the Build column of a
    VERSIONS.md row;
  * every row whose Build column names an existing third_party/*.cmake script passes
    `tools/versions_to_cmake.py --check` (the script reads OVT_TP_<dep>_URL/_SHA256 and never spells the
    version itself; WI-0.010). Rows whose script does not exist yet are listed as pending, not errors:
    the scripts arrive with their own work items.

File arguments (from pre-commit) are ignored; the check always looks at the whole table. Exit 0 when there
is no VERSIONS.md yet. Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT_RE = re.compile(r"third_party/[A-Za-z0-9_.-]+\.cmake")


def build_column_scripts(versions_md: str) -> dict[str, str]:
    """Script path -> row name, from the Build column (7th) of every table row."""
    out: dict[str, str] = {}
    for line in versions_md.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 8 or cells[0] in ("Name", "") or set(cells[0]) <= set("-:"):
            continue
        for m in SCRIPT_RE.finditer(cells[6]):
            out[m.group(0)] = cells[0]
    return out


def check(root: Path) -> tuple[list[str], list[str]]:
    versions = root / "third_party" / "VERSIONS.md"
    if not versions.exists():
        return [], []
    named = build_column_scripts(versions.read_text(encoding="utf-8"))
    errors, pending = [], []
    for script in sorted((root / "third_party").glob("*.cmake")):
        rel = script.relative_to(root).as_posix()
        if script.name.startswith("_"):
            continue
        if rel not in named:
            errors.append(f"{rel} has no third_party/VERSIONS.md row naming it in the Build column")
    for rel, row in sorted(named.items()):
        if not (root / rel).exists():
            pending.append(f"{rel} ({row})")
    forward = root / "tools" / "versions_to_cmake.py"
    if forward.exists():
        result = subprocess.run(
            [sys.executable, "-I", str(forward), "--versions", str(versions), "--check", "--source-dir", str(root)],
            capture_output=True,
            text=True,
        )
        errors += [ln.removeprefix("error: ") for ln in result.stderr.splitlines() if ln.strip()]
    return errors, pending


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root")
    ap.add_argument("files", nargs="*", help="ignored (pre-commit passes the staged files)")
    args = ap.parse_args(argv)
    errors, pending = check(args.root)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if pending:
        print(f"scripts not written yet ({len(pending)}): {', '.join(pending)}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
