# Performance runners

How the performance harness of chapter 14 §14.9 is run, and how to register the reference machines of §14.9.2.
Registering a runner is an owner step (it needs a GitHub registration token and an administrator on the machine).
The harness code (WI-0.560) is in place without any runner; nothing below has been done yet.

## What runs where

| Piece | File |
|---|---|
| Benchmarks, `ovt_add_benchmark(<area> <name>)` | `tests/perf/CMakeLists.txt`, binaries in `<build>/tests/perf/<area>/bench_*` |
| Budgets (the §14.9.3 table in machine form) | `tests/perf/budgets.json`, checked against the spec by `tools/check_budgets.py` |
| Run, collect, stamp `ovt_meta` | `tools/perf_run.py` |
| Compare with budgets and baseline | `tools/perf_compare.py` |
| Baselines | `tests/perf/baselines/<hardware-id>.json` (see its README) |
| Nightly | `.github/workflows/perf-nightly.yml` |

A result file is Google Benchmark JSON plus `ovt_meta{hardware_id, os, compiler, commit, date, cpu, cpu_count,
memory_bytes, cpu_scaling}`. Results of different `hardware_id`s are never compared.

## Running on your own machine

Google Benchmark must be installed (`brew install google-benchmark`, `apt install libbenchmark-dev`, or the vcpkg
feature `benchmarks`: `cmake --preset <release preset> -DVCPKG_MANIFEST_FEATURES=benchmarks`). Without it
`tests/perf` is skipped with a status message.

```sh
cmake --preset macos-arm64-release
cmake --build --preset macos-arm64-release --target ovt_perf
python3 -I tools/perf_run.py --build-dir build/macos-arm64-release --compare
```

On an unregistered machine the hardware id is `unknown-<model>`: the run is stored under `results/unknown-<model>/`,
but no budget column or baseline applies, so `--compare` only prints notices. `--min-time 0.5 --repetitions 3` gives
a quick look; the numbers are not authoritative (§14.9.4: only the nightly runs on the reference machines are).

`tools/perf_run.py --list` prints the detected hardware id and the benchmark binaries without running anything.

## Registering a runner

Common steps, as the owner:

1. Make the machine idle and on mains power, with the display asleep (overlay benchmarks excepted). Leave CPU
   frequency scaling at its default; it is recorded in `ovt_meta.cpu_scaling`.
2. GitHub, repository **Settings, Actions, Runners, New self-hosted runner**: follow the page for the platform
   and add the label named below. The runner must run as an unprivileged user and must not be shared with other
   jobs (the Mac Mini's runners are a different machine).
3. Set the repository variable `OVT_PERF_RUNNERS` to the comma-separated list of registered hardware ids, for
   example `m1-air-8gb,linux-x64-ref`. A job of `perf-nightly.yml` whose id is not listed is skipped.
4. Run the workflow once with **Run workflow** (workflow_dispatch). The first run has no baseline: download the
   artefact `perf-<hardware-id>` and follow `tests/perf/baselines/README.md` to commit the baseline.

| Hardware id | Machine | Runner label | Notes |
|---|---|---|---|
| `m1-air-8gb` | MacBook Air M1 2020, 8 GB, macOS 15 | `m1-air-8gb` | Detected automatically (`sysctl hw.model` is `MacBookAir10,1`, 8 GiB). Normative for the §0.7.4 budgets. |
| `linux-x64-ref` | 8-core Ryzen 7 5800X or i7-12700 class, 32 GB, Ubuntu 24.04, Mesa + lavapipe or a discrete GPU | `linux-x64-ref` | Cannot be told from another Linux box: the job sets `OVT_PERF_HARDWARE_ID=linux-x64-ref`. Same budgets as the M1 column. |
| `pixel-tablet` | Google Pixel Tablet, Android 14+ | none: attached by USB to a Linux or macOS host | Android budgets are the `pixel-tablet` column. See below. |
| `chromebook-hdr` | HDR-capable Chromebook (ARCVM) | none: adb over the network or USB | Uses the `pixel-tablet` column; set `OVT_PERF_HARDWARE_ID=chromebook-hdr`. |
| `android-hdr` | HDR-capable Android tablet | none | Acceptance device, not a benchmark device: no budgets. |

## Android devices

The benchmarks are cross-compiled with an `android-*-release` preset, pushed with `adb` to
`/data/local/tmp/ovt-perf`, run through `adb shell`, and the result files are pulled back:

```sh
python3 -I tools/perf_run.py --build-dir build/android-arm64-release --android --serial <serial> --compare
```

The device must be on mains power with the screen on and unlocked for the GPU benchmarks (the script warns when
the device is not charging). `ro.product.model` `Pixel Tablet` becomes `pixel-tablet`; any other device needs
`OVT_PERF_HARDWARE_ID`. The workflow job for the device host is added with the Android build of WI-0.034, because
no Android build exists in CI yet; until then run the command above by hand on the host.

## Failures

`perf_compare.py` exits 1 when a median exceeds its budget (`budget exceeded: ...`) or is more than 20 % worse than
the baseline (`REGRESSION: ... +25.0 % ...`). `--json-out` writes both lists for the issue reporting of §14.9.4
(labels `perf-budget` and `perf-regression`); the nightly job does not open those issues yet, because that needs
`issues: write` on a workflow token, which is an owner decision.
