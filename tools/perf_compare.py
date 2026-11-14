#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Compare benchmark results with the absolute budgets and the stored baseline (chapter 14 §14.9, WI-0.560).

    perf_compare.py RESULTS [BASELINE] [--budgets tests/perf/budgets.json] [--regression-pct 20]
    perf_compare.py --accept RESULTS_DIR [--baselines-dir tests/perf/baselines]

RESULTS is a Google Benchmark JSON file or a directory of them (``results/<hardware-id>/``, written by
tools/perf_run.py). Every file carries ``ovt_meta{hardware_id, os, compiler, commit}``; all files of one
comparison must name the same hardware id, and so must the baseline, because numbers from different machines
are never compared (§14.9.2).

Per benchmark the *median* aggregate is used (the harness runs 5 repetitions with ReportAggregatesOnly); a
file without aggregates falls back to the median of its iteration entries. A benchmark fails when

  * a budget limit of ``budgets.json`` for the hardware is violated: ``budget exceeded``; or
  * the same metric is more than ``--regression-pct`` percent worse than the baseline: ``REGRESSION``. "Worse"
    is slower for a time and lower for a rate, so ``+25.0 %`` always means 25 % worse than the baseline.

Benchmarks without a budget row are checked against the baseline on ``real_time_ms``. A benchmark that has no
baseline entry yet is reported as ``new`` and does not fail. A missing baseline file is a notice, not a failure,
unless ``--require-baseline``.

``--accept`` writes the baseline ``<baselines-dir>/<hardware-id>.json`` from a results directory (§14.9.4:
done by a maintainer in a PR titled ``perf: update baseline <hardware-id>``).

``--json-out FILE`` writes the failures as JSON (``budget`` and ``regression`` lists) for the nightly issue
reporting (labels ``perf-budget`` and ``perf-regression``, §14.9.4).

Metric names: ``real_time_ms`` and ``cpu_time_ms`` (converted from the unit of the file), the standard Google
Benchmark rates ``bytes_per_second`` and ``items_per_second``, and every user counter under its own name.

Exit codes: 0 all within budget and baseline; 1 a budget or a regression failed; 2 usage error or invalid input
(unreadable file, hardware ids that differ).
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from pathlib import Path

_TIME_TO_MS = {"ns": 1e-6, "us": 1e-3, "ms": 1.0, "s": 1e3}
_STANDARD_KEYS = {
    "name",
    "family_index",
    "per_family_instance_index",
    "run_name",
    "run_type",
    "repetitions",
    "repetition_index",
    "threads",
    "iterations",
    "real_time",
    "cpu_time",
    "time_unit",
    "aggregate_name",
    "aggregate_unit",
    "label",
    "error_occurred",
    "error_message",
}
_DEFAULT_METRIC = "real_time_ms"
# Google Benchmark appends the settings registered in code to the run name (bench_x/min_time:2.000/repeats:5).
_SETTINGS_SUFFIX = re.compile(r"/(?:min_time|min_warmup_time|repeats|iterations):[^/]+")


class InputError(Exception):
    """Invalid results, baseline or budgets file (exit 2)."""


