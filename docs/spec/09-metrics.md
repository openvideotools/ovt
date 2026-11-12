# 9. Video quality metrics

> **Status:** normative chapter of the OVT Requirements & Architecture Specification. Conforms to the backbone (`00-backbone.md`): ADR-011 (metrics plugin system), §0.5.7 (metrics schema), §0.6.3 (metric plugin ABI, reproduced verbatim in §9.2.2), §0.7.4 (performance budgets), ADR-001 (C++20, C ABI). Background facts: Appendix B.3 and F.7.
>
> **How to read this chapter.** §9.1 says what is built and when. §9.2–§9.3 define the pipeline that every metric shares (job lifecycle, normalisation, working domains, alignment). §9.4–§9.7 define the four engines (own kernels, libvmaf, UVQ via ONNX Runtime, sidecar). §9.8–§9.10 define pooling, BD-rate, exports and the GUI contract. §9.11 lists oracles and tolerances, §9.12 the work items, §9.13 glossary additions. Formulas are normative; where a third-party implementation is named as *oracle*, the OVT implementation MUST match it within the stated tolerance.

---

## 9.1 Scope, personas, starter set and tiers

### 9.1.1 Scope

The `metrics` module of `libovt-core` (`core/src/metrics/`, C API `core/include/ovt/metrics.h`, plugin ABI `core/include/ovt/abi/metric_v1.h`) computes objective video quality scores between a **distorted** video and, for full-reference metrics, a **reference** video; produces per-frame series, per-block maps, pooled summaries, BD-rate tables and exports; and exposes everything to the CLI (`ovt metrics`, `ovt bdrate`), the GUI metrics panel (Chapter 11) and the Python module (Chapter 12).

Out of scope for this chapter: rendering of heatmaps (Chapter 10), the metrics panel layout (Chapter 11), decoding (Chapter 7). This chapter *consumes* `ovt_Picture` from the playback path (§0.6.1) and `FrameInfo` from the `StreamIndex` (§0.5.3).

### 9.1.2 Personas

| Persona | Needs from this module |
|---|---|
| **Codec researcher** | PSNR (Y/U/V, JVET-weighted), SSIM, MS-SSIM, VMAF, VMAF-NEG, XPSNR, BD-rate with the exact JVET/AOM interpolators, bit-exact parity with `vmaf` CLI and encoder console output, per-frame-type grouping, per-block maps aligned to the CTU grid. |
| **Video quality team engineer** | Reproducible normalisation (declared and recorded), alignment that survives dropped leading frames, harmonic mean/percentile pooling, per-scene tables, CSV/JSON export, HDR metrics on PQ/HLG content. |
| **Video platform team (ladder decisions)** | VMAF 4K/phone models, UVQ on UGC uploads (no reference), RD curves, convex hull, encoding-ladder table, fast "quick look" with subsampling. |
| **QA / release engineer** | Identity/lossless check, CLI with stable exit codes and JSON, CI-friendly thresholds. |

### 9.1.3 Starter set and tiers

The owner's starter set is **PSNR, SSIM, VMAF, UVQ** (§0.0). Tiers below condense the metrics survey of Appendix B.3. "Phase" refers to §0.8.

| Tier | Metric (OVT `id`) | Engine | Licence of engine | Phase |
|---|---|---|---|---|
| **Baseline** | `psnr` (Y/U/V, JVET 6:1:1, FFmpeg MSE-avg, APSNR) | own kernels (§9.4.2) | project | 3 |
| | `ssim` (Wang 11×11 Gaussian), `ssim_x264` (4×4 fast) | own kernels (§9.4.3–4) | project | 3 |
| | `ms_ssim` | libvmaf `float_ms_ssim` | BSD-2-Clause-Patent | 3 |
| | `vmaf`, `vmaf_neg`, `vmaf_4k`, `vmaf_b` (bootstrapped CI), phone transform; newer bundled models when present | libvmaf ≥ 3.0.0 (3.x) | BSD-2-Clause-Patent | 3 |
| | `identity` (lossless check) | own | project | 3 |
| | `uvq` (no-reference UGC MOS, UVQ 1.5 and 1.0) | ONNX Runtime host; models exported from google/uvq | Apache-2.0 (models), MIT (ORT) | 3 |
| **Extended** | `psnr_hvs` (PSNR-HVS-M) | libvmaf `psnr_hvs` | BSD | 3 |
| | `xpsnr` | own port of FFmpeg 7.1 `vf_xpsnr.c` | LGPL-2.1+ source; **no patent grant** (§9.4.8) | 4 |
| | `ciede2000` (SDR BT.709) | libvmaf `ciede` | BSD | 3 |
| | `cambi` (banding, incl. heatmap) | libvmaf `cambi` | BSD-2-Clause-Patent | 3 (score) / 4 (heatmap) |
| | `vif` | libvmaf feature extractor `integer_vif` (default, fixed-point) or `float_vif` (option `"float": true`); there is no libvmaf extractor named `vif` | BSD-2-Clause-Patent | 3 |
| | `si_ti` (P.910 2008 and 2023) | own | project | 4 |
| | BD-rate / BD-PSNR (cubic, PCHIP, Akima) | own, validated against FAU-LMS `bjontegaard` | BSD-3 oracle | 3 |
| **HDR** | `psnr`/`ssim` on PQ/HLG code values (CTC default) | own kernels, 10/12-bit | project | 3 |
| | `wpsnr`, `psnr_l100`, `de100` (JVET HDR CTC) | own port from VTM/HDRTools | BSD-3 source | 4 |
| | `pu21_psnr`, `pu21_ssim` | own port of PU21 + display model | MIT (gfxdisp/pu21 repository `LICENSE`) | 4 |
| | `de_itp` (ΔE-ITP, BT.2124) per pixel + maps | own | spec | 4 |
| | `cambi` with PQ EOTF | libvmaf (`eotf=pq`) | BSD-2-Clause-Patent | 4 |
| **Research** | `cvvdp` (ColorVideoVDP), `hdrvdp3` | Python sidecar (§9.7) | MIT / GPLv2 (sidecar only) | 4 |
| **Excluded** | VQMT (research-only licence), SSIMPLUS (closed), HDR-VMAF (not released), NTIA VQM (MATLAB, non-FOSS) | — | — | — |

### 9.1.4 Requirements realised by this chapter

Requirement ids are defined only in chapter 04 (§4.8 VQ, §4.7 HDR, §4.15 NFR). The rows below copy chapter 04's sentence and name the sections and work items of this chapter that realise it. Where this chapter's normative definition is more specific than the chapter 04 wording, the *Note* says so.

| Id | Chapter 04 requirement (verbatim) | Realised in | Note |
|---|---|---|---|
| **FR-VQ-001** | Every metric MUST be implemented behind `ovt_metric_v1` (`core/include/ovt/abi/metric_v1.h`) with a `MetricDescriptor` declaring `id`, `kind`, `domain`, `per_plane`, `higher_is_better`, `range_min/max`, `temporal_unit_frames`, `outputs`, `license` and `version`, and the core MUST discover metrics only through this ABI (built-in metrics included). | §9.2.2, §9.2.3, WI-3.300, WI-3.318 | Adding a metric requires no change to `core/src/metrics/host/*`: a new metric is a new shared library exporting `ovt_metric_v1` or a new sidecar package (§9.7). |
| **FR-VQ-002** | The normalisation stage MUST, before any `score()` call, (a) align frames temporally by index after an offset search, (b) crop both inputs by container crop and by user crop rectangles, (c) scale the distorted frame to the reference resolution with a declared kernel ∈ {`bicubic` (default), `lanczos`, `bilinear`, `spline36`} (chapter 9 §9.3.2), (d) convert matrix/primaries/transfer/range/chroma siting to the reference's `ovt_ColorDescription` via zimg, (e) promote bit depth to `max(ref, dist)` by left shift, and MUST record every step in `MetricsResult.normalization` as JSON. | §9.2.4, §9.3, WI-3.301–WI-3.305 | `scale.kernel` enum here is `bicubic`, `lanczos` (taps = 3 default), `area`; no `bilinear` for the luma scale (chroma only). |
| **FR-VQ-003** | Temporal alignment MUST search offsets in the range ±`K` frames (`K` default 30, user-settable) by maximising the mean normalised cross-correlation `S(o)` of downscaled luma signatures over the first `N` frames exactly as chapter 9 §9.3.1 specifies, MUST report `alignment_offset_frames` (sign convention D9: distorted frame *i* corresponds to reference frame *i + o*, so a distorted stream that drops 7 leading frames yields `+7`) and `alignment_confidence` (= `S(o*) − S(o₂)`, the margin between the best and second-best offset, range 0–2; below `min_confidence` = 0.05 the result is flagged `low_confidence` with `VQ-ALIGN-0001`), MUST allow a manual override (`--offset`), MUST emit `VQ-ALIGN-0002` (Warning) when a non-zero offset was applied and `VQ-ALIGN-0003` (Warning) when the two inputs have different frame counts after alignment (chapter 9 §9.3.1). | §9.3.1, §9.3.3, WI-3.304, WI-4.309 | Manual override is `--align N` (chapter 12 §12.2.9). Here `VQ-ALIGN-0001` = low confidence and `VQ-ALIGN-0003` = frame-count mismatch (§9.11.4). |
| **FR-VQ-004** | When frame rates differ, the default policy MUST be to pair only frames whose timestamps match within ε = half the shorter frame duration, MUST report `frames_skipped`, and MUST NOT duplicate or interpolate frames; the user MAY select `--fps-policy refuse\|nearest`. | §9.3.4 | Policy is `frame_rate.policy` of the `NormalizationConfig` (passed through `--normalize`); ε = `frame_rate.epsilon_seconds` (default 0.5 ms); fields `skipped_ref_frames`/`skipped_dist_frames`. |
| **FR-VQ-005** | PSNR MUST be computed per plane as `10·log10(peak² / MSE)` with `peak = 2^bit_depth − 1` of the working depth, MUST report Y, U, V separately, MUST offer the named combinations `jvet_weighted` ((6·Y + U + V)/8 on PSNR), `ffmpeg_avg` (MSE weighted by sample count), and `y_only`, and MUST cap infinite values at `psnr_max` = 100 dB with a `lossless` flag. | §9.4.2, WI-3.306 | Output names here: `jvet`, `ffmpeg_avg`; MSE = 0 yields `psnr_max = 60 + 6·(b − 8)` dB (libvmaf convention) unless `psnr_max_db` is set; the lossless flag is the `identity` metric (§9.4.6). |
| **FR-VQ-006** | SSIM MUST be offered in two variants: `ssim` (Wang et al. 11×11 Gaussian window σ = 1.5, C1 = (0.01·L)², C2 = (0.03·L)², L = 2^bd − 1) and `ssim_x264` (8×8 blocks of 4×4 sums as in x264/libaom `ssim.c`), each per plane with a per-pixel/per-block map output. | §9.4.3, §9.4.4, §9.4.7, WI-3.307, WI-3.308 |  |
| **FR-VQ-007** | VMAF MUST be computed by calling libvmaf (≥ 3.0, fixed-point build) directly on the normalised planes with the bundled models addressed by the ids `vmaf` (= `vmaf_v0.6.1`), `vmaf_neg` (= `vmaf_v0.6.1neg`), `vmaf_4k` (= `vmaf_4k_v0.6.1`), `vmaf_b` (= `vmaf_b_v0.6.3`, bootstrap CI) and `vmaf_<name>` for any other model file shipped with libvmaf ≥ 3.0.0 (chapter 9 §9.6.2), and the per-frame and pooled scores MUST be bit-identical to the `vmaf` CLI on the Netflix test vectors. | §9.5, WI-3.309, WI-3.310, WI-3.591 (chapter 14) | Models: `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1`, `vmaf_b_v0.6.3`; newer bundled models when present (§9.5.3). |
| **FR-VQ-008** | The libvmaf host MUST also expose `ms_ssim`, `psnr_hvs`, `ciede2000`, `cambi` (with its heat-map output) and `vif` as metric ids through the same ABI, with results identical to the `vmaf --feature <name>` output. | §9.5.3, §9.5.5, WI-3.309, WI-4.307 |  |
| **FR-VQ-009** | UVQ MUST be implemented as a `NoReference` metric running the google/uvq 1.5 (default) and 1.0 models exported to ONNX (ContentNet, CompressionNet, DistortionNet, AggregationNet) (TensorFlow SavedModels exported via `tf2onnx`, D11) through ONNX Runtime with the CoreML EP on macOS, the XNNPACK EP on Android (NNAPI is deprecated from API 35) and the CPU EP on Linux, with preprocessing (1 fps sampling default, resize to the model input geometry with bilinear interpolation, RGB float normalisation, 1-second chunks) reproducing the reference CLI, and the `compression_content_distortion` output as the MOS score. | §9.6, WI-3.314, WI-3.315 | Android default EP is XNNPACK; NNAPI only on API < 35 when explicitly requested (§9.6.1). |
| **FR-VQ-010** | UVQ MUST also emit its per-chunk 4×4 compression-level map and 2×2 distortion-type labels as map outputs so that the GUI can overlay them, and model weights (≈ 100 MB) MUST be downloaded on first use into the user cache directory, never bundled in the Android APK. | §9.6.3, §9.6.5 |  |
| **FR-VQ-011** | Every full-reference metric that can be computed locally (PSNR, SSIM variants, ΔE-ITP in phase 4) MUST emit a per-block map at a user-selectable block size `N` ∈ {4, 8, 16, 32, 64} (default: the codec's largest block size when known, else 16) as a `map16` blob, which the GUI MUST render as a heat-map overlay with a selectable colour map including `viridis` and `cividis` and a legend. | §9.4.7, WI-3.306, WI-3.316 | Block sizes here are {8, 16, 32, 64} (4 is not offered: a 4K map at B = 4 is 518 400 cells per frame). |
| **FR-VQ-012** | Pooled statistics `pooled_mean`, `pooled_harmonic`, `pooled_min`, `pooled_p1`, `pooled_p5`, `pooled_std` MUST be computed per plane/output for every metric, and per-scene means MUST be available when a scene list is supplied (`--scenes file.json`) or detected (phase 4). | §9.8.1–§9.8.3, WI-3.311 | Scene detection is delivered in phase 3 here (WI-3.311). |
| **FR-VQ-014** | Metric results MUST be exportable as CSV in the layout of chapter 9 §9.9.1 (`<stem>.csv` with one row per frame and columns `frame,display_index,<metric>_<plane>…`, plus `<stem>_summary.csv` with the pooled values; `.` decimal separator, `,` field separator, UTF-8, LF line endings, regardless of system locale), as OVT JSON (`--format json`), as libvmaf-compatible JSON (`--format vmaf-json`: `frames[].metrics.{vmaf,psnr_y,…}`, `pooled_metrics`), and as OVT `MetricsResult` FlatBuffers (`--format fb`). | §9.9, WI-3.312, WI-3.310 | The normative CSV layout is §9.9.1 (long format, one row per frame × metric × component, RFC 4180 `\r\n`). |
| **FR-VQ-015** | PSNR + SSIM together MUST process ≥ 200 fps at 1080p 8-bit on 4 threads of the M1 reference, and VMAF (`vmaf_v0.6.1`) MUST process ≥ 60 fps at 1080p on 4 threads (chapter 0 §0.7.4). | §9.11.3, WI-3.317 | The gate values are owned by chapter 14 §14.9.3 (`tests/perf/budgets.json`, D19); this chapter repeats no numbers. |
| **FR-VQ-017** | OVT MUST compute BD-rate and BD-PSNR (Bjøntegaard) between two rate–distortion sets of ≥ 4 points with interpolators `cubic` (default, piecewise cubic as in JCTVC-L1100 / VCEG-M33), `pchip` and `akima`, for any metric id and the default pooling `mean` (`--pool` selects another), and results MUST match the FAU-LMS `bjontegaard` reference implementation. | §9.8.4, WI-3.313 | Default interpolator in `ovt bdrate` is `pchip` (chapter 12 §12.2.10); `cubic` is the VCEG-M33 least-squares cubic. |
| **FR-VQ-019** | OVT MUST implement the sidecar metric protocol: a child process started from a sidecar manifest (`<id>.ovt-sidecar.json` naming the executable and its `MetricDescriptor`; chapter 9 §9.7), speaking JSON Lines over stdin/stdout for control (`hello`, `configure`, `frame`, `finish`) and receiving frames through a shared-memory region (POSIX `shm_open`) or a Y4M pipe, running with a restricted environment (no inherited env except `PATH`, `HOME`, `TMPDIR`; working directory a temp dir; 2-minute inactivity timeout). | §9.7, WI-4.304–WI-4.306 | Manifest suffix here is `*.ovt-sidecar.json`; messages are the handshake line, `configure`, `score`, `finish`, `quit` (§9.7.2–§9.7.3); the inherited environment list is §9.7.1. |
| **FR-VQ-020** | The following metrics MUST be added through the plugin ABI without core changes: `xpsnr` (port of the FFmpeg ≥ 7.1 filter, with its patent note in `license`), `de_itp` (ΔE-ITP per BT.2124 on `ICtCp` domain with map output), `pu21_psnr`/`pu21_ssim` (PU21 encoding on `LinearLight` with display model), `wpsnr` (JVET HDR CTC), `si_ti` (ITU-T P.910 2008 and 2023 definitions, `Bitstream`-free, no-reference). | §9.4.8–§9.4.10, WI-4.300–WI-4.303 | SI/TI id here is `si_ti`. |
| **FR-VQ-021** | Scene-change detection MUST be available with the semantics of the FFmpeg `scdet` filter (SAD-based score, threshold default 10.0 on the 0–100 scale; chapter 9 §9.8.2), plus the libvmaf motion score when VMAF is computed, MUST produce a scene list JSON, and per-scene pooling MUST use it automatically when no `--scenes` file is given. | §9.8.2, WI-3.311 | Detector here is exactly the FFmpeg `scdet` algorithm over **all** planes (D31; §9.8.2), threshold 10.0, chosen so that FFmpeg is an exact oracle. |
| **FR-VQ-023** | A **lossless check** metric `identity` MUST report, per frame, whether all planes are bit-identical and the number of differing samples per plane, at ≥ 500 fps for 1080p 8-bit on the M1 reference. | §9.4.6, WI-3.308 |  |
| **FR-VQ-024** | The metric subsystem MUST process inputs in all combinations of {8, 10, 12}-bit and {4:2:0, 4:2:2, 4:4:4} where the reference and distorted may differ in bit depth, and MUST refuse with `VQ-NORM-0003` when chroma formats differ unless `--chroma-policy upsample444` is given. | §9.2.4, §9.5.6, WI-3.301, WI-3.302 | The opt-in is `chroma_upsample.enabled = "always"` in the `NormalizationConfig` (`--normalize '{"chroma_upsample":{"enabled":"always"}}'`). |
| **FR-HDR-009** | The metrics normalisation stage MUST support working domains `CodeValue`, `LinearLight`, `PU21`, `ICtCp` and `ModelRGB` (chapter 0 §0.5.7) and MUST convert via zimg with 32-bit float intermediates, honouring `ovt_ColorDescription` of the reference as the target, with display-model presets `pq_1000`, `pq_4000`, `pq_10000`, `hlg_1000`, `bt1886_100` (chapter 9 §9.4) for `LinearLight`/`PU21`. | §9.2.5, WI-3.303 | Preset spellings here: `pq_1000`, `pq_4000`, `pq_10000`, `hlg_1000`, `bt1886_100`. |
| **NFR-005** | PSNR + SSIM MUST run at ≥ 200 fps and VMAF at ≥ 60 fps for 1080p on 4 threads. | §9.11.3 | Gate values: chapter 14 §14.9.3 (D19). |
| **NFR-018** | Metric results MUST be reproducible to the last printed digit across runs and thread counts, and the engine version (`MetricsResult.engine`) MUST be recorded so that differences between library versions are attributable. | §9.2.3 (Determinism), §9.4.1 |  |

Chapter 04 requirements FR-VQ-013 (ROI rectangles) is not specified by this chapter (open item OI-07, chapter 17); FR-VQ-016, FR-VQ-018 and FR-VQ-022 are GUI requirements realised by chapter 11 §11.3.13 on top of the contract in §9.10.

---

## 9.2 Architecture

### 9.2.1 Module layout

```
core/src/metrics/
  host/        MetricJob, plugin loader (dlopen of ovt_metric_v1), scheduler, progress
  normalize/   NormalizationConfig parsing, pipeline steps, zimg wrapper, domain conversions
  align/       temporal + spatial alignment
  kernels/     own SIMD kernels (Highway): psnr, ssim, ssim_x264, identity, maps, si_ti, xpsnr, pu21, de_itp, wpsnr
  vmaf/        libvmaf host plugin (built into libovt-core, exposes ovt_metric_v1 for each libvmaf-backed id)
  uvq/         ONNX Runtime host plugin
  sidecar/     out-of-process protocol host
  pool/        pooling, scene segmentation, frame-type grouping
  bd/          BD-rate, RD curves, convex hull
  export/      CSV, JSON (OVT + libvmaf schema), FlatBuffers writer
core/include/ovt/metrics.h             C API (ovt_metrics_*)
core/include/ovt/abi/metric_v1.h       plugin ABI (BSD-2-Clause)
core/schema/ovt_metrics.fbs            MetricDescriptor, FrameScore, MetricsResult, RdPoint, BdRateResult (chapter 06 §6.5)
core/tests/metrics/                    Catch2 tests, golden files, oracle scripts (tests/oracles/*.py)
```

### 9.2.2 Plugin ABI (verbatim from backbone §0.6.3)

```c
typedef struct ovt_MetricApi {
  uint32_t struct_size;                       /* D4 */
  uint32_t abi_version;                       /* = 1 */
  ovt_Buffer (*descriptor)(void);             /* MetricDescriptor FlatBuffer */
  ovt_status (*create)(const char* options_json, ovt_Metric** out);
  void       (*destroy)(ovt_Metric*);
  /* Frames are delivered already normalised into descriptor.domain; ref == NULL for NoReference metrics. */
  ovt_status (*score)(ovt_Metric*, const ovt_MetricPlanes* ref /* nullable */, const ovt_MetricPlanes* dist, ovt_FrameId id, ovt_Buffer* out /* FrameScore */);
  ovt_status (*finish)(ovt_Metric*, ovt_Buffer* out /* optional sequence-level FrameScore */);
} ovt_MetricApi;
const ovt_MetricApi* ovt_metric_v1(void);
```

Rules (binding on all plugins, in-tree or third-party):

1. One `ovt_Metric` instance is used by exactly one thread. The host creates one instance per (job, metric, chunk).
2. `score` MUST be called with frames in increasing `id.decode_index` order within an instance; a plugin MAY keep state (motion features need the previous frame).
3. `score` MAY return `out->data == NULL` (e.g. a chunked metric that emits one `FrameScore` every N frames, `temporal_unit_frames = N`); the host then expects the chunk's score on a later call or from `finish`.
4. The `FrameScore` returned is owned by the plugin until the next `score`/`finish`/`destroy` call on that instance; the host copies it immediately.
5. Plugins MUST NOT write to `ref`/`dist` buffers.
6. `options_json` is a JSON object; unknown keys MUST be rejected with `OVT_E_INVALID_ARG` (so typos are caught).

Supporting types (`ovt_FrameId` is owned by the Data Model chapter and reproduced here; `ovt_MetricPlanes` is owned by this chapter and lives only in `metric_v1.h` — if Chapter 6 differs on `ovt_FrameId`, Chapter 6 wins):

```c
typedef struct ovt_FrameId { int32_t decode_index; int32_t display_index; } ovt_FrameId;

typedef struct ovt_MetricPlanes {   /* the multi-plane view a metric scores; not ovt_PlaneView of ovt/decode.h, which is one plane (#156) */
  uint32_t struct_size;            /* D4: sizeof(ovt_MetricPlanes) as compiled into the host */
  const uint8_t* data[3];          /* Y, U, V (or R,G,B / I,Ct,Cp for RGB-like domains); data[1],data[2] NULL for 4:0:0 */
  int32_t  stride_bytes[3];
  uint32_t width[3], height[3];    /* per plane, in samples */
  uint8_t  planes;                 /* 1 or 3 */
  uint8_t  bit_depth;              /* 8..16 for integer samples; 32 for float */
  uint8_t  sample_bytes;           /* 1, 2 or 4 */
  uint8_t  sample_format;          /* 0 = unsigned integer, 1 = IEEE float32 */
  uint8_t  domain;                 /* ovt.fb.WorkingDomain */
  ovt_ColorDescription color;      /* §0.5.6; describes the samples as delivered; frozen, embedded by value */
  double   display_peak_nits;      /* display model peak used for linear/pu21/ictcp; 0 for code_value */
} ovt_MetricPlanes;
```

Sample layout per domain (normative):

| `domain` | planes | sample_format | meaning of samples |
|---|---|---|---|
| `CodeValue` | 1 or 3 | uint, `bit_depth` ∈ {8,10,12,16}, `sample_bytes` 1 or 2 (little-endian uint16) | Y'CbCr (or G'B'R' for RGB sources) code values, chroma at source subsampling unless `chroma_upsample` requested |
| `LinearLight` | 3 (R,G,B), 4:4:4 | float32 | absolute luminance in cd/m² per channel, display-model applied |
| `PU21` | 3 (R,G,B) or 1 (luminance Y) | float32 | PU21 `banding_glare` encoded values (≈ 0…595) |
| `ICtCp` | 3 (I,Ct,Cp), 4:4:4 | float32 | BT.2100 ICtCp with PQ encoding, I ∈ [0,1], Ct,Cp ∈ [−0.5,0.5] |
| `ModelRGB` | 3 (R,G,B) interleaved-free planar, 4:4:4 | float32 | model-specific normalised input (UVQ: see §9.6.4); geometry is the model's |

