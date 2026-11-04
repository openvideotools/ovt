#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The oracle image's version report and pin check (chapter 14 §14.2.6, WI-0.554).

    oracle-versions                 run every probe of ORACLE_VERSIONS.md, print the table, exit 1 on a mismatch
    oracle_versions.py --check-pins [DIR]
                                    static check of tests/oracles: every pin is a tag, commit or hash
                                    (acceptance test 3: no `latest`, no unpinned fetch), and every pin
                                    value in the scripts appears in ORACLE_VERSIONS.md

The table between the `oracle-versions:begin/end` markers of ORACLE_VERSIONS.md is the single list of
tools. Each row has a `Probe` (a shell command, or `python-package:<dist>`) and an `Expect` substring; the
report prints the first output line that contains it, so build dates and host details never reach the
output and two builds of the same Dockerfile print identical reports.
"""

from __future__ import annotations

import argparse
import importlib.metadata
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

BEGIN = "<!-- oracle-versions:begin -->"
END = "<!-- oracle-versions:end -->"
PIN_ASSIGN = re.compile(r'^\s*(?:ARG\s+)?([A-Z0-9_]+_(?:VERSION|COMMIT|SHA256|REVISION|SNAPSHOT))="?([^"\s]+)"?', re.M)
PLACEHOLDER = re.compile(r"^0+$")
LATEST = re.compile(r"(?<![\w-])latest(?![\w-])|[:@]latest\b", re.I)


@dataclass(frozen=True)
class Row:
    tool: str
    version: str
    probe: str
    expect: str


def _cell(text: str) -> str:
    text = text.strip()
    if text.startswith("`") and text.endswith("`") and len(text) >= 2:
        text = text[1:-1]
    return text.replace("\\|", "|")


def parse_table(markdown: str) -> list[Row]:
    """Rows of the marked table; columns Tool | Version | Source | Licence | Probe | Expect."""
    try:
        body = markdown.split(BEGIN, 1)[1].split(END, 1)[0]
    except IndexError:
        raise ValueError("ORACLE_VERSIONS.md has no oracle-versions:begin/end markers") from None
    rows = []
    for line in body.splitlines():
        if not line.startswith("|") or set(line) <= set("|-: "):
            continue
        cells = [_cell(c) for c in re.split(r"(?<!\\)\|", line.strip().strip("|"))]
        if cells[0] == "Tool":
            continue
        if len(cells) != 6:
            raise ValueError(f"expected 6 columns: {line}")
        rows.append(Row(tool=cells[0], version=cells[1], probe=cells[4], expect=cells[5]))
    if not rows:
        raise ValueError("the oracle-versions table is empty")
    return rows


def run_probe(probe: str) -> str:
    if probe.startswith("python-package:"):
        try:
            return importlib.metadata.version(probe.split(":", 1)[1])
        except importlib.metadata.PackageNotFoundError:
            return ""
    proc = subprocess.run(
        ["bash", "-c", probe], capture_output=True, text=True, errors="replace", timeout=120, check=False
    )
    return proc.stdout + proc.stderr


def reported_line(output: str, expect: str) -> str | None:
    for line in output.splitlines():
        if expect in line:
            return " ".join(line.split())
    return None


def report(rows: list[Row]) -> tuple[str, bool]:
    lines = ["| Tool | Pinned | Reported | Status |", "|---|---|---|---|"]
    ok = True
    for row in rows:
        line = reported_line(run_probe(row.probe), row.expect)
        ok &= line is not None
        lines.append(
            f"| {row.tool} | {row.version} | {line or '(no line contains ' + row.expect + ')'} | "
            f"{'ok' if line else 'MISMATCH'} |"
        )
    return "\n".join(lines) + "\n", ok


def check_pins(oracle_dir: Path) -> list[str]:
    """Problems that would make two builds of the same Dockerfile differ (acceptance test 3)."""
    problems: list[str] = []
    versions_md = (oracle_dir / "ORACLE_VERSIONS.md").read_text(encoding="utf-8")
    sources = [oracle_dir / "Dockerfile", *sorted(oracle_dir.glob("*.sh"))]
    for path in sources:
        text = path.read_text(encoding="utf-8")
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        for name, value in PIN_ASSIGN.findall(code):
            if PLACEHOLDER.match(value):
                problems.append(f"{path.name}: {name} is a placeholder")
            elif value not in versions_md:
                problems.append(f"{path.name}: {name}={value} is not recorded in ORACLE_VERSIONS.md")
        if LATEST.search(code):
            problems.append(f"{path.name}: refers to 'latest'")
        if re.search(r"\bgit\s+clone\b", code):
            problems.append(f"{path.name}: uses git clone; use ovt_git_fetch (commit-checked)")
        for m in re.finditer(r"\bgo\s+install\s+(\S+)", code):
            if not re.search(r"@(v\d|\$\{?[A-Z0-9_]+_VERSION)", m.group(1)):
                problems.append(f"{path.name}: go install {m.group(1)} has no @version")
    dockerfile = (oracle_dir / "Dockerfile").read_text(encoding="utf-8")
    stages = set(re.findall(r"^FROM\s+\S+\s+AS\s+(\S+)", dockerfile, re.M | re.I))
    for image in re.findall(r"^FROM\s+(\S+)", dockerfile, re.M | re.I):
        if image not in stages and "@sha256:" not in image:
            problems.append(f"Dockerfile: base image {image} is not pinned by digest")
    for line in (oracle_dir / "requirements.txt").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith((" ", "#", "-")) and "==" not in line:
            problems.append(f"requirements.txt: {line.split()[0]} is not pinned with ==")
    if "--hash=sha256:" not in (oracle_dir / "requirements.txt").read_text(encoding="utf-8"):
        problems.append("requirements.txt: no --hash pins (regenerate with uv pip compile --generate-hashes)")
    if not (oracle_dir / "package-lock.json").is_file():
        problems.append("package-lock.json is missing (npm ci needs it)")
    return problems


def main(argv: list[str] | None = None) -> int:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check-pins", nargs="?", const=str(here), metavar="DIR")
    parser.add_argument("--table", default=os.environ.get("OVT_ORACLE_VERSIONS", str(here / "ORACLE_VERSIONS.md")))
    args = parser.parse_args(argv)
    if args.check_pins:
        problems = check_pins(Path(args.check_pins))
        for p in problems:
            print(f"error: {p}", file=sys.stderr)
        return 1 if problems else 0
    text, ok = report(parse_table(Path(args.table).read_text(encoding="utf-8")))
    sys.stdout.write(text)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
