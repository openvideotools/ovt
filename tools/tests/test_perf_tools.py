# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/perf_compare.py, tools/check_budgets.py and tools/perf_run.py (chapter 14 §14.9, WI-0.560)."""

from __future__ import annotations

import copy
import importlib.util
import json
import os
import stat
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
COMPARE = REPO / "tools" / "perf_compare.py"
CHECK = REPO / "tools" / "check_budgets.py"
RUN = REPO / "tools" / "perf_run.py"
BUDGETS = REPO / "tests" / "perf" / "budgets.json"
SPEC = REPO / "docs" / "spec" / "14-testing.md"


def load(path: Path):
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[path.stem] = module
    spec.loader.exec_module(module)
    return module


run_mod = load(RUN)


def script(path: Path, *args: str | Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-I", str(path), *map(str, args)], capture_output=True, text=True, check=False
    )


def gb_result(hardware_id: str, medians: dict[str, dict], unit: str = "ms") -> dict:
    """A Google Benchmark document with median aggregates (settings suffix in the run name, like the real thing)."""
    rows = []
    for name, fields in medians.items():
        row = {
            "name": f"{name}/min_time:2.000/repeats:5_median",
            "run_name": f"{name}/min_time:2.000/repeats:5",
            "run_type": "aggregate",
            "aggregate_name": "median",
            "time_unit": unit,
            "real_time": fields.pop("real_time", 1.0),
            "cpu_time": 1.0,
        }
        row.update(fields)
        rows.append(row)
        rows.append({**row, "aggregate_name": "mean", "name": f"{name}_mean", "real_time": 999.0})  # must be ignored
    return {
        "context": {},
        "ovt_meta": {"hardware_id": hardware_id, "os": "test", "compiler": "gcc", "commit": "abc"},
        "benchmarks": rows,
    }


def write(path: Path, doc: dict) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")
    return path


def baseline_from(tmp_path: Path, hardware_id: str, medians: dict[str, dict]) -> Path:
    write(tmp_path / "base" / "r.json", gb_result(hardware_id, copy.deepcopy(medians)))
    out = tmp_path / "baselines"
    assert script(COMPARE, "--accept", tmp_path / "base", "--baselines-dir", out).returncode == 0
    return out / f"{hardware_id}.json"


# ---- perf_compare.py: acceptance tests 1 and 2 ---------------------------------------------------------------------


def test_regression_of_25_percent_fails_and_lists_the_benchmark(tmp_path: Path) -> None:
    baseline = baseline_from(tmp_path, "m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 10.0}})
    results = write(
        tmp_path / "res" / "m1-air-8gb" / "r.json",
        gb_result("m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 12.5}}),
    )
    done = script(COMPARE, results.parent, baseline, "--budgets", BUDGETS, "--regression-pct", "20")
    assert done.returncode == 1, done.stdout + done.stderr
    assert "bench_analysis_cached_frame" in done.stdout
    assert "+25.0 %" in done.stdout


def test_regression_within_limit_passes(tmp_path: Path) -> None:
    baseline = baseline_from(tmp_path, "m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 10.0}})
    results = write(
        tmp_path / "res" / "r.json", gb_result("m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 11.9}})
    )
    done = script(COMPARE, results, baseline, "--budgets", BUDGETS)
    assert done.returncode == 0, done.stdout
    assert "REGRESSION" not in done.stdout


def test_over_budget_fails_with_budget_exceeded(tmp_path: Path) -> None:
    results = write(tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 31.0}}))
    done = script(COMPARE, results, "--budgets", BUDGETS)
    assert done.returncode == 1
    assert "budget exceeded" in done.stdout
    assert "bench_analysis_cached_frame" in done.stdout


def test_exactly_at_budget_passes(tmp_path: Path) -> None:
    results = write(tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 30.0}}))
    assert script(COMPARE, results, "--budgets", BUDGETS).returncode == 0