### 9.2.3 `MetricJob` lifecycle

A `MetricJob` (`core/src/metrics/host/metric_job.h`) owns one comparison: one reference stream (nullable), one distorted stream, a list of metric ids with options, a `NormalizationConfig`, and the output `MetricsResult` per metric.

```cpp
namespace ovt::metrics {
enum class JobState : uint8_t { Created, Probing, Aligning, Scoring, Pooling, Done, Failed, Cancelled };

struct MetricRequest { std::string id; std::string options_json; /* "{}" default */ };

struct JobSpec {
  std::optional<std::string> ref_path;   // required for FullReference ids
  std::string dist_path;
  int ref_track = 0, dist_track = 0;
  std::vector<MetricRequest> metrics;
  std::string normalization_json;        // NormalizationConfig, §9.2.4; "{}" = defaults
  int threads = 0;                       // 0 = hardware_concurrency()
  int subsample = 1;                     // score every Nth frame pair (1 = all)
  std::optional<int> frame_begin, frame_end;   // display indices [begin, end) on the reference
  std::string map_dir;                   // where map blobs are written; empty = in-memory only
};

struct JobEvent { JobState state; uint32_t frames_done, frames_total; double eta_seconds; std::string message; };

class MetricJob {
 public:
  static std::unique_ptr<MetricJob> create(const JobSpec&, jobs::Pool&, std::function<void(const JobEvent&)> on_event);
  ovt_status run(ovt_Job* cancel);                       // blocking; call from a job thread
  const std::vector<flatbuffers::DetachedBuffer>& results() const;   // one MetricsResult per MetricRequest
  const AlignmentReport& alignment() const;
};
}
```

State machine (every transition emits a `JobEvent`):

```
Created ──run()──▶ Probing ──▶ Aligning ──▶ Scoring ──▶ Pooling ──▶ Done
                     │            │            │            │
                     └────────────┴────────────┴────────────┴──▶ Failed | Cancelled
```

| State | Work | Budget / rules |
|---|---|---|
| **Probing** | open both streams (Chapter 7 `ovt_decoder_open`, software decoder, `prefer_hw = 0` — hardware decoders are not bit-exact and MUST NOT feed metrics), read `ovt_ColorDescription`, dimensions, frame count, frame rate; load plugin descriptors; group metrics by `WorkingDomain`; validate: FullReference ids need `ref_path`, else `OVT_E_INVALID_ARG`. | ≤ 500 ms |
| **Aligning** | §9.3; skipped when `temporal.mode == "fixed"` and `spatial.mode == "none"`. | decodes ≤ 2·(60+2K) frames |
| **Scoring** | decode in display order; for each pair run the normalisation pipeline once per required domain; dispatch to plugins; collect `FrameScore`; write map blobs. Cancellation checked per frame (≥ every 50 ms per §0.7.2). | throughput per §0.7.4 |
| **Pooling** | §9.8: pooled vectors, scene segments, frame-type groups. | ≤ 100 ms per 10 000 frames |
| **Done** | `results()` valid. | |

**Parallelism.** The job splits the display-index range into `chunks = min(threads, ceil(frames / 240))` contiguous chunks; each chunk has its own decoder pair, normaliser and plugin instances. Chunks overlap by `descriptor.temporal_unit_frames` plus the plugin's declared `lookbehind`/`lookahead` (from `options` of the descriptor; libvmaf needs `lookbehind = 1`, `lookahead = 0`, see §9.5.2); overlapped frames are scored and discarded. Own kernels additionally parallelise inside a frame by row bands (Highway, §9.4.1). Decoders are never shared across threads (§0.7.2).

**Determinism (§0.7.5).** The same inputs and `JobSpec` MUST produce a byte-identical `MetricsResult` regardless of `threads` and chunking. Float reductions MUST therefore use a fixed order: per-row partial sums in double, rows combined in ascending order; no `std::reduce` with unspecified order.

### 9.2.4 Normalisation stage

The normalisation stage converts a decoded `ovt_Picture` pair into `ovt_MetricPlanes` pairs in each requested `WorkingDomain`. It is a pipeline of **named steps** executed in this fixed order; each step is a no-op when its config says so.

```
crop → scale → chroma_upsample → bit_depth → range → colour(zimg) → domain → temporal(offset/pairing)
```

| # | Step | Input → output | Implementation |
|---|---|---|---|
| 1 | `crop` | apply container/`FrameInfo.crop`, then user rect | pointer arithmetic on planes (no copy) |
| 2 | `scale` | distorted to **reference** resolution (policy fixed: `target = "reference"`) | zimg resize (`bicubic`, `lanczos`) or own `area` box filter |
| 3 | `chroma_upsample` | 4:2:0/4:2:2 → 4:4:4 when a domain needs RGB or when `chroma_upsample.enabled` | zimg with explicit siting |
| 4 | `bit_depth` | promote lower depth to higher by left shift (`promote_lower`) or to a fixed depth | own kernel |
| 5 | `range` | limited ↔ full to the reference's range | zimg (part of the colour graph) |
| 6 | `colour` | matrix / primaries / transfer to the reference's description | zimg (`zimg_filter_graph`) |
| 7 | `domain` | code_value → linear_light / pu21 / ictcp / model_rgb | own float kernels (§9.2.5) |
| 8 | `temporal` | pair frames by index after alignment; frame-rate policy | §9.3.4 |

**Rule:** the reference is never converted except in steps 3, 4 (when the distorted stream has the higher depth), 7 and when the user forces `colour.target = "explicit"`. The distorted stream is converted to match the reference. The *actually applied* configuration (defaults filled in, alignment results inserted) is serialised to JSON and stored verbatim in `MetricsResult.normalization` (§0.5.7).

#### `NormalizationConfig` JSON schema (normative, `core/schema/normalization_config.schema.json`)

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://ovt.dev/schema/normalization_config.json",
  "type": "object",
  "additionalProperties": false,
  "properties": {
    "version": { "type": "integer", "const": 1 },
    "crop": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "honour_container_crop": { "type": "boolean", "default": true },
        "ref":  { "$ref": "#/$defs/rect", "description": "additional crop on the reference, in reference luma samples after container crop; null = none" },
        "dist": { "$ref": "#/$defs/rect", "description": "additional crop on the distorted, in distorted luma samples after container crop; null = none" },
        "auto_detect": { "type": "boolean", "default": false, "description": "letterbox/pillarbox detection, §9.3.2" },
        "auto_apply": { "type": "boolean", "default": false, "description": "apply the detected letterbox crop (requires auto_detect)" }
      }
    },
    "scale": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "target": { "type": "string", "enum": ["reference"], "default": "reference" },
        "kernel": { "type": "string", "enum": ["bicubic", "lanczos", "area"], "default": "bicubic" },
        "bicubic_b": { "type": "number", "default": 0.3333333333 },
        "bicubic_c": { "type": "number", "default": 0.3333333333 },
        "lanczos_taps": { "type": "integer", "minimum": 2, "maximum": 8, "default": 3 },
        "chroma_kernel": { "type": "string", "enum": ["same", "bilinear"], "default": "same" }
      }
    },
    "chroma_upsample": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "enabled": { "type": "string", "enum": ["auto", "always", "never"], "default": "auto", "description": "auto = only when the domain or a mismatch of subsampling requires 4:4:4" },
        "kernel": { "type": "string", "enum": ["bicubic", "bilinear", "lanczos", "point"], "default": "bicubic" },
        "siting": { "type": "string", "enum": ["from_stream", "left", "center", "top_left"], "default": "from_stream", "description": "from_stream reads ovt_ColorDescription.chroma_siting; unknown → left for H.264/HEVC 4:2:0, top_left for AV1 when chroma_sample_position=colocated" }
      }
    },
    "bit_depth": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "rule": { "type": "string", "enum": ["promote_lower", "fixed"], "default": "promote_lower", "description": "promote_lower: the stream with lower depth is left-shifted by (b_hi - b_lo); fixed: both converted to 'fixed_depth' (left shift up, right shift with rounding down)" },
        "fixed_depth": { "type": "integer", "enum": [8, 10, 12, 16], "default": 10 }
      }
    },
    "range": { "type": "string", "enum": ["reference", "limited", "full"], "default": "reference" },
    "colour": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "target": { "type": "string", "enum": ["reference", "explicit"], "default": "reference" },
        "primaries": { "type": "integer", "description": "H.273 code when target=explicit" },
        "transfer":  { "type": "integer" },
        "matrix":    { "type": "integer" },
        "dither": { "type": "string", "enum": ["none", "ordered", "random", "error_diffusion"], "default": "error_diffusion" },
        "assume_unknown_as": { "type": "object", "additionalProperties": false,
          "properties": { "primaries": {"type":"integer","default":1}, "transfer": {"type":"integer","default":1}, "matrix": {"type":"integer","default":1}, "full_range": {"type":"integer","default":0} },
          "description": "used when a stream signals 2 (unspecified); for >= 1280x720 BT.709 is the default, otherwise the caller MUST set these" }
      }
    },
    "display_model": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "preset": { "type": "string", "enum": ["auto", "pq_1000", "pq_4000", "pq_10000", "hlg_1000", "bt1886_100", "custom"], "default": "auto" },
        "peak_nits": { "type": "number", "exclusiveMinimum": 0 },
        "black_nits": { "type": "number", "minimum": 0 },
        "hlg_system_gamma": { "type": "number", "description": "if absent: 1.2 + 0.42*log10(peak_nits/1000)" },
        "clip_to_peak": { "type": "boolean", "default": false }
      }
    },
    "frame_rate": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "policy": { "type": "string", "enum": ["refuse", "resample_to_reference", "match_timestamps"], "default": "match_timestamps" },
        "epsilon_seconds": { "type": "number", "default": 0.0005, "description": "for match_timestamps: |pts_dist - pts_ref| <= epsilon after offset" }
      }
    },
    "temporal": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "mode": { "type": "string", "enum": ["auto", "fixed"], "default": "auto" },
        "offset_frames": { "type": "integer", "default": 0, "description": "ref_index = dist_index + offset_frames (D9: distorted frame i corresponds to reference frame i + offset); written back by auto alignment" },
        "search_frames": { "type": "integer", "minimum": 0, "maximum": 600, "default": 30 },
        "probe_frames": { "type": "integer", "minimum": 8, "default": 60 },
        "drift_search": { "type": "boolean", "default": false },
        "min_confidence": { "type": "number", "default": 0.05 },
        "realign_per_gop": { "type": "boolean", "default": false }
      }
    },
    "spatial": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "mode": { "type": "string", "enum": ["none", "search"], "default": "none" },
        "max_shift_px": { "type": "integer", "minimum": 0, "maximum": 8, "default": 2 },
        "shift_x": { "type": "integer", "default": 0 }, "shift_y": { "type": "integer", "default": 0 }
      }
    }
  },
  "$defs": { "rect": { "type": ["object", "null"], "additionalProperties": false, "required": ["x","y","w","h"],
    "properties": { "x": {"type":"integer","minimum":0}, "y": {"type":"integer","minimum":0}, "w": {"type":"integer","minimum":1}, "h": {"type":"integer","minimum":1} } } }
}
```

Two JSON documents are derived from this schema:

* **`MetricsResult.normalization`** (chapter 06 §6.5, rule V-030) is the canonical serialisation of the *resolved* `NormalizationConfig` — every property present with its resolved value (defaults expanded; `display_model.preset` resolved from `auto`; `temporal.offset_frames` and `spatial.shift_*` as found by alignment) — in chapter 06's canonical JSON form: **keys sorted lexicographically, no whitespace**, numbers in shortest round-trip form. It contains nothing but the schema's properties, so it is byte-deterministic (§0.7.5) and comparable across runs.
* **The export-level `normalization` object** of the OVT JSON (§9.9.2) is the same resolved configuration plus two read-only objects the parser ignores on input: `"applied": {"ref": {...stream description...}, "dist": {...}}` (dimensions, bit depth, range, matrix/primaries/transfer, fps, crop actually applied per stream) and `"alignment": {AlignmentReport}` (§9.3.1). It is pretty-printed in schema order for readability; `applied` and `alignment` are never written into `MetricsResult.normalization`.

#### Target resolution and kernels

* `scale.target` is fixed to `reference` in v1 (industry convention; research §4.2). The distorted picture is resized to the reference's **cropped** luma size; chroma follows the reference subsampling.
* `bicubic` → zimg `ZIMG_RESIZE_BICUBIC` with `filter_param_a = bicubic_b`, `filter_param_b = bicubic_c` (Mitchell parameters b=c=1/3 default, i.e. FFmpeg `bicubic` is *not* identical — FFmpeg swscale bicubic uses b=0,c=0.6; the report must state the kernel so numbers are comparable).
* `lanczos` → `ZIMG_RESIZE_LANCZOS`, `filter_param_a = lanczos_taps`.
* `area` → own `ovt::metrics::resample_area()` (box filter with fractional coverage weights, exact for integer ratios; downscale only; for upscale `area` falls back to `bicubic` and the applied config records `"kernel":"bicubic"` with `"note":"area requested, upscale → bicubic"`).

The scale kernel used MUST be recorded in `MetricsResult.normalization` (realises FR-VQ-002 (c)). *Acceptance test `metrics_norm_kernel_recorded`:* Given a 720p dist and 1080p ref, When scored with `ovt metrics -r ref.y4m -d dist.mp4 -m psnr --normalize '{"scale":{"kernel":"lanczos"}}' --format json -o out.json` (chapter 12 §12.2.9), Then `normalization.scale.kernel == "lanczos"` in `out.json` and in the decoded `MetricsResult.normalization` string.

### 9.2.5 Working-domain conversions (exact formulas)

All formulas operate on float32 planes (double accumulators for sums). Inputs are **code values** `C` with bit depth `b`.

**(a) Code value → normalised non-linear signal `E'` ∈ [0,1]**

Limited range (SDR and HDR, H.273): luma `E' = (C − 16·2^(b−8)) / (219·2^(b−8))`; chroma `E'c = (C − 128·2^(b−8)) / (224·2^(b−8))`. Full range (H.273 Eq. 8-14/8-15 inverted): `E' = C / (2^b − 1)`, chroma `E'c = (C − 2^(b−1)) / (2^b − 1)` (the achromatic code `2^(b−1)` maps exactly to 0). Results are clamped to [0,1] (luma/RGB) and [−0.5, 0.5] (chroma) **only** for `linear_light`/`pu21`/`ictcp`; `code_value` metrics never clamp.

**(b) Y'CbCr → R'G'B'** (non-constant luminance, H.273 Eq. 38–40)

```
R' = Y' + 2(1−Kr)·Cr
G' = Y' − (2Kb(1−Kb)/Kg)·Cb − (2Kr(1−Kr)/Kg)·Cr
B' = Y' + 2(1−Kb)·Cb
BT.709 (matrix=1):   Kr = 0.2126, Kg = 0.7152, Kb = 0.0722
BT.2020 NCL (matrix=9): Kr = 0.2627, Kg = 0.6780, Kb = 0.0593
```
Constant-luminance BT.2020 (matrix=10) and ICtCp-coded streams (matrix=14) are handed to zimg; OVT's own kernels only implement 1 and 9.

**(c) EOTF → absolute linear light (cd/m²)**

*PQ (transfer=16, SMPTE ST 2084):*
```
m1 = 2610/16384 = 0.1593017578125      m2 = 2523/4096 · 128 = 78.84375
c1 = 3424/4096 = 0.8359375             c2 = 2413/4096 · 32 = 18.8515625      c3 = 2392/4096 · 32 = 18.6875
EOTF:   L = 10000 · ( max(E'^(1/m2) − c1, 0) / (c2 − c3·E'^(1/m2)) )^(1/m1)         [cd/m²]
inverse: E' = ( (c1 + c2·(L/10000)^m1) / (1 + c3·(L/10000)^m1) )^m2
```
Applied per channel to R',G',B' → R,G,B (display-referred, absolute). With `clip_to_peak` the result is `min(L, peak_nits)`.

*HLG (transfer=18, BT.2100-2 Table 5):* inverse OETF per channel then OOTF on the three channels together.
```
a = 0.17883277   b = 1 − 4a = 0.28466892   c = 0.5 − a·ln(4a) = 0.55991073
inverse OETF:  E = E'^2 / 3                     for 0 ≤ E' ≤ 1/2
               E = ( exp((E' − c)/a) + b ) / 12  for 1/2 < E' ≤ 1          (scene-linear, E ∈ [0,1])
Ys = 0.2627·Rs + 0.6780·Gs + 0.0593·Bs
γ  = 1.2 + 0.42·log10(Lw / 1000)                 (Lw = display_model.peak_nits; γ = 1.2 at 1000 nits)
OOTF:          R = α·Ys^(γ−1)·Rs + β,  likewise G, B;   α = Lw − Lb,  β = Lb  (Lb = display_model.black_nits, default 0)
```
*BT.1886 (transfer=1, 6, 14, 15; sRGB transfer=13 uses the IEC 61966-2-1 piecewise curve instead, exactly as chapter 10 §10.3.3 maps it to `PL_COLOR_TRC_SRGB`, scaled to `Lw`):*
```
γ = 2.4;   a = ( Lw^(1/γ) − Lb^(1/γ) )^γ;   b = Lb^(1/γ) / ( Lw^(1/γ) − Lb^(1/γ) )
L = a · max(E' + b, 0)^γ          (Lw = peak_nits default 100, Lb = black_nits default 0.1)
```

**(d) Primaries conversion (linear light, D65)**
```
BT.709 RGB → XYZ:  [0.4123908  0.3575843  0.1804808]      BT.2020 RGB → XYZ: [0.6369580  0.1446169  0.1688810]
                   [0.2126390  0.7151687  0.0721923]                         [0.2627002  0.6779981  0.0593017]
                   [0.0193308  0.1191948  0.9505322]                         [0.0000000  0.0280727  1.0609851]
BT.2020 → BT.709 RGB:  [ 1.6604910 −0.5876411 −0.0728499]
                       [−0.1245505  1.1328999 −0.0083494]
                       [−0.0181508 −0.1005789  1.1187297]
BT.709 → BT.2020 RGB:  [ 0.6274040  0.3292820  0.0433136]
                       [ 0.0690970  0.9195400  0.0113612]
                       [ 0.0163916  0.0880132  0.8955950]
```
P3-D65 (primaries=12) is handled via zimg. Negative values after a gamut conversion are kept (not clipped) in `linear_light`; `pu21`/`ictcp` clamp to ≥ 0.

**(e) PU21 (`banding_glare` variant, Mantiuk & Azimi 2021; coefficients from `gfxdisp/pu21` `pu21_encoder.m`)**
```
p = [0.353487901, 0.3734658629, 8.277049286e-05, 0.9062562627, 0.09150303166, 0.9099517204, 596.3148142]
Yc = clamp(Y, 0.005, 10000)                                 (Y in cd/m²; applied per channel R,G,B or to luminance)
V  = p7 · ( ( (p1 + p2·Yc^p4) / (1 + p3·Yc^p4) )^p5 − p6 )
```
Properties the implementation MUST assert in a unit test: `V(0.005) ∈ [−0.01, 0.01]`, `V(100) ∈ [256.0, 257.0]`, `V(10000) ∈ [595.0, 596.0]`. For PU21-PSNR/SSIM the **peak** is `P_pu = V(100)` (≈ 256.4), i.e. SDR 100-nit content yields numbers comparable to 8-bit PSNR; this follows `pu21_metric.m`'s convention and the WI MUST re-verify it against the pinned repo and record the value used in `FrameScore`-level metadata (`"peak": 256.39`).

**(f) ICtCp (BT.2100-2 Table 7; PQ variant; ΔE-ITP per BT.2124)**

Input: linear R,G,B in BT.2020 primaries, absolute cd/m² (HLG content is first converted to absolute light with the display model, then PQ-encoded — BT.2124 §2 requires PQ-based ICtCp for ΔE).
```
L = (1688·R + 2146·G +  262·B) / 4096
M = ( 683·R + 2951·G +  462·B) / 4096
S = (  99·R +  309·G + 3688·B) / 4096
L' = PQ_inverse(L), M' = PQ_inverse(M), S' = PQ_inverse(S)          (inverse EOTF from (c), input in cd/m²)
I  = ( 2048·L' +  2048·M'           ) / 4096
Ct = ( 6610·L' − 13613·M' +  7003·S') / 4096
Cp = (17933·L' − 17390·M' −   543·S') / 4096
ΔE_ITP = 720 · sqrt( ΔI² + 0.25·ΔCt² + ΔCp² )          (T = 0.5·Ct, P = Cp; BT.2124 Eq. 1)
```

**(g) `model_rgb`** — RGB float in the layout a learned model expects; defined per model (UVQ: §9.6.4). Conversion path: code_value → (b) → BT.1886/PQ/HLG display-referred linear → BT.709 primaries → sRGB-encoded 8-bit equivalent → model normalisation. For SDR BT.709 sources this reduces to the reference implementation's `yuv420p → rgb24` conversion (limited-range BT.709 matrix, no transfer change), which is what the UVQ reference uses; HDR sources are tone-mapped by the display model to 100 nits (BT.2390 EETF via libplacebo CPU path, Chapter 10) before encoding to sRGB, and the UI shows the caveat in §9.6.7.

#### Display-model presets

| `preset` | EOTF | `peak_nits` (Lw) | `black_nits` (Lb) | HLG γ | Selected by `auto` when |
|---|---|---|---|---|---|
| `pq_1000` | PQ | 1000 | 0.005 | — | transfer = 16 and (MDCV max ≤ 1000 or absent) |
| `pq_4000` | PQ | 4000 | 0.005 | — | transfer = 16 and 1000 < MDCV max ≤ 4000 |
| `pq_10000` | PQ | 10000 | 0.005 | — | transfer = 16 and MDCV max > 4000 |
| `hlg_1000` | HLG | 1000 | 0 | 1.2 | transfer = 18 |
| `bt1886_100` | BT.1886 | 100 | 0.1 | — | every other transfer |
| `custom` | from stream transfer | user | user | user or formula | never auto |

For PQ, `peak_nits` affects only `clip_to_peak`, the `normalize_to_peak` option of `linear_light` consumers, and the UI's nits scale; PQ itself is absolute.

#### zimg usage (`core/src/metrics/normalize/zimg_convert.cpp`)

zimg (≥ 3.0.5, vcpkg port `zimg`) performs steps 2, 3, 5, 6. One `zimg_filter_graph` is built per (job, chunk, step bundle) and reused for every frame.

```cpp
zimg_image_format src{}, dst{};
zimg_image_format_default(&src, ZIMG_API_VERSION); zimg_image_format_default(&dst, ZIMG_API_VERSION);
src.width = w_in;  src.height = h_in;  src.pixel_type = (bd <= 8) ? ZIMG_PIXEL_BYTE : ZIMG_PIXEL_WORD;  src.depth = bd;
src.subsample_w = log2(sub_w); src.subsample_h = log2(sub_h);
src.color_family = ZIMG_COLOR_YUV;                 // ZIMG_COLOR_RGB for RGB sources, ZIMG_COLOR_GREY for 4:0:0
src.matrix_coefficients = color.matrix;           // H.273 codes are used by zimg directly
src.transfer_characteristics = color.transfer;
src.color_primaries = color.primaries;
src.pixel_range = color.full_range ? ZIMG_RANGE_FULL : ZIMG_RANGE_LIMITED;
src.chroma_location = map_siting(color.chroma_siting);   // 1→ZIMG_CHROMA_LEFT, 2→ZIMG_CHROMA_CENTER, 3→ZIMG_CHROMA_TOP_LEFT
src.active_region = { crop.x, crop.y, crop.w, crop.h }; // step 1 for fractional/odd crops
// dst = reference's format (or the requested 4:4:4 / float RGB for RGB-like domains: pixel_type ZIMG_PIXEL_FLOAT, depth 32)
zimg_graph_builder_params p; zimg_graph_builder_params_default(&p, ZIMG_API_VERSION);
p.resample_filter = (kernel == "lanczos") ? ZIMG_RESIZE_LANCZOS : ZIMG_RESIZE_BICUBIC;
p.filter_param_a = (kernel == "lanczos") ? lanczos_taps : bicubic_b;
p.filter_param_b = (kernel == "lanczos") ? NAN : bicubic_c;
p.resample_filter_uv = chroma_kernel == "bilinear" ? ZIMG_RESIZE_BILINEAR : p.resample_filter; p.filter_param_a_uv = p.filter_param_a; p.filter_param_b_uv = p.filter_param_b;
p.dither_type = map_dither(cfg.colour.dither);   // ZIMG_DITHER_ERROR_DIFFUSION default; ZIMG_DITHER_NONE when dst is float
p.cpu_type = ZIMG_CPU_AUTO_64B;
p.nominal_peak_luminance = display_model.peak_nits;   // used by zimg for HLG OOTF / scene-referred handling
p.allow_approximate_gamma = 0;                          // exactness over speed
zimg_filter_graph* g = zimg_filter_graph_build(&src, &dst, &p);
size_t tmp; zimg_filter_graph_get_tmp_size(g, &tmp);   // allocate once per graph, 64-byte aligned
zimg_filter_graph_process(g, &src_buf, &dst_buf, tmp_ptr, nullptr, nullptr, nullptr, nullptr);
```
Rules: `allow_approximate_gamma = 0`; 32-bit float intermediates (zimg default); dithering only when the destination is integer; errors mapped via `zimg_get_last_error` to `OVT_E_INTERNAL` with the zimg message in the `Diagnostic`. Transfer conversions used for the `linear_light`/`pu21`/`ictcp` domains are done by OVT's own kernels (§9.2.5 c–f) so the constants are testable; zimg is used for them only as an oracle in tests (tolerance 1e-4 in normalised units).

