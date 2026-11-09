# Performance runners

How the performance harness of chapter 14 §14.9 is run, and how to register the reference machines of §14.9.2.
Registering a runner is an owner step (it needs a GitHub registration token and an administrator on the machine).
The harness code (WI-0.560) is in place without any runner; nothing below has been done yet. The CMake helper, the
`bench_smoke` benchmark and `.github/workflows/perf-nightly.yml` arrive with part b (WI-0.560b); the paths below that
belong to it are marked.

## What runs where

| Piece | File |
|---|---|
| Benchmarks, `ovt_add_benchmark(<area> <name>)` (part b) | `tests/perf/CMakeLists.txt`, binaries in `<build>/tests/perf/<area>/bench_*` |
| Budgets (the §14.9.3 table in machine form) | `tests/perf/budgets.json`, checked against the spec by `tools/check_budgets.py` |
| Run, collect, stamp `ovt_meta` | `tools/perf_run.py` |
| Compare with budgets and baseline | `tools/perf_compare.py` |
| Baselines | `tests/perf/baselines/<hardware-id>.json` (see its README) |
| Nightly (part b) | `.github/workflows/perf-nightly.yml` |

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

## Security: this repository is public

A self-hosted runner executes whatever job is routed to it, and in a public repository any workflow, including one
added by a pull request from a fork, can name `runs-on: [self-hosted, ...]`. A runner that is not locked down is a
remote-code-execution hole into the machine and its network. Before registering any runner, as the owner:

- Keep **Settings, Actions, General, Fork pull request workflows, "Require approval for all external
  contributors"** on, and never approve a fork run that touches `.github/`.
- Put the runners in their own **runner group** (organisation settings, Actions, Runner groups) that is restricted
  to this repository and, where the plan allows it, to the workflow `perf-nightly.yml` on `main`
  (the group's "Selected workflows" setting). Do not leave them in the Default group.
- Run the runner as a **dedicated unprivileged user**, preferably **ephemeral** (`config.sh --ephemeral`, one job
  per registration, or a fresh VM or container per job), on a machine that holds **no stored credentials**: no SSH
  keys, cloud or registry tokens, browser sessions, password managers or signing material, and no route to other
  machines on the network that it does not need. Do not reuse the Mac Mini build host.
- `perf-nightly.yml` runs on `schedule` and `workflow_dispatch` only, with `contents: read`; keep it that way and
  never add a `pull_request` trigger or a write-scoped token to a job that runs on these machines.

## Registering a runner

Common steps, as the owner (after the security measures above):

1. Make the machine idle and on mains power, with the display asleep (overlay benchmarks excepted). Leave CPU
   frequency scaling at its default; it is recorded in `ovt_meta.cpu_scaling`.
2. GitHub, organisation **Settings, Actions, Runners, New runner, New self-hosted runner** (runner groups exist only
   at organisation level, so register here and not under the repository): follow the page for the platform, choose
   the runner group created above and add the label named below. The runner is not shared with other jobs.
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
