# WI-3.590 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU, AVX2 and AVX-512), GCC 13.3, system packages (Highway 1.0.7)
plus flatbuffers 25.2.10 and libvmaf 3.0.0 built from source. Build: `linux-x64-debug`. Date: 2026-11-15. Spec
version: 1.0-rc34.

Corpora present locally: `netflix_src01_hrc00_576x324`, `netflix_src01_hrc01_576x324` (`tools/fetch_corpora.py`).
The `syn_metrics_1080p_pair` and `syn_metrics_pq_1080p_pair` entries are generated (recipes 25 and 58) and were not
available here; their test cases skip and run where the nightly generates the synthetic streams. For
`syn_metrics_1080p` the test decodes the first 60 pictures of `syn_metrics_1080p_dist.mp4` with the ffmpeg CLI into a
temporary Y4M (the command of `gen_metrics.py` `to_y4m()`), so no fetcher change is needed; that path was exercised
locally on a stand-in entry (an x264 encode of the Netflix reference), where the decode, the float64 recomputation and
the per-frame loop ran and only the golden values differed, as expected for stand-in content.

| Acceptance test | Test (binary `ovt_tests_metrics`) | Local result |
|---|---|---|
| 1. PSNR equals FFmpeg's `%0.2f` after rounding, float64 recomputation within 1e-6 dB, `inf` → `psnr_max` | `psnr golden suite: FFmpeg psnr on the Netflix pair` / `… on syn_metrics_1080p` / `… on the 10-bit syn_metrics_pq_1080p`; `psnr golden suite: FFmpeg's printed value and the inf rule` | Netflix: 48 frames, all planes, psnr_avg and every mse, 1029 assertions pass. Rule test passes. syn pairs skipped (corpus). |
| 2. `ssim_x264` within 1e-6 of x264_ssim (8-bit integer, 10-bit float); `ssim` within 1e-4 of libvmaf `float_ssim` | `ssim_x264 golden suite: x264 kernel oracle on …` (three pairs); `ssim golden suite: libvmaf float_ssim on …` (three pairs) | Netflix: 48 frames pass for both (Y/U/V for ssim_x264). syn pairs skipped (corpus). |
| 3. psnr maps with `map_block = 16` conserve MSE × area within 1e-9 relative | `psnr maps conserve MSE x area over the block grid`, `psnr maps conserve MSE x area on the Netflix pair` | Pass (synthetic 8/10/12-bit incl. odd sizes; Netflix 48 frames). Tolerance split per spec question #566. |
| 4. Integer outputs bit-identical across x86_64 / arm64 / scalar; float `ssim` within 1e-4 relative | `xarch: psnr, ssim_x264 and ssim agree with the x86_64 AVX2 golden on every target` (golden `core/tests/metrics/golden/xarch_kernels.json`) | Pass on every target this CPU supports (AVX2, AVX3, AVX3_DL / ZEN4 as generated) and the scalar reference. arm64 runs in the macOS CI job. |
| 5. `bench_metrics_psnr_ssim_1080p` meets its budget on `m1-air-8gb` with 4 threads | `tests/perf/metrics/bench_metrics.cpp` | **Pending hardware** (`needs-hardware:m1-air-8gb`) and pending the benchmark harness of WI-0.560b, which adds `tests/perf/CMakeLists.txt`. |

Determinism: `OVT_UPDATE_GOLDEN=1 ovt_tests_metrics "[xarch]"` run twice rewrites `xarch_kernels.json` with identical
bytes.

The bench binary was compiled with the warning flags of the build and linked against the module objects by hand
(with the `tests/perf/ovt_perf.hpp` of the WI-0.560b patch), then run with `OVT_PERF_MIN_TIME=0.5
OVT_PERF_REPETITIONS=1` on the synthetic fallback input. Unoptimised build, so the numbers only show that the rows and
counters work:

```console
bench_metrics_psnr_1080p/.../real_time        fps_1_thread=108.9/s          synthetic 1080p
bench_metrics_psnr_ssim_1080p/.../real_time   fps_4_threads=49.5/s          synthetic 1080p
bench_metrics_maps/.../real_time              overhead_ms_per_frame=41.7    synthetic 1080p
```

The same binary from the `linux-x64-release` build (optimised), same container and input; informational, this is not
the budget machine:

```console
bench_metrics_psnr_1080p/.../real_time        fps_1_thread=1315.8/s         budget on m1-air-8gb: >= 600
bench_metrics_psnr_ssim_1080p/.../real_time   fps_4_threads=473.9/s         budget on m1-air-8gb: >= 200
bench_metrics_maps/.../real_time              overhead_ms_per_frame=2.66    budget on m1-air-8gb: <= 1.5
```

The maps overhead is above its budget on this machine. It is the cost of the WI-3.306/3.307 map code (psnr maps on
three planes, the ssim window accumulator), not of this WI; the `m1-air-8gb` run will show whether it holds there.