---

## 9.3 Alignment

### 9.3.1 Temporal alignment algorithm

Produces `offset_frames`, optional `drift`, and `confidence`. **Sign convention (D9, used by every chapter):** offset `o` means that distorted frame `i` corresponds to reference frame `i + o`, i.e. `ref_index = dist_index + offset_frames`. A distorted stream that lacks the first 7 reference frames therefore has `offset_frames = +7`; a distorted stream with 3 extra leading frames has `offset_frames = −3`.

1. **Probe decode.** Decode the first `P = temporal.probe_frames` (default 60) displayed frames of the reference and the first `P + 2K` (K = `search_frames`, default 30) displayed frames of the distorted stream, luma only, via the software decoder.
2. **Signature.** For each frame compute `sig[i]`: downscale the cropped luma plane to **32×18** with the `area` box filter (exactly: each output cell is the mean of the samples whose centres fall in the cell), convert to float, subtract the mean of the 576 values. If both streams are not 16:9, still use 32×18 (aspect does not matter for correlation).
3. **Score per candidate offset** `o ∈ [−K, +K]`: `S(o) = (1/N_o) Σ_i NCC(sig_dist[i], sig_ref[i+o])` over all `i` where both indices exist (`N_o ≥ 8`, else the candidate is skipped); `NCC(a,b) = Σ a·b / sqrt(Σ a² · Σ b²)` (returns 0 when either norm is 0, i.e. flat frames).
4. **Drift search** (only if `drift_search`): for each `o`, additionally evaluate `d ∈ {−2,−1,1,2}` frames per 1000 frames with `j = round(i·(1 + d/1000)) + o` as the reference index paired with distorted frame `i`; keep the best `(o, d)`. Drift ≠ 0 forces `frame_rate.policy = match_timestamps` semantics.
5. **Best and confidence.** `o* = argmax S(o)`; `confidence = S(o*) − S(o₂)` where `o₂` is the best offset with `|o₂ − o*| ≥ 1`. Range [0, 2].
6. **Decision.** If `confidence < min_confidence` (default 0.05) the job continues with `o*` but sets `AlignmentReport.status = "low_confidence"` and emits `Diagnostic{code:"VQ-ALIGN-0001", severity: Warning}`; the GUI MUST show the warning and offer the manual override. Static content (all-flat signatures) yields `S ≡ 0` and `status = "undetermined"`, offset 0.
7. **Per-GOP re-alignment** (`realign_per_gop`): repeat steps 1–5 at each reference RAP (from `StreamIndex.frames[].is_rap`) with `K = 3`, `P = min(GOP length, 60)`; the per-segment offsets are stored in `AlignmentReport.segments`.
8. **Override flags** (chapter 12 §12.2.9 grammar). `--align N` sets `temporal.mode = fixed, offset_frames = N` (D9 sign: dist frame `i` ↔ ref frame `i + N`; positive N = the distorted stream is missing N leading reference frames); `--align none` sets `mode = fixed, offset 0`; `--align auto` (default) runs steps 1–7. `search_frames`, `drift_search` and `realign_per_gop` have no dedicated flags: they are set through `--normalize '{"temporal":{"search_frames":60,"drift_search":true}}'`. (Note for users of ffmpeg-quality-metrics: its `--dist-delay` has the opposite sign.)

```cpp
struct AlignmentReport {           // serialised into normalization.alignment
  std::string status;              // "ok" | "low_confidence" | "undetermined" | "fixed"
  int offset_frames; double drift_per_1000; double confidence; double best_score, second_score;
  int shift_x, shift_y;            // spatial (§9.3.2)
  Rect ref_crop_detected, dist_crop_detected;
  std::vector<std::pair<int,int>> segments;   // (ref_rap_display_index, offset) when realign_per_gop
  uint32_t ref_frames, dist_frames, paired_frames, skipped_ref_frames, skipped_dist_frames;
};
```
`MetricsResult.alignment_offset_frames` and `alignment_confidence` (§0.5.7) are copies of `offset_frames` and `confidence`.

**Acceptance test `metrics_align_leading_drop`** (realises FR-VQ-003): Given a distorted stream equal to the reference with its first 7 frames removed (fixture `syn_align_drop7`, chapter 14 §14.3.3), When alignment runs with defaults, Then `offset_frames == +7`, `status == "ok"` and `confidence ≥ 0.5` (the two streams are identical after the shift, so `S(7) = 1` and every other candidate is far below; the normative flagging threshold remains `min_confidence = 0.05`).

### 9.3.2 Spatial alignment

Runs after temporal alignment on the 8 best-correlated frame pairs when `spatial.mode == "search"`:

1. For every integer `(dx, dy)` with `|dx|,|dy| ≤ max_shift_px` (default 2 → 25 candidates), compute luma MSE between the reference and the distorted frame shifted by `(dx, dy)` over the common area (borders excluded).
2. Pick the minimum MSE; `shift_x, shift_y` are recorded; the normalisation `crop` step offsets the distorted picture by `(shift_x, shift_y)` and shrinks both to the common area (recorded in `applied.dist.crop`).
3. If the best shift is non-zero, emit `Diagnostic{code:"VQ-ALIGN-0002", Warning, "distorted stream is shifted by (dx,dy) samples"}`. Chroma is shifted by `dx / sub_w`, `dy / sub_h` only when divisible; otherwise the shift is applied to luma and the chroma siting mismatch is noted.

**Crop detection** (`crop.auto_detect`): on the first 16 reference frames compute per-row and per-column mean luma; rows/columns whose mean ≤ `16·2^(b−8) + 2·2^(b−8)` and whose standard deviation ≤ `2^(b−8)` are letterbox; the largest such border on each side is proposed as `ref_crop_detected` (same for dist). The detection is *applied* only when `crop.auto_apply` is true (`--normalize '{"crop":{"auto_detect":true,"auto_apply":true}}'`); otherwise it is reported in `AlignmentReport.ref_crop_detected/dist_crop_detected` only.

### 9.3.3 Frame pairing

After alignment, frames are paired **by display index**: dist frame `i` ↔ ref frame `i + offset_frames` (plus per-segment offsets). Pairing by PTS is forbidden except as the `match_timestamps` tie-breaker below. Frames with no partner (leading/trailing) are counted in `skipped_*_frames` and `Diagnostic VQ-ALIGN-0003` is emitted when `ref_frames ≠ dist_frames + offset`.

### 9.3.4 Frame-rate mismatch policy

Frame rate is taken from the stream index (median of pts deltas) for each stream. If they differ by more than 0.1 %:

| `frame_rate.policy` | Behaviour |
|---|---|
| `refuse` | job fails with `OVT_E_UNSUPPORTED`, code `VQ-FPS-0001` |
| `resample_to_reference` | the distorted stream is duplicated/dropped to the reference's timestamps (FFmpeg `framesync`-like); frames duplicated are listed in `AlignmentReport`; **never the default** |
| `match_timestamps` (**default**) | after applying the offset as a *time* offset (`offset_frames / fps_ref`), pair ref frame `i` with the dist frame whose pts is within `epsilon_seconds` of ref pts; unmatched reference frames are skipped and counted; the result records `skipped_ref_frames` and `MetricsResult.frames` contains only paired frames |

Because VMAF's `motion` feature depends on frame rate, `MetricsResult.normalization.applied.{ref,dist}.fps` is always recorded. Temporal interpolation is never performed.


---

## 9.4 Own kernels

### 9.4.1 Common rules

* **Language / SIMD.** C++20; SIMD via **Google Highway** (Apache-2.0, vcpkg port `highway`, pinned ≥ 1.2.0). Highway is chosen over `std::experimental::simd` because it ships runtime dispatch (`HWY_DYNAMIC_DISPATCH`) for SSE4/AVX2/AVX-512 on x86_64 and NEON on arm64 from one source, is header-plus-small-library, and is already used in production image codecs (libjxl). Every kernel has a scalar reference (`*_scalar.cpp`) and a Highway version (`*_hwy.cpp`); CI runs both and asserts bit-identical integer results and `|Δ| ≤ 1e-9` relative for float results on the same host (sums are accumulated in `double` in a fixed order, see §9.2.3 determinism). Across architectures (x86_64 AVX2 vs arm64 NEON) float kernels (`ssim`, `ms_ssim`, PU21/ICtCp paths) are compared at **1e-4 relative** (chapter 14 WI-3.590); integer kernels and libvmaf's fixed-point features must be bit-identical.
* **Tiling.** Planes are processed in row bands of 64 rows per task so a band of 1080p 8-bit luma (≈ 123 KB per plane pair) fits L2; bands are combined in ascending order.
* **Bit depth.** `peak = 2^b − 1` where `b` is the working bit depth after promotion (§9.2.4 step 4). All constants that depend on the peak are recomputed per job, never hard-coded for 8-bit.
* **Per-plane treatment.** Full-reference pixel metrics run on Y, U, V independently (`per_plane = true`) and return `values = [Y, U, V, <named combinations…>]`; the order of `values` is declared in `descriptor.outputs` (e.g. `["y","u","v","jvet","ffmpeg_avg","map16"]`). For 4:0:0 only Y is produced and combinations equal Y.
* **Maps.** §9.4.11.
* **Oracles and tolerances.** Each kernel names an oracle; `core/tests/metrics/oracle_*.cpp` compares against golden numbers produced by `tests/oracles/run_<oracle>.py` and stored under `core/tests/metrics/golden/`. Golden files carry the oracle version in their name.

### 9.4.2 `psnr`

Per plane `p` with `N_p` samples, `d_i = ref_i − dist_i` (integers in the working depth):

```
SSE_p  = Σ d_i²                         (uint64 accumulator; exact)
MSE_p  = SSE_p / N_p                    (double)
PSNR_p = 10·log10( peak² / MSE_p )      if MSE_p > 0
       = psnr_max                       if MSE_p == 0, with psnr_max = 60 + 6·(b − 8) dB (libvmaf psnr convention: 60 dB at 8-bit, 72 dB at 10-bit, 84 dB at 12-bit)
```
Combinations (all reported as separate `values` entries):

| Name | Definition |
|---|---|
| `jvet` | `(6·PSNR_Y + PSNR_U + PSNR_V) / 8` (JVET CTC weighted PSNR, JVET-T2010) |
| `ffmpeg_avg` | `10·log10( peak² / MSE_avg )`, `MSE_avg = (SSE_Y + SSE_U + SSE_V) / (N_Y + N_U + N_V)` (FFmpeg `psnr_avg`, MSE-weighted by sample count) |
| `mse_y`, `mse_u`, `mse_v` | raw MSE per plane (so users can re-pool) |

Sequence-level (emitted by `finish`): **APSNR** per plane `= 10·log10( peak² / mean_frames(MSE_p) )` — the AOM CTC "average PSNR from mean MSE" — in `values` named `apsnr_y/u/v`, plus `apsnr_jvet`. Note that `pooled_mean` of per-frame PSNR is *not* APSNR; both are reported.

Options JSON: `{"psnr_max_db": number|null, "planes": ["y","u","v"]}`.

**Oracles:** (1) libvmaf `psnr` feature (`psnr_y`, `psnr_cb`, `psnr_cr`), tolerance `1e-6 dB` (same formula, same clipping); (2) FFmpeg `psnr` filter stats file (`psnr_y`, `psnr_avg`), tolerance `1e-4 dB` (FFmpeg prints 2–4 decimals; compare at 2 decimals when parsing its text; MSE==0 is printed as `inf` by FFmpeg and compared as `psnr_max`).

### 9.4.3 `ssim` (Wang et al. 2004, Gaussian 11×11, σ = 1.5)

Window `w[k,l]`, `k,l ∈ [−5,5]`: `w = g(k)·g(l)`, `g(k) = exp(−k²/(2·1.5²)) / Σ_j exp(−j²/(2·1.5²))` (1-D normalised, separable). Constants: `C1 = (0.01·peak)²`, `C2 = (0.03·peak)²`, `K1 = 0.01`, `K2 = 0.03` fixed.

