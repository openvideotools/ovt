#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run the Google Benchmark binaries and collect result files (chapter 14 §14.9.1, WI-0.560).

    perf_run.py --build-dir build/macos-arm64-release [--results-dir results] [--compare]
    perf_run.py --build-dir build/android-arm64-release --android [--serial SERIAL] --compare
    perf_run.py --build-dir DIR --list

For every executable ``<build-dir>/tests/perf/<area>/bench_*`` (built by the ``ovt_perf`` CMake target; see
tests/perf/CMakeLists.txt) it runs the binary with ``--benchmark_format=json
--benchmark_out=<results-dir>/<hardware-id>/<binary>.json``, then adds an ``ovt_meta`` object to the file:

    {"hardware_id", "os", "compiler", "commit", "date", "cpu", "cpu_count", "memory_bytes", "cpu_scaling"}

(Google Benchmark format plus ``ovt_meta``; the CPU summary and the frequency-scaling policy are recorded
because §14.9.4 leaves scaling at the default and documents it in the result file.)

Hardware id (§14.9.2), first match wins: ``--hardware-id``, the environment variable ``OVT_PERF_HARDWARE_ID``,
then detection: macOS ``sysctl hw.model`` (``MacBookAir10,1`` with 8 GiB is ``m1-air-8gb``), Linux
``/sys/devices/virtual/dmi/id/product_name``, Android ``getprop ro.product.model`` (``Pixel Tablet`` is
``pixel-tablet``). A machine that matches nothing gets ``unknown-<model>``: its results are kept, but there is no
baseline or budget key for it. The ids ``linux-x64-ref``, ``android-hdr`` and ``chromebook-hdr`` cannot be told
from other machines of their kind, so a registered runner sets ``OVT_PERF_HARDWARE_ID`` (docs/dev/perf-runners.md).

Android (``--android``): the benchmark binaries are cross-compiled with an android preset, pushed with ``adb`` to
``--device-dir``, run through ``adb shell`` and the result files are pulled back. The device must be on mains
power with the screen on for GPU benchmarks; a device that is not charging gets a warning.

``--min-time SECONDS`` and ``--repetitions N`` reach the binaries through ``OVT_PERF_MIN_TIME`` and
``OVT_PERF_REPETITIONS`` (tests/perf/ovt_perf.hpp), because values registered in code win over the command-line
flags of Google Benchmark. The defaults of the code are 2.0 s and 5 repetitions (§14.9.1).

``--compare`` then runs tools/perf_compare.py against ``<baselines>/<hardware-id>.json`` and ``--budgets``.

Exit codes: 0 done (and, with --compare, within budgets and baseline); 1 the comparison failed; 2 usage or
environment error (no benchmark binaries, adb missing, no device); 3 a benchmark binary failed.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UNKNOWN_PREFIX = "unknown-"
GIB = 1024**3


class RunError(Exception):
    """Usage or environment problem (exit 2)."""


def _capture(argv: list[str]) -> str | None:
    """Stdout of a command, or None when it is missing or fails."""
    try:
        done = subprocess.run(argv, capture_output=True, text=True, check=False, timeout=60)
    except (OSError, subprocess.SubprocessError):
        return None
    return done.stdout.strip() if done.returncode == 0 else None


def _read(path: str) -> str | None:
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "device"


# ---- hardware identification (§14.9.2) ------------------------------------------------------------------------------


def hardware_id_from(system: str, model: str, memory_bytes: int | None) -> str:
    """Maps a platform and its model string to a hardware id of §14.9.2 (or ``unknown-<model>``)."""
    if system == "Darwin" and model == "MacBookAir10,1" and memory_bytes == 8 * GIB:
        return "m1-air-8gb"
    if system == "Android" and model == "Pixel Tablet":
        return "pixel-tablet"
    return UNKNOWN_PREFIX + slug(model or system)


def detect_local(override: str | None = None) -> tuple[str, dict]:
    """(hardware id, machine summary) of the machine running this script."""
    system = platform.system()
    info: dict = {"cpu_count": os.cpu_count()}
    memory: int | None = None
    model = ""
    if system == "Darwin":
        model = _capture(["sysctl", "-n", "hw.model"]) or ""
        mem = _capture(["sysctl", "-n", "hw.memsize"])
        memory = int(mem) if mem and mem.isdigit() else None
        info["cpu"] = _capture(["sysctl", "-n", "machdep.cpu.brand_string"]) or platform.processor()
        info["cpu_scaling"] = "macOS default (not controllable)"
    elif system == "Linux":
        model = _read("/sys/devices/virtual/dmi/id/product_name") or ""
        cpuinfo = _read("/proc/cpuinfo") or ""
        found = re.search(r"^model name\s*:\s*(.+)$", cpuinfo, re.MULTILINE)
        info["cpu"] = found.group(1) if found else platform.processor()
        meminfo = _read("/proc/meminfo") or ""
        found = re.search(r"^MemTotal:\s*(\d+) kB", meminfo, re.MULTILINE)
        memory = int(found.group(1)) * 1024 if found else None
        governor = _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
        info["cpu_scaling"] = f"governor {governor}" if governor else "unknown (no cpufreq)"
    else:
        info["cpu"] = platform.processor()
        info["cpu_scaling"] = "unknown"
    info["memory_bytes"] = memory
    info["model"] = model
    return override or hardware_id_from(system, model, memory), info


