# WI-3.306 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU Xeon 2.1 GHz), GCC 13.3, system packages plus flatbuffers
24.12.23 and libvmaf 3.0.0 built from source. Date: 2026-11-13. Spec version: 1.0.0-rc22.

Automated tests (all in `core/tests/metrics/test_metrics_psnr.cpp`, binary `ovt_tests_metrics`, label `metrics`):

| Acceptance test | Result |
|---|---|
| 1. Netflix pair equals libvmaf `psnr_y/cb/cr` within 1e-6 dB on every frame | `psnr matches libvmaf and FFmpeg on the Netflix pair`: 48 frames pass (corpus `netflix_src01_hrc00/01_576x324`, golden `golden/psnr_netflix_src01_576x324.json`) |
| 2. Identical 10-bit inputs give 72.0 and an all-72.0 map | `psnr identical 10-bit inputs give 72 dB and an all-72 map` |
| 3. Scalar and Highway SSE are bit-identical on 100 random frames | `psnr scalar and Highway SSE are bit-identical on 100 random frames` (8/10/12/16 bit, odd sizes, padded strides, worst-case 16-bit sums) |
| 4. `bench_metrics_psnr_1080p` meets its budget on `m1-air-8gb` | **Pending hardware** (`needs-hardware:m1-air-8gb`). No benchmark binary exists on main yet (WI-0.560). The hidden test `[bench]` prints the single-thread number of the same call. |

Informational throughput (not the budget machine) with `ovt_tests_metrics "[bench]"`, 1080p 8-bit 4:2:0, one thread:

```console
$ build/linux-x64-release/core/tests/metrics/ovt_tests_metrics "[bench]"
psnr 1080p: 3193.3 fps (1 thread)
$ build/linux-x64-debug/core/tests/metrics/ovt_tests_metrics "[bench]"     # unoptimised
psnr 1080p: 134.3 fps (1 thread)
```

The budget is `fps_1_thread >= 600` (tests/perf/budgets.json).

FFmpeg comparison uses the golden's 2-decimal values (tolerance 0.0051 dB). Sequence APSNR, combinations, `psnr_max`
clipping, 4:0:0, `planes` and block maps are covered by the other `[psnr]` cases.