For each window position (valid region only: output size `(W−10)×(H−10)`, matching `filter2(..., 'valid')` of Wang's `ssim.m`):
```
μx = Σ w·x        μy = Σ w·y
σx² = Σ w·x² − μx²        σy² = Σ w·y² − μy²        σxy = Σ w·x·y − μx·μy
SSIM = ( (2·μx·μy + C1) · (2·σxy + C2) ) / ( (μx² + μy² + C1) · (σx² + σy² + C2) )
SSIM_plane = mean over all valid window positions
```
Computation in float32 with the five separable convolutions (x, y, x², y², xy) in float32 and the final reduction in double. Luma by default; `planes` option as for PSNR. `values = [y, u, v]` (u/v = NaN unless requested).

Options JSON: `{"planes":["y"], "border":"valid"|"libvmaf"}`. `border = "libvmaf"` reproduces libvmaf's `float_ssim` border handling exactly; the WI implementer MUST read `libvmaf/src/feature/ssim_tools.c` of the pinned libvmaf and implement whatever padding/valid policy it uses as `"libvmaf"`, then make the one that matches libvmaf the **default** so the oracle test is tight. Record the border policy in `descriptor.version` string (`"ssim 1.0 border=valid"`).

**Oracle:** libvmaf `float_ssim` (feature score `float_ssim`), tolerance `1e-4` per frame with the matching border policy. Secondary cross-check (documented as *expected to differ*): FFmpeg `ssim` (8×8 blocks, no Gaussian) — recorded in `docs/metrics_differences.md`, no test tolerance.

### 9.4.4 `ssim_x264` (x264 4×4 fast variant)

This is the SSIM printed by x264 and x265 (`--ssim`). Definition = exactly x264 `common/pixel.c` (`ssim_4x4x2_core`, `ssim_end1`, `ssim_end4`, `x264_pixel_ssim_wxh`), transcribed here as the normative algorithm:

1. **4×4 sums.** The plane is first offset by 2 samples in x and y and reduced to `(W−2)×(H−2)` (`pix + 2 + 2*stride`, `width − 2`, `height − 2`, as `x264_pixel_ssim_wxh` is called by the encoder). For every 4×4 block `(bx, by)`, `0 ≤ bx < W'/4`, `0 ≤ by < H'/4` (`W' = W − 2`, `H' = H − 2`, integer division), compute the integer sums `s1 = Σx`, `s2 = Σy`, `ss = Σx² + Σy²`, `s12 = Σx·y`.
2. **8×8 windows stepping by 4.** For every window `(wx, wy)` with `0 ≤ wx < W'/4 − 1`, `0 ≤ wy < H'/4 − 1`, add the sums of the four blocks `(wx..wx+1, wy..wy+1)`: `fs1, fs2, fss, fs12` (64 samples per window).
3. **Constants and per-window SSIM** (`ssim_end1`; `peak = 2^b − 1`):
   ```
   b ≤ 9 (x264 `#else` branch, type = int):
     ssim_c1 = (int)( 0.01·0.01 · peak² · 64      + 0.5 )
     ssim_c2 = (int)( 0.03·0.03 · peak² · 64 · 63 + 0.5 )
   b > 9 (x264 `#if BIT_DEPTH > 9` branch, type = float; NO rounding):
     ssim_c1 = (float)( 0.01·0.01 · peak² · 64 )          (6697.7856 at 10-bit)
     ssim_c2 = (float)( 0.03·0.03 · peak² · 64 · 63 )     (3797644.4 at 10-bit)
     fs1, fs2, fss, fs12 converted to float before the products below
   vars  = fss·64 − fs1² − fs2²
   covar = fs12·64 − fs1·fs2
   ssim_win = ( (2·fs1·fs2 + ssim_c1) · (2·covar + ssim_c2) ) / ( (fs1² + fs2² + ssim_c1) · (vars + ssim_c2) )
   ```
   For b ≤ 9 `vars`, `covar` and the numerator/denominator factors are integers and only the final division is `float`; for b > 9 every term is `float`. `ssim_end4` sums four windows in `float`. OVT reproduces the branch selection and the integer/float types exactly, evaluates the final division in `double`, and documents the resulting difference (≤ 1e-6) in `docs/metrics_differences.md`.
4. **Plane SSIM** = `Σ ssim_win / count` with `count = (W'/4 − 1)·(H'/4 − 1)` windows. Integer sums use `int64` for b > 8 (x264 high-bit-depth builds use `int`/`float` mixes; the integer sums are exact for 4×4 blocks up to 16-bit either way).
5. x264 additionally prints `ssim_db = −10·log10(1 − ssim)` (`x264_ssim()`), clamped: when `1 − ssim ≤ 1e-10` x264 returns exactly `100` dB; OVT applies the same clamp and reports it as `values[3] = ssim_db_y`.

`values = [y, u, v, ssim_db_y]`. Options: `{"planes":["y","u","v"]}` (x264 computes luma only; U/V use the same kernel on the chroma planes and are OVT additions).

**Oracle:** a **vendored, unmodified copy of x264's C kernel** (`ssim_4x4x2_core`, `ssim_end1`, `ssim_end4`, `x264_pixel_ssim_wxh` from `common/pixel.c` of the pinned x264 tag) in `tests/oracles/x264_ssim/pixel_ssim.c` (GPL-2.0-or-later; built only as the oracle executable `x264_ssim <ref>.y4m <dist>.y4m` — the Y4M header carries size and bit depth; one executable holds the 8-bit integer path and the 10-bit float path and picks by the header (chapter 14 §14.4.12, issue #306) — never linked into OVT) driven over the same raw planes; tolerance `1e-6` per plane (x264 float vs OVT double) for 8-bit and 10-bit inputs. Secondary cross-check: the `SSIM Mean Y` line of an x264 encode log (`x264 --ssim --psnr --qp 20 --bframes 0 ref.y4m -o out.264`; with B-frames x264 scores non-reference B pictures before deblocking, so the log would not match the decoded pictures, issue #306) is reproduced by running OVT on the source and the decoded x264 output, tolerance `1e-4` (the log prints 7 digits). libvpx's `tiny_ssim` is **not** an oracle for this id: it uses 8×8 windows stepping by 4 without the 2-sample offset, constants scaled by `64²` (`cc1 = 26634`, `cc2 = 239708` at 8-bit), and an "All" = `0.8·Y + 0.1·U + 0.1·V`; OVT does not ship that variant (`ssim_tiny` is reserved as an id, not implemented).

### 9.4.5 `ms_ssim`

Provided by libvmaf `float_ms_ssim` (§9.5); 5 scales with weights `[0.0448, 0.2856, 0.3001, 0.2363, 0.1333]` (Wang 2003), luma only. Not reimplemented in phase 3. The id is listed here so the GUI shows it in the same group as `ssim`.

### 9.4.6 `identity` (lossless check)

Per plane: `identical_fraction_p = (number of samples with ref_i == dist_i) / N_p`; `values = [y, u, v, all]` where `all` is sample-weighted; plus a sequence-level `finish` output `values = [frames_identical, frames_total, sha256_match]` where `sha256_match = 1.0` when the SHA-256 over all paired planes (row by row, working depth, little-endian) is equal for ref and dist. `higher_is_better = true`, range [0,1]. Oracle: FFmpeg `identity` filter, exact equality of `identity_y/u/v`.

### 9.4.7 Per-block map generation

Every own full-reference kernel also produces a **per-block map** when the request's options contain `"map_block": B`, `B ∈ {8,16,32,64}`:

* The block grid origin is the top-left of the (cropped) reference picture. If the distorted stream has a `FrameAnalysis` available (Chapter 7) or a codec is known, the GUI passes `"map_block": "ctu"` which resolves to the codec's largest block (H.264: 16, HEVC: `CtbSizeY` from the SPS, AV1: superblock size 64 or 128 → stored as 64 with 4 blocks per 128-SB); the resolved number is written to `FrameScore.map_block_size`.
* PSNR map: value = PSNR of the block (`psnr_max` when MSE 0). SSIM maps: mean of the window values whose centre falls in the block. Identity map: fraction identical. ΔE-ITP map: mean ΔE in the block. Edge blocks (partial) use only existing samples.
* Storage: one `MapBlob` per (frame, metric, component); the in-memory/plane-store representation is the chapter 06 §6.5 `MapBlob` descriptor (`sample_type` ∈ {`U8`, `U16`, `F32`}, `value_min/value_max` quantisation rule; OVT's own kernels write `F32`, libvmaf/CAMBI heatmaps may be stored as `U8`/`U16`). The **on-disk/export** form (`.ovtmap`) is always `float32` little-endian, row-major (U8/U16 blobs are dequantised on export), with this 32-byte header:

```c
struct ovt_MapBlobHeader {        /* little-endian, 32 bytes */
  char     magic[8];              /* "OVTMAP01" */
  uint32_t blocks_w, blocks_h;    /* ceil(W/B), ceil(H/B) */
  uint16_t block_size;            /* B in luma samples */
  uint8_t  component;             /* 0=Y 1=U 2=V 3=combined */
  uint8_t  value_kind;            /* 0 = higher is better, 1 = lower is better */
  float    value_min, value_max;  /* for colour-map scaling; NaN when unknown */
  uint32_t reserved;
};                                 /* followed by blocks_w*blocks_h float32 */
```
* `FrameScore.map_blob_id = "<metric_id>/<component>/<decode_index>/<B>"`, e.g. `"psnr/y/000123/16"`. In memory the blob lives in the job's `MapStore` (LRU, ≤ 256 MB); on disk at `<map_dir>/<metric_id>/<component>/f<decode_index:06>_b<B>.ovtmap`. Descriptors list `"map16"` (or `"map"` generally) in `outputs` when maps are supported.

### 9.4.8 `xpsnr` (phase 4; port of FFmpeg 7.1 `libavfilter/vf_xpsnr.c`, LGPL-2.1+)

**Patent caveat (MUST appear in `descriptor.license` and the About dialog):** Fraunhofer HHI's original plugin licence excludes patent rights; the FFmpeg version is LGPL but grants no patent licence either. OVT ships `xpsnr` as a separately built plugin (`plugins/ovt-metric-xpsnr/`, option `OVT_BUILD_METRIC_XPSNR`, default ON) so distributors can omit it.

Definition (Helmrich et al., ITU Journal 2020, as implemented by FFmpeg):
```
N  = 4·round( 32·sqrt( W·H / (3840·2160) ) )                  block size (64 at 1080p, 128 at 2160p)
for each N×N block k of plane Y (partial blocks allowed), frame t:
  SSE_k   = Σ (ref − dist)²
  a_s,k   = Σ |h_s(x,y)| / (block samples)        spatial activity, h_s = high-pass (FFmpeg: 12-tap 2-D kernel for N≥...; see source) on the reference
  a_t,k   = Σ |h_t(x,y)| / (block samples)        temporal activity, h_t = ref_t − ref_{t−1} (1st order), or the 2nd-order difference ref_t − 2·ref_{t−1} + ref_{t−2} when fps ≥ 32 (vf_xpsnr.c: `if (fps >= 32) diff2nd else diff1st`)
  a_k     = max( a_min, (a_s,k + 2·a_t,k) / ... )  — exact normalisation per vf_xpsnr.c
  w_k     = sqrt( a_pic / a_k )                   with a_pic = sqrt( 16 · 2^(2b−9) / sqrt( max( W·H / (3840·2160), 0.00001 ) ) )  (vf_xpsnr.c `init()`: `s->avg_act`)
wSSE_t   = Σ_k w_k·SSE_k ;  XPSNR_t = 10·log10( peak²·W·H / wSSE_t )
sequence: XPSNR = 10·log10( peak²·W·H·T / Σ_t wSSE_t )
chroma planes reuse the luma block weights w_k of the co-located luma block (vf_xpsnr.c computes the weights once from the luma plane and applies them to every plane; chroma blocks are N/sub_w × N/sub_h); the per-plane XPSNR triple is reported, no combined value
```
The high-pass kernel coefficients, the `a_min` floor, the temporal term's frame-rate switch and the exact `a_pic` normalisation MUST be transcribed from the pinned FFmpeg source (7.1.x, `vf_xpsnr.c` functions `get_avg_xpsnr`, `highds`, `diff1st`, `diff2nd`, `calc_squared_error_and_weight`) rather than from this summary; this chapter fixes the structure and the oracle. Output `values = [y, u, v]` per frame and sequence-level `[y, u, v]` from `finish`.

**Oracle:** FFmpeg 7.1 `xpsnr` filter `stats_file`, per-frame and sequence XPSNR, tolerance `1e-3 dB` (the stats file prints 4 decimals). The oracle run uses one clip at 30 fps (1st-order temporal term) and one at 60 fps (2nd-order term).

### 9.4.9 `si_ti` (ITU-T P.910 Spatial / Temporal Information; no-reference)

Both definitions are implemented; option `{"definition":"2008"|"2023"}`, default `2023`.

*P.910 (04/2008) §7.7, as FFmpeg `siti`:* on the 8-bit luma plane `Y` (higher depths right-shifted to 8 bits for `2008`):
```
Sobel: Gx = [−1 0 1; −2 0 2; −1 0 1] ⊛ Y,  Gy = Gxᵀ ⊛ Y,  G = sqrt(Gx² + Gy²)   (interior pixels only, 1-pixel border excluded)
SI_t = std_space(G),   TI_t = std_space(Y_t − Y_{t−1})   (population std; TI undefined for the first frame)
sequence: SI = max_t SI_t,  TI = max_t TI_t
```
*P.910 (10/2023) Annex, as `siti-tools` (Robitza):* convert `Y` to normalised `E'` (range per stream), apply the EOTF (BT.1886 with Lw=100/Lb=0.1 for SDR; PQ; HLG with the display model) to obtain absolute luminance `L`, then re-encode with the inverse PQ EOTF (default `"domain":"pq"`) or PU21 (`"domain":"pu21"`), scale to the `[0, 255]` range (`pq`: `E'_pq·255`; `pu21`: `V/V(10000)·255`), and apply the same Sobel/std/max steps on the float result. The exact scaling and the EOTF defaults MUST be matched to the pinned `siti-tools` version (`siti_tools/siti.py`) and recorded in `docs/si_ti.md`.

`values = [si, ti]` per frame (ti = NaN for frame 0), `finish` → `[si_max, ti_max, si_mean, ti_mean]`. `kind = NoReference`.

**Oracles:** FFmpeg `siti` (2008) tolerance `1e-3`; `siti-tools` ≥ 0.2 (2023) tolerance `1e-3`.

### 9.4.10 HDR kernels (phase 4)

**`de_itp`** — ΔE-ITP per pixel from the `ICtCp` domain (§9.2.5 f) for ref and dist; `values = [mean, p95, max]` per frame (lower is better, `range 0..∞`, typical < 10); map = block mean. Oracle: Python reference `tests/oracles/de_itp_ref.py` (NumPy, double precision, same formulas), tolerance `1e-3`. Cross-check against libplacebo's `pl_color_space` ICtCp conversion on 1000 random samples, tolerance `1e-4` in I/Ct/Cp.

**`pu21_psnr`, `pu21_ssim`** — run the `psnr` and `ssim` kernels on the `PU21` domain planes (float32), `peak = P_pu = V(100)`; for `pu21_ssim`, `C1 = (0.01·P_pu)², C2 = (0.03·P_pu)²`. Luminance channel: `Y = 0.2627R + 0.6780G + 0.0593B` (BT.2020) or `0.2126R + 0.7152G + 0.0722B` (BT.709) in linear light before PU21; RGB channels optional. `values = [y, r, g, b]`. Oracle: `gfxdisp/pu21` MATLAB run in Octave via `tests/oracles/run_pu21.m` on 8 frames, tolerance `1e-3 dB` / `1e-4` SSIM.

**`wpsnr`** (JVET HDR CTC, port of VTM `Picture::createLumaLevelToWeightTable` and the `WEIGHTED_PSNR` path):
```
x = luma code value of the REFERENCE scaled to 10-bit: x = C_ref · 2^(10 − b)
y = clamp( 0.015·x − 7.5, −3, 6 )
w(x) = 2^( y / 3 )                                 (per-sample weight; precomputed as a 1024-entry table)
wSSE_p = Σ w(x_i)·d_i²  for each plane (chroma uses the co-located luma weight)
wPSNR_p = 10·log10( peak² · N_p / wSSE_p )
```
`values = [y, u, v]`. The exact weight function MUST be re-verified against the pinned VTM tag (`source/Lib/CommonLib/Picture.cpp`); oracle: the VTM **encoder** log (`EncoderApp -c encoder_randomaccess_vtm.cfg -c <HDR CTC cfg> --PrintWPSNR=1 --PrintHexPSNR=1`, which prints per-frame and summary wPSNR; `PrintWPSNR` is an encoder-only option — the decoder has none), comparing OVT's `wpsnr` between the VTM source and the VTM reconstruction (`--ReconFile`), tolerance `1e-3 dB`.

**`psnr_l100`, `de100`** (JVET HDR CTC via HDRTools, BSD-3): convert both pictures to linear-light RGB BT.2020 (PQ EOTF), clip to `[0, 100]` cd/m², normalise by 100. `psnr_l100 = 10·log10( 1 / MSE_RGB )` over all three channels jointly. `de100`: convert the normalised linear RGB to CIE XYZ (BT.2020 matrix, §9.2.5 d) with white point = (100 nits → Y = 1), then to CIELAB (D65), then CIEDE2000 (`ΔE00`, Sharma et al. 2005 formulation, k_L = k_C = k_H = 1) per pixel; `de100 = mean ΔE00`, `psnr_de100 = 10·log10( 10000 / mean(ΔE00²) )`. `values = [psnr_l100]` and `[de100, psnr_de100]`. Oracle: HDRTools `HDRMetrics` (`-p EnableTFPSNR=1 -p EnableDE100=1`) pinned tag, tolerance `1e-3 dB`. CIEDE2000 scalar function is shared with nothing else in OVT (libvmaf has its own); it is unit-tested against the 34 Sharma test pairs (exact to `1e-4`).

### 9.4.11 Kernel summary table

| id | domain | per_plane | outputs | oracle | tol |
|---|---|---|---|---|---|
| `psnr` | CodeValue | yes | y,u,v,jvet,ffmpeg_avg,mse_*,map | libvmaf psnr / FFmpeg psnr | 1e-6 / 1e-4 dB |
| `ssim` | CodeValue | yes | y,(u,v),map | libvmaf float_ssim | 1e-4 |
| `ssim_x264` | CodeValue | yes | y,u,v,ssim_db_y | vendored x264 `pixel_ssim.c` oracle | 1e-6 |
| `identity` | CodeValue | yes | y,u,v,all | FFmpeg identity | exact |
| `xpsnr` | CodeValue | yes | y,u,v | FFmpeg 7.1 xpsnr | 1e-3 dB |
| `si_ti` | CodeValue→(2023: linear) | no | si,ti | FFmpeg siti / siti-tools | 1e-3 |
| `de_itp` | ICtCp | no | mean,p95,max,map | NumPy ref / libplacebo | 1e-3 / 1e-4 |
| `pu21_psnr`,`pu21_ssim` | PU21 | yes | y,r,g,b | pu21 (Octave) | 1e-3 dB / 1e-4 |
| `wpsnr` | CodeValue | yes | y,u,v | VTM encoder log (`--PrintWPSNR=1`) | 1e-3 dB |
| `psnr_l100`,`de100` | LinearLight | no | see text | HDRTools | 1e-3 dB |

---

## 9.5 libvmaf host

### 9.5.1 Version and build

* **Pinned version:** libvmaf **≥ 3.0.0** (3.x; D5), models `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1`, `vmaf_b_v0.6.3`, plus newer bundled models when the pinned tag ships them; the exact tag is recorded in `third_party/VERSIONS.md` and `MetricsResult.engine = "libvmaf <tag>"` is filled from `vmaf_version()`.
* **Build** (`third_party/libvmaf.cmake`, ExternalProject, Meson ≥ 1.1 + Ninja): `meson setup build --buildtype release --default-library static -Denable_float=true -Dbuilt_in_models=true -Denable_avx512=true -Denable_nvtx=false -Denable_cuda=false -Denable_docs=false` (these are the options of libvmaf 3.x `libvmaf/meson_options.txt`; there is no `enable_zimg` option). Chapter 14 §14.4.11 builds the oracle `vmaf` CLI with the **same** option set. NASM is required on x86_64 (installed via vcpkg `nasm`). On arm64 the NEON paths (`vif_neon.c` and whatever else the pinned tag has) are compiled automatically; the CMake script fails if `meson introspect` does not list the NEON sources on an arm64 host. For Android a Meson cross-file is generated from the NDK toolchain (`third_party/meson/android-<abi>.ini`).
* `enable_float=true` is needed to compile `float_ssim`/`float_ms_ssim`; **the VMAF integer (fixed-point) VIF/ADM path remains the default** used for `vmaf*` ids, matching the `vmaf` CLI default build. The parity test (§9.5.7) runs the CLI built from the *same* tree with the *same* options.
* Licence: BSD-2-Clause-Patent; the notice is added to `THIRD_PARTY_NOTICES.md` and the About dialog shows the patent grant text.

### 9.5.2 Threading model

* One `VmafContext` per (job, chunk). `VmafConfiguration.n_threads = clamp(threads / chunks, 1, 8)`, `n_subsample = spec.subsample`, `log_level = VMAF_LOG_LEVEL_WARNING`, `cpumask = 0`.
* For sequences longer than 240 frames and `threads > 4`, the job runs `chunks` contexts in parallel (§9.2.3). Each chunk is fed **one extra frame pair before** its range (`lookbehind = 1`, `lookahead = 0`): libvmaf's `motion` feature of frame `t` is the SAD between the blurred reference frames `t−1` and `t` (0 for a frame without predecessor), and `integer_motion2(t) = min(motion(t), motion(t+1))` is computed by libvmaf from already-buffered frames when `t+1` arrives — a chunk that ends at frame `e` therefore gets `motion2(e)` only at flush, where libvmaf uses `motion2(e) = motion(e)` exactly as the CLI does for the last frame of a sequence. To keep per-frame scores identical to a single-context run, a chunk's **last** frame score is discarded and recomputed by the following chunk (which has it as its lookbehind frame); the first chunk's frame 0 and the last chunk's final frame are taken as is. With this overlap the per-frame scores are identical to a single-context run; CI asserts this (`metrics_vmaf_chunking_identity`).
* Pooling across chunks is done by OVT from the per-frame values (§9.8). For the bootstrapped model the CI (p95 lo/hi) is pooled by libvmaf only within a chunk; across chunks OVT pools the per-frame bagging scores and CIs by the same mean rule; the result records `"ci_pooling":"ovt_mean"`.
* `VmafContext` is never shared between threads; `vmaf_read_pictures` is called from the chunk's thread only.

### 9.5.3 Bundled models and OVT ids

All models are compiled in (`built_in_models=true`) and loaded with `vmaf_model_load(&model, &cfg, "<name>")`; the JSON files are also installed to `share/ovt/vmaf/*.json` for the parity test and for users who want to inspect them.

| OVT id | libvmaf model / feature | `VmafModelConfig.flags` | Notes |
|---|---|---|---|
| `vmaf` | `vmaf_v0.6.1` | 0 | default; also drives `vmaf_phone` when option `{"phone":true}` → `VMAF_MODEL_FLAG_ENABLE_TRANSFORM` |
| `vmaf_neg` | `vmaf_v0.6.1neg` | 0 | codec-evaluation variant |
| `vmaf_4k` | `vmaf_4k_v0.6.1` | 0 | |
| `vmaf_b` | `vmaf_b_v0.6.3` (model collection) | 0 | `vmaf_model_collection_load`; values `[bagging, ci_p95_lo, ci_p95_hi, stddev]` |
| `vmaf_<name>` | any further model JSON bundled by the pinned tag (`model/*.json` with `built_in_models`) | 0 | ids generated from the model file names found at build time by `tools/gen_vmaf_ids.py` (`vmaf_float_v0.6.1` → `vmaf_float`, …); the generated list is checked into `core/src/metrics/vmaf/vmaf_models.inc`; absence of a model is not an error |
| `ms_ssim` | feature `float_ms_ssim` | — | luma |
| `psnr_hvs` | feature `psnr_hvs` | — | values `[psnr_hvs, psnr_hvs_y, psnr_hvs_cb, psnr_hvs_cr]` |
| `ciede2000` | feature `ciede` (CLI `--feature ciede`; JSON key `ciede2000`) | — | value `[ciede2000]` |
| `cambi` | feature `cambi` | — | options map 1:1 to libvmaf's (`window_size, topk, tvi_threshold, max_log_contrast, enc_width, enc_height, enc_bitdepth, src_width, src_height, full_ref, eotf (bt1886\|pq), heatmaps_path`); `kind = NoReference` unless `full_ref=true` |
| `vif` | feature `integer_vif` (default) or `float_vif` (option `{"float": true}`; requires `-Denable_float=true`, §9.5.1) — `vmaf_use_feature(ctx, "vif", …)` would fail, libvmaf has no extractor of that name | — | values `[integer_vif_scale0, integer_vif_scale1, integer_vif_scale2, integer_vif_scale3]` (or `[vif_scale0 … vif_scale3]` for `float_vif`, the libvmaf JSON aliases) |
| `vmaf_psnr` | feature `psnr` with `enable_chroma=true, enable_apsnr=true` | — | only used by the parity test and the libvmaf-schema JSON export; the GUI shows OVT's own `psnr` |

Each id is a separate `ovt_MetricApi` exported from `libovt-core` itself (`ovt_metric_vmaf_<id>()`), registered by the host at start-up; third-party plugins use the same ABI.

### 9.5.4 Filling `VmafPicture` from `ovt_MetricPlanes`

```cpp
VmafPixelFormat fmt = (pv.planes == 1) ? VMAF_PIX_FMT_YUV400P
                    : (sub_w == 2 && sub_h == 2) ? VMAF_PIX_FMT_YUV420P
                    : (sub_w == 2)               ? VMAF_PIX_FMT_YUV422P : VMAF_PIX_FMT_YUV444P;
VmafPicture pic; vmaf_picture_alloc(&pic, fmt, pv.bit_depth, pv.width[0], pv.height[0]);
for (int p = 0; p < pv.planes; ++p)
  for (uint32_t y = 0; y < pv.height[p]; ++y)
    memcpy((uint8_t*)pic.data[p] + y * pic.stride[p], pv.data[p] + y * pv.stride_bytes[p], pv.width[p] * pv.sample_bytes);
vmaf_read_pictures(ctx, &ref_pic, &dist_pic, index);   // libvmaf takes ownership and unrefs both pictures
```
Rules: `index` is the 0-based position within the chunk (libvmaf requires consecutive indices starting at 0 per context; OVT keeps a `chunk_index → decode_index` table). Bit depth must be 8, 10, 12 or 16 and **equal for ref and dist** — the normalisation stage guarantees this (`promote_lower`). 8-bit samples are `uint8_t`, higher depths `uint16_t` host-endian (little-endian on all target platforms). A copy is unavoidable because libvmaf frees the picture. `vmaf_read_pictures(ctx, NULL, NULL, 0)` flushes at the end of the chunk.

### 9.5.5 Reading scores

* Per frame: `vmaf_score_at_index(ctx, model, &score, idx)` for model-based ids; `vmaf_feature_score_at_index(ctx, "float_ssim", &score, idx)` etc. for feature ids. Feature names are exactly those libvmaf writes in its JSON (`"vmaf"`, `"integer_motion2"`, `"float_ssim"`, `"psnr_y"`, …).
* Pooled: OVT computes mean/harmonic/min/percentiles itself (§9.8) but **also** calls `vmaf_score_pooled(ctx, model, method, &s, 0, n−1)` once per method — `method` is a single `enum VmafPoolingMethod` value, so three calls: `VMAF_POOL_METHOD_MEAN`, `VMAF_POOL_METHOD_HARMONIC_MEAN`, `VMAF_POOL_METHOD_MIN` (and `VMAF_POOL_METHOD_MAX` for the `max` key of the libvmaf JSON) — in the parity test and asserts `|Δ| ≤ 1e-9`, which pins OVT's pooling to libvmaf's.
* Every feature libvmaf computed for the model (e.g. `integer_adm2`, `integer_vif_scale0..3`, `integer_motion2`) is captured into `FrameScore.values` behind the main score, with names in `descriptor.outputs`, so the libvmaf-schema JSON export (§9.9.2) can be reproduced in full.

### 9.5.6 Bit-depth handling

libvmaf accepts 8–16 bit input and internally scales the VIF/ADM features so VMAF scores are comparable across depths. OVT does **not** convert 10-bit content to 8-bit before VMAF (unlike ffmpeg-quality-metrics' default). When ref and dist differ in depth the lower is promoted (shift), which is exactly what the `vmaf` CLI cannot do (it requires equal `-b`); the parity test therefore uses equal-depth inputs only. The applied depth is in `normalization.applied`.

### 9.5.7 Bit-exact parity test against the `vmaf` CLI

Test `metrics_vmaf_cli_parity` (`core/tests/metrics/vmaf_parity_test.cpp`, runs in CI on macOS arm64 and Linux x86_64):

1. Build the `vmaf` CLI from the same pinned tree with the same Meson options (`-Denable_float=true -Dbuilt_in_models=true`); the ExternalProject installs it as `third_party/install/bin/vmaf`.
2. Inputs: Netflix `python/test/resource/yuv/src01_hrc00_576x324.yuv` (reference) and `src01_hrc01_576x324.yuv` (distorted), 8-bit 4:2:0, 576×324, 48 frames; plus a 10-bit pair generated by `tests/oracles/make_10bit.py` (left shift by 2, lossless).
3. Run the CLI: `vmaf -r ref.yuv -d dist.yuv -w 576 -h 324 -p 420 -b 8 --model version=vmaf_v0.6.1 --model version=vmaf_v0.6.1neg --feature psnr --feature float_ssim --feature float_ms_ssim --feature psnr_hvs --feature ciede --feature cambi --json -o cli.json` and once more with `--subsample 3` and `--threads 4`. (For `.y4m` inputs `-w/-h/-p/-b` are omitted; the CLI reads them from the Y4M header.)
4. Run OVT: `ovt metrics -r ref.yuv --reference-raw 576x324:yuv420p:8 -d dist.yuv --distorted-raw 576x324:yuv420p:8 -m vmaf,vmaf_neg,vmaf_psnr,ssim,ms_ssim,psnr_hvs,ciede2000,cambi --align none --threads 4 --format vmaf-json -o ovt.json` (chapter 12 §12.2.9 grammar, `--format vmaf-json` = the libvmaf-compatible layout of §9.5.8; and with `--subsample 3`). `--align none` and no scaling guarantee the same planes are fed; raw YUV has no colour metadata so no zimg conversion occurs.
5. Compare `cli.json` and `ovt.json`: for every frame and every metric key present in both, the numbers MUST be equal after formatting with libvmaf's own `%.6f`; `pooled_metrics.{mean,harmonic_mean,min,max}` likewise; `version` and `fps` keys are ignored. Any difference fails the test and prints the first differing frame/key.
6. Additionally assert OVT's own `psnr` equals the CLI's `psnr_y/cb/cr` within `1e-6` and OVT's own `ssim` (with the `libvmaf` border policy) equals `float_ssim` within `1e-4`.

### 9.5.8 libvmaf-compatible JSON export

`ovt metrics --format vmaf-json -o <file>` (chapter 12 §12.2.9; exporter `ovt_metrics_export_vmaf_json`, WI-3.310) writes the exact schema `vmaf_write_output(..., VMAF_OUTPUT_FORMAT_JSON)` produces for libvmaf 3.x:

```json
{ "version": "<libvmaf tag>", "fps": 24.0,
  "frames": [ { "frameNum": 0, "metrics": { "integer_adm2": 0.96, "integer_motion2": 0.0, "integer_vif_scale0": 0.5, "...": 0, "psnr_y": 34.1, "float_ssim": 0.93, "vmaf": 76.1 } } ],
  "pooled_metrics": { "vmaf": { "min": 0, "max": 0, "mean": 0, "harmonic_mean": 0 }, "psnr_y": { "min": 0, "max": 0, "mean": 0, "harmonic_mean": 0 } },
  "aggregate_metrics": {} }
```
Metric keys are libvmaf's names; multiple models add `vmaf_neg`, `vmaf_4k` keys; the bootstrapped model adds `vmaf_b_v0.6.3_bagging`, `..._ci_p95_lo`, `..._ci_p95_hi`, `..._stddev` (names as emitted by the pinned libvmaf). Numbers are printed with `%.6f`; `fps` is the reference frame rate. OVT-only metrics never appear in this file; they go to the OVT JSON (§9.9.2).


---

## 9.6 UVQ host (no-reference, ONNX Runtime)

### 9.6.1 Source and pins

* **Source repository:** `google/uvq` (https://github.com/google/uvq), **Apache-2.0** (code and weights). Pinned to a commit hash recorded in `third_party/VERSIONS.md` (`UVQ_COMMIT`). The published models are **TensorFlow SavedModels** (`models/` directory of the repository release, loaded by `uvq_main.py`/`uvq_utils.py` with `tf.saved_model.load`); two model generations are used: **UVQ 1.5** (default) and **UVQ 1.0** (the 2021 model). Sub-networks: ContentNet, CompressionNet, DistortionNet, AggregationNet (research §4b). The exact directory and signature names MUST be read from the pinned commit and recorded in `docs/dev/uvq_preprocessing.md`.
* **ONNX Runtime:** pinned ≥ 1.20 (exact version in `VERSIONS.md`), MIT licence, consumed as the prebuilt C/C++ package per platform (vcpkg `onnxruntime` where available, else `third_party/onnxruntime.cmake` download with SHA-256). Execution providers: **CoreML EP** on macOS (`OrtSessionOptionsAppendExecutionProvider_CoreML`, flags `COREML_FLAG_USE_CPU_AND_GPU`), **XNNPACK EP** on Android (default; the NNAPI EP is deprecated by Android as of API 35 and is used only when the option `"ep":"nnapi"` is given on a device with API < 35, falling back to XNNPACK when NNAPI rejects the graph), **CPU EP** on Linux (and as the universal fallback). `OVT_UVQ_FORCE_CPU=1` forces the CPU EP everywhere (used by golden tests so scores are platform-independent).
* `MetricsResult.engine = "onnxruntime <OrtGetApiBase()->GetVersionString()> / uvq <1.5|1.0> <UVQ_COMMIT[:8]> / ep=<coreml|nnapi|xnnpack|cpu>"`.

### 9.6.2 Model export work item (deliverable spec)

The export is a Python tool `tools/uvq_export/export_uvq.py` (runs in a venv with the pinned TensorFlow 2.x and `tf2onnx`; **TensorFlow is never a runtime dependency of OVT**):

1. Clone `google/uvq` at `UVQ_COMMIT`; download the official SavedModel weights through the repo's own instructions; verify their SHA-256 against `tools/uvq_export/weights.sha256`.
2. For each sub-network and each generation, convert the SavedModel with **tf2onnx** (`python -m tf2onnx.convert --saved-model <dir> --opset 17 --output <file>.onnx`, with `--inputs <name>:0[1,F,H,W,C]` to freeze the batch and frame dimensions — fixed input shapes; the geometry is read from the pinned source, see §9.6.4). tf2onnx is the only converter (`keras2onnx` is archived and does not support TensorFlow ≥ 2.6; tf2onnx also converts Keras SavedModels). When tf2onnx rejects an op, re-implement the failing layer manually in the ONNX graph (or register a tf2onnx custom-op handler in `export_uvq.py`) and document it in `report.json`. CompressionNet takes the fixed frame stack; the aggregator takes the concatenated feature vector (size read from the source and recorded in the preprocessing table).
3. Produce **fp32 reference** models (`uvq15_{content,compression,distortion,aggregation}.onnx`) and **fp16 variants** (`onnxconverter_common.float16.convert_float_to_float16`, keep IO fp32) for Android/CoreML.
4. Run `onnxruntime` CPU on the fp32 models and compare with the reference CLI (`python uvq_main.py --input_files=<clip>,<len_s>,<w>,<h> --output_dir=out --model_dir=models` at the pinned commit; the exact entry point and flags are read from the pinned README) on the **20 YouTube-UGC clips** `ytugc_clip01` … `ytugc_clip20` (entries of the single manifest `tests/corpora/manifest.json`, chapter 14 §14.3.1; mixed resolutions and orientations): MOS `|Δ| ≤ 0.01` per clip; per-chunk labels identical in argmax. fp16: `|Δ| ≤ 0.02` MOS, documented in the export report.
5. Write `docs/dev/uvq_preprocessing.md` (§9.6.4) and `tools/uvq_export/report.json` (per-clip deltas, ORT version, opset, hashes).
6. Upload the `.onnx` files as release assets; write `core/share/models/uvq/manifest.json` (installed to `${CMAKE_INSTALL_DATADIR}/ovt/models/uvq/manifest.json`, chapter 13 §13.2.6 install layout) with URL, size, SHA-256 and licence for each file.

### 9.6.3 Weight distribution

Models are **not** in the repository or the APK/DMG. `manifest.json` (schema below) is bundled; the GUI/CLI downloads on first use into `<cache_root>/models/uvq/`, where `<cache_root>` is the cache root of chapter 06 §6.7 (D32: `$XDG_CACHE_HOME/ovt/` on Linux, `~/Library/Caches/org.openvideotools.ovt/` on macOS, `Context.getCacheDir()/ovt/` on Android), verifies SHA-256, and refuses to load a file whose hash mismatches (`OVT_E_CORRUPT`, code `VQ-UVQ-0002`). `ovt models fetch uvq` does the same from the CLI.

```json
{ "schema": 1, "model": "uvq", "version": "1.5", "source": "https://github.com/google/uvq", "commit": "<UVQ_COMMIT>", "license": "Apache-2.0",
  "files": [ { "name": "uvq15_compression_fp32.onnx", "url": "https://.../uvq15_compression_fp32.onnx", "bytes": 0, "sha256": "<hex>", "precision": "fp32" } ] }
```

### 9.6.4 Preprocessing specification

The host reproduces the reference CLI's preprocessing exactly. **The exact geometry, frames-per-chunk, channel order, mean/std normalisation and interpolation MUST be read from the pinned source (`uvq_main.py`, `uvq_utils.py`, the SavedModel signatures, and the FFmpeg command lines they issue) and written to `docs/dev/uvq_preprocessing.md`; implementers MUST NOT guess them from this chapter.** What this chapter fixes is the *structure*:

1. **Frame sampling.** Option `fps` (default `1`, CLI `--opt uvq.fps=<n>`, chapter 12 §12.2.9; `-1` = every frame). Sampled frame `k` is the displayed frame whose pts is the closest to `k / fps` seconds (the reference uses FFmpeg `fps=` filter semantics; the WI records whether the reference picks nearest or floor and matches it).
2. **Rotation.** Apply the container display matrix (`TrackInfo.rotation`, Chapter 8) so the frame is upright, as the reference does via `--transpose`/ffprobe rotation. Option `{"rotation":"auto"|0|90|180|270}`.
3. **Colour.** Decode → `model_rgb` domain (§9.2.5 g): limited/full range per stream, matrix per stream; result is 8-bit-equivalent sRGB-encoded RGB float. For SDR BT.709 this equals FFmpeg's `format=rgb24` output used by the reference (bit-exact except for rounding, which the ≤ 0.01 MOS tolerance absorbs).
4. **Resize** to the model input geometry with **bilinear** interpolation, implemented by zimg `ZIMG_RESIZE_BILINEAR` on the RGB float planes (the reference uses FFmpeg `scale` → `bilinear` per the research note; the WI confirms the flag from the source and records it). Aspect-ratio handling (stretch vs letterbox) as in the source.
5. **Normalisation** per model (e.g. `/255`, or mean/std) as in the source; **chunking** into 1-second windows of `F` frames; padding of the last partial chunk as in the source.
6. **Inference**: run the three feature nets per chunk, concatenate, run AggregationNet; `compression_content_distortion` is the official MOS.

`docs/dev/uvq_preprocessing.md` MUST contain a table: `model version | input tensor name | shape | dtype | channel order | value range/normalisation | frames per chunk | resize filter | source file:line`.

### 9.6.5 Descriptor and outputs

```json
{ "id": "uvq", "display_name": "UVQ (YouTube UGC MOS)", "kind": "NoReference", "domain": "ModelRGB", "per_plane": false,
  "higher_is_better": true, "range_min": 1.0, "range_max": 5.0, "temporal_unit_frames": 1,
  "outputs": ["mos", "compression", "content", "distortion", "compression_content", "compression_distortion", "content_distortion", "compression_map4"],
  "license": "Apache-2.0 (google/uvq models); MIT (ONNX Runtime)", "version": "uvq 1.5 <commit8>" }
```
* `temporal_unit_frames = 1` on the *sampled* frame sequence (the descriptor counts sampled frames, so the host neither buffers nor overlaps extra frames; the plugin itself accumulates `F` sampled frames per 1-second chunk and emits nothing until a chunk is complete). UVQ scores **chunks** (1 s). The plugin emits one `FrameScore` per chunk with `frame.decode_index` of the chunk's first sampled frame; `values = [mos, compression, content, distortion, compression_content, compression_distortion, content_distortion]` (the seven combinations of the README; `mos` = `compression_content_distortion`). `finish` emits the sequence-level `FrameScore` with the same vector (mean over chunks, as the reference CLI prints).
* **Maps:** per chunk, the CompressionNet 4×4 grid of predicted compression levels and the DistortionNet 2×2 grid (upsampled to 4×4 by repetition) are written as `MapBlob`s with `blocks_w = blocks_h = 4`, `block_size = ceil(W/4)` (`value_kind = 1`, lower level = less compression), `map_blob_id = "uvq/compression/<decode_index>/grid4"` and `"uvq/distortion/<decode_index>/grid4"`. Content labels (top-5 of 3 862 YouTube-8M classes with probabilities) are **not** stored in the FlatBuffer (`FrameScore` has no string field, chapter 06 §6.5); they are written as the side file `<map_dir>/uvq/labels.json` (`{"chunks":[{"decode_index":0,"labels":[["Music",0.42],…]}]}`) next to the maps, and the OVT JSON export (§9.9.2) embeds the same array under `results[].extra.uvq_labels`. Label names come from `share/ovt/models/uvq/labels_yt8m.txt` (exported from the repo).

Options JSON: `{"version":"1.5"|"1.0", "fps": 1, "rotation":"auto", "precision":"fp32"|"fp16", "ep":"auto"|"cpu"}`.

### 9.6.6 Performance budget

Benchmark `bench_metrics_uvq_chunk` (§9.11.3): one 1-second chunk of a 1080p clip at `fps=1`, CPU EP on `m1-air-8gb`, XNNPACK fp16 on `pixel-tablet`. The pass/fail values are those of chapter 14 §14.9.3 (D19); this chapter repeats no numbers.

### 9.6.7 Caveats text (shown by the GUI next to UVQ results and in `ovt metrics -m uvq` stderr once)

> UVQ predicts a mean opinion score (1–5) for user-generated content as learned from the YouTube-UGC dataset. It uses no reference and is **not** a codec-comparison metric: it will not resolve small bitrate or QP differences the way VMAF or PSNR do, and its values are not comparable to VMAF's 0–100 scale. Scores depend on the frame-sampling rate (default 1 frame per second). HDR inputs are tone-mapped to SDR before scoring; UVQ was not trained on HDR. Model: google/uvq v{version} ({commit}), Apache-2.0; runtime: ONNX Runtime {ort_version}, execution provider {ep}.

---

## 9.7 Sidecar protocol (phase 4)

Out-of-process metrics for Python research code (ColorVideoVDP, HDR-VDP-3 via Octave, vmaf-torch). Host: `core/src/metrics/sidecar/`. Not available on Android (no Python runtime); the descriptor loader skips sidecars there.

### 9.7.1 Discovery and launch

* A sidecar is described by a manifest `*.ovt-sidecar.json` in `share/ovt/sidecars/` or `$OVT_SIDECAR_PATH`:
  ```json
  { "schema": 1, "id": "cvvdp", "exec": ["python3", "-m", "ovt_sidecar_cvvdp"], "cwd": null, "env": {"PYTORCH_ENABLE_MPS_FALLBACK": "1"},
    "handshake_timeout_s": 120, "frame_timeout_s": 120, "shm": "posix" }
  ```
* Launch: `posix_spawn` with stdin/stdout pipes, stderr to the OVT log (level info), **restricted environment** (§0.7.8): only `PATH`, `HOME`, `LANG`, `TMPDIR`, `XDG_*`, `PYTHON*`, `CUDA_VISIBLE_DEVICES`, `OMP_NUM_THREADS` and the manifest's `env` are inherited; `OVT_SIDECAR_PROTOCOL=1` is set. The child is placed in its own process group so it can be killed with its descendants.
* Messages are **JSON Lines** (one UTF-8 JSON object per `\n`-terminated line, no embedded newlines, ≤ 1 MiB per line) in both directions. Anything on stdout that is not valid JSON is a protocol error (`VQ-SIDE-0003`).

### 9.7.2 Handshake

1. Child → host, first line within `handshake_timeout_s` (heartbeats `{"status":"loading","message":"..."}` allowed every ≤ 10 s while a model loads):
   `{"ovt_sidecar":1,"descriptor":{<MetricDescriptor as JSON, same field names as the FlatBuffer>},"capabilities":{"shm":["posix"],"domains":["linear_light","code_value"],"max_width":4096,"max_height":2304,"map":true}}`
2. Host → child: `{"cmd":"configure","options":{...plugin options...},"display_model":{"preset":"pq_1000","peak_nits":1000,"black_nits":0.005,"eotf":"pq"},"geometry":{"width":1920,"height":1080,"planes":3,"sample_format":"float32","domain":"linear_light","fps":24.0,"color":{"primaries":9,"transfer":16,"matrix":9,"full_range":0}}}`
3. Child → host: `{"ok":true}` or `{"error":{"code":"unsupported_geometry","message":"..."}}` → host fails the metric with `OVT_E_PLUGIN`.

### 9.7.3 Frame delivery

* The host creates two POSIX shared-memory objects per sidecar (`shm_open`, names `/ovt-<pid>-<job>-ref-<slot>`, `/ovt-<pid>-<job>-dist-<slot>`, `slot ∈ {0,1}` double-buffering), each large enough for one frame in the configured geometry, with this header at offset 0:
  ```c
  struct ovt_ShmFrameHeader {   /* 64 bytes, little-endian */
    char magic[8];               /* "OVTSHM01" */
    uint32_t width, height, planes, sample_format /*0 uint,1 f32*/, bit_depth, domain;
    uint32_t plane_offset[3];    /* byte offsets from start of object */
    uint32_t plane_stride[3];    /* bytes */
    uint32_t frame_number;       /* same as the "frame" field of the score message */
    uint8_t  reserved[4];
  };
  ```
* Host → child: `{"cmd":"score","frame":123,"ref":"shm:///ovt-4242-7-ref-1","dist":"shm:///ovt-4242-7-dist-1","display_index":123}`; `"ref": null` for no-reference metrics. The host MUST NOT overwrite a slot until the child's response for it has arrived.
* Child → host: `{"frame":123,"values":[7.81],"map":{"shm":"/ovt-4242-7-map-1","blocks_w":120,"blocks_h":68,"block_size":16} | null}` — the map object is created by the host at `configure` time when `capabilities.map` is true (size = max blocks at B = 16, float32) and filled by the child.
* End: host `{"cmd":"finish"}` → child `{"values":[...sequence-level...]}` or `{"values":null}`; host `{"cmd":"quit"}` → child exits 0.

### 9.7.4 Timeouts, errors and kill rules

| Situation | Host action |
|---|---|
| No handshake within `handshake_timeout_s` | SIGTERM, wait 5 s, SIGKILL; metric fails `VQ-SIDE-0001` |
| No response to `score` within `frame_timeout_s` | same; `VQ-SIDE-0002` (the job continues with the other metrics) |
| Child exits unexpectedly | `VQ-SIDE-0004` with the last 20 stderr lines in the diagnostic |
| `{"error":...}` for one frame | the frame's values are NaN, diagnostic Warning; after 3 consecutive errors the metric fails |
| Job cancelled | `{"cmd":"quit"}`, wait 2 s, SIGTERM, 5 s, SIGKILL; unlink all shm objects |

The host always `shm_unlink`s its objects on exit, including abnormal exit (a scope guard plus a start-up sweep of stale `/ovt-<dead pid>-*` objects).

### 9.7.5 Reference Python package layout

```
python/ovt_sidecar/                       (GPL-3.0-or-later, separate wheel "ovt-sidecar")
  ovt_sidecar/__init__.py                 protocol helpers: read_lines(), write(), open_shm_frame(), FrameView (numpy)
  ovt_sidecar/base.py                     class SidecarMetric: descriptor() -> dict; configure(cfg); score(ref: np.ndarray|None, dist) -> (values, map|None); finish()
  ovt_sidecar/cvvdp/__main__.py           ColorVideoVDP adapter: pip extra [cvvdp]; maps display_model → cvvdp display spec ("standard_hdr_pq", custom); JOD score, heatmap → map
  ovt_sidecar/hdrvdp3/__main__.py         HDR-VDP-3 adapter via oct2py (GPLv2 runtime dependency, optional extra [hdrvdp3]); Q score, P_det
  ovt_sidecar/vmaf_torch/__main__.py      optional, research cross-check only
  tests/test_protocol.py                  round-trip test with a fake host
