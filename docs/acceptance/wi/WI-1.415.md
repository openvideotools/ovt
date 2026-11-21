# WI-1.415 acceptance record

Spec version: 1.0-rc38. Date: 2026-11-16.

## Mac Mini run (informational, not the reference machine)

Machine: Mac16,10 Apple M4, macOS 15.8.2, `macos-arm64-release`, MoltenVK, main 33a6641 plus the crop-constructor fix
of this change. Command: `build/macos-arm64-release/tests/perf/render/bench_render_video` (defaults: 4K synthetic
picture, 100 frames, 5 repetitions of 2 s). **Noisy:** the Mini was running CI jobs (load average about 60), and it is
not `m1-air-8gb`, so these numbers are not gated and do not close the acceptance tests.

| Benchmark | Median of 5 repetitions | §14.9.3 budget (`m1-air-8gb`) |
|---|---|---|
| `bench_render_video_4k` | p50 1.63 ms, p95 2.80 ms, p99 4.05 ms per frame (5.7k frames; 169 ms per 100-frame path) | p95 ≤ 8 ms |
| `bench_render_first_frame` | cold 88.7 ms, warm 104.8 ms | warm ≤ 150 ms, cold ≤ 1500 ms |

Both rows carry `gpu_time=null` (timestamp queries are hidden, #304; follow-up #628). Log:
`/mnt/project-files/wi1415-mini-20261116T192519Z.txt`.

| Acceptance test | Benchmark | Status |
|---|---|---|
| 1. `bench_render_video_4k` p95 meets its budget on `m1-air-8gb` | `tests/perf/render/bench_render_video.cpp`, counter `p95_ms` | Pending `m1-air-8gb` (`needs-hardware`). The M4 figure above is well inside the budget even under load. Run: `cmake --build --preset macos-arm64-release --target ovt_perf && tools/perf_run.py --build-dir build/macos-arm64-release --filter '^bench_render_video$' --compare`. |
| 2. `bench_render_first_frame` warm ≤ 150 ms, cold ≤ 1500 ms on `m1-air-8gb` | same file, counters `warm_ms`, `cold_ms` | Pending `m1-air-8gb`; the M4 figures are inside both limits. |

`tools/check_budgets.py`: `budgets.json matches §14.9.3 (30 benchmarks)`. The budget rows already existed (WI-0.560);
this work item adds no numbers.