def test_seconds_are_converted_to_milliseconds(tmp_path: Path) -> None:
    results = write(
        tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 0.05}}, unit="s")
    )
    done = script(COMPARE, results, "--budgets", BUDGETS)
    assert done.returncode == 1 and "50 ms" in done.stdout  # 0.05 s = 50 ms > 30 ms


def test_rate_budget_is_a_minimum(tmp_path: Path) -> None:
    low = write(tmp_path / "low.json", gb_result("m1-air-8gb", {"bench_metrics_vmaf_1080p": {"fps": 59.0}}))
    high = write(tmp_path / "high.json", gb_result("m1-air-8gb", {"bench_metrics_vmaf_1080p": {"fps": 61.0}}))
    assert script(COMPARE, low, "--budgets", BUDGETS).returncode == 1
    assert script(COMPARE, high, "--budgets", BUDGETS).returncode == 0


def test_rate_regression_counts_a_drop_as_worse(tmp_path: Path) -> None:
    baseline = baseline_from(tmp_path, "m1-air-8gb", {"bench_metrics_vmaf_1080p": {"fps": 100.0}})
    results = write(tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_metrics_vmaf_1080p": {"fps": 75.0}}))
    done = script(COMPARE, results, baseline, "--budgets", BUDGETS)
    assert done.returncode == 1 and "+25.0 %" in done.stdout


def test_bytes_and_unit_conversion(tmp_path: Path) -> None:
    ok = write(
        tmp_path / "ok.json", gb_result("m1-air-8gb", {"bench_memory_4k_stream": {"peak_rss_bytes": 1.9 * 2**30}})
    )
    bad = write(
        tmp_path / "bad.json", gb_result("m1-air-8gb", {"bench_memory_4k_stream": {"peak_rss_bytes": 2.1 * 2**30}})
    )
    assert script(COMPARE, ok, "--budgets", BUDGETS).returncode == 0
    assert script(COMPARE, bad, "--budgets", BUDGETS).returncode == 1
    rate = write(tmp_path / "rate.json", gb_result("m1-air-8gb", {"bench_cache_write": {"bytes_per_second": 190e6}}))
    assert script(COMPARE, rate, "--budgets", BUDGETS).returncode == 1  # 190 MB/s < 200 MB/s


def test_templated_budget_names_match_concrete_benchmarks(tmp_path: Path) -> None:
    results = write(
        tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_first_frame_h264_mp4": {"hw_ms": 400.0, "sw_ms": 900.0}})
    )
    done = script(COMPARE, results, "--budgets", BUDGETS)
    assert done.returncode == 1
    assert "sw_ms" in done.stdout and "hw_ms" not in done.stdout


def test_android_hardware_uses_the_pixel_tablet_column(tmp_path: Path) -> None:
    results = write(tmp_path / "r.json", gb_result("pixel-tablet", {"bench_gui_cold_start": {"real_time": 2500.0}}))
    assert script(COMPARE, results, "--budgets", BUDGETS).returncode == 0  # 2 500 ms <= 3 000 ms (M1: 1 500 ms)
    m1 = write(tmp_path / "m1.json", gb_result("m1-air-8gb", {"bench_gui_cold_start": {"real_time": 2500.0}}))
    assert script(COMPARE, m1, "--budgets", BUDGETS).returncode == 1


def test_benchmark_not_run_on_android_has_no_android_budget(tmp_path: Path) -> None:
    results = write(
        tmp_path / "r.json", gb_result("pixel-tablet", {"bench_analysis_cached_frame": {"real_time": 500.0}})
    )
    done = script(COMPARE, results, "--budgets", BUDGETS)
    assert done.returncode == 0 and "0 budget limits checked" in done.stdout


def test_new_benchmark_and_missing_baseline_do_not_fail(tmp_path: Path) -> None:
    baseline = baseline_from(tmp_path, "m1-air-8gb", {"bench_old": {"real_time": 1.0}})
    results = write(tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_new": {"real_time": 1.0}}))
    done = script(COMPARE, results, baseline)
    assert done.returncode == 0 and "new: bench_new" in done.stdout and "missing: bench_old" in done.stdout
    assert script(COMPARE, results, tmp_path / "none.json").returncode == 0
    assert script(COMPARE, results, tmp_path / "none.json", "--require-baseline").returncode == 2


def test_hardware_ids_are_never_mixed(tmp_path: Path) -> None:
    baseline = baseline_from(tmp_path, "linux-x64-ref", {"bench_x": {"real_time": 1.0}})
    results = write(tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_x": {"real_time": 1.0}}))
    done = script(COMPARE, results, baseline)
    assert done.returncode == 2 and "hardware_id differs" in done.stderr
    write(tmp_path / "dir" / "a.json", gb_result("m1-air-8gb", {"bench_a": {}}))
    write(tmp_path / "dir" / "b.json", gb_result("linux-x64-ref", {"bench_b": {}}))
    assert script(COMPARE, tmp_path / "dir").returncode == 2


def test_input_errors_exit_2(tmp_path: Path) -> None:
    assert script(COMPARE, tmp_path / "missing").returncode == 2
    bad = tmp_path / "bad.json"
    bad.write_text("{not json", encoding="utf-8")
    assert script(COMPARE, bad).returncode == 2
    no_meta = write(tmp_path / "nometa.json", {"benchmarks": []})
    assert script(COMPARE, no_meta).returncode == 2


def test_iteration_entries_without_aggregates_use_their_median(tmp_path: Path) -> None:
    doc = gb_result("m1-air-8gb", {})
    doc["benchmarks"] = [
        {
            "name": "bench_x",
            "run_name": "bench_x",
            "run_type": "iteration",
            "time_unit": "ms",
            "real_time": v,
            "cpu_time": v,
        }
        for v in (1.0, 9.0, 2.0)
    ]
    compare = load(COMPARE)
    meta, benchmarks = compare.load_results(write(tmp_path / "r.json", doc))
    assert benchmarks["bench_x"]["real_time_ms"] == 2.0 and meta["hardware_id"] == "m1-air-8gb"


def test_json_out_lists_failures(tmp_path: Path) -> None:
    results = write(tmp_path / "r.json", gb_result("m1-air-8gb", {"bench_analysis_cached_frame": {"real_time": 99.0}}))
    out = tmp_path / "failures.json"
    assert script(COMPARE, results, "--budgets", BUDGETS, "--json-out", out).returncode == 1
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["hardware_id"] == "m1-air-8gb" and len(doc["budget"]) == 1 and doc["regression"] == []


# ---- check_budgets.py: acceptance test 4 -----------------------------------------------------------------------------


def test_committed_budgets_match_the_spec_tables() -> None:
    done = script(CHECK)
    assert done.returncode == 0, done.stdout + done.stderr


def test_every_row_of_the_main_table_is_in_budgets_json() -> None:
    check = load(CHECK)
    main, _, android = check.parse_spec(SPEC.read_text(encoding="utf-8"))
    names = set(json.loads(BUDGETS.read_text(encoding="utf-8"))["benchmarks"])
    assert names == set(main)
    assert len(main) >= 25  # the table has 30 benchmark names; a parser that finds a handful has lost rows
    assert set(android) <= set(main)
    assert main["bench_index_h264_1h_1080p"] == [("<=", 2000.0, "ms")]  # "≤ 2 000 ms": the thousands separator
    assert main["bench_memory_4k_stream"] == [("<=", 2.0, "GiB")]
    assert android["bench_memory_4k_stream"] == [("<=", 1.0, "GiB")]
    assert main["bench_render_video_4k"] == [("<=", 8.0, "ms")] and android["bench_render_video_4k"] == [
        ("<=", 16.0, "ms")
    ]
    assert main["bench_demux_packets_1h"] == [(">=", 50000.0, "packets/s")]


@pytest.mark.parametrize(
    "mutate, expected",
    [
        (
            lambda b: b["benchmarks"]["bench_analysis_cached_frame"]["m1-air-8gb"][0].update(value=31),
            "bench_analysis_cached_frame: m1-air-8gb",
        ),
        (lambda b: b["benchmarks"].pop("bench_validator"), "bench_validator: row of §14.9.3 missing"),
        (
            lambda b: b["benchmarks"].update(bench_extra={"m1-air-8gb": [], "pixel-tablet": None}),
            "bench_extra: in budgets.json but not in §14.9.3",
        ),
        (lambda b: b["benchmarks"]["bench_gui_scrub"].update({"pixel-tablet": None}), "bench_gui_scrub: pixel-tablet"),
        (lambda b: b["benchmarks"]["bench_gui_scrub"].pop("pixel-tablet"), "key pixel-tablet missing"),
    ],
)
def test_check_budgets_reports_differences(tmp_path: Path, mutate, expected: str) -> None:
    doc = json.loads(BUDGETS.read_text(encoding="utf-8"))
    mutate(doc)
    changed = write(tmp_path / "budgets.json", doc)
    done = script(CHECK, "--budgets", changed)
    assert done.returncode == 1 and expected in done.stdout


def test_check_budgets_detects_a_changed_spec_number(tmp_path: Path) -> None:
    text = SPEC.read_text(encoding="utf-8")
    assert "≤ 2 000 ms" in text
    spec = tmp_path / "spec.md"
    spec.write_text(text.replace("≤ 2 000 ms", "≤ 2 100 ms", 1), encoding="utf-8")
    done = script(CHECK, "--spec", spec)
    assert done.returncode == 1 and "bench_index_h264_1h_1080p" in done.stdout


def test_check_budgets_input_errors(tmp_path: Path) -> None:
    assert script(CHECK, "--spec", tmp_path / "none.md").returncode == 2
    empty = tmp_path / "empty.md"
    empty.write_text("# nothing\n", encoding="utf-8")
    assert script(CHECK, "--spec", empty).returncode == 2


# ---- perf_run.py ------------------------------------------------------------------------------------------------------


def test_hardware_id_mapping() -> None:
    f = run_mod.hardware_id_from
    assert f("Darwin", "MacBookAir10,1", 8 * 1024**3) == "m1-air-8gb"
    assert f("Darwin", "MacBookAir10,1", 16 * 1024**3) == "unknown-macbookair10-1"
    assert f("Android", "Pixel Tablet", None) == "pixel-tablet"
    assert f("Linux", "OptiPlex 7090", None) == "unknown-optiplex-7090"
    assert f("Linux", "", None) == "unknown-linux"


def fake_benchmark(path: Path, body: str = "") -> Path:
    """A shell script that writes a Google Benchmark file to --benchmark_out, like a real benchmark binary."""
    path.parent.mkdir(parents=True, exist_ok=True)
    doc = json.dumps(gb_result("ignored", {"bench_fake": {"real_time": 5.0}}))
    path.write_text(
        '#!/bin/sh\nfor a in "$@"; do case "$a" in --benchmark_out=*) out="${a#--benchmark_out=}";; esac; done\n'
        f'{body}\nprintf \'%s\' \'{doc}\' > "$out"\necho "min=$OVT_PERF_MIN_TIME reps=$OVT_PERF_REPETITIONS" > "$out.env"\n',
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


@pytest.mark.skipif(os.name == "nt", reason="uses a shell script as the benchmark binary")
def test_perf_run_collects_results_with_meta_and_compares(tmp_path: Path) -> None:
    build = tmp_path / "build"
    fake_benchmark(build / "tests" / "perf" / "core" / "bench_fake")
    results = tmp_path / "results"
    done = script(
        RUN, "--build-dir", build, "--results-dir", results, "--hardware-id", "m1-air-8gb", "--min-time", "0.5",
        "--repetitions", "2", "--baselines", tmp_path / "baselines", "--budgets", BUDGETS, "--compare",
    )  # fmt: skip
    assert done.returncode == 0, done.stdout + done.stderr
    out = results / "m1-air-8gb" / "bench_fake.json"
    meta = json.loads(out.read_text(encoding="utf-8"))["ovt_meta"]
    assert meta["hardware_id"] == "m1-air-8gb"  # replaced the "ignored" id the fake wrote
    assert {"os", "compiler", "commit", "date"} <= meta.keys()
    assert (results / "m1-air-8gb" / "bench_fake.json.env").read_text(encoding="utf-8").strip() == "min=0.5 reps=2"
    assert "no baseline" in done.stdout


@pytest.mark.skipif(os.name == "nt", reason="uses a shell script as the benchmark binary")
def test_perf_run_failures(tmp_path: Path) -> None:
    empty = tmp_path / "empty"
    empty.mkdir()
    assert script(RUN, "--build-dir", empty, "--hardware-id", "x").returncode == 2
    build = tmp_path / "build"
    fake_benchmark(build / "tests" / "perf" / "core" / "bench_bad", body="exit 7")
    done = script(RUN, "--build-dir", build, "--results-dir", tmp_path / "r", "--hardware-id", "x")
    assert done.returncode == 3 and "bench_bad" in done.stderr
    listed = script(RUN, "--build-dir", build, "--hardware-id", "x", "--list")
    assert listed.returncode == 0 and "bench_bad" in listed.stdout


@pytest.mark.skipif(os.name == "nt", reason="uses shell scripts as adb")
def test_perf_run_android_pushes_runs_and_pulls(tmp_path: Path) -> None:
    build = tmp_path / "build"
    (build / "tests" / "perf" / "core").mkdir(parents=True)
    binary = build / "tests" / "perf" / "core" / "bench_fake"
    binary.write_text("not run on the host", encoding="utf-8")
    binary.chmod(0o755)
    device = tmp_path / "device"
    device.mkdir()
    doc = json.dumps(gb_result("ignored", {"bench_fake": {"real_time": 5.0}}))
    adb = tmp_path / "adb"
    adb.write_text(
        "#!/bin/sh\n"
        f"echo \"$@\" >> '{tmp_path}/adb.log'\n"
        'case "$1" in\n'
        f'  push) cp "$2" \'{device}/\'$(basename "$3");;\n'
        f"  pull) printf '%s' '{doc}' > \"$3\";;\n"
        '  shell) case "$2" in\n'
        "    getprop) case \"$3\" in ro.product.model) echo 'Pixel Tablet';; ro.build.version.release) echo 14;; ro.build.version.sdk) echo 34;; esac;;\n"
        "    grep) echo 'MemTotal: 7900000 kB';;\n"
        "    dumpsys) echo 'AC powered: true';;\n"
        "  esac;;\n"
        "esac\n",
        encoding="utf-8",
    )
    adb.chmod(0o755)
    done = script(
        RUN, "--build-dir", build, "--results-dir", tmp_path / "r", "--android", "--adb", adb, "--min-time", "0.1"
    )
    assert done.returncode == 0, done.stdout + done.stderr
    meta = json.loads((tmp_path / "r" / "pixel-tablet" / "bench_fake.json").read_text(encoding="utf-8"))["ovt_meta"]
    assert meta["hardware_id"] == "pixel-tablet" and meta["os"].startswith("Android 14")
    log = (tmp_path / "adb.log").read_text(encoding="utf-8")
    assert "push" in log and "/data/local/tmp/ovt-perf/bench_fake" in log and "OVT_PERF_MIN_TIME=0.1" in log
    assert "--benchmark_out=/data/local/tmp/ovt-perf/bench_fake.json" in log and "pull" in log
