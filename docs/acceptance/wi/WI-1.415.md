# WI-1.415 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3. Date: 2026-11-16. Spec version: 1.0-rc37.

This container has no Google Benchmark build of `libovt_render` (no vcpkg, no GPU), so here the benchmark source was
compiled for syntax and warnings only (`g++ -std=c++20 -fsyntax-only -Wall -Wextra -Wconversion` against the generated
FlatBuffers header) and `tools/check_budgets.py` was run. Neither acceptance test can pass or fail here: both are
`needs-hardware:m1-air-8gb`.

| Acceptance test | Benchmark | Result |
|---|---|---|
| 1. `bench_render_video_4k` p95 meets its §14.9.3 budget (≤ 8 ms) | `tests/perf/render/bench_render_video.cpp`, counter `p95_ms` | **Pending hardware.** Run: `cmake --build --preset macos-arm64-release --target ovt_perf && tools/perf_run.py --build-dir build/macos-arm64-release --filter '^bench_render_video$' --compare` on the `m1-air-8gb` runner (or a `perf-nightly` dispatch). |
| 2. `bench_render_first_frame` warm ≤ 150 ms and cold ≤ 1500 ms | same file, counters `warm_ms` and `cold_ms` | **Pending hardware.** Same command. |

`tools/check_budgets.py`: `budgets.json matches §14.9.3 (30 benchmarks)`. The budget rows of both benchmarks were already
in `tests/perf/budgets.json` (WI-0.560); this work item adds no numbers.
