#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Diff the budget tables of chapter 14 §14.9.3 against tests/perf/budgets.json (WI-0.560).

    check_budgets.py [--spec docs/spec/14-testing.md] [--budgets tests/perf/budgets.json]

The specification table is the single source of every performance budget and benchmark name (D5/D14);
``budgets.json`` is its machine form. The checker reads the two tables of §14.9.3 (the main table, whose last
column holds the M1 budget, and the Android table of ``pixel-tablet`` budgets), pulls every limit out of the
budget cells (``≤ 2 000 ms``, ``p95 ≤ 16 ms``, ``≥ 200 fps``, ``≤ 2 GiB``, ``≥ 50 000 packets/s``,
``≥ 200 MB/s``, ``≤ 1.5 ms/frame``) and requires that

  * the benchmark names of the spec and of the file are the same set (a row may list several names);
  * for every name the limits for ``m1-air-8gb`` are exactly the spec limits not marked Android, and the limits
    for ``pixel-tablet`` are exactly those of the Android table (``null`` when the benchmark is not run on
    Android);
  * a limit the main table marks for Android (``Android ≤ 16 ms``, ``≤ 1 GiB Android``, ``≤ 4 500 ms
    `pixel-tablet```) is also in the Android table.

Every ``budgets.json`` entry has the keys ``m1-air-8gb`` and ``pixel-tablet`` and, per limit, ``metric``,
``op`` (``<=`` or ``>=``), ``value`` and ``unit``. The metric names are those of tools/perf_compare.py.

Exit codes: 0 identical; 1 a difference (printed one per line); 2 an input file is unreadable or the
section is not found.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

HARDWARE = ("m1-air-8gb", "pixel-tablet")
_NUM = r"\d+(?:[   ]\d{3})*(?:\.\d+)?"
_LIMIT = re.compile(rf"(≤|≥)\s*({_NUM})\s*(ms/frame|ms|fps|packets/s|MB/s|GiB)")
_ANDROID = re.compile(r"Android|pixel-tablet")
_NAME = re.compile(r"`(bench_[A-Za-z0-9_<>]+)`")

Limit = tuple[str, float, str]


def section(text: str) -> list[str]:
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith("### 14.9.3")), None)
    if start is None:
        raise ValueError("section '### 14.9.3' not found")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith(("### ", "## ", "---"))), len(lines))
    return lines[start:end]


def cells(row: str) -> list[str]:
    return [c.strip() for c in row.strip().strip("|").split("|")]


def parse_limits(cell: str) -> tuple[list[Limit], list[Limit]]:
    """(desktop limits, Android-marked limits) of one budget cell."""
    desktop: list[Limit] = []
    android: list[Limit] = []
    for match in _LIMIT.finditer(cell):
        head_start = max(cell.rfind(",", 0, match.start()), cell.rfind(";", 0, match.start())) + 1
        tail_ends = [i for i in (cell.find(",", match.end()), cell.find(";", match.end())) if i >= 0]
        tail_end = min(tail_ends) if tail_ends else len(cell)
        context = cell[head_start : match.start()] + " " + cell[match.end() : tail_end]
        value = float(re.sub(r"[   ]", "", match.group(2)))
        limit: Limit = ("<=" if match.group(1) == "≤" else ">=", value, match.group(3))
        (android if _ANDROID.search(context) else desktop).append(limit)
    return desktop, android


def parse_spec(text: str) -> tuple[dict[str, list[Limit]], dict[str, list[Limit]], dict[str, list[Limit]]]:
    """(M1 limits, Android limits marked in the main table, Android-table limits) by benchmark name."""
    main: dict[str, list[Limit]] = {}
    main_android: dict[str, list[Limit]] = {}
    android_table: dict[str, list[Limit]] = {}
    table: str | None = None
    for line in section(text):
        if not line.startswith("|"):
            table = None if line.strip() == "" else table
            continue
        parts = cells(line)
        if parts[0] == "Benchmark":
            table = "android" if "pixel-tablet" in parts[-1] else "main"
            continue
        if set(parts[0]) <= set("-: ") or table is None:
            continue
        names = [n for n in _NAME.findall(parts[0])]
        desktop, android = parse_limits(parts[-1])
        for name in names:
            if table == "main":
                main[name] = desktop
                main_android[name] = android
            else:
                android_table[name] = desktop + android
    return main, main_android, android_table


def json_limits(name: str, key: str, row: dict, problems: list[str]) -> list[Limit]:
    if key not in row:
        problems.append(f"{name}: key {key} missing in budgets.json (use null when not run there)")
        return []
    limits = row[key]
    if limits is None:
        return []
    out: list[Limit] = []
    for limit in limits:
        if (
            not isinstance(limit, dict)
            or not {"metric", "op", "value", "unit"} <= limit.keys()
            or limit["op"] not in ("<=", ">=")
        ):
            problems.append(f"{name}: {key}: malformed limit {limit!r}")
            continue
        out.append((limit["op"], float(limit["value"]), limit["unit"]))
    return out


def fmt(limits: list[Limit]) -> str:
    return ", ".join(f"{'≤' if op == '<=' else '≥'} {value:g} {unit}" for op, value, unit in sorted(limits)) or "none"


def check(spec_text: str, budgets: dict) -> list[str]:
    main, main_android, android_table = parse_spec(spec_text)
    problems: list[str] = []
    if not main:
        return ["no benchmark rows found in the §14.9.3 main table"]
    rows = budgets.get("benchmarks")
    if not isinstance(rows, dict):
        return ["budgets.json: missing 'benchmarks' object"]
    for name in sorted(set(main) - set(rows)):
        problems.append(f"{name}: row of §14.9.3 missing in budgets.json")
    for name in sorted(set(rows) - set(main)):
        problems.append(f"{name}: in budgets.json but not in §14.9.3")
    for name in sorted(set(android_table) - set(main)):
        problems.append(f"{name}: in the Android table of §14.9.3 but not in the main table")
    for name in sorted(set(main) & set(rows)):
        m1 = json_limits(name, "m1-air-8gb", rows[name], problems)
        px = json_limits(name, "pixel-tablet", rows[name], problems)
        if Counter(m1) != Counter(main[name]):
            problems.append(f"{name}: m1-air-8gb: spec {fmt(main[name])}; budgets.json {fmt(m1)}")
        want_px = android_table.get(name, [])
        if Counter(px) != Counter(want_px):
            problems.append(f"{name}: pixel-tablet: spec {fmt(want_px)}; budgets.json {fmt(px)}")
        missing = Counter(main_android.get(name, [])) - Counter(want_px)
        if missing:
            problems.append(f"{name}: main table marks Android limit {fmt(list(missing))} that the Android table lacks")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    root = Path(__file__).resolve().parents[1]
    parser.add_argument("--spec", type=Path, default=root / "docs/spec/14-testing.md")
    parser.add_argument("--budgets", type=Path, default=root / "tests/perf/budgets.json")
    args = parser.parse_args(argv)
    try:
        spec_text = args.spec.read_text(encoding="utf-8")
        budgets = json.loads(args.budgets.read_text(encoding="utf-8"))
        problems = check(spec_text, budgets)
    except (OSError, ValueError) as exc:
        print(f"check_budgets: {exc}", file=sys.stderr)
        return 2
    for line in problems:
        print(line)
    if problems:
        print(f"check_budgets: {len(problems)} difference(s) between {args.spec} and {args.budgets}")
        return 1
    print(f"check_budgets: {args.budgets} matches §14.9.3 ({len(budgets['benchmarks'])} benchmarks)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
