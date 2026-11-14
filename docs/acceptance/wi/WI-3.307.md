# WI-3.307 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU Xeon 2.1 GHz), GCC 13.3, system packages plus flatbuffers
25.2.10 and libvmaf 3.0.0 built from source. Date: 2026-11-13. Spec version: 1.0-rc23.

Automated tests (all in `core/tests/metrics/test_metrics_ssim.cpp`, binary `ovt_tests_metrics`, label `metrics`):

| Acceptance test | Result |
|---|---|
| 1. Netflix pair: per-frame `ssim/y` equals libvmaf `float_ssim` within 1e-4 | `ssim matches libvmaf float_ssim on the Netflix pair` ([needs-corpus], golden `golden/ssim_netflix_src01_576x324.json`, copied out of `tests/golden/metrics/netflix_src01_576x324.vmaf_float_v0.6.1.vmaf.json.gz` by `tests/oracles/run_libvmaf_ssim.py netflix`): 48 frames pass, largest difference 5.7e-7 |
| 1b. libvmaf's down-scaling (f = 2, f = 3, odd sizes) and 10-bit input | `ssim matches libvmaf float_ssim on the synthetic down-scaled pairs` (golden `golden/ssim_synth_libvmaf.json`, produced by `run_libvmaf_ssim.py synth` with the libvmaf 3.0.0 CLI; the pairs are generated in the test, no corpus needed) |
| 2. 8-bit pair and its 10-bit ×4 copy with offsets +1 / +4 give equal SSIM within 1e-6 | `ssim of a bit-depth-promoted pair is equal within 1e-6` (Y, U, V; both borders) |
| 3. `bench_metrics_psnr_ssim_1080p` meets its budget on `m1-air-8gb` | **Pending hardware** (`needs-hardware:m1-air-8gb`). No benchmark binary exists yet (WI-0.560 / WI-3.317). The hidden test `[bench]` prints the single-thread number of the same call (below). |

Further cases in the same file: descriptor and registration, option and input rejection, identical pictures give 1 and
an all-1 map, `border = "valid"` against a double-precision reference (8/10/12-bit, 4:2:0, odd sizes, padded strides),
block maps against a brute-force reference, scalar and Highway kernels bit-identical on 100 random inputs.

Informational throughput (not the budget machine) with `ovt_tests_metrics "[bench]"`, 1080p 8-bit 4:2:0, luma only,
one thread:

```console
$ build/linux-x64-release/core/tests/metrics/ovt_tests_metrics "[bench][ssim]"
ssim 1080p luma, border=libvmaf: 146.3 fps (1 thread)
ssim 1080p luma, border=valid: 37.5 fps (1 thread)
$ build/linux-x64-release/core/tests/metrics/ovt_tests_metrics "[bench][psnr]"
psnr 1080p: 3049.8 fps (1 thread)
```

The budget is `fps_4_threads >= 200` for `psnr` + `ssim` (tests/perf/budgets.json); the default border scores the
480 × 270 down-scaled picture (docs/metrics_differences.md).

Determinism: the metric is a pure function of its inputs; `ovt_tests_metrics "[ssim]"` run twice gives identical
output, and the golden comparisons do not depend on the Highway target (scalar and SIMD kernels are bit-identical).
