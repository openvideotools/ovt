# ADR-011

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 78, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Metrics layer is a plugin system** (`ovt_metric_v1` descriptor + `score` function). Engines: own SIMD kernels (PSNR, SSIM Gaussian 11×11 and x264 4×4 variants, per-block maps), **libvmaf** (VMAF v0.6.1 family and v1, MS-SSIM, PSNR-HVS, CIEDE2000, CAMBI), **ONNX Runtime** host for **UVQ** (models exported from google/uvq PyTorch), and an out-of-process **sidecar protocol** (JSON Lines over stdio + shared memory/Y4M) for Python research metrics (ColorVideoVDP, HDR-VDP-3). A shared **normalisation stage** (alignment, crop/scale, colour/range/transfer via zimg, bit-depth promotion) feeds every plugin in its declared working domain (`code_value`, `linear_light`, `pu21`, `ictcp`, `model_rgb`).

## Why

Owner's starter set mixes full-reference and no-reference metrics; HDR metrics must be addable without core changes; bit-exact parity with the `vmaf` CLI requires calling libvmaf directly.

## Alternatives rejected

FFmpeg filters as the engine (framesync surprises, GPL coupling, no maps).