def detect_android(adb: list[str]) -> tuple[str, dict]:
    def prop(name: str) -> str:
        return _capture(adb + ["shell", "getprop", name]) or ""

    model = prop("ro.product.model")
    if not model:
        raise RunError("adb: no device answered 'getprop ro.product.model' (is one connected and authorised?)")
    mem = _capture(adb + ["shell", "grep", "MemTotal", "/proc/meminfo"]) or ""
    found = re.search(r"(\d+)\s*kB", mem)
    cpuinfo = _capture(adb + ["shell", "cat", "/proc/cpuinfo"]) or ""
    found_cpu = re.search(r"^(?:Hardware|model name)\s*:\s*(.+)$", cpuinfo, re.MULTILINE)
    memory_bytes = int(found.group(1)) * 1024 if found else None
    info: dict = {
        "model": model,
        "cpu": found_cpu.group(1) if found_cpu else prop("ro.board.platform"),
        "cpu_count": len(re.findall(r"^processor\s*:", cpuinfo, re.MULTILINE)) or None,
        "memory_bytes": memory_bytes,
        "cpu_scaling": "Android default",
        "os": f"Android {prop('ro.build.version.release')} (SDK {prop('ro.build.version.sdk')})",
    }
    return hardware_id_from("Android", model, memory_bytes), info


# ---- metadata ------------------------------------------------------------------------------------------------------


def compiler_of(build_dir: Path) -> str:
    """``<id> <version>`` of the C++ compiler the build directory was configured with."""
    for path in sorted(build_dir.glob("CMakeFiles/*/CMakeCXXCompiler.cmake")):
        text = path.read_text(encoding="utf-8", errors="replace")
        ident = re.search(r'set\(CMAKE_CXX_COMPILER_ID "([^"]*)"\)', text)
        version = re.search(r'set\(CMAKE_CXX_COMPILER_VERSION "([^"]*)"\)', text)
        if ident:
            return f"{ident.group(1)} {version.group(1) if version else ''}".strip()
    return "unknown"


def commit_of(root: Path) -> str:
    return _capture(["git", "-C", str(root), "rev-parse", "HEAD"]) or os.environ.get("GITHUB_SHA", "unknown")


def host_os() -> str:
    if platform.system() == "Darwin":
        return f"macOS {platform.mac_ver()[0]}"
    return f"{platform.system()} {platform.release()}"


def make_meta(hardware_id: str, info: dict, build_dir: Path) -> dict:
    return {
        "hardware_id": hardware_id,
        "os": info.get("os") or host_os(),
        "compiler": compiler_of(build_dir),
        "commit": commit_of(ROOT),
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "cpu": info.get("cpu"),
        "cpu_count": info.get("cpu_count"),
        "memory_bytes": info.get("memory_bytes"),
        "cpu_scaling": info.get("cpu_scaling"),
    }


# ---- running -------------------------------------------------------------------------------------------------------


def find_binaries(build_dir: Path, pattern: str | None) -> list[Path]:
    perf = build_dir / "tests" / "perf"
    found = sorted(
        p
        for p in perf.glob("*/bench_*")
        if p.is_file() and os.access(p, os.X_OK) and p.suffix not in {".json", ".txt", ".a"}
    )
    if pattern:
        rx = re.compile(pattern)
        found = [p for p in found if rx.search(p.name)]
    if not found:
        raise RunError(
            f"no benchmark binaries under {perf} (configure with Google Benchmark installed, then "
            f"'cmake --build {build_dir} --target ovt_perf')"
        )
    return found


def bench_args(out: Path, bench_filter: str | None) -> list[str]:
    args = ["--benchmark_format=json", f"--benchmark_out={out}", "--benchmark_out_format=json"]
    if bench_filter:
        args.append(f"--benchmark_filter={bench_filter}")
    return args


def tuning_env(min_time: float | None, repetitions: int | None) -> dict[str, str]:
    env: dict[str, str] = {}
    if min_time is not None:
        env["OVT_PERF_MIN_TIME"] = f"{min_time:g}"
    if repetitions is not None:
        env["OVT_PERF_REPETITIONS"] = str(repetitions)
    return env


def run_local(binary: Path, out: Path, env_extra: dict[str, str], bench_filter: str | None) -> bool:
    cmd = [str(binary)] + bench_args(out, bench_filter)
    print(f"perf_run: {' '.join(cmd)}", flush=True)
    # The JSON also goes to stdout (--benchmark_format=json); the file is what counts.
    done = subprocess.run(cmd, env={**os.environ, **env_extra}, stdout=subprocess.DEVNULL, check=False)
    return done.returncode == 0 and out.is_file()