def _load_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise InputError(f"{path}: cannot read JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise InputError(f"{path}: top level is not an object")
    return data


def _entry_metrics(entry: dict) -> dict[str, float]:
    scale = _TIME_TO_MS.get(entry.get("time_unit", "ns"))
    if scale is None:
        raise InputError(f"benchmark {entry.get('name')}: unknown time_unit {entry.get('time_unit')!r}")
    metrics: dict[str, float] = {}
    if isinstance(entry.get("real_time"), (int, float)):
        metrics["real_time_ms"] = entry["real_time"] * scale
    if isinstance(entry.get("cpu_time"), (int, float)):
        metrics["cpu_time_ms"] = entry["cpu_time"] * scale
    for key, value in entry.items():
        if (
            key not in _STANDARD_KEYS
            and key not in metrics
            and isinstance(value, (int, float))
            and not isinstance(value, bool)
        ):
            metrics[key] = float(value)
    return metrics


def summarise(doc: dict, source: str) -> dict[str, dict[str, float]]:
    """Medians per benchmark (run_name) of one Google Benchmark document."""
    medians: dict[str, dict[str, float]] = {}
    iterations: dict[str, list[dict[str, float]]] = {}
    for entry in doc.get("benchmarks", []):
        if not isinstance(entry, dict) or "name" not in entry:
            raise InputError(f"{source}: malformed benchmark entry")
        run_name = _SETTINGS_SUFFIX.sub("", str(entry.get("run_name", entry["name"])))
        if entry.get("error_occurred"):
            raise InputError(f"{source}: benchmark {run_name} reported an error: {entry.get('error_message', '')}")
        if entry.get("run_type") == "aggregate":
            if entry.get("aggregate_name") == "median":
                medians[run_name] = _entry_metrics(entry)
        else:
            iterations.setdefault(run_name, []).append(_entry_metrics(entry))
    for run_name, rows in iterations.items():
        if run_name in medians:
            continue
        keys = set().union(*(row.keys() for row in rows))
        medians[run_name] = {k: statistics.median(row[k] for row in rows if k in row) for k in keys}
    return medians


def load_results(path: Path) -> tuple[dict, dict[str, dict[str, float]]]:
    """Returns (ovt_meta, benchmarks) of a results file or directory."""
    if path.is_dir():
        files = sorted(path.glob("*.json"))
        if not files:
            raise InputError(f"{path}: no *.json result files")
    elif path.is_file():
        files = [path]
    else:
        raise InputError(f"{path}: no such file or directory")
    meta: dict = {}
    benchmarks: dict[str, dict[str, float]] = {}
    for file in files:
        doc = _load_json(file)
        file_meta = doc.get("ovt_meta")
        if not isinstance(file_meta, dict) or not file_meta.get("hardware_id"):
            raise InputError(f"{file}: missing ovt_meta.hardware_id")
        if meta and file_meta["hardware_id"] != meta["hardware_id"]:
            raise InputError(
                f"{file}: hardware_id {file_meta['hardware_id']!r} differs from {meta['hardware_id']!r} of the other files"
            )
        meta = meta or file_meta
        for name, metrics in summarise(doc, str(file)).items():
            if name in benchmarks:
                raise InputError(f"{file}: benchmark {name} is also in another result file")
            benchmarks[name] = metrics
    return meta, benchmarks


def load_baseline(path: Path) -> tuple[dict, dict[str, dict[str, float]]]:
    doc = _load_json(path)
    meta = doc.get("ovt_meta")
    benchmarks = doc.get("benchmarks")
    if not isinstance(meta, dict) or not meta.get("hardware_id") or not isinstance(benchmarks, dict):
        raise InputError(f"{path}: not a baseline file (needs ovt_meta.hardware_id and benchmarks)")
    return meta, benchmarks


class Budgets:
    """``tests/perf/budgets.json``: limits per benchmark and budget key (``m1-air-8gb`` or ``pixel-tablet``)."""

    def __init__(self, doc: dict, source: str) -> None:
        self.hardware_keys: dict[str, str] = doc.get("hardware_budget_keys", {})
        self.exact: dict[str, dict] = {}
        self.patterns: list[tuple[re.Pattern[str], dict]] = []
        benchmarks = doc.get("benchmarks")
        if not isinstance(benchmarks, dict):
            raise InputError(f"{source}: missing 'benchmarks' object")
        for name, row in benchmarks.items():
            if "<" in name:  # bench_first_frame_<codec>_<container>
                regex = "[A-Za-z0-9]+".join(re.escape(part) for part in re.split(r"<[A-Za-z0-9_]+>", name))
                self.patterns.append((re.compile(f"^{regex}$"), row))
            else:
                self.exact[name] = row

    def row(self, name: str) -> dict | None:
        if name in self.exact:
            return self.exact[name]
        for pattern, row in self.patterns:
            if pattern.match(name):
                return row
        return None

    def limits(self, name: str, key: str) -> list[dict]:
        row = self.row(name)
        if row is None:
            return []
        return row.get(key) or []


def to_unit(metric: str, unit: str, value: float) -> float:
    """Converts a measured metric to the unit of a limit."""
    if unit == "GiB" and metric.endswith("_bytes"):
        return value / 2**30
    if unit == "MB/s" and metric == "bytes_per_second":
        return value / 1e6
    return value


def _fmt(value: float, unit: str) -> str:
    text = f"{value:.4g}" if abs(value) < 1000 else f"{value:.0f}"
    return f"{text} {unit}"


def check_budgets(benchmarks: dict[str, dict[str, float]], budgets: Budgets, key: str) -> tuple[list[str], int]:
    failures: list[str] = []
    checked = 0
    for name in sorted(benchmarks):
        for limit in budgets.limits(name, key):
            checked += 1
            metric, unit, op, bound = limit["metric"], limit["unit"], limit["op"], limit["value"]
            raw = benchmarks[name].get(metric)
            if raw is None:
                failures.append(
                    f"budget exceeded: {name}: metric {metric} missing from the result (budget {op} {_fmt(bound, unit)})"
                )
                continue
            value = to_unit(metric, unit, raw)
            over = value > bound if op == "<=" else value < bound
            if over:
                sign = ">" if op == "<=" else "<"
                failures.append(
                    f"budget exceeded: {name}: {metric} {_fmt(value, unit)} {sign} {_fmt(bound, unit)} ({key})"
                )
    return failures, checked


def _tracked_metrics(name: str, budgets: Budgets | None, key: str | None) -> list[tuple[str, bool]]:
    """(metric, lower_is_better) pairs checked against the baseline for one benchmark."""
    if budgets is not None and key is not None:
        limits = budgets.limits(name, key)
        if limits:
            return [(limit["metric"], limit["op"] == "<=") for limit in limits]
    return [(_DEFAULT_METRIC, True)]


def check_regressions(
    benchmarks: dict[str, dict[str, float]],
    baseline: dict[str, dict[str, float]],
    budgets: Budgets | None,
    key: str | None,
    pct: float,
) -> tuple[list[str], list[str]]:
    failures: list[str] = []
    notes: list[str] = []
    for name in sorted(benchmarks):
        if name not in baseline:
            notes.append(f"new: {name} has no baseline entry")
            continue
        for metric, lower_is_better in _tracked_metrics(name, budgets, key):
            now, then = benchmarks[name].get(metric), baseline[name].get(metric)
            if now is None or then is None or then == 0:
                notes.append(f"skipped: {name} {metric} (missing in result or baseline)")
                continue
            worse = (now - then) / then * 100.0 if lower_is_better else (then - now) / then * 100.0
            if worse > pct:
                failures.append(
                    f"REGRESSION: {name}: {metric} {worse:+.1f} % (baseline {then:.6g}, now {now:.6g}, limit +{pct:g} %)"
                )
    for name in sorted(set(baseline) - set(benchmarks)):
        notes.append(f"missing: {name} is in the baseline but not in the result")
    return failures, notes


def write_baseline(results: Path, baselines_dir: Path) -> Path:
    meta, benchmarks = load_results(results)
    out = baselines_dir / f"{meta['hardware_id']}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    doc = {"schema": 1, "ovt_meta": meta, "benchmarks": {n: benchmarks[n] for n in sorted(benchmarks)}}
    out.write_text(json.dumps(doc, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("results", nargs="?", type=Path, help="result JSON file or results/<hardware-id>/ directory")
    parser.add_argument("baseline", nargs="?", type=Path, help="baselines/<hardware-id>.json")
    parser.add_argument("--budgets", type=Path, help="tests/perf/budgets.json (absolute limits)")
    parser.add_argument("--budget-key", help="budgets.json column to use (default: from the hardware id)")
    parser.add_argument(
        "--regression-pct", type=float, default=20.0, help="allowed slowdown against the baseline (default 20)"
    )
    parser.add_argument("--require-baseline", action="store_true", help="fail when the baseline file does not exist")
    parser.add_argument("--accept", type=Path, metavar="RESULTS_DIR", help="write the baseline from RESULTS_DIR")
    parser.add_argument("--baselines-dir", type=Path, default=Path("tests/perf/baselines"))
    parser.add_argument("--json-out", type=Path, help="write the failures as JSON")
    args = parser.parse_args(argv)

    try:
        if args.accept is not None:
            out = write_baseline(args.accept, args.baselines_dir)
            print(f"wrote {out}")
            return 0
        if args.results is None:
            parser.error("RESULTS is required (or --accept)")
        meta, benchmarks = load_results(args.results)
        hardware_id = meta["hardware_id"]

        baseline: dict[str, dict[str, float]] | None = None
        if args.baseline is not None:
            if args.baseline.exists():
                base_meta, baseline = load_baseline(args.baseline)
                if base_meta["hardware_id"] != hardware_id:
                    raise InputError(
                        f"hardware_id differs: results {hardware_id!r}, baseline {base_meta['hardware_id']!r}; "
                        "numbers of different machines are never compared"
                    )
            elif args.require_baseline:
                raise InputError(f"{args.baseline}: baseline missing (--require-baseline)")

        budgets: Budgets | None = None
        key: str | None = None
        if args.budgets is not None:
            budgets = Budgets(_load_json(args.budgets), str(args.budgets))
            key = args.budget_key or budgets.hardware_keys.get(hardware_id)
    except InputError as exc:
        print(f"perf_compare: {exc}", file=sys.stderr)
        return 2

    budget_failures: list[str] = []
    regressions: list[str] = []
    notes: list[str] = []
    checked = 0
    if budgets is not None:
        if key is None:
            notes.append(f"no budgets for hardware_id {hardware_id!r}: absolute budgets not checked")
        else:
            budget_failures, checked = check_budgets(benchmarks, budgets, key)
    if args.baseline is not None:
        if baseline is None:
            notes.append(f"no baseline {args.baseline}: regressions not checked")
        else:
            regressions, more = check_regressions(benchmarks, baseline, budgets, key, args.regression_pct)
            notes.extend(more)

    for line in notes:
        print(f"note: {line}")
    for line in budget_failures + regressions:
        print(line)
    print(
        f"perf_compare: {hardware_id}: {len(benchmarks)} benchmarks, {checked} budget limits checked, "
        f"{len(budget_failures)} over budget, {len(regressions)} regressions"
    )
    if args.json_out is not None:
        args.json_out.write_text(
            json.dumps(
                {
                    "hardware_id": hardware_id,
                    "commit": meta.get("commit"),
                    "budget": budget_failures,
                    "regression": regressions,
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    return 1 if budget_failures or regressions else 0


if __name__ == "__main__":
    sys.exit(main())
