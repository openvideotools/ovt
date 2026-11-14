# WI-3.308a acceptance record

WI-3.308 is split per chapter 16 §16.2.6 because the whole diff exceeded the PR size limit. **Part a** (this record):
steps 1, 2 and 4 for `ssim_x264`, the oracle runner and kernel registration; acceptance tests 1 and 2. **Part b**
(WI-3.308b): the `identity` metric, SHA-256 verdict and `VQ-DIFF-0001` (step 3); acceptance test 3.

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU), GCC 13.3, `linux-x64-debug` configuration with system
packages plus flatbuffers 25.2.10, libvmaf 3.0.0 and json-schema-validator 2.3.0 built from source. Date: 2026-11-14.
Spec version: 1.0-rc24. Spec question: #419 (point 4 and 5 concern this part).

Automated tests (binary `ovt_tests_metrics`, label `metrics`; `core/tests/metrics/test_metrics_ssim_x264.cpp`):

| Acceptance test | Result |
|---|---|
| 1a. Y/U/V equal the x264 kernel oracle within 1e-6, 8-bit integer path | `ssim_x264 equals the x264 kernel oracle on the synthetic pairs` (golden `golden/ssim_x264_synth.json`, written by `tests/oracles/run_x264_ssim.py synth` with the `x264_ssim` executable built from `tests/oracles/x264_ssim`): `8bit_342x226`, `8bit_330x194` pass |
| 1b. same, 10-bit float path | same test, `10bit_330x194`, `10bit_342x226` pass. The corpus has no 10-bit re-encode of `syn_metrics_10bit_8bit_pair` (#419 point 5), so synthetic 10-bit pairs stand in. **This narrows the corpus clip of the acceptance line; flagged for the owner** |
| 1c. Netflix pair, 48 frames | `ssim_x264 equals the x264 kernel oracle on the Netflix pair` ([needs-corpus]; golden `golden/ssim_x264_netflix_src01_576x324.json` written by `run_x264_ssim.py netflix-run` (x264_ssim on x264 r3191 built with `-DOVT_X264_SSIM_DOUBLE_ACCUM=ON`, i.e. accumulating in double as §9.4.4 defines, run on the real pair, nine decimals; the unpatched float build is up to 1.5e-6 away on seven frames, which failed the macOS CI run on b19aacf;  the six-decimal `tests/golden/metrics/netflix_src01_576x324.x264ssim.json.gz` differs from it by at most 5e-7, which is why it could not serve a 1e-6 comparison on macOS); tolerance 1e-6). **Not run in this container** (corpus not downloadable); runs in CI with `--tags fast` |
| 1d. `ssim_db_y == 100` for an identical pair | `ssim_x264 on an identical pair is 1 and ssim_db_y is exactly 100` (8-bit and 10-bit) |
| 2. Mean `ssim_x264/y` of source vs x264 reconstruction equals the log's `SSIM Mean Y` within 1e-4 | Manual (hidden test `[manual]`), below: Netflix reference run on the Mac Mini, and a synthetic clip here |

Further cases: registration in `register_builtin_kernels()`, option and input rejection (a plane with no window),
a per-window double reference at 8, 9, 10 and 12 bits with padded strides and odd sizes, the `planes` option with NaN
for planes not computed, and block maps (mean of the windows whose centre falls in the block).

## Manual record: acceptance test 2

**Netflix reference (Mac Mini, 2026-11-14, commit e6cbb34):** `src01_hrc00_576x324`, pinned x264 0.164.3191,
`--qp 20 --ssim --bframes 0 --dump-yuv`. x264 logged `SSIM Mean Y 0.9841438` over 48 frames; OVT's `ssim_x264/y`
mean of source vs reconstruction matched exactly (hidden test `[manual]`, 101 assertions passed). Run by the Mini
thread, relayed by the coordinator.

Synthetic-clip run in the cloud container (same mechanism):

```console
$ ffmpeg -f lavfi -i "testsrc2=size=352x288:rate=25,format=yuv420p" -frames:v 30 src.y4m
$ ffmpeg -i src.y4m -c:v libx264 -qp 20 -bf 0 -x264-params "ssim=1:threads=1:scenecut=0" -f h264 recon.264
[libx264] SSIM Mean Y:0.9988320 (29.326db)
$ ffmpeg -i recon.264 -pix_fmt yuv420p recon.y4m
$ OVT_AT2_REF=src.y4m OVT_AT2_DIST=recon.y4m OVT_AT2_LOG_MEAN_Y=0.9988320 ovt_tests_metrics "[manual]"
frames 30, ssim_x264/y mean 0.9988320, x264 log 0.9988320
All tests passed (65 assertions in 1 test case)
```

FFmpeg 6.1.1 with libx264, 30 frames, no B-frames. The `x264_ssim` oracle prints the same mean (0.998832). The Netflix
reference run is the Mac Mini record above.

## Informational throughput

`ovt_tests_metrics "[bench][ssim_x264]"`, 1080p 8-bit 4:2:0, one thread, **unoptimised `linux-x64-debug` build**:
luma only 42.6 fps, three planes 27.5 fps. `ssim_x264` has no row in the §14.9.3 budget table.

## Deviation

The 4 × 4 block sums are plain scalar C++, not Highway `SumsOf4` (step 1). No acceptance test or budget depends on it
and the integer sums do not depend on the vector width; a SIMD version is a self-contained follow-up.
