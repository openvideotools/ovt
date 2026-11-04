# ADR-008

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 75, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Block-level analysis data comes from instrumented software decoders behind a C plugin ABI** (`ovt_analysis_decoder_v1`), never from an analysis-only parser. Per codec: **AV1 → libaom** built with `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1` (upstream API); **HEVC → libde265** with a small read-only internals patch (upstreaming attempt mandatory; HM wrapped as bit-exact oracle and for RExt/SCC); **H.264 → FFmpeg** public API (`export_side_data=mvs+venc_params`) for MVs and QP in phase 1, plus a small side-data extension patch (MB type, ref_idx, intra modes, CBP) submitted upstream. **dav1d** is used for AV1 *playback* only; no dav1d fork.

## Why

Owner's upstream-first policy; libaom inspection is upstream; libde265 holds all data in `de265_image` and the patch touches no decoding code; FFmpeg already exports H.264 MVs/QP. An analysis-only parser would be a decoder minus reconstruction (15–25 kLOC per codec) and still could not show prediction/residual planes.

## Alternatives rejected

dav1d fork (fast but brittle; YUView's fork rotted); own full parsers; hardware decoders (pixels only).
