# Where OVT metrics differ from other implementations

Chapter 9 names an oracle for every own kernel. This page lists the places where OVT's number is *expected* to differ
from a tool that looks like an oracle, with the reason. A difference listed here has no test tolerance; a difference
that is not listed is a bug. Newest work item first.

## `ssim` (WI-3.307, chapter 9 §9.4.3)

### Against libvmaf `float_ssim` (the oracle)

`ssim` with the default `border = "libvmaf"` follows libvmaf 3.0.0 (`libvmaf/src/feature/ssim.c`,
`feature/iqa/ssim_tools.c`, `convolve.c`, `decimate.c`, `float_ssim.c`) and equals it within 1e-4 per frame; on the
Netflix pair the largest difference over 48 frames is 6e-7 (the golden has six decimals). libvmaf's border policy, as
read from the source:

| Step | libvmaf | OVT `border = "libvmaf"` | OVT `border = "valid"` |
|---|---|---|---|
| Planes | luma only | `planes` option, default `["y"]`; chroma uses the same kernel | same |
| Sample scale | 8-bit as is; 10/12/16-bit divided by 4/16/256 (`picture_copy`); bit depths 9, 11, 13–15 are read as 8-bit (a libvmaf bug) | `1 / 2^(bit_depth − 8)` for every depth | `255 / (2^bit_depth − 1)`: Wang's definition with `peak = 2^bd − 1` |
| Constants | `L = 255`, `K1 = 0.01f`, `K2 = 0.03f`, in float | the same, on the scaled samples | the same, on the scaled samples (equal to `C1 = (0.01·peak)²` on code values) |
| Down-scale | `f = max(1, round(min(W, H) / 256))`; `f × f` box average, mirror border, sample positions `x·f`, output `(W/f + (W & 1)) × (H/f + (H & 1))` (`decimate.c`) | the same, including the odd-size term | none |
| Window | 11 taps rounded to six decimals (sum 1.000002) | the same table | exact Gaussian, σ = 1.5, normalised in double |
| Borders of the filter | none: "valid", output `(w − 10) × (h − 10)` of the down-scaled picture | the same | the same, on the whole plane |
| Per-window formula | `l · c · s` with `s = (σxy + C2/2) / (√(σx²σy²) + C2/2)`, a flat window with a slightly negative covariance scores 1 | the same | the same (algebraically Wang's formula) |
| Variances | `E[x²] − μ²` after filtering `x²`, clamped to ≥ 0 | the same | the same |
| Reduction | double sum over windows, divided by the window count, then cast to float | the same, kept in double | the same |

What this means in practice:

* `f` is taken from the size of the plane being scored, so 4:2:0 chroma of a 1080p picture (960 × 540) uses `f = 2`.
* A 1080p picture is scored at 480 × 270 (`f = 4`) with the default border. That is libvmaf's number, and the reason
  the default is cheap. For a full-resolution score use `border = "valid"`.
* The six-decimal window sums to 1.000002 instead of 1. Because `E[x²] − μ²` cancels, a noise-like picture moves by up
  to 1.5e-4 between the two windows (test `ssim libvmaf border below the down-scale threshold equals valid within the
  tap table error`). On the Netflix pair the two windows differ by at most 2e-5.
* `border = "valid"` is not a libvmaf oracle; it is tested against a double-precision reference (5e-5).
* The 10-bit pair of `ssim_synth_libvmaf.json` matches libvmaf 3.0.0 within 1e-4; an 8-bit and a 10-bit (×4) copy of
  one pair score equally within 1e-6 (WI-3.307 AT2).

### Against FFmpeg's `ssim` filter (expected to differ, no tolerance)

FFmpeg's `ssim` uses 8 × 8 blocks stepping by 4 with a uniform window and no Gaussian, over all three planes
weighted by plane size. Its numbers are higher than the Gaussian SSIM on noisy content and equal to OVT's `ssim_x264`
only up to x264's integer rounding. Do not compare the two.

### Against the x264 / x265 `--ssim` log

That is `ssim_x264` (below), a different kernel (4 × 4 sums, 2-sample offset, integer constants). Its value is
normally a few thousandths away from `ssim`.

### Against Wang's `ssim.m`

`ssim.m` also down-scales by `f = max(1, round(min(M, N) / 256))` before filtering, then filters with MATLAB's
`filter2(..., 'valid')`, so it matches OVT's `border = "libvmaf"` up to the six-decimal window. `border = "valid"` is
`ssim.m` without the down-scale.

### Determinism

The five moment planes are float32 sums in a fixed tap order (a multiply, then an add; no fused multiply-add: the two
kernel files are built with `-ffp-contract=off`), the per-row window values are float32, the frame score is a double
sum in raster order. The scalar and the Highway kernels return the same bits whatever the vector width, so a score does
not depend on the CPU it ran on.

## `ssim_x264` (WI-3.308a, chapter 9 §9.4.4)

`ssim_x264` is the SSIM that x264 and x265 print with `--ssim`: 4 × 4 integer sums over the plane cut by two samples
at the top and left to `(W − 2) × (H − 2)`, 8 × 8 windows stepping by 4, `(W'/4 − 1)·(H'/4 − 1)` windows per plane.
It is written from the algorithm of §9.4.4; x264's own code exists only in `tests/oracles/x264_ssim` (GPL, oracle).

### Against the x264 kernel oracle

The oracle is x264 r3191's `ssim_end1` / `ssim_end4` / `x264_pixel_ssim_wxh`. Differences, all below the 1e-6 of the
acceptance test:

* x264 turns its integer factors into `float` before multiplying and adds the windows of a row in `float`, then adds
  the rows in `float`. OVT forms the products from exact integers (8-bit and 9-bit branch) or from the same `float`
  operations in the same order (10-bit and deeper), divides in `double` and sums the windows in `double`.
* The plane score is `sum / count` in double; the oracle prints `float(sum) / float(count)` with nine digits.
* On the synthetic pairs of `core/tests/metrics/golden/ssim_x264_synth.json` (8-bit and 10-bit, 330 × 194 and
  342 × 226) the largest difference is below 1e-7 on every plane.

Bit depths above 10 use the float branch with `peak = 2^b − 1` (x264 has no such build; 12-bit input agrees with an
exact double evaluation within 1e-4 per plane). A plane with fewer than 2 × 2 blocks after the offset has no window
and is rejected (`OVT_E_INVALID_ARG`); x264's caller would print 1.0.

### Against the `SSIM Mean Y` line of an x264 log

`x264 --ssim --qp 20 --bframes 0` prints the luma mean over the frames it encoded. With no B-frames the encoder's
reconstruction is the decoded picture, and `ssim_x264/y` of the source against the decoded stream reproduces the line
(the run recorded in `docs/acceptance/wi/WI-3.308a.md` agrees to the seven printed digits). With B-frames x264 scores
non-reference pictures before deblocking and the numbers differ (issue #306).

### Against FFmpeg's `ssim` filter and libvpx

Do not compare: FFmpeg's `ssim` has no 2-sample offset and different constants, libvpx's `tiny_ssim` uses `64²`
constants without the offset and weights the planes `0.8/0.1/0.1`.