share/ovt/sidecars/cvvdp.ovt-sidecar.json, hdrvdp3.ovt-sidecar.json
```
The adapters declare `domain = "linear_light"` and read absolute-luminance float32 RGB; ColorVideoVDP's own display model is set to "photometric input" so OVT's display model is the single source of truth.

---

## 9.8 Pooling and reporting

### 9.8.1 Pooled statistics

For each `values[k]` component over the paired frames (NaN excluded; count of excluded frames reported). The six fixed `MetricsResult.pooled_*` vectors below are **always** produced for every metric and component; any further method of the `ovt.fb.PoolingMethod` enum (chapter 06 §6.5: `Mean`, `HarmonicMean`, `GeometricMean`, `Min`, `Max`, `Median`, `Percentile`, `LogMean`, `MinkowskiP4`, `PerScene`) is produced on request (`--pool <method>[:percentile]`, GUI pooling combo) as a `PoolingResult` in `MetricsResult.pooling[]`; `LogMean` is the PSNR-appropriate `10·log10(mean 10^(x/10))`, `PerScene` carries `scene_index`:

| Field (§0.5.7) | Definition |
|---|---|
| `pooled_mean` | arithmetic mean |
| `pooled_harmonic` | `n / Σ (1 / (v_i + 1))  − 1` — libvmaf's harmonic mean definition (adds 1 to avoid division by zero, subtracts 1 after); verified against `vmaf_score_pooled(... HARMONIC_MEAN)` |
| `pooled_min` | minimum |
| `pooled_p1`, `pooled_p5` | 1st / 5th percentile, **linear interpolation between order statistics** (NumPy default `linear`, `h = (n−1)·p`) |
| `pooled_std` | population standard deviation (`/n`) |

For `higher_is_better = false` metrics (ΔE) the GUI labels p1/p5 as "1 %-worst" using the 99th/95th percentile; the stored fields keep their names and the exporter adds `p95`/`p99` as extra columns for such metrics.

### 9.8.2 Scene segmentation

Scenes are detected once per job on the **reference** (or the distorted stream for NR jobs) and stored in `MetricsResult`-adjacent `SceneTable {cuts:[display_index], scores:[double]}` (Chapter 6 schema addition). Detector = **FFmpeg `scdet`** (`libavfilter/vf_scdet.c`, FFmpeg ≥ 7.1), own implementation (`core/src/metrics/pool/scene_cut.cpp`) that MUST reproduce it exactly:
```
input: every plane p of the decoded frame at full resolution (no downscale; for 4:2:0 the two chroma planes are W/2 × H/2), b = bit depth
sad_t   = Σ_p Σ |P_t,p − P_{t−1},p|                       (scdet get_scene_score(): s->sad() per plane, summed over all nb_planes)
count   = Σ_p W_p · H_p                                   (total sample count of all planes)
mafd_t  = sad_t · 100 / count / (1 << b)                  (scdet: (double)sad * 100.0 / count / (1ULL << bitdepth))
diff_t  = |mafd_t − mafd_{t−1}|
score_t = clip( min(mafd_t, diff_t), 0, 100 )
cut at t when score_t ≥ threshold                         (default threshold 10.0, as scdet's `threshold` default; option "scene.threshold")
frame 0 has no predecessor: no mafd is computed, mafd_{−1} := 0 for frame 1, score_0 = 0 (never a cut)
a frame whose dimensions differ from its predecessor gets score 0 and does not update mafd_{t−1} (scdet behaviour)
```
A luma-only SAD would give different `mafd` values on every stream with chroma and therefore different cuts; OVT MUST sum all planes (D31).
`scdet` is driven on the same decoded frames in the same pixel format (`-vf scdet=threshold=10` on the Y4M dump of the reference); the per-frame `lavfi.scd.score` and `lavfi.scd.mafd` metadata values are the oracle quantities.
Alternative source when VMAF ran: `"scene.source":"vmaf_motion"` uses `integer_motion2 ≥ threshold_motion` (default 15.0). Per-scene pooled rows (mean/min/harmonic per scene) are produced for every metric; the oracle for the detector is FFmpeg `scdet` with `threshold=10` on the Netflix test vectors and on 10 corpus clips: identical cut frame indices, and `|mafd − lavfi.scd.mafd| ≤ 1e-3` per frame (FFmpeg prints `%f`).

### 9.8.3 Frame-type grouping

When a `StreamIndex` exists for the distorted stream, each paired frame carries `FrameInfo.type` (`I/P/B/Key/Inter/IntraOnly/Switch`) and `temporal_id`; pooling produces one row per type (`group = "type:I"`) and per temporal layer (`group = "tid:2"`). `ovt metrics --group-by type,tid,scene` selects the groups in CSV/JSON; each row is a `MetricsReport.groups[]` entry (`PoolingGroup`, chapter 06 §6.5) with the same six pooled vectors as `MetricsResult.pooled_*`.

### 9.8.4 BD-rate and BD-PSNR

Data model (FlatBuffers, `ovt_metrics.fbs`, normative text in chapter 06 §6.5): `RdPoint { bitrate_kbps, quality, label, source_hash, metrics_result_index }` and `BdRateResult { metric_id, plane, pooling, anchor:[RdPoint], test:[RdPoint], interpolator:Interpolator, bd_rate_percent, bd_quality, overlap_low, overlap_high, overlap_fraction, log_base_bitrate, diagnostics, anchor_name, test_name, range_mode }`. An "RD curve" in this section is one `[RdPoint]` vector sorted by `bitrate_kbps` ascending (`BdRateResult.anchor` / `.test`); its name is `anchor_name` / `test_name`, its component is `plane` (255 = combined). In the algorithm below `R` = `bitrate_kbps`, `D` = `quality`, "BD-metric" = `bd_quality`, point counts = `anchor.length` / `test.length`, and warnings are `Diagnostic`s (`VQ-BD-0002` fewer points than the interpolator wants, `VQ-BD-0003` non-monotonic RD curve, `VQ-BD-0004` extrapolated range; registry §9.11.4) in `diagnostics`.
Algorithm (`core/src/metrics/bd/bjontegaard.cpp`), inputs anchor `A = {(R_i, D_i)}`, test `T`, with `R` in kbps and `D` the metric value (higher-is-better metrics; for lower-is-better metrics negate `D` first and record it):

1. Sort each set by `R`; require `n ≥ 4` for `cubic`, `n ≥ 2` for `pchip`/`akima` (Akima needs ≥ 5 points to be truly Akima; with fewer, fall back to PCHIP and add a warning). Reject non-monotonic `D` vs `R` with `OVT_E_INVALID_ARG` unless `"allow_non_monotonic": true` (then sort by `D` for the rate integral).
2. `lr_i = log10(R_i)` (base 10; the ratio is base-independent).
3. **BD-rate:** interpolate `lr = f(D)` for each set:
   * `cubic` — least-squares 3rd-order polynomial in `D` (VCEG-M33; exact interpolation for 4 points) evaluated analytically;
   * `pchip` — piecewise cubic Hermite with the derivative formula of the JCT-VC/JVET BD-rate Excel macro (reproduced by FAU-LMS `bjontegaard` `method='pchip'`), integrated piecewise analytically;
   * `akima` — Akima (1970) spline, integrated piecewise analytically.
   Overlap `[D_lo, D_hi] = [max(min D_A, min D_T), min(max D_A, max D_T)]`. If `D_hi ≤ D_lo` → error `VQ-BD-0001` ("no overlap"). `overlap_fraction = (D_hi − D_lo) / (max(max D_A, max D_T) − min(min D_A, min D_T))`; if `< overlap_min_fraction` (default 0.0 = off; AOM CTC uses the full overlap) add a warning.
   `avg_diff = ( ∫_{D_lo}^{D_hi} f_T(D) dD − ∫_{D_lo}^{D_hi} f_A(D) dD ) / (D_hi − D_lo)`; **`bd_rate_percent = (10^avg_diff − 1) · 100`**.
4. **BD-metric (BD-PSNR):** interpolate `D = g(lr)`, overlap in `lr`, `bd_metric = ( ∫ g_T − ∫ g_A ) / (lr_hi − lr_lo)`.
5. **Range policy** (`BdRateResult.range_mode`, CLI `--range overlap|anchor`, chapter 12 §12.2.10): `overlap` (default, integrate only over the overlap — JCT-VC/JVET and AOM behaviour) or `anchor` (integrate over the anchor's full range, extrapolating the test curve by the chosen interpolator's end pieces; **non-standard**, adds `VQ-BD-0004`).
6. Per-plane and per-metric: `ovt bdrate` runs the computation for every `(metric_id, component)` present in both result sets or selected with `--metric id[/component]`; JVET-weighted PSNR BD-rate uses the `jvet` component; the default report lists `psnr/y,u,v,jvet`, `ssim/y`, `ms_ssim`, `vmaf`, `vmaf_neg`, `psnr_hvs`, `ciede2000`, `apsnr_*` (the AOM CTC set); `uvq/mos` is computed only when named explicitly with `--metric uvq/mos` (research §4b caveat: UVQ is not a codec-comparison metric).

**Oracle:** FAU-LMS `bjontegaard` ≥ 1.2 (BSD-3), all three methods, on the test sets in `core/tests/metrics/golden/bd/*.json` (including the package's own example data and 12 four-point sets from AWCY runs): `|Δ bd_rate_percent| ≤ 1e-6` percentage points and `|Δ bd_metric| ≤ 1e-6`. Also reproduce the JCT-VC Excel sheet values quoted in the package README to the printed decimals.

### 9.8.5 Convex hull and encoding ladder (optional)

`ovt bdrate --hull [--targets <kbps,…>]` (chapter 12 §12.2.10) and the GUI's RD panel accept several RD curves (`[RdPoint]` vectors, one per resolution) and compute the **upper convex hull** in the `(log10 rate, metric)` plane (Andrew's monotone chain over all points, keep the upper chain); hull points carry their source curve name. Option `"hull.space": "log_rate"` (default) or `"linear_rate"` (libaom convexhull_framework behaviour). The **encoding ladder table** lists, for a user-supplied set of target bitrates, the hull point with the highest metric at `rate ≤ target` (resolution, label, rate, metric). Exported as CSV/JSON like any other table.

---

## 9.9 Exports

### 9.9.1 CSV (RFC 4180, UTF-8 without BOM, `\r\n` line ends, `.` decimal separator, fields quoted only when they contain `,`, `"` or newlines)

`ovt metrics --format csv -o <path>` (chapter 12 §12.2.9) writes the per-frame table to `<path>`; the pooled table goes to `<stem>_summary.csv` next to it (`out.csv` → `out_summary.csv`). The C API (`ovt_metrics_export_csv(report, base)`) writes `<base>_frames.csv` and `<base>_summary.csv`.

`_frames.csv` header row (fixed, in this order):
```
decode_index,display_index,frame_type,temporal_id,pts_seconds,scene,metric,component,value,map_blob_id
```
One row per (frame, metric, component). Numbers: `%.6f` for values, `%.6f` for pts; NaN written as empty field; `map_blob_id` empty when no map.

`_summary.csv` header row:
```
group,metric,component,count,mean,harmonic_mean,min,p1,p5,std,apsnr_or_sequence_value
```
`group` ∈ `all`, `scene:<n>`, `type:<T>`, `tid:<n>`. The last column holds the sequence-level value from `finish` when the metric has one (APSNR for `psnr`, sequence XPSNR, UVQ sequence MOS), else empty.

Per-frame-type grouping and scene columns are populated only when the index/scene detection is available; otherwise `frame_type = Unknown`, `temporal_id = 0`, `scene = 0`.

### 9.9.2 JSON

* `--format vmaf-json`: §9.5.8 (VMAF-family and libvmaf features only).
* `--format json` (alias `--json`, chapter 12 §12.1.2): OVT schema `core/schema/metrics_report.schema.json`; the document is the FlatBuffers → JSON reflection of `MetricsReport` (§9.9.3) wrapped with job metadata:
```json
{ "ovt_metrics": 1, "engine_versions": {"ovt":"0.3.0","libvmaf":"3.0.0","onnxruntime":"1.20.1"},
  "spec": {<JobSpec as given>}, "normalization": {<resolved NormalizationConfig incl. applied + alignment>},
  "results": [ { "descriptor": {...}, "frames": [ {"frame":{"decode_index":0,"display_index":0}, "values":[...], "map_blob_id":"", "map_block_size":0} ],
                 "pooled_mean":[...], "pooled_harmonic":[...], "pooled_min":[...], "pooled_p1":[...], "pooled_p5":[...], "pooled_std":[...],
                 "alignment_offset_frames":0, "alignment_confidence":1.0, "engine":"ovt-kernels 1.0",
                 "sequence":{"values":[...]}, "groups":[{"group":"scene:0","first":0,"last":119,"pooled_mean":[...], "...":[]}],
                 "extra": {"uvq_labels": [...]} } ],
  "scenes": {"cuts":[...], "threshold":10.0, "source":"ovt_scdet"} }
```
Keys are emitted in schema order; numbers use the shortest round-trip representation (`std::to_chars`), NaN as `null`. `normalization` here is the export-level object of §9.2.4 (resolved config + `applied` + `alignment`); `results[].normalization` (inside each reflected `MetricsResult`) is the canonical sorted-key string of chapter 06.

### 9.9.3 FlatBuffers

`--format fb -o out.ovtm`: a `MetricsReport` root table (chapter 06 §6.5; scenes live in each `MetricsResult.scenes:[SceneSegment]`) `{ results:[MetricsResult]; bd_rates:[BdRateResult]; reference_name; distorted_name; ovt_version; schema_version:uint = 1; }` written size-prefixed with the file identifier **`"OVTR"`** (chapter 06 §6.5 assigns `"OVTR"` to `MetricsReport` and `"OVTM"` to a bare `MetricsResult` root; a `.ovtm` file always holds a `MetricsReport`, so its identifier is `OVTR`), optionally zstd-compressed (`.ovtm.zst`). This is the format the GUI caches and the Python module reads.

### 9.9.4 Map blob naming

In-memory id: `"<metric_id>/<component>/<decode_index:06>/<B|grid4>"`. On disk: `<map_dir>/<metric_id>/<component>/f<decode_index:06>_b<B>.ovtmap` (§9.4.7 header). A job's `map_dir` defaults to `<cache>/metrics/<job_hash>/maps/` where `job_hash = SHA-256(spec JSON + ref source_hash + dist source_hash)[:16]`.

---

## 9.10 GUI contract

What Chapter 11 (GUI) and Chapter 10 (rendering) MAY assume from this module, via the C API (`ovt/metrics.h`) and the FlatBuffers. The header declares exactly these functions (ABI rules of chapter 12 §12.3: `noexcept`, `ovt_status` return, caller frees every out-buffer with `ovt_buffer_free`); chapter 12 §12.3 lists them in the public C API index:

```c
/* core/include/ovt/metrics.h — metrics C API (WI-3.300 skeleton, WI-3.316 final) */
typedef struct ovt_MetricsJob ovt_MetricsJob;                /* opaque */
typedef struct ovt_MapView { uint32_t struct_size; /* D4 d: = sizeof(ovt_MapView) */ const struct ovt_MapBlobHeader* header; const float* values; } ovt_MapView;
typedef struct ovt_PlanePair { uint32_t struct_size; /* D4 d */ ovt_MetricPlanes ref, dist; } ovt_PlanePair;
typedef void (*ovt_MetricsEventFn)(const char* event_json, void* user);   /* JobEvent as JSON, ≤ 10 Hz */

ovt_status ovt_metrics_list_descriptors(ovt_Buffer* out_descriptors_json);               /* JSON array of MetricDescriptor */
ovt_status ovt_metrics_job_create(const char* spec_json, ovt_MetricsEventFn on_event, void* user, ovt_MetricsJob** out_job);
ovt_status ovt_metrics_job_run(ovt_MetricsJob* job, ovt_Job* cancel);                     /* blocking; call from a worker thread */
ovt_status ovt_metrics_job_results(const ovt_MetricsJob* job, ovt_Buffer* out_report_fb); /* MetricsReport, size-prefixed "OVTR" */
ovt_status ovt_metrics_job_alignment(const ovt_MetricsJob* job, ovt_Buffer* out_alignment_json); /* valid after Aligning */
ovt_status ovt_metrics_job_set_alignment(ovt_MetricsJob* job, int offset_frames, int shift_x, int shift_y); /* WI-4.309 */
ovt_status ovt_metrics_job_planes(ovt_MetricsJob* job, uint32_t display_index, uint32_t domain, ovt_PlanePair* out, ovt_Buffer* out_storage);
ovt_status ovt_metrics_map(const ovt_MetricsJob* job, const char* map_blob_id, ovt_MapView* out, ovt_Buffer* out_storage);
void       ovt_metrics_job_release(ovt_MetricsJob* job);
ovt_status ovt_metrics_export_csv(const void* report_fb, size_t report_size, const char* base_path);
ovt_status ovt_metrics_export_json(const void* report_fb, size_t report_size, int pretty, ovt_Buffer* out);
ovt_status ovt_metrics_export_vmaf_json(const void* report_fb, size_t report_size, ovt_Buffer* out);
ovt_status ovt_metrics_export_fb(const void* report_fb, size_t report_size, int zstd_level, ovt_Buffer* out);
ovt_status ovt_metrics_bd(const char* curves_json, const char* options_json, ovt_Buffer* out_bd_results_fb);  /* [BdRateResult] + hull */
```


1. **Metric list** — `ovt_metrics_list_descriptors()` returns all `MetricDescriptor`s (built-in, plugin, sidecar) with `kind`, `domain`, `outputs`, `higher_is_better`, `range_*`, `license`; the GUI builds its checklist from this and never hard-codes metric names. NR metrics (`kind != FullReference`) disable the reference picker and the diff views.
2. **Job control** — `ovt_metrics_job_create(spec_json)`, `ovt_metrics_job_run(job, ovt_Job*)`, `ovt_metrics_job_results(job)`; progress via `JobEvent` (`state`, `frames_done/total`, `eta_seconds`) delivered on the job thread at ≤ 10 Hz; alignment result available after `Aligning` so the GUI can show offset/confidence before scoring finishes and offer the override (which cancels and re-runs with `temporal.mode = fixed`).
3. **Per-frame series** — `MetricsResult.frames[]` ordered by display index; `values[k]` named by `descriptor.outputs[k]`; the GUI plots any `k` and overlays `FrameInfo.size_bytes` from the index.
4. **Maps** — `ovt_metrics_map(job, map_blob_id, ovt_MapView*)` returns header + float32 pointer; keyed by frame via `FrameScore.map_blob_id`; `value_min/max` give the colour-map range; `map_block_size` gives the grid. UVQ maps are 4×4 grids.
5. **A/B/diff** — the GUI obtains the *normalised* planes for the current frame pair through `ovt_metrics_job_planes(job, display_index, domain, &ref_pv, &dist_pv)` (re-runs the normaliser for one frame, ≤ 50 ms at 1080p) so A/B and difference views show exactly what the metric saw, including scaling and colour conversion; the difference view is `|ref − dist|` amplified by a user gain, rendered by Chapter 10.
6. **Pooled tables** — `pooled_*` vectors plus `groups[]` (scenes, frame types, temporal ids); the GUI renders them without computing statistics itself.
7. **RD / BD** — `ovt_metrics_bd(curves_json, options_json)` returns `BdRateResult[]` and hull data.
8. **Caveats** — descriptors with non-empty `license` text or a `notes` key in their options schema (`xpsnr` patent note, `uvq` caveat §9.6.7) MUST be shown in an info tooltip.

---

## 9.11 Test oracles, corpora and performance tests

### 9.11.1 Corpora (entries of the single manifest `tests/corpora/manifest.json`, chapter 14 §14.3, fetched by `tools/fetch_corpora.py`, SHA-256 pinned)

| Corpus | Content | Used for |
|---|---|---|
| `netflix_src01_hrc00_576x324`, `netflix_src01_hrc01_576x324` (Netflix vmaf `python/test/resource/yuv/`) and the further `netflix` pairs of the pinned tree | 8-bit 4:2:0 576×324, 48 frames (wrapped to Y4M by the fetcher) | libvmaf parity, PSNR/SSIM oracles, pooling, scene detector |
| `ytugc_clip01` … `ytugc_clip20` (YouTube-UGC, https://media.withyoutube.com; `ytugc_clip01` and `ytugc_clip03` are `fast` (chapter 14 §14.3.2, issue #179); mixed 360p–1080p, landscape/portrait) | MP4 H.264 | UVQ parity |
| OVT synthetic `syn_metrics_*` (`syn_metrics_1080p_pair`, `syn_metrics_1080p_bitflip_f37`, `syn_metrics_10bit_8bit_pair`, `syn_metrics_444_420_pair`, `syn_metrics_fps_mismatch_pair`, `syn_metrics_scale_range_pair`, `syn_metrics_ladder_*`, `syn_metrics_pq_1080p_pair`), `syn_align_drop7`, `syn_scenes_5cuts` (chapter 14 §14.3.3 recipes 24, 25, 52–59); in-test frames from `tools/gen_metrics_synth.py` (gradient/noise/flat, 8/10-bit, 4:2:0/4:2:2/4:4:4, generated in memory by the unit tests, not corpus entries) | Y4M | kernel unit tests, HDR domain tests, alignment tests (frame drops, shifts, letterbox inserted programmatically) |
| AWCY sample RD data (`bd/*.json`) | numbers | BD-rate |
| HDR: `syn_metrics_pq_1080p_pair`, `syn_h265_main10_pq_hdr`, `syn_hlg_1080p` and the `patterns`-tagged ids (`syn_pat_pq_patches_1000`, …; chapter 14 §14.3.3 recipes 10, 38, 58, 73) | HEVC Main10 PQ/HLG | HDR domain, wPSNR, ΔE-ITP. Netflix "Sol Levante" is CC BY-NC-ND 4.0 (no derivatives, non-commercial) and therefore **not** used as a corpus. |

### 9.11.2 Oracle tools (`tests/oracles/`, built or installed in CI, never linked)

| Oracle | Source / version | Compared quantity | Tolerance |
|---|---|---|---|
| `vmaf` CLI | pinned libvmaf tree | all libvmaf features and models, pooled | bit-exact at `%.6f` |
| FFmpeg ≥ 7.1 `psnr`, `ssim`, `identity`, `siti`, `xpsnr`, `scdet` | distro/Homebrew FFmpeg, version logged | per-frame stats files / frame metadata | psnr 1e-4 dB on the printed value (FFmpeg prints `%0.2f`, so the comparison rounds OVT to 2 decimals first; identical frames: FFmpeg `inf` ↔ OVT `psnr_max`); identity exact; siti 1e-3; xpsnr 1e-3 dB; scdet exact cut list and mafd 1e-3; ssim documented-different (no test) |
| Daala tools `dump_psnrhvs`, `dump_ciede2000` | xiph/daala pinned | cross-check libvmaf `psnr_hvs`/`ciede` | 1e-3 (ports differ in rounding; failures are investigated not blocked) |
| x264 `pixel_ssim.c` kernel (vendored, GPL oracle executable) | pinned x264 tag | `ssim_x264` Y/U/V | 1e-6 |
| FAU-LMS `bjontegaard` | PyPI, exact version pinned in `tests/oracles/requirements.txt` (`pip index versions bjontegaard` at pin time; the version string in this chapter is not authoritative) | BD-rate/BD-metric, 3 methods | 1e-6 |
| google/uvq reference CLI | `UVQ_COMMIT`, CPU | MOS per clip, labels argmax | 0.01 MOS |
| `siti-tools` | PyPI pinned (defaults `--l-max 300 --l-min 0.1` for the BT.1886 SDR EOTF are reproduced in `display_model` for the 2023 definition) | SI/TI 2023 | 1e-3 |
| gfxdisp/pu21 (Octave), VTM, HDRTools | pinned tags | PU21 metrics, wPSNR, PSNR-L100/DE100 | 1e-3 dB / 1e-4 |
| NumPy references (`*_ref.py`) | in-tree | ICtCp/ΔE-ITP, EOTFs, matrices, Gaussian window, PU21 curve | 1e-4 (float32 path) |

Golden numbers are regenerated by `tests/oracles/regen_golden.sh` and committed with the oracle version in the file name; a test failing after an oracle upgrade requires a reviewed golden update, never a tolerance change.

### 9.11.3 Performance tests (§0.7.4, reference machine M1 MacBook Air, release build, `tests/perf/` harness of chapter 14 §14.9)

Benchmarks live in `tests/perf/metrics/` and follow the `bench_*` naming and budget mechanism of chapter 14 §14.9.3. **Chapter 14 §14.9.3 and its machine form `tests/perf/budgets.json` are the single source of every input, thread count and threshold (D19); this chapter names the benchmarks only and repeats no numbers:** `bench_metrics_psnr_ssim_1080p`, `bench_metrics_psnr_1080p`, `bench_metrics_vmaf_1080p`, `bench_metrics_normalize_1080p_pq`, `bench_metrics_align`, `bench_metrics_maps`, `bench_metrics_uvq_chunk`, `bench_metrics_memory_4k` (all in `tests/perf/metrics/bench_metrics.cpp`, WI-3.317).

Budgets are enforced on the reference runners of chapter 14 §14.9.2 and only recorded elsewhere (chapter 14 §14.9.4 regression policy).

### 9.11.4 Diagnostic registry (`VQ-*`)

Every `VQ-*` code emitted by the metrics module is listed here and nowhere else (D2; format `VQ-<QUALIFIER>-<nnnn>`). Codes are carried in `Diagnostic` records (chapter 06 §6.4) attached to the `MetricsResult`, the `BdRateResult` or the job event stream, and are printed by `ovt metrics`/`ovt bdrate` on stderr.

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `VQ-ALIGN-0001` | Warning | `alignment confidence {confidence:.3f} below {min_confidence}; using offset {offset}` | §9.3.1 step 6, `confidence < min_confidence` | §9.3.1; chapter 04 FR-VQ-003 |
| `VQ-ALIGN-0002` | Warning | `distorted stream is shifted by ({dx},{dy}) samples; crop adjusted` | §9.3.2 best spatial shift ≠ (0,0) | §9.3.2 |
| `VQ-ALIGN-0003` | Warning | `frame counts differ after alignment: ref {ref_frames}, dist {dist_frames}, offset {offset}; {skipped_ref}+{skipped_dist} frames skipped` | §9.3.3, `ref_frames ≠ dist_frames + offset` | §9.3.3 |
| `VQ-FPS-0001` | Error | `frame rates differ ({fps_ref} vs {fps_dist}) and frame_rate.policy = refuse` | §9.3.4 | §9.3.4 |
| `VQ-NORM-0001` | Warning | `{stream}: colour description unspecified; assuming primaries {p} transfer {t} matrix {m} range {r}` | §9.2.4 `colour.assume_unknown_as` applied | §9.2.4 |
| `VQ-NORM-0002` | Info | `scale kernel area requested for upscale; using bicubic` | §9.2.4 target resolution rules | §9.2.4 |
| `VQ-NORM-0003` | Error | `chroma formats differ ({ref_fmt} vs {dist_fmt}); set chroma_upsample.enabled = always to compare in 4:4:4` | §9.2.4 step 3 with `chroma_upsample.enabled = auto`/`never` and different subsampling | §9.2.4; chapter 04 FR-VQ-024 |
| `VQ-HDR-0001` | Error | `metric {id} needs a display model: stream transfer is unknown and no display_model preset is given` | §9.2.5 (c) when `transfer ∈ {2, unknown}` and `display_model.preset = auto` for a `LinearLight`/`PU21`/`ICtCp` metric | §9.2.5, WI-4.301 |
| `VQ-HDR-0002` | Warning | `measured {MaxCLL\|MaxFALL} {measured} cd/m² exceeds declared {declared} cd/m² by {pct:.1f} %` | `ovt hdr --measure --compare-declared` (chapter 12 §12.2.11) and the GUI HDR panel verdict row (chapter 11 §11.3.14): the CTA-861.3 sequence MaxCLL or MaxFALL measured from the decoded pictures exceeds the declared `ovt_ColorDescription.max_cll`/`max_fall` by more than 5 %; raised once per stream per quantity; never raised when no value is declared | CTA-861.3; chapter 10 §10.7.4 |
| `VQ-JOB-0001` | Error | `JobSpec field {path}: {reason}` | `parse_job_spec` validation failure (type, range, unknown key) | §9.2.3, WI-3.312 |
| `VQ-JOB-0002` | Error | `unknown metric id {id}; available: {ids}` | `MetricRequest.id` not in the registry | §9.2.3, WI-3.312 |
| `VQ-JOB-0003` | Error | `metric {id} is full-reference but no reference was given` | §9.2.3 Probing validation | §9.2.3 |
| `VQ-PLUG-0001` | Warning | `metric plugin {path} rejected: {reason}` | `PluginRegistry` cannot load a `libovt_metric_*` (ABI version, missing symbol, descriptor invalid) | §9.2.2, WI-3.300 |
| `VQ-BD-0001` | Error | `RD curves do not overlap in {metric}/{component}` | §9.8.4 step 3, `D_hi ≤ D_lo` | §9.8.4 |
| `VQ-BD-0002` | Warning | `{curve}: {n} points; {interpolator} needs {needed}; using {fallback}` | §9.8.4 step 1 (Akima with < 5 points → PCHIP; cubic with < 4 points is an error, `OVT_E_INVALID_ARG`) | §9.8.4 |
| `VQ-BD-0003` | Warning | `{curve}: quality is not monotonic in bitrate` | §9.8.4 step 1 with `allow_non_monotonic` | §9.8.4 |
| `VQ-BD-0004` | Warning | `range_mode = anchor: test curve extrapolated over [{lo},{hi}]` | §9.8.4 step 5 | §9.8.4 |
| `VQ-BD-0005` | Warning | `overlap fraction {f:.2f} below {min}` | §9.8.4 step 3 `overlap_min_fraction` | §9.8.4 |
| `VQ-UVQ-0001` | Error | `UVQ model files are not present; run 'ovt models fetch uvq' or confirm the download` | §9.6.3 model missing and no download consent | §9.6.3; chapter 12 §12.2.23 |
| `VQ-UVQ-0002` | Error | `UVQ model {file} failed SHA-256 verification` | §9.6.3 hash mismatch (`OVT_E_CORRUPT`) | §9.6.3 |
| `VQ-MODEL-0001` | Error | `model {model_id}: {download\|checksum} failed for {file}: {reason}` | `ovt models fetch\|verify` (chapter 12 §12.2.23) for any downloadable metric model when a download fails or a SHA-256 does not match the bundled `manifest.json` (`OVT_E_IO`, CLI exit 3); the load-time check of a UVQ file raises `VQ-UVQ-0002` instead (D20 rename of `MET-MODEL-0001`) | §9.6.3; chapter 12 §12.2.23 |
| `VQ-UVQ-0003` | Info | `UVQ caveat: {caveat_text}` | once per job that includes `uvq` | §9.6.7 |
| `VQ-SIDE-0001` | Error | `sidecar {id}: no handshake within {timeout} s` | §9.7.4 | §9.7.2, §9.7.4 |
| `VQ-SIDE-0002` | Error | `sidecar {id}: no response for frame {frame} within {timeout} s` | §9.7.4 | §9.7.4 |
| `VQ-SIDE-0003` | Error | `sidecar {id}: protocol error: {detail}` | non-JSON stdout line, line > 1 MiB, unknown message | §9.7.1 |
| `VQ-SIDE-0004` | Error | `sidecar {id} exited with status {status}; last stderr: {tail}` | §9.7.4 unexpected exit | §9.7.4 |
| `VQ-SIDE-0005` | Warning | `sidecar {id}: frame {frame} scored NaN ({message})` | per-frame `{"error":…}` reply | §9.7.4 |
| `VQ-DIFF-0001` | Info | `first differing sample: frame {display_index}, plane {plane}, ({x},{y}): ref {a} dist {b}` | the `identity` metric finds the first mismatch (used by the GUI's first-mismatch finder, chapter 11 §11.3.13) | §9.4.6; chapter 04 FR-PLN-009 |

`VQ-HDR-0002` is the code chapters 11 and 12 use for the measured-vs-declared HDR verdict (D20, formerly `HDR-CLL-0001`).


---

## 9.12 Work items

All WIs below use the backbone template (§0.1.6). Phase-3 items are WI-3.300–WI-3.319 (3.320–3.349 reserved), phase-4 items WI-4.300–WI-4.310 (4.311–4.329 reserved). Unless stated, every WI produces Catch2 tests under `core/tests/metrics/` and is ≤ L. "Chapter 7 decoder" means the software playback decoder `ovt_decoder_*` from Chapter 7; "Chapter 6 schema" means `core/schema/ovt_metrics.fbs` as finalised by the Data Model chapter.

### WI-3.300 — Create the metrics schema, C API skeleton and plugin ABI header
**Goal:** land `ovt_metrics.fbs`, `metric_v1.h`, `metrics.h` and an empty `MetricJob` so every later WI compiles against fixed interfaces.
**Depends on:** WI-0.082 (`ovt_metrics.fbs`), WI-0.083 (generated headers), WI-1.512 (`jobs::Pool`).
**Produces:** `core/schema/ovt_metrics.fbs` (§0.5.7 tables + `RdPoint`, `BdRateResult`, `SceneSegment`, `MetricsReport` — names as in chapter 06 §6.5), `core/include/ovt/abi/metric_v1.h` (BSD-2-Clause, §9.2.2 verbatim + `ovt_MetricPlanes`, `ovt_FrameId`), `core/include/ovt/metrics.h`, `core/src/metrics/host/{metric_job.h,.cpp,plugin_registry.h,.cpp}`, `core/schema/normalization_config.schema.json`.
**Interface:** §9.2.2, §9.2.3, §9.2.4 schema.
**Steps:** 1. Write the `.fbs` and run `flatc --cpp --python --reflect-names`. 2. Write the ABI header; compile it as C11 and C++20 in a test. 3. Implement `PluginRegistry`: static registration of in-tree metrics + `dlopen` of `libovt_metric_*.{so,dylib}` from `OVT_PLUGIN_DIR` (chapter 13 §13.2.6, the same directories and `$OVT_PLUGIN_PATH` rule as decoder plugins, chapter 12 §12.3.11) and `$OVT_METRIC_PATH`, calling `ovt_metric_v1()`. 4. `MetricJob` state machine with events, no scoring yet. 5. JSON-Schema validator for `NormalizationConfig` (use `nlohmann/json-schema-validator`, MIT) with default expansion and deterministic key order.
**Acceptance tests:** 1. Given a `NormalizationConfig` of `{}`, When resolved, Then the output JSON equals `golden/norm_defaults.json` byte for byte. 2. Given `{"scale":{"kernel":"nearest"}}`, When parsed, Then `OVT_E_INVALID_ARG` with message naming `scale.kernel`. 3. Given a dummy plugin `libovt_metric_example`, When the registry loads, Then `ovt_metrics_list_descriptors()` contains id `example`.
**Size:** M.
**Notes / pitfalls:** `MetricsResult.normalization` uses chapter 06's canonical form (sorted keys, no whitespace — V-030); only the pretty export of §9.9.2 uses schema order. The ABI header must not include any C++ or FlatBuffers headers.

### WI-3.301 — Implement the normalisation pipeline (crop, bit depth, range, pairing) without zimg
**Goal:** produce `ovt_MetricPlanes` pairs in `CodeValue` for equal-geometry inputs.
**Depends on:** WI-3.300, WI-2.171 (playback decoder for frame access).
**Produces:** `core/src/metrics/normalize/{pipeline.h,.cpp,crop.cpp,bit_depth.cpp,frame_pairing.cpp}`.
**Interface:** §9.2.4 steps 1, 4, 8; `AlignmentReport` fields `ref_frames,dist_frames,paired_frames,skipped_*`.
**Steps:** 1. Crop via pointer offsets honouring `FrameInfo.crop` and user rects (odd chroma offsets rounded toward the siting). 2. `promote_lower`/`fixed` bit-depth kernels (scalar + Highway). 3. Frame pairing by display index with `offset_frames`; `match_timestamps`, `refuse`, `resample_to_reference` policies. 4. Record `applied.{ref,dist}` descriptions.
**Acceptance tests:** 1. Given 8-bit and 10-bit synthetic frames with identical content, When promoted, Then every sample of the 8-bit frame ×4 equals the 10-bit sample. 2. Given a 29.97 fps dist and 30 fps ref, When `policy=match_timestamps`, Then exactly 1 of every 1000 reference frames is skipped and `skipped_ref_frames` says so. 3. Given the same pair and `policy=refuse`, When the job runs, Then `OVT_E_UNSUPPORTED` with code `VQ-FPS-0001`.
**Size:** M.
**Notes / pitfalls:** never pair by PTS except as the `match_timestamps` tie-breaker; frame rate from the median pts delta, not from the container's nominal rate.

### WI-3.302 — Integrate zimg for scaling, chroma upsampling, range and colour conversion
**Goal:** complete steps 2, 3, 5, 6 of the pipeline with zimg.
**Depends on:** WI-3.301; vcpkg `zimg`.
**Produces:** `core/src/metrics/normalize/zimg_convert.{h,cpp}`, `resample_area.cpp`.
**Interface:** §9.2.4 zimg usage.
**Steps:** 1. Map `ovt_ColorDescription` → `zimg_image_format`. 2. Graph cache keyed by (src fmt, dst fmt, params). 3. `area` box filter kernel for downscale. 4. Record kernel and dither in `applied`.
**Acceptance tests:** 1. Given a 1280×720 BT.709 limited dist and 1920×1080 ref, When normalised with `bicubic`, Then output geometry equals the ref and `normalization.scale.kernel == "bicubic"`. 2. Given a full-range dist and limited ref of the same content, When normalised, Then PSNR_Y ≥ 50 dB (vs ≈ 10 dB lower without range conversion). 3. Given an exact 2:1 downscale with `area`, When normalised, Then each output sample equals the mean of its 2×2 block (exact for integer inputs ×4).
**Size:** M.
**Notes / pitfalls:** zimg wants `active_region` for sub-pixel crops; set `dither_type = NONE` when the destination is float; call `zimg_filter_graph_get_tmp_size` once per graph and align the buffer to 64 bytes.

### WI-3.303 — Implement working-domain conversions (linear_light, pu21, ictcp) and display-model presets
**Goal:** float kernels for §9.2.5 (a)–(f) with presets table.
**Depends on:** WI-3.302.
**Produces:** `core/src/metrics/normalize/{transfer.cpp,matrices.cpp,pu21.cpp,ictcp.cpp,display_model.cpp}`, `tests/oracles/domain_ref.py`.
**Interface:** §9.2.5, display-model presets table.
**Steps:** 1. PQ EOTF/inverse, HLG inverse OETF + OOTF, BT.1886. 2. YCbCr→RGB (709/2020), primaries matrices. 3. PU21 `banding_glare`. 4. ICtCp. 5. Preset resolution (`auto`). 6. Highway versions for PQ/PU21 (use `hwy::Exp`/`Log` from `contrib/math`).
**Acceptance tests:** 1. Given E' = 0.5081 (PQ), When the PQ EOTF is applied, Then L ∈ [100 − 0.1, 100 + 0.1] cd/m². 2. Given HLG E' = 0.75 and Lw = 1000, When the HLG inverse OETF + OOTF is applied, Then L_Y = 203 cd/m² within 1 %. 3. Given the PU21 anchor luminances of §9.2.5 (e), When encoded by `pu21.cpp`, Then every assert of §9.2.5 (e) holds. 4. Given 10 000 random linear RGB triples, When converted to ICtCp by OVT and by `domain_ref.py`, Then max |Δ| ≤ 1e-4. 5. Given a stream with transfer 16 and MDCV max 4000 nits, When the display model resolves with `preset = auto`, Then `preset == "pq_4000"`.
**Size:** M.
**Notes / pitfalls:** PQ `L/10000` normalisation; HLG OOTF applies `Ys^(γ−1)` to all three channels using *scene* luminance; clamp only in RGB-like domains.

### WI-3.304 — Implement temporal alignment
**Goal:** §9.3.1 steps 1–8.
**Depends on:** WI-3.301.
**Produces:** `core/src/metrics/align/temporal.{h,cpp}`, `signature.cpp`.
**Interface:** `AlignmentReport`; config `temporal.*`; CLI `--align auto|N|none` (chapter 12 §12.2.9; mapped by WI-3.530).
**Steps:** 1. 32×18 area signatures with mean removal. 2. NCC score over offsets. 3. Confidence and status. 4. Drift search. 5. Per-GOP re-alignment. 6. Write back `offset_frames`.
**Acceptance tests:** 1. Given `syn_align_drop7` and its reference (test `metrics_align_leading_drop`, §9.3.1; realises FR-VQ-003), When alignment runs with `--align auto`, Then `offset_frames == +7`. 2. Given a dist with 3 duplicated frames inserted at frame 40 and `realign_per_gop`, When alignment runs, Then `segments` contains an offset change of −3 at the next RAP (dist frame `i` now corresponds to ref frame `i − 3`). 3. Given two flat grey streams, When alignment runs, Then `status == "undetermined"` and offset 0. 4. Given `--align 5`, When the job runs, Then `status == "fixed"` and `offset_frames == 5` without decoding probes. 5. Given `syn_align_drop7` with reference and distorted swapped, When alignment runs, Then `offset_frames == −7` (D9 sign convention; chapter 04 FR-VQ-003's fixture is `syn_align_drop7`, there is no `dist_delay3`).
**Size:** M.
**Notes / pitfalls:** NCC norms can be 0; skip those terms. The signature downscale must be the exact area filter, not zimg bicubic, so results are deterministic across platforms.

### WI-3.305 — Implement spatial alignment and crop detection
**Goal:** §9.3.2.
**Depends on:** WI-3.304.
**Produces:** `core/src/metrics/align/spatial.cpp`, `crop_detect.cpp`.
**Interface:** config `spatial.*`, `crop.auto_detect`, `crop.auto_apply` (via `--normalize`).
**Steps:** 1. 25-candidate MSE shift search on 8 frames. 2. Apply shift via crop step. 3. Letterbox detection by row/column statistics. 4. Diagnostics `VQ-ALIGN-0002`.
**Acceptance tests:** 1. Given a dist shifted right by 1 sample, When spatial alignment runs, Then `shift_x == 1, shift_y == 0`. 2. Given a ref with 140-row black bars top and bottom, When crop detection runs, Then `ref_crop_detected == {0,140,W,H−280}`.
**Size:** S.
**Notes / pitfalls:** exclude a 2-sample border from the MSE; the dark threshold scales with bit depth.

### WI-3.306 — Implement the PSNR kernel with combinations, APSNR and maps
**Goal:** §9.4.2 + §9.4.7 for PSNR.
**Depends on:** WI-3.300; vcpkg `highway`.
**Produces:** `core/src/metrics/kernels/{psnr_scalar.cpp,psnr_hwy.cpp,psnr_metric.cpp}`, `tests/oracles/run_ffmpeg_psnr.py` (the map store `core/src/metrics/host/map_store.{h,cpp}` is WI-3.316; this WI only writes into it).
**Interface:** `ovt_metric_v1` for id `psnr`; `ovt_MapBlobHeader`.
**Steps:** 1. SSE per row band (uint64) scalar. 2. Highway version with `hwy::HWY_NAMESPACE::ReduceSum` on widened lanes. 3. Combinations + `psnr_max` rule. 4. APSNR in `finish`. 5. Block maps for B ∈ {8,16,32,64}. 6. Golden tests against libvmaf `psnr_y/cb/cr` (from the parity run) and FFmpeg stats.
**Acceptance tests:** 1. Given the Netflix pair, When `psnr` runs, Then `values[0..2]` equal libvmaf's `psnr_y/cb/cr` within 1e-6 dB on every frame. 2. Given identical inputs at 10-bit, When `psnr` runs, Then `values == 72.0` and the map is all 72.0. 3. Given 100 random frames, When the scalar and the Highway kernels run, Then their SSE values are bit-identical. 4. Given `syn_metrics_1080p_pair` on `m1-air-8gb`, When `bench_metrics_psnr_1080p` runs, Then it meets its chapter 14 §14.9.3 budget.
**Size:** M.
**Notes / pitfalls:** `ffmpeg_avg` is MSE-weighted, not a mean of dBs; FFmpeg prints `inf` for identical frames.

### WI-3.307 — Implement the Gaussian SSIM kernel
**Goal:** §9.4.3 with libvmaf-matching border policy.
**Depends on:** WI-3.306.
**Produces:** `core/src/metrics/kernels/{ssim_scalar.cpp,ssim_hwy.cpp,ssim_metric.cpp}`, `docs/metrics_differences.md`.
**Interface:** id `ssim`, options `planes`, `border`.
**Steps:** 1. Read `libvmaf/src/feature/ssim_tools.c` and document its border handling. 2. Separable 11-tap float convolutions for the five moments (band-tiled). 3. Window formula + double reduction. 4. Map by block mean of window values. 5. Golden test vs `float_ssim`.
**Acceptance tests:** 1. Given the Netflix pair, When `ssim` runs, Then per-frame `ssim/y` equals libvmaf `float_ssim` within 1e-4. 2. Given an 8-bit pair and the same pair promoted to 10-bit (×4), When `ssim` runs on both with a constant offset of +1 (8-bit) / +4 (10-bit) on the distorted samples, Then the two SSIM values are equal within 1e-6 (C1/C2 scale with 1023²). 3. Given `syn_metrics_1080p_pair` on `m1-air-8gb`, When `bench_metrics_psnr_ssim_1080p` runs, Then it meets its chapter 14 §14.9.3 budget.
**Size:** M.
**Notes / pitfalls:** compute σ² as `E[x²] − μ²` in float32 only after the convolution of x² — do not square after filtering; clamp tiny negative variances to 0.

### WI-3.308 — Implement the x264 4×4 SSIM, identity and the kernel plugin registration
**Goal:** §9.4.4 and §9.4.6.
**Depends on:** WI-3.306.
**Produces:** `core/src/metrics/kernels/{ssim_x264.cpp,identity.cpp}`, `tests/oracles/x264_ssim/{pixel_ssim.c,main.c,CMakeLists.txt}` (vendored x264 kernel, GPL-2.0-or-later, oracle only), `tests/oracles/run_x264_ssim.py`.
**Interface:** ids `ssim_x264`, `identity`; `VQ-DIFF-0001`.
**Steps:** 1. 4×4 sums (Highway `SumsOf4`-style), 8×8 window combination stepping 4, integer constants, 2-sample offset, `(W'/4 − 1)·(H'/4 − 1)` window count. 2. `ssim_db`. 3. Identity fractions + SHA-256 (use the io module's SHA-256) + first-mismatch position (`VQ-DIFF-0001`). 4. Vendor the x264 kernel as the oracle executable; golden tests.
**Acceptance tests:** 1. Given the Netflix pair and `syn_metrics_10bit_8bit_pair`'s 10-bit reference against its 10-bit re-encode, When `ssim_x264` runs, Then Y/U/V equal the x264 kernel oracle within 1e-6 at 8-bit and at 10-bit (float path, §9.4.4) and `ssim_db_y == 100` for an identical pair. 2. Given the x264 reconstruction of the Netflix reference (`x264 --qp 20 --ssim --dump-yuv`), When OVT compares source and reconstruction, Then OVT's `ssim_x264/y` mean equals the log's `SSIM Mean Y` within 1e-4. 3. Given ref == dist, When `identity` runs, Then `identity.all == 1.0` and `sha256_match == 1.0`; given `syn_metrics_1080p_bitflip_f37` against its reference, When `identity` runs, Then `all < 1.0`, `sha256_match == 0.0` and `VQ-DIFF-0001` names the sample recorded in `tests/corpora/compare_expected.json`.
**Size:** S.
**Notes / pitfalls:** the `+2+2*stride` offset and `(W−2)×(H−2)` size are part of the definition; `ssim_c2` uses `64·63`, not `64²`; libvpx `tiny_ssim` is a different kernel (no offset, `64²` constants) — do not use it as an oracle (D10).

### WI-3.309 — Build libvmaf via ExternalProject and expose the libvmaf host plugins
**Goal:** §9.5.1–9.5.5 for `vmaf`, `vmaf_neg`, `vmaf_4k`, `vmaf_b`, generated ids for further bundled models, and the feature ids.
**Depends on:** WI-3.300, WI-0.015 (`third_party/libvmaf.cmake`), WI-0.011 (Meson wrapper and Android cross files).
**Produces:** `core/src/metrics/vmaf/{vmaf_host.h,.cpp,vmaf_models.inc}`, `tools/gen_vmaf_ids.py` (the ExternalProject script and the Meson cross files are WI-0.015/WI-0.011 deliverables, see Overlap).
**Interface:** table §9.5.3; `VmafPicture` filling §9.5.4.
**Steps:** 1. Consume the static libvmaf install of WI-0.015 (verify the Meson option set of §9.5.1 from `meson introspect --buildoptions`). 2. Model id generation at build time. 3. Context per chunk, picture copy, index table. 4. Score and feature read-back into `FrameScore.values` with `outputs` names. 5. Bootstrapped collection path. 6. Lookbehind-1 chunk overlap and last-frame recomputation (§9.5.2) and the chunking-identity test.
**Acceptance tests:** 1. Given the Netflix pair, When `vmaf` runs with `threads=1` and `threads=8`, Then the per-frame vectors are identical (`metrics_vmaf_chunking_identity`). 2. Given `vmaf_b` on the Netflix pair, When it runs, Then `values` has 4 entries and `ci_p95_lo ≤ bagging ≤ ci_p95_hi` for every frame. 3. Given an arm64 host, When the build runs, Then `vif_neon.c` appears in the Meson introspection (build fails otherwise). 4. Given `syn_metrics_1080p_pair` on `m1-air-8gb`, When `bench_metrics_vmaf_1080p` runs with 4 threads, Then it meets its chapter 14 §14.9.3 budget. 5. Given metric id `vif`, When the job runs, Then the host calls `vmaf_use_feature` with `integer_vif` and `values` holds the four `integer_vif_scale*` scores.
**Size:** L.
**Notes / pitfalls:** libvmaf frees the pictures you pass; indices must start at 0 per context; the `motion` feature of the first frame of a chunk is wrong without the overlap frame — discard overlap scores, never shift them. **Overlap:** this WI delivers only the libvmaf host plugins; WI-0.015 owns `third_party/libvmaf.cmake` and WI-0.011 the Meson cross files. See chapter 16 §16.3.

### WI-3.310 — Bit-exact parity test against the `vmaf` CLI and libvmaf-schema JSON export
**Goal:** §9.5.7 and §9.5.8.
**Depends on:** WI-3.309, WI-3.306, WI-3.307, WI-3.530 (`ovt metrics` CLI).
**Produces:** `core/tests/metrics/vmaf_parity_test.cpp`, `core/src/metrics/export/libvmaf_json.cpp`, `tests/oracles/make_10bit.py`.
**Interface:** `ovt metrics --format vmaf-json` (chapter 12 §12.2.9) → `ovt_metrics_export_vmaf_json` (§9.10).
**Steps:** 1. Install the CLI from the ExternalProject. 2. Writer reproducing the libvmaf JSON layout. 3. Comparison harness with `%.6f` formatting. 4. Subsample and 10-bit variants.
**Acceptance tests:** 1. Given the Netflix pair, When CLI and OVT (`--format vmaf-json`) run per §9.5.7 steps 3–4, Then every common key is equal at `%.6f` for all 48 frames and pooled values. 2. Given the same pair and `--subsample 3` on both tools, When they run, Then every common key is equal at `%.6f`. 3. Given the 10-bit pair of §9.5.7, When both tools run, Then every common key is equal at `%.6f`.
**Size:** M.
**Notes / pitfalls:** the CLI counts `frameNum` from 0 across the whole sequence even with subsampling; `pooled_metrics` for subsampled runs include only scored frames. **Overlap:** this WI delivers only `core/src/metrics/export/libvmaf_json.cpp` (`ovt_metrics_export_vmaf_json`) and `tests/oracles/make_10bit.py`; the parity test suite is WI-3.591. See chapter 16 §16.3.

### WI-3.311 — Implement pooling, scene detection and frame-type grouping
**Goal:** §9.8.1–9.8.3.
**Depends on:** WI-3.300, WI-3.306, WI-1.121 (`StreamIndex`).
**Produces:** `core/src/metrics/pool/{pooling.cpp,scene_cut.cpp,grouping.cpp}`, `tests/oracles/run_ffmpeg_scdet.py`.
**Interface:** `MetricsResult.pooled_*`, `pooling[]` (`PoolingResult`), `scenes[]` (`SceneSegment`), `groups[]` (export-level, chapter 06 §6.5).
**Steps:** 1. Mean/harmonic/min/p1/p5/std with NaN handling. 2. scdet-equivalent detector. 3. Groups by scene/type/tid. 4. Harmonic-mean parity with `vmaf_score_pooled`.
**Acceptance tests:** 1. Given the Netflix pair VMAF series, When pooled, Then `pooled_harmonic[0]` equals `vmaf_score_pooled(HARMONIC_MEAN)` within 1e-9. 2. Given `syn_scenes_5cuts` and 9 further corpus clips (4:2:0 8-bit and 10-bit), When scene detection runs, Then the cut list equals FFmpeg `scdet=threshold=10` exactly and every per-frame `mafd` is within 1e-3 of `lavfi.scd.mafd` (all planes summed, §9.8.2). 3. Given values `[1,2,3,4,5]`, When pooled, Then `p5 == 1.2` (linear interpolation).
**Size:** M.
**Notes / pitfalls:** libvmaf's harmonic mean adds 1 before inverting; population std, not sample std.

### WI-3.312 — Implement the metrics exporters and the `JobSpec` JSON parser (core side of `ovt metrics`)
**Goal:** The core-side pieces that `ovt metrics` (WI-3.530, chapter 12 §12.2.9) calls: CSV/JSON/FlatBuffers exporters per §9.9 and the `JobSpec` JSON parser.
**Overlap:** reduced per chapter 16 §16.3.8 — no `cli/src/cmd/metrics.cpp`, no grammar here; the command-line grammar, flags and exit codes are those of chapter 12 §12.2.9 and §12.1.3 (`-m/--metric`, `--format json|csv|fb|vmaf-json -o`, `--json` = alias of `--format json`, exit 8 for `--fail-below`, 9 for partial multi-input runs).
**Depends on:** WI-3.301, WI-3.302, WI-3.303, WI-3.304, WI-3.305, WI-3.306, WI-3.307, WI-3.308, WI-3.309, WI-3.311.
**Produces:** `core/src/metrics/export/{csv.cpp,json.cpp,fb.cpp}`, `core/src/metrics/jobspec_json.cpp`, `core/include/ovt/metrics_export.h`; tests `core/tests/metrics/test_metrics_export.cpp`.
**Interface:** `ovt_metrics_export_csv(report, size, base_path)` (writes `<base>_frames.csv` and `<base>_summary.csv`, §9.9.1), `ovt_metrics_export_json(report, size, pretty, out)` (§9.9.2), `ovt_metrics_export_fb(report, size, zstd_level, out)` (§9.9.3, size-prefixed `"OVTR"`) — signatures in §9.10; `ovt::metrics::JobSpec parse_job_spec(std::string_view json, DiagnosticSink&)` accepting the `NormalizationConfig`/`--opt` JSON objects of chapter 12 §12.2.9. Consumed by WI-3.530 (`cli/src/cmd/metrics.cpp`), which maps the chapter 12 flags onto `JobSpec`.
**Steps:** 1. `JobSpec` JSON parser with per-field validation diagnostics (`VQ-JOB-0001…`). 2. CSV exporter (§9.9.1 header, RFC 4180 quoting, `groups[]` rows from `--group-by`). 3. JSON exporter (§9.9.2 schema, reflection-based via WI-0.088). 4. FlatBuffers exporter and the `MetricsReport.groups` population. 5. Atomic writes (temp file + rename) and tests.
**Acceptance tests:** 1. Given a `MetricsReport` with two metrics and 10 frames, When `ovt_metrics_export_csv(.., "out")` runs, Then `out_frames.csv` has the exact header of §9.9.1 and `python -c "import csv"` parses it with 10 columns per row. 2. Given the same report, When exported to JSON, Then it validates against `core/schema/metrics_report.schema.json` (§9.9.2). 3. Given `{"metrics":["nosuch"]}`, When parsed, Then a `VQ-JOB-0002` diagnostic names the available ids and `ovt metrics -m nosuch` (WI-3.530) exits 2. 4. Given an export interrupted by `SIGKILL` during write, When the output directory is listed, Then no partially written `out_frames.csv` exists (only the temp file).
**Size:** M.
**Notes / pitfalls:** write exports atomically (temp file + rename); `--opt uvq.fps=2` is JSON-parsed (`2`), strings need quotes. See chapter 16 §16.3.8 for the WI-3.310 ↔ WI-3.312 cycle removal.

### WI-3.313 — Implement BD-rate / BD-metric with three interpolators and `ovt bdrate`
**Goal:** §9.8.4 and the CLI hook.
**Depends on:** WI-3.312.
**Produces:** `core/src/metrics/bd/{bjontegaard.{h,cpp},pchip.cpp,akima.cpp,cubic.cpp}`, `tests/oracles/run_bjontegaard.py` (`cli/src/cmd/bdrate.cpp` is WI-3.531, see Overlap).
**Interface:** `RdPoint`, `BdRateResult` (chapter 06 §6.5); `ovt_metrics_bd(curves_json, options_json, out)` (signature §9.10, algorithm §9.8.4); the CLI grammar is chapter 12 §12.2.10 (`ovt bdrate --anchor … --test … [--metric id[/component]] [--interpolator pchip|cubic|akima] [--range overlap|anchor] [--rd "kbps:score,…"] [--pool mean|harmonic] [--min-points N] --format json|csv -o F`).
**Steps:** 1. Cubic LS fit + analytic integral. 2. PCHIP with the Excel-macro derivative rule (port from FAU `bjontegaard/bd_rate.py` semantics, re-implemented in C++). 3. Akima. 4. Range policies and `VQ-BD-*` diagnostics. 5. Library entry point reading OVT JSON/`.ovtm` results, extracting `rate_kbps` from the container (`size_bytes·8/duration`) and pooled means (the CLI wrapper is WI-3.531).
**Acceptance tests:** 1. Given `golden/bd/*.json`, When each method runs, Then `|Δ| ≤ 1e-6` vs the FAU package for BD-rate and BD-metric. 2. Given non-overlapping curves, When BD-rate runs, Then `VQ-BD-0001`. 3. Given 3 points with `cubic`, When BD-rate runs, Then `OVT_E_INVALID_ARG` with `VQ-BD-0002`; given exactly 4 points with `cubic`, When BD-rate runs, Then the fit interpolates all four points exactly (the FAU-LMS package's cubic mode requires exactly 4 points per curve; the oracle run uses 4-point sets for `cubic`).
**Size:** M.
**Notes / pitfalls:** integrate in `log10(rate)`; the PCHIP end-point derivative formula is the thing that breaks parity — copy it exactly; sort by rate first. **Overlap:** this WI delivers the BD-rate maths (`core/src/metrics/bd/`) and `tests/oracles/run_bjontegaard.py`; WI-3.531 owns `cli/src/cmd/bdrate.cpp` and `ovt_bdrate`. See chapter 16 §16.3.

### WI-3.314 — UVQ model export tool and preprocessing document
**Goal:** §9.6.2 deliverables.
**Depends on:** none (Python only).
**Produces:** `tools/uvq_export/{export_uvq.py,check_doc.py,weights.sha256,requirements.txt,report.json}`, `docs/dev/uvq_preprocessing.md`, `core/share/models/uvq/manifest.json`, `core/share/models/uvq/labels_yt8m.txt` (installed to `<prefix>/share/ovt/models/uvq/`).
**Interface:** manifest schema §9.6.3; preprocessing table §9.6.4.
**Steps:** 1. Pin `UVQ_COMMIT`, venv, weights fetch + hash. 2. Read the source and fill the preprocessing table (geometry, F, normalisation, resize flag). 3. Export fp32 opset 17, fixed shapes, for 1.5 and 1.0. 4. fp16 variants. 5. Validation run on ≥ 20 YouTube-UGC clips. 6. Manifest with SHA-256.
**Acceptance tests:** 1. Given `ytugc_clip01` … `ytugc_clip20`, When ORT CPU fp32 runs the exported graphs with the Python preprocessing copied from the reference, Then `|Δ MOS| ≤ 0.01` per clip. 2. Given each exported `.onnx` file, When `onnx.checker.check_model` runs, Then no error is raised. 3. Given `docs/dev/uvq_preprocessing.md`, When `tools/uvq_export/check_doc.py` parses it, Then every table row carries a `source file:line` reference.
**Size:** L.
**Notes / pitfalls:** do not guess the input geometry; tf2onnx needs `--opset ≥ 13` for the `tf.nn.silu`/`Swish` patterns of EfficientNet-style nets (use 17); freeze batch size 1 and the frame count with `--inputs name:0[shape]`; if a SavedModel signature uses string tensors (file names) write a thin wrapper that exposes the image tensor input before converting.

### WI-3.315 — UVQ host plugin on ONNX Runtime
**Goal:** §9.6.4–9.6.6 in C++.
**Depends on:** WI-3.314, WI-3.303 (model_rgb), WI-0.017 (`third_party/onnxruntime.cmake`), WI-1.217 (track rotation from the `tkhd` matrix in `TrackInfo`).
**Produces:** `core/src/metrics/uvq/{uvq_metric.cpp,uvq_preprocess.cpp,model_fetch.cpp}` (`third_party/onnxruntime.cmake` is WI-0.017).
**Interface:** descriptor §9.6.5; options JSON; `ovt models fetch uvq`.
**Steps:** 1. ORT session creation with EP selection and `OVT_UVQ_FORCE_CPU`. 2. Model download/verify/cache. 3. Frame sampling, rotation, `model_rgb`, bilinear resize (zimg), normalisation, chunking exactly per `docs/dev/uvq_preprocessing.md`. 4. Inference, seven outputs, maps, labels. 5. Caveat text.
**Acceptance tests:** 1. Given `ytugc_clip01` … `ytugc_clip20`, When `ovt metrics -d clip.mp4 -m uvq` runs with CPU EP, Then sequence MOS within 0.01 of the reference CLI (`metrics_uvq_parity`). 2. Given a tampered model file in `<cache_root>/models/uvq/`, When `uvq` runs, Then `VQ-UVQ-0002` is reported and no inference happens. 3. Given `ytugc_clip01` on `m1-air-8gb`, When `bench_metrics_uvq_chunk` runs, Then it meets its chapter 14 §14.9.3 budget.
**Size:** L.
**Notes / pitfalls:** ORT CoreML EP may silently fall back to CPU for some ops — log the assigned EP per node count; XNNPACK is the Android default (NNAPI is deprecated from API 35 and rejects dynamic shapes, hence fixed shapes either way).

### WI-3.316 — Map store, FlatBuffers export and C API for GUI
**Goal:** §9.4.7 storage, §9.9.3, §9.10 items 2, 4, 5.
**Depends on:** WI-3.306, WI-3.312.
**Produces:** `core/src/metrics/host/{map_store.cpp,c_api.cpp,planes_for_frame.cpp}`, `core/include/ovt/metrics.h` finalised.
**Interface:** `ovt_metrics_job_create/run/results/map/planes`, `ovt_MapView`.
**Steps:** 1. LRU map store with disk spill. 2. `.ovtm` writer/reader with zstd. 3. One-frame renormalisation for A/B/diff. 4. Progress callback marshalling.
**Acceptance tests:** 1. Given a job with `map_block=16` at 1080p, When `ovt_metrics_map` is called, Then it returns `blocks_w == 120, blocks_h == 68`. 2. Given `.ovtm` written and read back, When the bytes are compared, Then the FlatBuffers are identical (determinism). 3. Given `ovt_metrics_job_planes` for frame 10, When called, Then the returned dist plane equals what the kernels received (hash comparison).
**Size:** M.
**Notes / pitfalls:** map ids must be stable strings; the planes call must not touch the job's decoders (use its own).

### WI-3.317 — Performance test suite and Android build of the metrics module
**Goal:** §9.11.3 benchmarks; metrics compile and run on Android (arm64-v8a, x86_64) without sidecars.
**Depends on:** WI-3.306–3.315, WI-0.560 (perf harness).
**Produces:** `tests/perf/metrics/bench_metrics_*.cpp` (Google Benchmark, chapter 14 §14.9.1), budget rows in `tests/perf/budgets.json`, Android instrumentation test `android/app/src/androidTest/java/org/ovt/MetricsParityTest.kt` running `psnr`+`ssim`+`vmaf` on a bundled 720p clip.
**Interface:** chapter 14 §14.9.3 budget table; `OVT_UVQ_FORCE_CPU`.
**Steps:** 1. Benchmarks with warm-up and median of 3 on the shared harness. 2. Budget rows per §9.11.3. 3. Android: ORT XNNPACK, libvmaf cross build (WI-0.011 cross files), test APK.
**Acceptance tests:** 1. Given the `m1-air-8gb` and `pixel-tablet` runners, When `bench_metrics_*` run, Then every row of chapter 14 §14.9.3 for that device passes. 2. Given the Android reference device of chapter 14 §14.9.2 (`pixel-tablet`, SDR panel — HDR is irrelevant for metrics), When the instrumentation test runs, Then `psnr`, `ssim_x264` and `identity` equal the desktop run bit for bit, `ssim` (float) is within 1e-4 relative, and `vmaf` (fixed-point) is bit-identical (if the pinned libvmaf's NEON and AVX2 paths disagree, the delta is recorded in `docs/metrics_differences.md` and the test asserts ≤ 1e-6).
**Size:** M.
**Notes / pitfalls:** libvmaf's fixed-point paths are designed to be bit-exact across SIMD flavours; if they are not on a given tag, open an upstream issue and note the delta in `docs/metrics_differences.md`.

### WI-3.318 — Example third-party metric plugin and plugin developer guide
**Goal:** prove FR-VQ-002.
**Depends on:** WI-3.300, WI-3.312.
**Produces:** `plugins/ovt-metric-example/` (CMake project, MIT, implements `msad` mean SAD), `docs/dev/metric_plugins.md`.
**Interface:** `ovt_metric_v1` (§9.2.2); `ovt plugins --metrics` (chapter 12 §12.2.14).
**Steps:** 1. Minimal plugin with descriptor and score. 2. Build/install instructions. 3. Guide covering domains, maps, options, determinism.
**Acceptance tests:** 1. Given the template built as `libovt_metric_example.so`, When placed in `OVT_PLUGIN_DIR` (`${CMAKE_INSTALL_LIBDIR}/ovt/plugins/`, chapter 13 §13.2.6; there is no `share/ovt/metrics/`), Then `ovt plugins --metrics` lists id `msad` without rebuilding `libovt-core` (FR-VQ-001 discovery rule). 2. Given the Netflix pair, When `msad` runs, Then it equals FFmpeg `msad` within 1e-4.
**Size:** S.
**Notes / pitfalls:** the example must compile as C11 against `metric_v1.h` alone (no C++ headers) to prove the ABI is usable from C.

### WI-3.319 — Documentation of metric differences and About-dialog notices
**Goal:** `docs/metrics_differences.md` (FFmpeg ssim vs Wang vs x264; bicubic kernels; harmonic mean; APSNR vs mean PSNR) and licence/patent notices for libvmaf, Highway, zimg, ORT, UVQ.
**Depends on:** WI-3.307, WI-3.309, WI-3.315.
**Produces:** `docs/metrics_differences.md`; entries in the `THIRD_PARTY_NOTICES.md` generator.
**Interface:** `MetricDescriptor.license` strings (§9.2.2); chapter 11 §11.2.3 About dialog.
**Steps:** 1. Collect the documented differences from WI-3.307/3.308/3.309 (border policy, x264 float vs double, bicubic kernels, harmonic mean, APSNR). 2. Licence/patent notices for libvmaf, Highway, zimg, ORT, UVQ, xpsnr. 3. Wire into the notices generator.
**Acceptance tests:** 1. Given the repository, When the licence gate (chapter 14 §14.3.4) runs, Then it passes. 2. Given the About dialog (Chapter 11), When opened, Then it lists every engine with its licence string from `descriptor.license`.
**Size:** S.
**Notes / pitfalls:** the xpsnr patent caveat text must be identical in `descriptor.license`, the About dialog and this document.

### Phase 4 work items

### WI-4.300 — Port XPSNR as a separately built plugin
**Goal:** §9.4.8.
**Depends on:** WI-3.306, WI-3.318.
**Produces:** `plugins/ovt-metric-xpsnr/` (LGPL-2.1+, option `OVT_BUILD_METRIC_XPSNR`), `tests/oracles/run_ffmpeg_xpsnr.py`.
**Interface:** id `xpsnr`, `values=[y,u,v]`, `finish` → sequence `[y,u,v]`; `descriptor.license` includes the patent caveat text.
**Steps:** 1. Transcribe block size, high-pass kernel, activity floor, temporal switch and `a_pic` from FFmpeg 7.1 `vf_xpsnr.c` into `xpsnr_core.cpp` with the source line numbers in comments. 2. Scalar then Highway version. 3. Golden test vs FFmpeg stats file for 8- and 10-bit.
**Acceptance tests:** 1. Given the Netflix pair (30 fps) and `syn_metrics_fps_mismatch_pair`'s 60 fps reference against itself shifted by one frame, When `xpsnr` runs, Then per-frame and sequence XPSNR Y/U/V are within 1e-3 dB of FFmpeg 7.1 (both temporal-term branches). 2. Given `OVT_BUILD_METRIC_XPSNR=OFF`, When the build is installed, Then `ovt plugins --metrics` does not show `xpsnr` and the licence gate output omits it.
**Size:** M.
**Notes / pitfalls:** the temporal term uses the *reference* previous frame(s); first frame has no temporal activity; frame rate ≥ 32 switches to second-order differences; chroma reuses the luma block weights.

### WI-4.301 — HDR domain metrics: PU21-PSNR/SSIM and ΔE-ITP with maps
**Goal:** §9.4.10 `pu21_psnr`, `pu21_ssim`, `de_itp`.
**Depends on:** WI-3.303, WI-3.306, WI-3.307.
**Produces:** `core/src/metrics/kernels/{pu21_metrics.cpp,de_itp.cpp}`, `tests/oracles/{de_itp_ref.py,run_pu21.m}`.
**Interface:** ids and `values` per §9.4.10; `display_model` required (job fails with `VQ-HDR-0001` when the stream transfer is unknown and no preset is given).
**Steps:** 1. Float-plane PSNR/SSIM with `peak = V(100)`. 2. ΔE-ITP per pixel + block maps + p95. 3. Octave oracle run. 4. Record `peak` used in the result JSON.
**Acceptance tests:** 1. Given `syn_metrics_pq_1080p_pair`, When `pu21_psnr` and `pu21_ssim` run, Then `pu21_psnr/y` is within 1e-3 dB of the Octave run and `pu21_ssim` within 1e-4. 2. Given ref == dist, When `de_itp` runs, Then `de_itp.mean == 0`. 3. Given a BT.2020 PQ pair with a +1 code-value luma offset, When `de_itp` runs, Then `de_itp.mean` within 1e-3 of `de_itp_ref.py`.
**Size:** M.
**Notes / pitfalls:** HLG content must go through the display-model OOTF before PU21/ICtCp; ΔE-ITP uses PQ-encoded LMS even for HLG sources.

### WI-4.302 — JVET HDR CTC metrics: wPSNR, PSNR-L100, DE100
**Goal:** §9.4.10 `wpsnr`, `psnr_l100`, `de100`.
**Depends on:** WI-4.301.
**Produces:** `core/src/metrics/kernels/{wpsnr.cpp,linear_metrics.cpp,ciede2000.cpp}`, oracle scripts for VTM and HDRTools.
**Interface:** ids per §9.4.10.
**Steps:** 1. Verify and port the VTM luma weight table. 2. Linear-light clip/normalise at 100 nits, PSNR-L100. 3. CIEDE2000 scalar + Sharma 34-pair test; DE100 and PSNR_DE100. 4. Oracle runs.
**Acceptance tests:** 1. Given the 34 Sharma pairs, When `ciede2000` runs, Then ΔE00 equals the published values within 1e-4. 2. Given a PQ corpus clip encoded by the VTM encoder with `--PrintWPSNR=1` and `--ReconFile`, When OVT compares source and reconstruction, Then `wpsnr/y` is within 1e-3 dB of the encoder log's per-frame wPSNR. 3. Given the same clip, When OVT and HDRTools run, Then `psnr_l100` and `de100` agree within 1e-3.
**Size:** M.
**Notes / pitfalls:** VTM weights are indexed by *10-bit reference* luma; chroma samples take the co-located luma weight.

### WI-4.303 — SI/TI (P.910 2008 and 2023)
**Goal:** §9.4.9.
**Depends on:** WI-3.303.
**Produces:** `core/src/metrics/kernels/si_ti.cpp`, `docs/si_ti.md`, `tools/check_doc_refs.py` (checks that every constant row of a doc table carries a `file:line` reference), `tests/oracles/run_ffmpeg_siti.py`, `tests/oracles/run_siti_tools.py`.
**Interface:** id `si_ti`, options `definition`, `domain`; `values = [si, ti]`.
**Steps:** 1. Sobel/std kernels (2008). 2. EOTF → PQ/PU21 re-encode path reusing WI-3.303 (2023). 3. Oracle scripts and goldens. 4. `docs/si_ti.md`.
**Acceptance tests:** 1. Given 5 clips, When the 2008 definition runs, Then SI/TI are within 1e-3 of FFmpeg `siti`. 2. Given the same clips plus one PQ clip, When the 2023 definition runs, Then SI/TI are within 1e-3 of `siti-tools` (pq and pu21 domains). 3. Given `docs/si_ti.md`, When reviewed by `tools/check_doc_refs.py`, Then every scaling constant carries a `siti-tools` file:line reference.
**Size:** S.
**Notes / pitfalls:** interior pixels only for Sobel; population std; first frame TI is NaN, not 0.

### WI-4.304 — Sidecar protocol host
**Goal:** §9.7.1–9.7.4.
**Depends on:** WI-3.300, WI-3.303.
**Produces:** `core/src/metrics/sidecar/{sidecar_host.{h,cpp},shm_posix.cpp,jsonl.cpp}`, `core/tests/metrics/sidecar_fake.py`.
**Interface:** manifest schema, messages, `ovt_ShmFrameHeader`.
**Steps:** 1. Manifest discovery. 2. `posix_spawn` with restricted env and process group. 3. JSONL reader with 1 MiB line cap. 4. shm double-buffering. 5. Timeouts/kill/unlink; stale-object sweep.
**Acceptance tests:** 1. Given `sidecar_fake.py` (echoes MSE), When a job runs it on the Netflix pair, Then values equal OVT `psnr.mse_y`. 2. Given a sidecar that never answers, When a job runs it, Then the metric fails with `VQ-SIDE-0002` after `frame_timeout_s`, the process is gone, and no `/ovt-*` shm object remains. 3. Given a sidecar printing non-JSON, When a job runs it, Then `VQ-SIDE-0003` is reported.
**Size:** L.
**Notes / pitfalls:** macOS limits shm names to 31 chars — keep names short (`/ovt-<pid>-<j>-r0`); check with `PSHMNAMLEN`.

### WI-4.305 — `ovt-sidecar` Python package with ColorVideoVDP adapter
**Goal:** §9.7.5 cvvdp adapter.
**Depends on:** WI-4.304.
**Produces:** `python/ovt_sidecar/` package, `python/ovt_sidecar/manifests/cvvdp.ovt-sidecar.json` (installed to `<prefix>/share/ovt/sidecars/`), `docs/dev/metrics_sidecars.md`.
**Interface:** §9.7.5 package layout; protocol §9.7.2–§9.7.3.
**Steps:** 1. Protocol helpers and `SidecarMetric` base class. 2. cvvdp adapter with display-model mapping. 3. Fake-host round-trip test. 4. Manifest and docs.
**Acceptance tests:** 1. Given ColorVideoVDP installed (MPS on macOS, CPU on Linux CI), When `ovt metrics -m cvvdp --display-model pq_1000` runs on a 10-frame PQ clip, Then the JOD score equals `cvvdp --ref --test --display standard_hdr_pq` within 1e-3. 2. Given the same run, When the sidecar returns its heatmap, Then it is stored as a `MapBlob` with `block_size == 16` and `ovt_metrics_map` returns it.
**Size:** M.
**Notes / pitfalls:** feed photometric (absolute cd/m²) frames and set cvvdp's display to photometric mode; do not let cvvdp re-apply an EOTF.

### WI-4.306 — HDR-VDP-3 adapter (Octave)
**Goal:** `hdrvdp3` sidecar via `oct2py`, GPLv2 kept out of the core.
**Depends on:** WI-4.305.
**Produces:** `python/ovt_sidecar/hdrvdp3/`, `python/ovt_sidecar/manifests/hdrvdp3.ovt-sidecar.json`.
**Interface:** §9.7.5; `oct2py` extra `[hdrvdp3]`.
**Steps:** 1. Octave session management via `oct2py`. 2. Frame hand-over as double arrays in cd/m². 3. Heartbeats during model load. 4. Tests.
**Acceptance tests:** 1. Given the HDR-VDP-3 example images, When scored, Then `Q` equals the MATLAB/Octave reference within 1e-3. 2. Given a sidecar whose model load takes 90 s, When it starts, Then the handshake survives through `{"status":"loading"}` heartbeats.
**Size:** M.
**Notes / pitfalls:** HDR-VDP-3 is GPLv2 — it is a runtime dependency of the separately licensed sidecar wheel only, never of `libovt-core`.

### WI-4.307 — CAMBI heatmap and PQ EOTF option
**Goal:** expose libvmaf CAMBI heatmaps as OVT maps and `eotf=pq` for HDR.
**Depends on:** WI-3.309, WI-3.316.
**Produces:** `core/src/metrics/vmaf/cambi_maps.cpp`.
**Steps:** 1. Pass `heatmaps_path` to a temp dir; read libvmaf's per-scale heatmap files after `finish`; convert to `MapBlob`s (`cambi/heat<scale>/<frame>/<B>`). 2. `eotf` option plumbing; `display_model` → `enc_bitdepth` etc.
**Interface:** `cambi` options table §9.5.3; `MapBlob` naming §9.9.4.
**Acceptance tests:** 1. Given a banding-heavy synthetic clip, When `cambi` runs with `heatmaps_path`, Then maps are produced for every scored frame. 2. Given the same clip, When the `vmaf` CLI runs with the same `cambi` options, Then per-frame `cambi` is bit-exact.
**Size:** S.
**Notes / pitfalls:** heatmaps are written by libvmaf at flush time, so maps are only available after the chunk finishes.

### WI-4.308 — Convex hull and encoding-ladder table
**Goal:** §9.8.5.
**Depends on:** WI-3.313.
**Produces:** `core/src/metrics/bd/convex_hull.cpp`; the `--hull`/`--targets` options of `ovt bdrate` (chapter 12 §12.2.10) are wired by WI-3.531.
**Interface:** `ovt_metrics_bd` options `{"hull":true,"hull.space":"log_rate"|"linear_rate","targets":[kbps…]}` (§9.8.5).
**Steps:** 1. Monotone-chain upper hull in `(log10 rate, metric)`. 2. `linear_rate` mode. 3. Ladder table. 4. Golden test against libaom `convexhull_framework`.
**Acceptance tests:** 1. Given the libaom convexhull_framework sample data, When the hull is computed in `linear_rate` mode, Then hull points equal `CalcBDRate.py`'s hull. 2. Given targets `500,1000,2000`, When the ladder is built, Then the ladder picks the max-metric hull point with rate ≤ each target.
**Size:** S.
**Notes / pitfalls:** keep hull points tagged with their source curve name; ties at equal rate keep the higher metric.

### WI-4.309 — Per-GOP alignment UI hook and manual override API
**Goal:** expose `AlignmentReport.segments` and `ovt_metrics_job_set_alignment(job, offset, shift_x, shift_y)` for the GUI's override flow (§9.10 item 2).
**Depends on:** WI-3.304, WI-3.316.
**Produces:** `core/src/metrics/host/c_api.cpp` (additions), `core/tests/metrics/test_alignment_override.cpp`.
**Interface:** `ovt_metrics_job_set_alignment`, `ovt_metrics_job_alignment` (§9.10).
**Steps:** 1. Expose `segments` in the alignment JSON. 2. Override entry point that cancels and re-runs with `temporal.mode = fixed`. 3. Test.
**Acceptance tests:** 1. Given a running job, When the override is set, Then the job cancels, re-runs with `temporal.mode = fixed` and the export's `normalization.alignment.status == "fixed"`.
**Size:** S.
**Notes / pitfalls:** the override must not race with `Scoring`: cancel, join, then restart on the same `jobs::Pool`.

### WI-4.310 — Python bindings for metrics
**Goal:** `ovt.metrics.run(spec: dict) -> MetricsResultSet`, `ovt.metrics.bd_rate(...)`, NumPy views of maps.
**Overlap:** superseded by WI-4.542 (Python metrics and BD-rate bindings); its acceptance test moves to WI-4.542. See chapter 16 §16.3.
**Depends on:** WI-3.316, WI-4.540 (superseded by WI-4.542).
**Produces:** none here (WI-4.542 owns `python/ovt/metrics.py`).
**Interface:** chapter 12 §12.4 Python API.
**Steps:** none (superseded).
**Acceptance tests:** 1. Given the Netflix pair, When `ovt.metrics.run(...)` executes, Then `['vmaf'].frames[0].values[0]` equals the CLI JSON and maps come back as `np.ndarray` float32 of shape `(blocks_h, blocks_w)` (executed under WI-4.542).
**Size:** M.
**Notes / pitfalls:** kept only so that references to WI-4.310 resolve; do not implement separately.

---

## 9.13 Glossary additions

### Glossary additions (chapter 9)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

* **Alignment (temporal / spatial)** — determining the frame offset (and optional pixel shift) between the distorted and reference streams before pairing frames; reported with a confidence.
* **APSNR** — "average PSNR" computed from the *mean MSE over all frames* (`10·log10(peak²/mean MSE)`), as used in the AOM CTC; differs from the mean of per-frame PSNR.
* **BD-rate / BD-PSNR (Bjøntegaard delta)** — percentage bitrate difference (or metric difference) between two RD curves over their overlapping quality (or rate) range, computed by integrating interpolated curves (VCEG-M33 cubic; JCT-VC/JVET PCHIP; Akima).
* **Block map / MapBlob** — a per-frame grid of metric values, one per B×B block of luma samples (`U8`/`U16`/`F32` in memory per chapter 06 `MapBlob.sample_type`; exported as float32 with the 32-byte `OVTMAP01` header).
* **CAMBI** — Contrast-Aware Multiscale Banding Index (Netflix, in libvmaf), a banding detector with optional heatmaps.
* **Code value (domain)** — the integer sample values as stored in the stream (Y'CbCr, 8–16 bit); the default metric domain (what encoders optimise).
* **Confidence (alignment)** — margin between the best and second-best NCC alignment scores; below `min_confidence` the result is flagged.
* **Display model** — the assumed display (EOTF, peak and black luminance, HLG system gamma) needed to convert code values into absolute light; presets `pq_1000`, `pq_4000`, `pq_10000`, `hlg_1000`, `bt1886_100`.
* **ΔE-ITP** — colour-difference metric of ITU-R BT.2124 in the ICtCp space (T = 0.5·Ct), scaled by 720.
* **Full-reference / no-reference metric** — a metric comparing distorted to reference frames (PSNR, SSIM, VMAF) vs one scoring the distorted video alone (UVQ, CAMBI in NR mode, SI/TI).
* **Harmonic mean (libvmaf)** — `n / Σ 1/(v_i+1) − 1`; penalises low frames.
* **Highway** — Google's portable SIMD library (Apache-2.0) used for OVT's kernels with runtime dispatch (SSE4/AVX2/AVX-512/NEON).
* **ICtCp** — BT.2100 colour representation (intensity, tritan, protan axes) built from PQ-encoded LMS.
* **Identity (metric)** — fraction of identical samples; the lossless check.
* **JVET-weighted PSNR** — `(6·PSNR_Y + PSNR_U + PSNR_V)/8`.
* **libvmaf** — Netflix's VMAF library (BSD-2-Clause-Patent) hosting VMAF models and the PSNR/PSNR-HVS/SSIM/MS-SSIM/CIEDE2000/CAMBI features.
* **Linear light (domain)** — display-referred absolute luminance per channel (cd/m²) after the display model's EOTF/OOTF.
* **MetricJob** — the object that runs one comparison through probing, alignment, normalisation, scoring and pooling.
* **MOS** — mean opinion score (1–5); UVQ predicts it.
* **MS-SSIM** — multi-scale SSIM (5 scales, Wang 2003 weights).
* **NCC** — normalised cross-correlation of two mean-removed signatures; the alignment score.
* **Normalisation stage** — the fixed pipeline `crop → scale → chroma_upsample → bit_depth → range → colour → domain → temporal` with a declarative, recorded `NormalizationConfig`.
* **ONNX Runtime (ORT)** — Microsoft's inference engine (MIT) used to run exported UVQ models with CoreML/XNNPACK/CPU execution providers (NNAPI only on request, API < 35).
* **Pooling** — reducing per-frame scores to mean, harmonic mean, min, p1, p5, std, and per-scene/per-frame-type groups.
* **PU21** — perceptually uniform encoding of absolute luminance (Mantiuk & Azimi 2021; `banding_glare` variant) enabling PU-PSNR/PU-SSIM on HDR content.
* **PSNR-HVS(-M)** — DCT-domain PSNR with contrast-sensitivity weighting (and masking), from Daala tools, available in libvmaf.
* **RdPoint / RD curve** — a (`bitrate_kbps`, `quality`, `label`) triple and a sorted `[RdPoint]` vector of them for one encoder configuration (chapter 06 §6.5).
* **Scene cut (scdet)** — frame where `min(mafd, |Δmafd|) ≥ 10`, with `mafd` = mean absolute frame difference summed over all planes as a percentage of full scale (`sad·100/N/2^b`, `N` = sample count of all planes), exactly as FFmpeg `vf_scdet.c` computes it.
* **Sidecar (metric)** — an out-of-process metric implementation speaking the JSON Lines + shared-memory protocol of §9.7 (Python research metrics).
* **Signature (alignment)** — 32×18 area-downscaled, mean-removed luma thumbnail of a frame.
* **SI / TI** — ITU-T P.910 spatial (Sobel std) and temporal (frame-difference std) information; 2008 and 2023 definitions.
* **SSIM (Gaussian) / SSIM x264** — Wang et al. 11×11 σ=1.5 SSIM vs the fast variant printed by x264/x265 (4×4 integer sums combined into 8×8 windows stepping by 4; §9.4.4). libvpx's `tiny_ssim` and libaom's `--psnr` SSIM are related but not identical kernels.
* **UVQ** — Google's Universal Video Quality no-reference model for UGC (ContentNet, CompressionNet, DistortionNet, AggregationNet), Apache-2.0.
* **VMAF / VMAF-NEG / bootstrapped VMAF** — Netflix's fused quality model (VIF, ADM, motion); NEG clamps enhancement gain; bootstrapped (`vmaf_b`) gives confidence intervals.
* **Working domain** — the representation a metric plugin requests (`code_value`, `linear_light`, `pu21`, `ictcp`, `model_rgb`); the normaliser delivers each pair once per domain.
* **wPSNR** — luma-level-weighted PSNR of the JVET HDR CTC (weights `2^(clamp(0.015·Y10 − 7.5, −3, 6)/3)`).
* **XPSNR** — extended PSNR (Fraunhofer HHI) weighting block SSE by spatial/temporal activity; LGPL source, no patent grant.
* **zimg** — the z.lib image scaling/colour library used for resizing, chroma siting, range and matrix/transfer/primaries conversion.