def run_android(
    adb: list[str], binary: Path, out: Path, device_dir: str, env_extra: dict[str, str], bench_filter: str | None
) -> bool:
    remote = f"{device_dir}/{binary.name}"
    remote_out = f"{remote}.json"
    steps = [
        adb + ["push", str(binary), remote],
        adb + ["shell", "chmod", "755", remote],
    ]
    env_words = " ".join(f"{k}={v}" for k, v in env_extra.items())
    run_line = f"cd {device_dir} && {env_words} {remote} " + " ".join(bench_args(Path(remote_out), bench_filter))
    steps.append(adb + ["shell", run_line.replace("  ", " ")])
    steps.append(adb + ["pull", remote_out, str(out)])
    for step in steps:
        print(f"perf_run: {' '.join(step)}", flush=True)
        if subprocess.run(step, check=False).returncode != 0:
            return False
    return out.is_file()


def stamp(out: Path, meta: dict) -> None:
    doc = json.loads(out.read_text(encoding="utf-8"))
    doc["ovt_meta"] = meta
    out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument(
        "--build-dir", type=Path, required=True, help="CMake build directory holding tests/perf/<area>/bench_*"
    )
    parser.add_argument("--results-dir", type=Path, default=Path("results"))
    parser.add_argument("--hardware-id", help="override detection (also OVT_PERF_HARDWARE_ID)")
    parser.add_argument("--filter", help="regex on benchmark binary names")
    parser.add_argument("--bench-filter", help="--benchmark_filter passed to the binaries")
    parser.add_argument("--min-time", type=float, help="seconds per benchmark (default 2.0 in the code)")
    parser.add_argument("--repetitions", type=int, help="repetitions per benchmark (default 5 in the code)")
    parser.add_argument("--list", action="store_true", help="print the hardware id and the binaries, run nothing")
    parser.add_argument("--android", action="store_true", help="run on a device through adb")
    parser.add_argument("--adb", default="adb", help="adb executable")
    parser.add_argument("--serial", help="adb device serial")
    parser.add_argument("--device-dir", default="/data/local/tmp/ovt-perf")
    parser.add_argument("--compare", action="store_true", help="run tools/perf_compare.py afterwards")
    parser.add_argument("--baselines", type=Path, default=ROOT / "tests/perf/baselines")
    parser.add_argument("--budgets", type=Path, default=ROOT / "tests/perf/budgets.json")
    parser.add_argument("--regression-pct", type=float, default=20.0)
    args = parser.parse_args(argv)

    try:
        binaries = find_binaries(args.build_dir, args.filter)
        override = args.hardware_id or os.environ.get("OVT_PERF_HARDWARE_ID")
        adb: list[str] = []
        if args.android:
            if shutil.which(args.adb) is None:
                raise RunError(f"{args.adb}: not found (install Android platform-tools)")
            adb = [args.adb] + (["-s", args.serial] if args.serial else [])
            detected, info = detect_android(adb)
            hardware_id = override or detected
            power = _capture(adb + ["shell", "dumpsys", "battery"]) or ""
            if not re.search(r"(AC|USB|Wireless) powered: true", power):
                print("perf_run: warning: the device is not on mains power (§14.9.4 noise control)", file=sys.stderr)
        else:
            hardware_id, info = detect_local(override)
    except RunError as exc:
        print(f"perf_run: {exc}", file=sys.stderr)
        return 2

    if hardware_id.startswith(UNKNOWN_PREFIX):
        print(
            f"perf_run: warning: hardware id {hardware_id!r} is not a reference machine of §14.9.2; "
            "set OVT_PERF_HARDWARE_ID on a registered runner. No budgets or baseline apply.",
            file=sys.stderr,
        )
    print(f"perf_run: hardware_id={hardware_id}")
    if args.list:
        for binary in binaries:
            print(binary)
        return 0

    out_dir = args.results_dir / hardware_id
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in out_dir.glob("*.json"):  # one run per directory: perf_compare reads every file in it
        stale.unlink()
    meta = make_meta(hardware_id, info, args.build_dir)
    env_extra = tuning_env(args.min_time, args.repetitions)
    failed: list[str] = []
    for binary in binaries:
        out = out_dir / f"{binary.name}.json"
        if args.android:
            ok = run_android(adb, binary, out, args.device_dir, env_extra, args.bench_filter)
        else:
            ok = run_local(binary, out, env_extra, args.bench_filter)
        if ok:
            stamp(out, meta)
        else:
            failed.append(binary.name)
    if failed:
        print(f"perf_run: benchmark binaries failed: {', '.join(failed)}", file=sys.stderr)
        return 3

    if args.compare:
        cmd = [
            sys.executable,
            "-I",
            str(Path(__file__).resolve().with_name("perf_compare.py")),
            str(out_dir),
            str(args.baselines / f"{hardware_id}.json"),
            "--budgets",
            str(args.budgets),
            "--regression-pct",
            f"{args.regression_pct:g}",
            "--json-out",
            str(args.results_dir / f"{hardware_id}-failures.json"),
        ]
        return subprocess.run(cmd, check=False).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
