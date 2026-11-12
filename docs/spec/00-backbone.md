# 0. Architecture Backbone

> **About this chapter.** The architecture backbone was written first and every chapter was authored against it; it is reprinted here as **chapter 0** so that the specification is self-contained. Its sections are numbered `0.n` (earlier drafts and chapter cross-references wrote `B.n`; `B.n` now denotes Appendix B only). A reference of the form `§0.n.m` to a section that has no sub-headings (§0.1, §0.7) means item *m* of that section's numbered list (`§0.7.4` = the performance-budget rule). Where a chapter and this backbone disagree on a fact that a later decision changed (the cross-chapter decisions D1–D18, §0.11), the owning chapter named in §0.10 wins and this chapter has been aligned to it: schema = chapter 6, CLI grammar = chapter 12, key bindings and settings = chapter 11, budgets = §0.7.4 mirrored by chapter 14 §14.9.3.
>
> **Status:** normative internal document. Every chapter of the Requirements & Architecture Specification MUST conform to this backbone. Where a chapter and this backbone disagree, the backbone wins and the chapter must be fixed.
>
> **Audience:** chapter authors (and, later, the implementation team). Written for implementers who may have limited domain knowledge: every term used is defined in the Glossary chapter; every decision records *why*; every interface is given as concrete code, not prose.

---

## 0.0 Project identity and scope (fixed)

| Item | Value |
|---|---|
| Project name | **Open Video Tools** (short: **OVT**). Component names below use the `ovt` prefix. The owner may rename later; names are placeholders but MUST be used consistently in the spec. |
| One-sentence goal | A free and open-source, cross-platform video analysis suite for H.264/AVC, H.265/HEVC and AV1 that covers the feature areas of commercial stream analyzers (bitstream syntax, block partitioning and motion vectors, decoded planes, container structure and quality metrics), with correct HDR (PQ and HLG) display, usable by codec researchers, video quality teams and video platform teams. OVT is an independent implementation (chapter 1 §1.2). |
| Primary platforms | **macOS (arm64 and x86_64)** — first development platform. **Android tablets and Chromebooks (Android app; arm64-v8a and x86_64 ABIs)** — developed concurrently from the start. **Linux desktop (x86_64, arm64; Wayland and X11)**. |
| Explicit non-targets | Windows, iOS, web browser builds. The design must not *preclude* them, but no work item may target them. |
| Initial codecs | H.264/AVC (ITU-T H.264 08/2021), H.265/HEVC (ITU-T H.265 09/2023, Main/Main10 required; RExt/SCC via oracle decoder), AV1 (AOM AV1 Bitstream & Decoding Process Specification v1.0.0 with Errata 1). VP9, VVC, MPEG-2 are **future** codecs; the data model and plugin ABI must not hard-code the initial three. |
| Required container | MP4 / ISOBMFF (ISO/IEC 14496-12, -14, -15; CMAF ISO/IEC 23000-19; CENC ISO/IEC 23001-7). Also required at low cost: IVF, Annex B elementary streams (`.h264`, `.264`, `.h265`, `.hevc`), raw AV1 OBU streams (`.obu`, Section 5 low-overhead and Annex B), raw YUV / Y4M. Secondary (later phases): Matroska/WebM, MPEG-TS, MXF. |
| Core capabilities (owner's list, all mandatory) | (1) raw encoded bitstream inspection; (2) VQ metrics between a video and its reference; (3) visual inspection of macroblocks/CUs/superblocks and motion vectors; (4) inspection of the separate Y/U/V planes; (5) container inspection (MP4 required). |
| Starter VQ metrics | PSNR, SSIM, VMAF, UVQ. The metric layer MUST be a plugin system so HDR metrics and others can be added without touching core code. |
| Licensing policy (owner) | GPL is acceptable and expected. **AGPL is forbidden** anywhere in the dependency tree. Functionality beats licence purity. |
| Decoder policy (owner) | Prefer **upstream public APIs**. Patches to third-party decoders are allowed only when (a) no public API exists, (b) the patch is small, behind a build flag, and (c) an upstreaming attempt is part of the work item. The project must be low-maintenance and not brittle. |
| Output of this effort | A Markdown specification. The owner will create the repository. |
| Implementation team | Coding agents, possibly using a less capable model. Consequence: the spec must be explicit, use small work items, give concrete interfaces, data formats, file layouts and acceptance tests, and never rely on implied knowledge. |

---

## 0.1 Document conventions (all chapters MUST follow)

1. **Identifiers.** Every requirement, decision and work item has a stable ID:
   - `FR-<AREA>-<nnn>` functional requirement, e.g. `FR-SYN-012`. Areas: `GEN` (general), `CNT` (container), `SYN` (syntax/bitstream), `DEC` (decoding/playback), `BLK` (block-level analysis), `PLN` (planes/pixel views), `HDR` (HDR/colour), `VQ` (quality metrics), `GUI`, `CLI`, `API` (library/scripting), `AND` (Android/ChromeOS), `MAC` (macOS), `LNX` (Linux).
   - `NFR-<nnn>` non-functional requirement (performance, robustness, security, accessibility, maintainability).
   - `ADR-<nnn>` architecture decision record.
   - `WI-<phase>.<nnn>` work item, e.g. `WI-2.014`.
   - `G:<term>` glossary entry reference, e.g. `G:CTU`.
2. **Requirement wording.** Use RFC 2119 terms: MUST, MUST NOT, SHOULD, MAY. One requirement = one testable sentence. Each FR/NFR MUST have at least one **Acceptance test** line that a tester (or CI) can execute, phrased as *Given / When / Then*.
3. **Interfaces are code.** Public interfaces are given as C (for the ABI) or C++20 declarations, FlatBuffers schema, JSON Schema, or command-line grammar. Prose may explain, but code is the source of truth.
4. **Numbers are explicit.** No "fast", "large", "soon". Write "≤ 100 ms", "≥ 60 fps at 1080p on an M1", "≤ 2 GB RSS".
5. **No implied knowledge.** When a chapter uses a codec concept (e.g. "merge candidate"), it either links to the Glossary or cites the spec section (`H.265 §8.5.3.2.2`). Chapter authors MUST add Glossary entries for every new term they introduce (collect them in a `### Glossary additions` section at the end of the chapter; the editor merges them).
6. **Work item shape.** Every WI uses exactly this template. Every field is mandatory; write `none` when a field is empty (D15):

   ```
   ### WI-<phase>.<nnn> — <imperative title>
   **Goal:** one sentence.
   **Depends on:** WI-... (concrete ids only, or "none"; chapter 16 §16.4 repeats them verbatim).
   **Produces:** files/targets/artifacts (exact paths).
   **Interface:** the signatures / schema / CLI grammar this WI implements or consumes (link to section).
   **Steps:** numbered, each one a single afternoon's work for an implementer.
   **Acceptance tests:** numbered Given/When/Then lines, with the exact command or test name where possible.
   **Size:** S (≤ 1 day) / M (≤ 3 days) / L (≤ 1 week) — the one legend, repeated in chapter 16 §16.0. No WI may be larger than L; split it.
   **Notes / pitfalls:** things an implementer will get wrong without being told.
   ```
7. **Source citations.** Facts about third-party software cite a URL (release notes, docs, source file). Facts about codecs cite spec section numbers.
8. **Terminology discipline.** Use the spec's term for each codec: H.264 *macroblock (MB)*, *sub-macroblock partition*; HEVC *coding tree unit (CTU)*, *coding unit (CU)*, *prediction unit (PU)*, *transform unit (TU)*; AV1 *superblock (SB)*, *block*, *transform block (TXB)*, *mode info (MI) unit*. The codec-neutral word in OVT is **block**; the codec-neutral word for the smallest addressable analysis cell is **cell** (a 4×4 luma sample square).
9. **Units.** Pixel positions are luma samples, origin top-left, in **coded picture** coordinates (before cropping). Motion vectors are stored in **1/16 luma sample units** in the neutral model (see §0.5). Time is in the container's time base plus a derived `double seconds`. Bit offsets are 0-based from the start of the referenced unit. File offsets are 0-based bytes from the start of the file.
10. **Diagrams.** ASCII or Mermaid in fenced blocks only.

---

## 0.2 Architecture decisions (ADR summary)

Each ADR below is binding. Chapters elaborate the *how*; they MUST NOT reopen the *what* without an explicit "Open question" flagged for the owner.

| ID | Decision | Why (short) | Alternatives rejected |
|---|---|---|---|
| **ADR-001** | **Implementation language: C++20** for all native code (core libraries, analysis plugins, renderer, GUI). Public ABI is **C** (`extern "C"`). | Every dependency that matters (FFmpeg, libaom, dav1d, libde265, HM, libvmaf, libplacebo, zimg, Qt) is C/C++; zero FFI cost; lowest maintenance against fast-moving upstreams; one language for the whole team. Memory-safety risk in parsers is mitigated by a mandatory bounds-checked `BitReader`, sanitizer CI and fuzzing (NFR). | Rust core (safer parsers, but every decoder binding is thin or unmaintained and a Qt bridge adds a layer); Dart/Kotlin (no path to decoder internals). |
| **ADR-002** | **Licensing:** `core/` and `render/` libraries **LGPL-2.1-or-later**; the `app/` GUI, `cli/`, `android/` and any code derived from YUView **GPL-3.0-or-later**; plugin ABI headers (`core/include/ovt/abi/*.h`) **BSD-2-Clause** so third parties can implement plugins. CI fails on any dependency licensed AGPL, SSPL, BUSL or non-commercial. | GPL-3 unlocks FFmpeg GPL builds, libde265 (LGPL-3), Qt including GPL-only modules, and YUView code reuse. An LGPL core lets companies embed the data layer in CI pipelines. BSD headers make vendor decoder plugins possible. | GPL-3 everywhere (simpler, blocks embedding); permissive everywhere (blocks libde265 static linking and YUView reuse). |
| **ADR-003** | **Layered architecture:** `libovt-core` (no UI, no GPU) → `libovt-render` (GPU, libplacebo, no toolkit types) → front-ends (`ovt` CLI, `ovt-studio` Qt GUI, Android shell, Python module). Everything a front-end shows is available from the core via the C API and from the CLI as JSON/FlatBuffers. | "Same binary for GUI and CLI" is the universal commercial convention; researchers script everything; Android and desktop share one core. | GUI-first monolith. |
| **ADR-004** | **GUI toolkit: Qt 6 (Qt Quick/QML, Qt 6.8.3 LTS; Wayland colour management needs a later Qt 6.x, 6.10 is not an LTS release — D5)** with `QQuickRhiItem`/`QSGRenderNode` for GPU items. Android/ChromeOS: Qt Quick embedded via `QtQuickView` in a conventional Android `Activity`, with a native `SurfaceView` sibling for HDR output. | Only toolkit with HDR swapchain formats today, merged Wayland `color-management-v1`, a mature Android port (API 28–36, arm64 + x86_64), model-virtualised trees, and desktop idioms (hover, right-click, shortcuts, docking) required by ChromeOS/Android desktop windowing. | Flutter (no HDR surface, platform views immature on desktop), egui/wgpu (no HDR renderer, weak Android), ImGui+SDL3 (not a product UI), GTK4, Avalonia, Compose. |
| **ADR-005** | **Frame rendering and colour management: libplacebo** (LGPL-2.1+) does upload, YUV→RGB, EOTF, tone mapping, gamut mapping, dithering and HDR metadata signalling for all platforms (Vulkan; MoltenVK on macOS; GLES/Vulkan on Android). Overlay geometry (grids, arrows, heatmaps, labels) is drawn by `libovt-render` in the same pass. | Only FOSS implementation of the complete HDR display toolbox, proven on all three platforms via mpv/VLC. | Hand-written shaders (months of colour-science work); Qt scene graph alone (SDR-only). |
| **ADR-006** | **Container inspection:** OVT writes its **own ISOBMFF reader** (C++, schema-assisted from the MP4RA registry and the MPEG FileFormatConformance JSON) that records byte/bit spans for every box and field. **libavformat** is used for demuxing packets to decoders. GPAC/MP4Box, mp4ff, mp4box.js and MediaInfo trace are **test oracles only**. | No FOSS library exposes a field-level tree with offsets for hex linkage; GPAC is LGPL C with internal structs and no offsets in its dump; Bento4 is GPL-2-only. | libgpac as inspection backend (fallback if the own reader stalls). |
| **ADR-007** | **Header-level syntax parsing (NAL/OBU tree, parameter sets, slice/frame headers, SEI/metadata):** OVT writes its **own position-recording parsers** for H.264, HEVC and AV1 in `core/src/syntax/`, validated against FFmpeg `trace_headers` and GStreamer `codecparsers` as oracles. | Field↔bit-offset linkage for the hex view is not available from any upstream API (FFmpeg CBS is private and offsets exist only in its trace text). Header syntax is frozen and small (2–3 kLOC per codec). | Vendoring FFmpeg CBS (private API, no struct-level offsets); GStreamer codecparsers (no offsets). |
| **ADR-008** | **Block-level analysis data comes from instrumented software decoders behind a C plugin ABI** (`ovt_analysis_decoder_v1`), never from an analysis-only parser. Per codec: **AV1 → libaom** built with `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1` (upstream API); **HEVC → libde265** with a small read-only internals patch (upstreaming attempt mandatory; HM wrapped as bit-exact oracle and for RExt/SCC); **H.264 → FFmpeg** public API (`export_side_data=mvs+venc_params`) for MVs and QP in phase 1, plus a small side-data extension patch (MB type, ref_idx, intra modes, CBP) submitted upstream. **dav1d** is used for AV1 *playback* only; no dav1d fork. | Owner's upstream-first policy; libaom inspection is upstream; libde265 holds all data in `de265_image` and the patch touches no decoding code; FFmpeg already exports H.264 MVs/QP. An analysis-only parser would be a decoder minus reconstruction (15–25 kLOC per codec) and still could not show prediction/residual planes. | dav1d fork (fast but brittle; YUView's fork rotted); own full parsers; hardware decoders (pixels only). |
| **ADR-009** | **Two decode paths.** *Playback path:* libavformat → libavcodec (hardware: VideoToolbox on macOS, MediaCodec on Android, VA-API on Linux; software fallback) → `libovt-render`. *Analysis path:* stream index → seek to nearest random access point → feed units to the analysis plugin → `FrameAnalysis` records + planes → cache. The analysis path runs on worker threads and is GOP-local (only the frames needed for the frame under inspection). | Hardware decoders expose no internals; instrumented decoders are slow (libde265 has no NEON; libaom inspection builds are 3–10× slower than dav1d). | Single software path (too slow on Android for 4K scrubbing). |
| **ADR-010** | **Serialisation: FlatBuffers** (Apache-2.0) for every analysis record (`ovt_model.fbs`, `ovt_metrics.fbs`, `ovt_container.fbs`). The same buffers are used in memory, across the plugin ABI, in the on-disk cache (zstd-compressed) and in CLI output. JSON (via FlatBuffers' reflection or explicit writers) and **YUView statistics CSV** are export formats. | Zero-copy, schema-versioned, generated readers for C++/Python/Kotlin/JS; one schema everywhere; large SoA vectors are cheap. | Protobuf (copying), Arrow (heavier, less natural for trees), hand-rolled structs (no versioning). |
| **ADR-011** | **Metrics layer is a plugin system** (`ovt_metric_v1` descriptor + `score` function). Engines: own SIMD kernels (PSNR, SSIM Gaussian 11×11 and x264 4×4 variants, per-block maps), **libvmaf** (VMAF v0.6.1 family and v1, MS-SSIM, PSNR-HVS, CIEDE2000, CAMBI), **ONNX Runtime** host for **UVQ** (models exported from google/uvq PyTorch), and an out-of-process **sidecar protocol** (JSON Lines over stdio + shared memory/Y4M) for Python research metrics (ColorVideoVDP, HDR-VDP-3). A shared **normalisation stage** (alignment, crop/scale, colour/range/transfer via zimg, bit-depth promotion) feeds every plugin in its declared working domain (`code_value`, `linear_light`, `pu21`, `ictcp`, `model_rgb`). | Owner's starter set mixes full-reference and no-reference metrics; HDR metrics must be addable without core changes; bit-exact parity with the `vmaf` CLI requires calling libvmaf directly. | FFmpeg filters as the engine (framesync surprises, GPL coupling, no maps). |
| **ADR-012** | **Build system: CMake ≥ 3.25 with CMakePresets.json**, dependencies via **vcpkg manifest** where ports exist and `third_party/*.cmake` ExternalProject scripts for libaom (custom flags), libde265 (patched), libvmaf and libplacebo (Meson), FFmpeg (configure). Android via Gradle + `externalNativeBuild` + Qt's `androiddeployqt`. One `cmake --preset <name>` builds each platform. | Mixed upstream build systems; reproducibility for GPL "Corresponding Source"; a single documented entry point for agents. | Conan; Meson-only; Bazel (owner-confirmed 2026-11-03: rules_foreign_cc can wrap the C libraries, but Qt 6/QML and Android packaging stay on CMake/Gradle, so Bazel would mean two build systems; see chapter 13 §13.2.0 for the path to move the core later). |
| **ADR-013** | **Process model (phase 1): in-process plugins**, one decoder instance per stream, never shared across threads; all plugin output is a FlatBuffer so an out-of-process mode (crash isolation for hostile streams) can be added later without changing consumers. CLI runs are naturally separate processes. | Simplicity for the implementation team; data model already IPC-ready. | Mandatory worker processes in phase 1. |
| **ADR-014** | **Android HDR output uses `SurfaceView` + `ANativeWindow_setBuffersDataSpace` (BT2020_PQ / BT2020_HLG) via EGL or `AHardwareBuffer`,** not Vulkan swapchain colourspaces (OEM drivers often hide them). `TextureView` is forbidden (forces SDR on API ≥ 33). Android 15+: `setDesiredHdrHeadroom`. ChromeOS HDR pass-through is **unverified** and is a week-one hardware spike (WI-0.055). | Platform facts from Android docs and Arm driver reports. | Vulkan-only HDR. |
| **ADR-015** | **Default download for AVC/HEVC decoding uses runtime-loaded decoder plugins** (`dlopen`/`QLibrary`), so official binaries can be built with or without pooled-patent codecs (Fedora-style split). AV1 (libaom, dav1d) and libvmaf ship in every official build with the AOM PATENTS notice. | Patent posture identical to VLC/HandBrake/YUView; lets companies substitute licensed decoders. | Always-bundled decoders. |
| **ADR-016** | **Governance:** Developer Certificate of Origin (DCO), no CLA; no foundation initially; distinctive trademark registered early; distribution: macOS notarised DMG + Homebrew cask (never Mac App Store), Linux distro packages + Flathub, Android Google Play + F-Droid with reproducible builds. | From licensing research; GPL and Mac App Store are incompatible per FSF/VLC precedent. | CLA; LF hosting at start. |

---

## 0.3 System overview

```
+-------------------------------------------------------------------------------------+
|  Front-ends                                                                         |
|  ovt (CLI, GPL)   ovt-studio (Qt Quick GUI, GPL)   Android shell (Kotlin+Qt, GPL)   |
|  python/ovt (pybind11 over the C API)                                               |
+-------------------------------------------------------------------------------------+
|  libovt-render (LGPL)   VideoSurface: libplacebo frame pipeline + overlay renderer   |
|                         (grids, MV arrows, heatmaps, labels, picking) — no Qt types  |
+-------------------------------------------------------------------------------------+
|  libovt-core (LGPL)  C API: ovt/ovt.h                                                |
|   io        RandomAccessFile (mmap / pread / Android content URI)                    |
|   bitreader BitReader with position recording and bounds checks                      |
|   container ISOBMFF reader (own), IVF, Annex B, OBU, Y4M/raw; later EBML, TS, MXF    |
|   demux     libavformat wrapper → packets/units                                      |
|   syntax    H.264 / HEVC / AV1 header parsers → SyntaxTree with bit spans            |
|   index     StreamIndex: units, frames, RAPs, GOP/ref structure, per-frame stats     |
|   decode    playback decoders (libavcodec; HW accel) → Picture                       |
|   analysis  plugin host for ovt_analysis_decoder_v1 → FrameAnalysis (FlatBuffer)     |
|   model     FlatBuffers schemas + helpers (grid, block tree, syntax tree)            |
|   color     ColorDescription, HDR static/dynamic metadata, nits math (CPU)           |
|   metrics   plugin host, normalisation (zimg), kernels, libvmaf host, UVQ host,      |
|             sidecar protocol, pooling, BD-rate, exporters                            |
|   cache     on-disk cache of index + FrameAnalysis (zstd)                            |
|   jobs      thread pool, cancellation, progress                                      |
+-------------------------------------------------------------------------------------+
|  plugins/ (each its own licence, separate shared library, C ABI)                     |
|   ovt-plugin-aom (BSD-2 + AOM patent) — AV1 block analysis via CONFIG_INSPECTION     |
|   ovt-plugin-de265 (LGPL-3) — HEVC block analysis via internals patch                |
|   ovt-plugin-ffmpeg (LGPL/GPL) — H.264 MVs/QP via side data; playback for all codecs |
|   ovt-plugin-hm (BSD-3) — HEVC oracle (bit-exact, RExt/SCC), optional, slow          |
+-------------------------------------------------------------------------------------+
|  third_party/: FFmpeg, libaom, dav1d, libde265, HM, libvmaf, libplacebo, zimg,       |
|  FlatBuffers, zstd, ONNX Runtime, Qt 6 (prebuilt), fmt, spdlog, Catch2, doctest     |
+-------------------------------------------------------------------------------------+
```

**Data flow for "open a file":**

1. `io` opens the file (mmap on desktop; `pread` through a file descriptor on Android).
2. `container` sniffs the format and builds the **container tree** (`ContainerTree` FlatBuffer) lazily (never reads `mdat` payloads), producing the list of **tracks** and, for the selected video track, the **unit map** (byte ranges of every NAL/OBU/sample).
3. `demux` (libavformat) is opened in parallel for the playback path.
4. `syntax` parses every non-VCL header unit (and slice/frame headers) into a `SyntaxTree`; `index` builds the `StreamIndex` (frames in decode order, display order, type, size, RAP flags, temporal id, references declared in headers, per-frame header-derived stats). Target: ≤ 2 s for a 1-hour 1080p stream on an M1 (header-only pass).
5. The GUI shows: container tree, syntax tree + hex, timeline (frame-size bar chart), first frame via the playback path.
6. When the user selects a frame for block analysis, `analysis` seeks to the preceding RAP, feeds units to the codec's analysis plugin, receives `FrameAnalysis` FlatBuffers for each decoded frame, caches them, and the renderer draws overlays.
7. Metrics jobs run in `jobs` over the normalisation stage and plugins, producing `MetricsResult` FlatBuffers, shown in the metrics panel and exportable.

---

## 0.4 Repository layout (fixed)

All paths are repository-rooted (D14). `cmake/` is a top-level directory; fuzz targets live in `core/tests/fuzz/`; performance benchmarks in `tests/perf/` with `bench_*` names.

```
/CMakeLists.txt            top-level; options OVT_BUILD_GUI, OVT_BUILD_CLI, OVT_BUILD_PYTHON,
                            OVT_BUILD_PLUGIN_AOM/DE265/FFMPEG/HM, OVT_FFMPEG_GPL, OVT_FFMPEG_GPL_PLUGIN_ONLY,
                            OVT_ENABLE_SANITIZERS, OVT_ENABLE_FUZZ (chapter 13 §13.2.2)
/CMakePresets.json         presets: macos-arm64-debug/release, linux-x64-*, linux-arm64-*, macos-x86_64-*, android-arm64-*, android-x86_64-* (chapter 13 §13.2.4)
/cmake/                    OvtOptions.cmake, OvtCompilerFlags.cmake, OvtSanitizers.cmake, OvtInstall.cmake,
                            toolchains/, modules/ (Find*.cmake)
/vcpkg.json                manifest for deps available as ports
/LICENSE                   LGPL-2.1-or-later (top-level default; per-directory policy in chapter 15)
/LICENSES/                 LGPL-2.1, GPL-3.0, BSD-2-Clause, Apache-2.0 texts (REUSE layout)
/REUSE.toml                REUSE annotations for files that cannot carry headers
/DCO                       Developer Certificate of Origin
/THIRD_PARTY_NOTICES.md    generated by tools/gen_notices.py (never edited by hand)
/MAINTAINERS.md            maintainers and licence reviewers; /TRADEMARKS.md trademark policy
/docs/                     this specification (docs/spec/*.md), ADRs (docs/adr/*.md),
  dev/                     developer guides (building, threading, adding a codec/metric, security/)
  platforms/               per-platform notes; governance/ project rules; acceptance/ phase records
/core/                     libovt-core (LGPL-2.1-or-later)
  include/ovt/ovt.h        umbrella C API
  include/ovt/abi/analysis_decoder_v1.h   plugin ABI (BSD-2-Clause)
  include/ovt/abi/metric_v1.h             metric plugin ABI (BSD-2-Clause)
  include/ovt/*.h          C API per module (C11, compiled as C by `api_example.c`): io.h, container.h, syntax.h, index.h,
                            decode.h, analysis.h, color.h, metrics.h, cache.h, jobs.h (ovt_Job), log.h (ovt_set_logger), status.h
  include/ovt/cxx/*.hpp    C++ helper headers (semi-public, no ABI promise; D22): result.hpp (ovt::Result<T>),
                            jobs.hpp (ovt::jobs::{Pool,Cancel,JobHandle}), log.hpp, cancel.hpp (ovt::Cancelled)
  schema/ovt_model.fbs     FlatBuffers: SyntaxTree, StreamIndex, FrameAnalysis, Picture descriptors
  schema/ovt_container.fbs ContainerTree, BoxNode, Field, Track, SampleTable views
  schema/ovt_metrics.fbs   MetricDescriptor, MetricsResult, maps, pooling
  share/models/            bundled model files (UVQ ONNX manifests, libvmaf model ids)
  src/<module>/            C++20 implementation, one directory per module in §0.3: io, bitreader, container, demux,
                            syntax, index, decode, analysis, model, color, metrics, cache, jobs (plus api/ for the C entry points)
  tests/                   unit tests (Catch2) per module
  tests/golden/            golden fixtures per module;  tests/fuzz/  libFuzzer sources `fuzz_<parser>.cpp`, CMake targets `ovt_fuzz_<parser>` (chapter 13 §13.2.1; D14)
/render/                   libovt-render (LGPL-2.1-or-later): include/ovt/render/*.h, src/, luts/, fonts/ (SDF atlas),
  tests/                   render tests + fixtures/
/plugins/ovt-plugin-aom/   CMake project producing libovt_plugin_aom.{dylib,so}
/plugins/ovt-plugin-de265/ + patches/0001-de265-internals-api.patch
/plugins/ovt-plugin-ffmpeg/ + patches/0001-h264-export-mb-params.patch (phase 4)
/plugins/ovt-plugin-hm/
/cli/                      ovt executable (GPL-3.0-or-later): src/cmd/<name>.cpp per subcommand
/app/                      ovt-studio Qt Quick application (GPL-3.0-or-later): qml/, src/, resources/, i18n/
  tests/                   models/, qml/, scripts/ (interaction scripts), screenshots/, a11y/
/android/                  Gradle project: app/ (Kotlin MainActivity, SurfaceView HDR sibling), QtQuickView glue
/python/                   pybind11 module `ovt`, pyproject.toml, tests; ovt_sidecar/ (manifests/)
/third_party/              *.cmake ExternalProject scripts, patches/, version pins (third_party/VERSIONS.md)
/packaging/                debian/, rpm/, arch/, appimage/
/flatpak/                  org.openvideotools.Studio.yml, org.openvideotools.Studio.Codecs.yml, modules/*.json
/tests/corpora/            manifests (URLs + SHA-256) of conformance streams; synthetic/ generated streams
/tests/oracles/            oracle Docker image, normalisers, oracle runner scripts
/tests/golden/             cross-module golden sets (CLI envelopes, screenshots)
/tests/perf/               bench_* benchmarks: core/, render/, metrics/, gui/, data/, baselines/, budgets.json
/tests/downstream/         install/export consumer test (find_package(ovt))
/tools/                    schema generators (MP4RA → C++ tables), fetch_corpora.py, release scripts,
                            license_gate.py, gen_notices.py, goldens/ (golden maintenance scripts)
/.github/workflows/        CI: build matrix, tests, sanitizers, fuzz smoke, license-gate, Android APK, release
/.github/actions/          composite actions;  .github/ISSUE_TEMPLATE/;  .github/CODEOWNERS
```

**Naming rules.** C API: `ovt_<module>_<verb>` (`ovt_container_open`), types `ovt_<Noun>` (`ovt_StreamIndex`), status `ovt_status`. C++: namespace `ovt::<module>`, `PascalCase` types, `snake_case` functions and members, `kCamelCase` constants. Files: `snake_case.cpp/.h`. FlatBuffers: namespace `ovt.fb`, `PascalCase` tables, `snake_case` fields. QML: `PascalCase.qml` components, `camelCase` properties. CLI: `kebab-case` subcommands and flags.

---

## 0.5 Core data model (normative schema excerpts)

The full schema lives in the Data Model chapter; chapters MUST reference these names exactly. Field semantics in brackets.

### 0.5.1 Identity and coordinates

```fbs
namespace ovt.fb;

enum Codec : ubyte { Unknown = 0, H264 = 1, H265 = 2, AV1 = 3, VP9 = 4, H266 = 5, MPEG2 = 6 }

struct Rect { x:int; y:int; w:int; h:int; }            // luma samples, coded-picture coordinates

struct ByteSpan { offset:ulong; length:ulong; }          // file byte range
struct BitSpan  { bit_offset:ulong; bit_length:uint; }   // bits from start of the owning unit's RBSP (H.26x) or OBU payload (AV1)

// Frame identity. decode_index counts every coded frame (AV1 hidden frames included);
// display_index counts only shown/output frames in output order. -1 = not shown / unknown.
struct FrameId { decode_index:int; display_index:int; }
```

### 0.5.2 Syntax tree (header-level parsing; container fields reuse the same node type)

```fbs
enum SyntaxValueKind : ubyte { None, UInt, Int, Bool, Bytes, String, Enum, Fixed /* fixed-point as int+scale */ }

table SyntaxNode {
  name:string (required);         // exact spec syntax element name, e.g. "log2_max_pic_order_cnt_lsb_minus4"
  descriptor:string;              // spec descriptor, e.g. "ue(v)", "u(8)", "f(1)", "le(n)", "ns(n)", "su(1+6)"
  kind:SyntaxValueKind;
  uvalue:ulong; ivalue:long; bvalue:bool; bytes:[ubyte]; svalue:string;
  enum_name:string;               // human name for enum values, e.g. "PART_2NxN"
  derived:string;                 // optional derived/explained value, e.g. "CtbSizeY = 64"
  span:BitSpan;                   // position inside the owning unit (see Unit.epb_positions for raw mapping)
  spec_ref:string;                // e.g. "H.265 7.3.2.2.1"
  children:[SyntaxNode];
  array_index:int = -1;           // when this node is an element of a syntax array
  diagnostics:[Diagnostic];
}

enum Severity : ubyte { Info, Warning, Error, Fatal }
table Diagnostic { severity:Severity; code:string; message:string; span:BitSpan; spec_ref:string; }
```

### 0.5.3 Units and stream index

```fbs
table Unit {                       // one NAL unit (H.26x) or OBU (AV1) or raw sample
  index:uint;                      // 0-based in decode order
  file_span:ByteSpan;              // raw bytes in the file, start code / length prefix excluded (prefix_span covers it)
  prefix_span:ByteSpan;            // start code or length field
  type:ubyte;                      // nal_unit_type / obu_type
  type_name:string;                // "IDR_W_RADL", "OBU_FRAME", ...
  layer_id:ubyte; temporal_id:ubyte;
  epb_positions:[uint];            // byte offsets (within file_span) of removed emulation-prevention bytes; empty for AV1
  frame:FrameId;                   // owning frame, or decode_index = -1 for non-VCL units
  header:SyntaxNode;               // parsed header (parameter set, slice header, OBU header + frame header, SEI...)
  sample_index:int = -1;           // container sample this unit belongs to, when demuxed from a container
  diagnostics:[Diagnostic];        // unit-level diagnostics (chapter 6 §6.3)
}

enum FrameType : ubyte { I, P, B, Key, Inter, IntraOnly, Switch, Unknown }
enum PictureStructure : ubyte { Frame, TopField, BottomField, Mbaff }

table FrameInfo {
  id:FrameId;
  type:FrameType;                  // codec-native; I/P/B for H.26x (slice-majority), Key/Inter/IntraOnly/Switch for AV1
  shown:bool; show_existing_of:int = -1;
  structure:PictureStructure;
  temporal_id:ubyte; layer_id:ubyte;
  is_rap:bool;                     // random access point (IDR/CRA/BLA/Key)
  poc:long;                        // PicOrderCntVal / frame_num-derived / order_hint
  pts:long; dts:long; timebase_num:int; timebase_den:int;
  size_bytes:ulong;                // coded size incl. start codes / length prefixes and the preceding AUD/SEI/parameter-set units of the access unit (D7, chapter 6 §6.4)
  units:[uint];                    // Unit.index list
  coded_size:Rect; crop:Rect;      // crop = conformance window / render size
  chroma_format:ubyte;             // 0=400,1=420,2=422,3=444
  bit_depth_luma:ubyte; bit_depth_chroma:ubyte;
  qp_base:short;                   // slice QP of first slice / base_q_idx
  declared_refs:[int];             // decode_index of frames referenced per headers (ref lists / ref_frame_idx)
  dpb_after:[int];                 // decode_index of frames in DPB after decoding this frame (if derivable)
}

table StreamIndex {
  codec:Codec;
  schema_version:uint;             // = 1
  source_hash:[ubyte];             // 32-byte SHA-256(first min(size, 1 MiB) ∥ LE64 size ∥ LE64 mtime_ns) — cache key, see chapter 6 §6.2.8
  units:[Unit];
  frames:[FrameInfo];              // decode order
  display_order:[int];             // decode_index per display slot
  parameter_sets:[Unit];           // convenience: all VPS/SPS/PPS/sequence headers
  sei:[Unit];                      // all SEI / metadata OBUs
  diagnostics:[Diagnostic];
}
```

### 0.5.4 Frame analysis (block-level truth from an analysis plugin)

```fbs
enum PredKind : ubyte { None, Intra, Inter, Skip, IntraBC, PCM, Palette }

// Structure-of-arrays grid over 4x4 luma cells. All vectors have length cells_w * cells_h
// (row-major) unless empty (= not provided by this plugin). -1 / 0xFF mean "not applicable".
table CellGrid {
  cells_w:uint; cells_h:uint;                  // ceil(coded_w/4), ceil(coded_h/4)
  block_id:[uint];                             // leaf Block owning this cell
  partition_id:[ushort];                       // slice / tile index
  pred:[ubyte];                                // PredKind
  qp:[short];                                  // final luma QP (H.26x) or qindex (AV1)
  intra_mode_y:[short]; intra_mode_c:[short];  // codec-numbered
  mv0_x:[short]; mv0_y:[short]; mv1_x:[short]; mv1_y:[short];   // 1/16 luma sample units
  ref0:[byte]; ref1:[byte];                    // index into FrameAnalysis.refs; -1 none
  tx_log2_w:[ubyte]; tx_log2_h:[ubyte]; tx_type:[ubyte]; cbf:[ubyte];   // cbf bit0=Y bit1=Cb bit2=Cr
  bits_x8:[uint];                              // estimated entropy cost ×8 for the owning block, distributed over the block's cells as floor(bits_x8 / cells) with the remainder added to the first cells (chapter 6 §6.3.4), so the sum over cells = block cost
  deblock_bs_v:[ubyte]; deblock_bs_h:[ubyte];  // boundary strength on left / top edge of the cell
  filter_idx:[ubyte];                          // SAO type (HEVC) / cdef_idx (AV1) / 0
}

enum BlockKind : ubyte { MB, SubMB, CTU, CU, PU, TU, SB, Block, TXB, Other }

table Block {
  id:uint; parent:int = -1; children:[uint];
  kind:BlockKind; rect:Rect; level:ubyte;
  partition_type:ubyte;            // codec numbering (mb_type/sub_mb_type, PartMode, PARTITION_*)
  pred:PredKind; skip:bool; merge_or_direct:bool;
  qp:short; bits_x8:uint;          // exact-as-available cost for this block incl. children
  ext_type:string;                 // "AvcMb", "HevcCu", "HevcPu", "HevcTu", "Av1Block", "Av1Txb"
  ext:[ubyte];                     // nested FlatBuffer of the codec extension table named by ext_type
}

table MotionVector {
  block_id:uint; list:ubyte;       // 0 = L0 / first ref, 1 = L1 / second ref
  ref:byte;                        // index into refs
  rect:Rect;                       // the prediction block
  mv_x:short; mv_y:short;          // 1/16 luma sample units
  mvd_x:short; mvd_y:short;        // signalled difference (if available), same units
  predictor:string;                // "merge[2]", "amvp[0]", "spatial_direct", "NEARESTMV", "GLOBALMV", ...
  flags:ushort;                    // bit0 bi-pred, bit1 weighted, bit2 global/warped, bit3 sub-block (affine/OBMC), bit4 field
}

table RefSlot { index:ubyte; /* position in refs[]: reference type, INTRA at 0 (D6) */ slot:byte = -1; /* DPB / AV1 ref_frame_idx slot, -1 = n/a */ frame:FrameId; name:string; /* "L0[0]", "LAST", "ALTREF" */ is_long_term:bool; usage_cells:uint; }

enum PlaneStage : ubyte { Reconstruction, Prediction, Residual, PreDeblock, PreSaoOrCdef, PreLoopRestoration, Coefficients }

table PlaneRef {                   // planes are not stored in the FlatBuffer; this describes a shared-memory / file blob
  stage:PlaneStage; plane:ubyte;   // 0=Y 1=U 2=V
  width:uint; height:uint; stride_bytes:uint; bit_depth:ubyte; sample_bytes:ubyte; is_signed:bool;
  blob_id:string;                  // key into the Picture store
}

table SymbolCost { x4:ushort; y4:ushort; symbol_id:ushort; bits_x8:uint; count:uint; }

table FrameAnalysis {
  schema_version:uint;             // = 1
  plugin_id:string; plugin_version:string;
  frame:FrameInfo;
  grid:CellGrid;
  blocks:[Block];                  // blocks[0] children are the top-level CTUs/SBs/MBs in raster order
  mvs:[MotionVector];
  refs:[RefSlot];
  partitions:[PartitionInfo];      // slices / tiles / tile groups with byte ranges and headers
  filter_params:[ubyte]; filter_params_type:string;   // nested codec table: HevcFilterParams / Av1FilterParams / AvcFilterParams
  planes:[PlaneRef];
  symbols:[SymbolCost]; symbol_names:[string];
  qp_hist:[uint];                  // 256 bins
  diagnostics:[Diagnostic];
}
```

Codec extension tables (`AvcMb`, `HevcCu`, `HevcPu`, `HevcTu`, `Av1Block`, `Av1Txb`, `*FilterParams`, `PartitionInfo`) are fully specified in the Data Model chapter from the per-codec tables in Appendix D. Rule: **every syntax element the decoder reads for a block is stored in the extension table**, with the exact spec name.

### 0.5.5 Container tree

```fbs
table Field { name:string; kind:SyntaxValueKind; uvalue:ulong; ivalue:long; svalue:string; bytes:[ubyte];
              span:BitSpan /* relative to BoxNode.file_span.offset */; spec_ref:string; children:[Field]; }
table BoxNode {
  fourcc:string; uuid:[ubyte];     // uuid set when fourcc == "uuid"
  file_span:ByteSpan;              // whole box incl. header
  header_length:ubyte;             // 8, 16 (largesize), +16 for uuid
  version:short = -1; flags:int = -1;   // FullBox
  fields:[Field];                  // decoded payload fields; empty for undecoded boxes
  payload_span:ByteSpan;           // undecoded remainder for hex display
  children:[BoxNode];
  spec_ref:string; description:string;   // from registry
  diagnostics:[Diagnostic];
}
table ContainerTree { format:string /* "isobmff","ivf","annexb","obu","y4m","raw","ebml","mpegts" */; root:[BoxNode]; tracks:[TrackInfo]; diagnostics:[Diagnostic]; }
```

`TrackInfo`, `SampleTable`, `EditList`, `FragmentTimeline`, `EncryptionInfo`, `HdrMetadata` are specified in the Container chapter.

### 0.5.6 Colour description (shared by decode, metrics, render)

```c
typedef struct ovt_ColorDescription {
  uint32_t struct_size;   /* = sizeof(ovt_ColorDescription) = 44; every ABI struct starts with this field (D4) */
  uint8_t primaries;      /* ITU-T H.273 ColourPrimaries code: 1=BT.709, 9=BT.2020, 12=P3-D65 ... */
  uint8_t transfer;       /* H.273 TransferCharacteristics: 1=BT.709, 16=PQ (ST 2084), 18=HLG, 13=sRGB ... */
  uint8_t matrix;         /* H.273 MatrixCoefficients: 1=BT.709, 9=BT.2020-NCL, 14=ICtCp ... */
  uint8_t full_range;     /* 0 limited, 1 full */
  uint8_t chroma_siting;  /* 0 unknown, 1 left (MPEG-2), 2 center (MPEG-1/JPEG), 3 top-left (HEVC/AV1 colocated) */
  uint8_t chroma_format;  /* 0=400 1=420 2=422 3=444 */
  uint8_t bit_depth;
  /* static HDR metadata (0 = absent); units per CTA-861.3 / ST 2086 */
  uint16_t mdcv_primaries[3][2]; uint16_t mdcv_white_point[2];
  uint32_t mdcv_max_luminance; uint32_t mdcv_min_luminance; /* 0.0001 cd/m^2 units */
  uint16_t max_cll; uint16_t max_fall;                       /* cd/m^2 */
  uint8_t has_hdr10plus; uint8_t has_dolby_vision; uint8_t dv_profile; uint8_t dv_level;
} ovt_ColorDescription;
```

### 0.5.7 Metrics

```fbs
enum MetricKind : ubyte { FullReference, NoReference, ReducedReference, Bitstream }
enum WorkingDomain : ubyte { CodeValue, LinearLight, PU21, ICtCp, ModelRGB }
table MetricDescriptor { id:string /* "psnr", "ssim", "ssim_x264", "ms_ssim", "vmaf", "vmaf_neg", "uvq", "xpsnr", "cambi", "de_itp", ... */;
  display_name:string; kind:MetricKind; domain:WorkingDomain; per_plane:bool; higher_is_better:bool;
  range_min:double; range_max:double; temporal_unit_frames:uint /* 1 = per frame; N = per N-frame chunk (UVQ sets N = round(fps), one 1-second chunk); 0 = per sequence */;
  outputs:[string] /* e.g. ["score","map16"] */; license:string; version:string; }
table FrameScore { frame:FrameId; values:[double] /* per plane or [combined] */; map_blob_id:string; map_block_size:ushort; }
table MetricsResult { descriptor:MetricDescriptor; normalization:string /* JSON of NormalizationConfig */; frames:[FrameScore];
  pooled_mean:[double]; pooled_harmonic:[double]; pooled_min:[double]; pooled_p1:[double]; pooled_p5:[double]; pooled_std:[double];
  alignment_offset_frames:int /* D9 sign: distorted i ↔ reference i+o */; alignment_confidence:double /* S(o*) − S(o₂) in [0, 2] */; engine:string /* "libvmaf 3.0.0", "ovt-kernels 1.0", "onnxruntime 1.2x" — exact pinned version string */; }
```

---

## 0.6 Public interfaces (normative signatures; chapters elaborate)

### 0.6.1 C API conventions (`core/include/ovt/*.h`)

Chapter 12 §12.3 holds the complete headers; this excerpt fixes the conventions. Every file under `core/include/ovt/*.h` is C11 (no C++ constructs); C++ helpers live under `core/include/ovt/cxx/*.hpp` and are never included from a C header (D22). Every entry point is `noexcept` (exceptions never cross the ABI), every ABI struct starts with `uint32_t struct_size`, and ownership follows D4.

```c
typedef enum ovt_status { OVT_OK = 0, OVT_E_INVALID_ARG, OVT_E_IO, OVT_E_UNSUPPORTED, OVT_E_CORRUPT,
                          OVT_E_NOMEM, OVT_E_CANCELLED, OVT_E_PLUGIN, OVT_E_INTERNAL } ovt_status;
const char* ovt_status_string(ovt_status);
typedef void (*ovt_log_fn)(int level /*0 trace..5 fatal*/, const char* module, const char* msg, void* user);
void ovt_set_logger(ovt_log_fn, void* user);

/* D4: a FlatBuffer or buffer returned through an out-parameter is owned by the CALLER and released with
   the single function ovt_buffer_free; where a type-specific release exists it is used instead, e.g.
   ovt_picture_release for the ref-counted ovt_Picture. A buffer returned by value (ovt_container_tree,
   ovt_stream_index, ovt_last_error) and a const char* (ovt_file_display_name) stay owned by the handle.
   See chapter 12 §12.3.1 rule 3. */
typedef struct ovt_Buffer { const uint8_t* data; size_t size; } ovt_Buffer;   /* exempt from struct_size */
void ovt_buffer_free(ovt_Buffer* buf);                 /* only for caller-owned buffers; zeroes *buf */
typedef struct ovt_FrameId { int32_t decode_index; int32_t display_index; } ovt_FrameId;   /* exempt from struct_size */

typedef struct ovt_File ovt_File;
ovt_status ovt_file_open(const char* path, ovt_File** out);                                             /* desktop */
ovt_status ovt_file_open_fd(int fd, uint64_t size, int take_ownership, const char* display_name, ovt_File** out);  /* Android content:// via ParcelFileDescriptor; pread on fd */
const char* ovt_file_display_name(const ovt_File*);                                                     /* handle-owned */
void       ovt_file_close(ovt_File*);

/* Container (chapter 12 §12.3.7 is the full block; chapter 8 is the owner) */
typedef struct ovt_Container ovt_Container;
ovt_status ovt_container_open(ovt_File*, ovt_Container** out);                 /* sniffs format; builds top-level tree lazily */
ovt_status ovt_container_open_format(ovt_File*, const char* format, const char* raw_format /*"WxH:pix_fmt[:num/den]"*/, const char* codec_hint, ovt_Container** out);
ovt_Buffer ovt_container_tree(ovt_Container*);                                  /* ContainerTree; by value → handle-owned */
ovt_status ovt_container_expand(ovt_Container*, uint64_t box_offset);
ovt_status ovt_container_track_info(ovt_Container*, int track_index, ovt_Buffer* out /* TrackInfo */);
ovt_status ovt_container_validate(ovt_Container*, ovt_Buffer* out /* DiagnosticList */);
ovt_status ovt_container_bytes(ovt_Container*, uint64_t offset, uint32_t len /*≤ 1 MiB*/, ovt_Buffer* out);   /* byte service, chapter 8 §8.8 */
void       ovt_container_close(ovt_Container*);

typedef struct ovt_Stream ovt_Stream;                 /* one video track / elementary stream */
ovt_status ovt_stream_open(ovt_Container*, int track_index, ovt_Stream** out);
ovt_status ovt_stream_build_index(ovt_Stream*, ovt_Job* job /* nullable */, ovt_Buffer* out_index /* StreamIndex */);
ovt_Buffer ovt_stream_index(ovt_Stream*);             /* last built StreamIndex; handle-owned */
ovt_status ovt_stream_unit_bytes(ovt_Stream*, uint32_t unit_index, ovt_Buffer* out_raw, ovt_Buffer* out_rbsp);
ovt_status ovt_stream_unit_syntax(ovt_Stream*, uint32_t unit_index, ovt_Buffer* out /* SyntaxTree, parsed on demand */);
ovt_status ovt_index_hrd_trace(ovt_Stream*, const ovt_HrdOptions* opts /*nullable = defaults*/, ovt_Buffer* out /* HrdTrace, phase 4 */);
ovt_status ovt_validate_stream_index(const uint8_t* fb, size_t len, ovt_Buffer* out /* DiagnosticList, rules V-001… */);

typedef struct ovt_Analysis ovt_Analysis;             /* bound to one stream and one plugin */
ovt_status ovt_analysis_rescan_plugins(void);          /* search paths: ovt_plugins_add_search_path, $OVT_PLUGIN_PATH, platform defaults (chapter 12 §12.3.11) */
ovt_status ovt_analysis_open(ovt_Stream*, const char* plugin_id /* nullable = default for codec */, ovt_Analysis** out);
ovt_status ovt_analysis_frame(ovt_Analysis*, int decode_index, uint32_t plane_stage_mask, ovt_Job*, ovt_Buffer* out /* FrameAnalysis */);
ovt_status ovt_analysis_plane(ovt_Analysis*, const char* blob_id, ovt_PlaneView* out);
ovt_status ovt_validate_frame_analysis(const uint8_t* fb, size_t len, const uint8_t* expected_source_hash /*nullable, 32 bytes*/, int codec_override /*-1 none*/, ovt_Buffer* out /* DiagnosticList */);
void       ovt_analysis_close(ovt_Analysis*);

typedef struct ovt_Decoder ovt_Decoder;              /* playback path */
ovt_status ovt_decoder_open(ovt_Stream*, int prefer_hw, ovt_Decoder** out);
ovt_status ovt_decoder_seek(ovt_Decoder*, int display_index);
ovt_status ovt_decoder_next(ovt_Decoder*, ovt_Picture** out);   /* ref-counted; release with ovt_picture_release */
void       ovt_picture_release(ovt_Picture*);

/* Jobs (chapter 12 §12.3.5) and cache (chapter 12 §12.3.14) */
typedef struct ovt_Job ovt_Job;
typedef struct ovt_JobProgress { uint32_t struct_size; double fraction /*0..1 or -1 unknown*/; uint64_t done; uint64_t total; const char* stage; } ovt_JobProgress;
typedef void (*ovt_progress_fn)(const ovt_JobProgress* p, void* user);   /* called from a worker thread, at most every 100 ms */
ovt_status ovt_job_create(ovt_Job** out);
void       ovt_job_set_progress_callback(ovt_Job*, ovt_progress_fn fn, void* user);
void       ovt_job_cancel(ovt_Job*);
void       ovt_job_progress(const ovt_Job*, ovt_JobProgress* out);   /* thread-safe snapshot */
void       ovt_job_free(ovt_Job*);
ovt_status ovt_cache_set_dir(const char* dir);   /* default root (D32, chapter 6 §6.7): ~/Library/Caches/org.openvideotools.ovt/ (macOS), $XDG_CACHE_HOME/ovt/ (Linux), app cache dir (Android) */
ovt_status ovt_cache_set_max_bytes(uint64_t max_bytes); /* default 20 GiB desktop, 2 GiB Android */
ovt_status ovt_cache_report(ovt_Buffer* out /* CacheReport */);
ovt_status ovt_cache_clear(const char* kind /*nullable*/, const char* source_path /*nullable*/, int older_than_days /*0 = any*/);

/* Plane and picture views handed across the ABI */
typedef struct ovt_PlaneView { uint32_t struct_size; const uint8_t* data; uint32_t width, height, stride_bytes; uint8_t bit_depth, sample_bytes, is_signed; } ovt_PlaneView;
typedef struct ovt_Picture   { uint32_t struct_size; ovt_FrameId id; ovt_PlaneView planes[3]; ovt_ColorDescription color; /* refcount internal */ } ovt_Picture;
```

### 0.6.2 Analysis decoder plugin ABI (`core/include/ovt/abi/analysis_decoder_v1.h`, BSD-2-Clause)

```c
#define OVT_ANALYSIS_ABI_VERSION 1
typedef struct ovt_AnalysisDecoder ovt_AnalysisDecoder;
typedef struct ovt_AnalysisCaps {
  uint32_t struct_size;          /* D4 */
  uint8_t codec;                 /* ovt.fb.Codec */
  uint32_t plane_stages;         /* bitmask of ovt.fb.PlaneStage this plugin can export */
  uint32_t grid_fields;          /* bitmask: which CellGrid vectors are filled */
  uint8_t has_symbol_accounting; uint8_t bit_exact; uint8_t supports_threads;
  const char* id; const char* version; const char* license; const char* notes;
} ovt_AnalysisCaps;
typedef struct ovt_AnalysisDecoderApi {
  uint32_t struct_size;          /* D4 */
  uint32_t abi_version;
  const ovt_AnalysisCaps* (*caps)(void);                       /* static, immutable for the plugin's lifetime */
  ovt_status (*create)(const uint8_t* codec_private, size_t len /* avcC/hvcC/av1C or NULL; valid only during create(): the plugin copies what it needs */, int threads, ovt_AnalysisDecoder** out);
  void       (*destroy)(ovt_AnalysisDecoder*);
  /* Push one Unit (Annex B NAL without start code / one OBU with size field). Returns OVT_OK; frames come out of pull. */
  ovt_status (*push)(ovt_AnalysisDecoder*, const uint8_t* unit, size_t len, int32_t decode_index_hint, int64_t pts);
  /* Pull the next completed frame. out_fb is a FrameAnalysis FlatBuffer owned by the plugin until release(). Returns OVT_OK with out_fb.data == NULL when no frame is ready. */
  ovt_status (*pull)(ovt_AnalysisDecoder*, uint32_t plane_stage_mask, ovt_Buffer* out_fb);
  ovt_status (*plane)(ovt_AnalysisDecoder*, const char* blob_id, const uint8_t** data, size_t* size);
  void       (*release)(ovt_AnalysisDecoder*, ovt_Buffer fb);
  ovt_status (*flush)(ovt_AnalysisDecoder*);
} ovt_AnalysisDecoderApi;
/* The only exported symbol of a plugin shared library (declared here, not in chapter 12 §12.3 — plugins do not link libovt-core; the FFmpeg plugin additionally exports the optional ovt_plugin_describe, chapter 7 §7.6.3). Declared with OVT_PLUGIN_EXPORT (default visibility; issue #211). All entry points are noexcept. */
const ovt_AnalysisDecoderApi* ovt_analysis_decoder_v1(void);
```

Rules: a plugin is single-threaded from the caller's point of view (one instance, one thread); it MUST never crash on malformed input (return `OVT_E_CORRUPT` with a `Diagnostic`); it MUST fill `FrameAnalysis.frame.id.decode_index` from `decode_index_hint`; all per-block syntax it reads MUST appear in the codec extension table.

### 0.6.3 Metric plugin ABI (`core/include/ovt/abi/metric_v1.h`, BSD-2-Clause)

```c
typedef struct ovt_MetricApi {
  uint32_t struct_size;                       /* D4 */
  uint32_t abi_version;                       /* = 1 */
  ovt_Buffer (*descriptor)(void);             /* MetricDescriptor FlatBuffer */
  ovt_status (*create)(const char* options_json, ovt_Metric** out);
  void       (*destroy)(ovt_Metric*);
  /* Frames are delivered already normalised into descriptor.domain; ref == NULL for NoReference metrics. */
  ovt_status (*score)(ovt_Metric*, const ovt_MetricPlanes* ref /* nullable */, const ovt_MetricPlanes* dist, ovt_FrameId id, ovt_Buffer* out /* FrameScore */);  /* ovt_MetricPlanes: chapter 9 §9.2.2 */
  ovt_status (*finish)(ovt_Metric*, ovt_Buffer* out /* optional sequence-level FrameScore */);
} ovt_MetricApi;
const ovt_MetricApi* ovt_metric_v1(void);   /* the only exported symbol of a metric plugin; noexcept */
```

### 0.6.4 Render surface (`render/include/ovt/render/video_surface.h`, no toolkit types)

Abridged from chapter 10 §10.2, which is normative for every field; these are C++ types of `libovt-render`, not FlatBuffers tables (they never appear in chapter 6).

```cpp
namespace ovt::render {
// Value types — defined in chapter 10 §10.2 (not reproduced here, not FlatBuffers tables):
//   DisplayTarget (§10.2.2): Kind {OffscreenTexture, MetalLayer, AndroidWindow, WaylandSurface, VulkanSurface}, native handles, size, hdr_capable, sdr_white_nits, headroom
//   ToneMap {Auto, BT2390, BT2446A, Spline, Clip, None}, GamutMap {Perceptual, Relative, Saturation, Absolute, Desaturate, Darken, Clip, Highlight}  (§10.2.3)
//   ViewState (§10.2.3): zoom, pan, plane_mode, raw_code_values, show_cropped, tone_map, gamut_map, clip_nits, false_color, rotation, field_mode …
//   CompareMode (§10.2.4): Kind {Single, SideBySide, Split, Overlay, Difference, Flicker}, orientation, gain 1–64, false colour
//   OverlaySet (§10.2.5): opaque GPU buffers built by OverlayBuilder from a FrameAnalysis
class VideoSurface { public:                 // §10.2.7
  static std::unique_ptr<VideoSurface> create(const char* log_prefix = "render");
  virtual bool init(const DisplayTarget&) = 0;
  virtual bool resize(int width, int height) = 0;
  virtual void retarget(const DisplayTarget&) = 0;             // EMBEDDED <-> NATIVE, keeps uploaded frames
  virtual void set_frame(const Picture& a) = 0;                // slot A; uploads planes (R8/R16)
  virtual void set_compare(const Picture& b, const CompareMode&) = 0;   // slot B + mode
  virtual void set_overlays(std::unique_ptr<OverlaySet>) = 0;
  virtual void render(const ViewState&) = 0;                   // one frame; deterministic for identical inputs (FR-PLN-101)
  virtual ExternalTexture output_texture() const = 0;          // OffscreenTexture targets only
  virtual Pick pick(int px, int py) const = 0;                 // block id / mv id / pixel value under the cursor
  virtual Measurements measure() = 0;                          // MaxCLL/FALL in nits, histogram, scopes
  virtual bool export_png(const char* path, ExportKind, const ViewState* view_or_null) = 0;
  virtual const char* last_error() const = 0;                  // "" or a RND-nnnn code
};
}
```

---

## 0.7 Cross-cutting rules

1. **Robustness (NFR):** every parser reads through `ovt::BitReader`, which bounds-checks every read and throws `ovt::ParseError` (caught at module boundary → `OVT_E_CORRUPT` + `Diagnostic`). No raw pointer arithmetic on input data outside `BitReader` and `RandomAccessFile`. Every parser has a libFuzzer target; CI runs ASan/UBSan on all tests.
2. **Threading:** core objects are **not** thread-safe unless documented. `ovt::jobs::Pool` owns worker threads; GUI calls core only on job threads except for cheap getters. Cancellation via `ovt_Job` checked at least every 50 ms of work.
3. **Memory budget:** per open stream ≤ 2 GB RSS on desktop at 4K, ≤ 1 GB PSS on Android; `FrameAnalysis` grids are kept for an LRU of 64 frames (16 on Android), planes for an LRU of 4 frames (2 on Android); everything else goes to the disk cache under the cache root of D32 (chapter 6 §6.7), capped at 20 GiB on desktop and 2 GiB on Android (D5).
4. **Performance budgets (NFR; D5):** reference machine = M1 MacBook Air 8 GB (`m1-air-8gb`, chapter 14 §14.9.2); Android reference = Google Pixel Tablet (SDR panel; `pixel-tablet`); Android HDR acceptance on `android-hdr`/`chromebook-hdr`. Header-only index of a 1-hour 1080p H.264 file ≤ 2 s; first frame displayed ≤ 500 ms after open; overlay redraw at 4K with 4×4 MV arrows ≤ 16 ms on desktop and ≤ 33 ms on Android (p95); block analysis of a frame from the nearest RAP in a GOP of ≤ 60 frames at 1080p ≤ 1.5 s (libde265), ≤ 3 s (libaom) on the M1 and ≤ 5 s on the Pixel Tablet; PSNR + SSIM ≥ 200 fps and VMAF ≥ 60 fps at 1080p on 4 threads; seek ≤ 400 ms (HW) / ≤ 1.5 s (SW); cancellation observed ≤ 50 ms. **The single normative source of every budget and benchmark name is chapter 14 §14.9.3 and its machine form `tests/perf/budgets.json` (D19)**; this rule and D5 restate the headline numbers for orientation, and chapters 08, 09 and 10 cite §14.9.3 without repeating numbers.
5. **Determinism:** identical input → byte-identical FlatBuffer output (no pointers, no timestamps inside analysis records; timestamps live in cache metadata).
6. **Logging & diagnostics:** every user-visible error has a stable `code` of the form `<FAMILY>(-<QUALIFIER>)?-<nnnn>` (`SYN-H265-0007`, `CNT-0040`; families CNT, SYN, DEC, VQ, MDL, IO, BR, VER, ANA, RND, GUI, CLI — D2, chapter 5 §5.7) and a spec reference where applicable.
7. **Accessibility & i18n:** all strings in QML via `qsTr`; keyboard-operable panels; minimum contrast 4.5:1 for text; colour maps selectable including a colour-blind-safe set (viridis, cividis).
8. **Security:** input files are untrusted; no code execution from inputs; sidecar metric processes run with a restricted environment; the Python module never `eval`s input.
9. **Compatibility exports:** YUView statistics CSV (`%;syntax-version;v1.2` …), libvmaf-compatible JSON, aom `inspect` JSON (subset) — exact formats in the Metrics and Analysis chapters.

---

## 0.8 Roadmap skeleton (phases are sequential for the core; Android is a parallel track)

| Phase | Theme | Exit criterion (demo) |
|---|---|---|
| **0 Foundations** | repo, CMake presets, vcpkg, third_party scripts, CI matrix (macOS, Linux, Android), FlatBuffers schemas, `io`, `BitReader`, logging, jobs, licence gate, corpora fetcher, **ChromeOS/Android HDR hardware spike** | `cmake --preset macos-arm64-release && ctest` green on CI; APK installs on a tablet and shows a PQ test pattern through `SurfaceView` |
| **1 See the stream** | own ISOBMFF reader (tree + offsets + tracks + sample table), IVF/Annex B/OBU, libavformat demux, H.264/HEVC/AV1 header parsers + SyntaxTree, StreamIndex, CLI `ovt probe/syntax/index`, playback decode (HW + SW), libplacebo VideoSurface with HDR output on macOS, Qt GUI skeleton: container tree, syntax tree ↔ hex, timeline bar chart, picture panel with zoom/pan, Y/U/V plane views, HDR metadata panel | Open any corpus MP4; click a syntax field → hex highlights; scrub timeline; view PQ content correctly on an XDR display and tone-mapped on SDR |
| **2 Look inside blocks** | plugin host; `ovt-plugin-aom` (libaom inspection + accounting); `ovt-plugin-de265` (internals patch + upstream PR); `ovt-plugin-ffmpeg` (H.264 MVs/QP via public API); overlays: partitions, prediction modes, MVs, QP map, bits heatmap, ref-index map; Selection Info; per-stage planes; GOP/reference graph; statistics panel; CLI `ovt analyze` (FlatBuffers/JSON/YUView CSV) | For each codec: click a block → see all its syntax; toggle overlays at 60 fps; export stats |
| **3 Measure quality** | normalisation stage (alignment, scale, zimg colour), PSNR/SSIM kernels + maps, libvmaf host, UVQ via ONNX Runtime, metrics panel (timeline, heatmap overlay, A/B/diff views), pooling, CSV/JSON export, BD-rate, CLI `ovt metrics` / `ovt bdrate` | Reproduce `vmaf` CLI output bit-exactly on Netflix test vectors; UVQ matches reference CLI within 0.01 MOS |
| **4 Deepen & extend** | H.264 MB-type/intra export patch; HM oracle plugin + verify mode; residual/coefficient views; HRD/buffer graph; conformance checks (decoded picture hash SEI, level limits); Matroska and TS trees; DASH/HLS virtual file; Python package; Linux Wayland HDR; sidecar metric protocol + ColorVideoVDP; HDR scopes (false colour, nits waveform, vectorscope) | Feature matrix parity with StreamEye "must-haves" for the three codecs |
| **A Android/ChromeOS track** (starts in Phase 0, ships with each phase) | Gradle project, Qt Quick embedding, `SurfaceView` HDR sibling, content:// I/O, x86_64 + arm64 ABIs, tablet/Chromebook layouts (resizable window, keyboard/mouse, hover), Play + F-Droid packaging | Every desktop phase demo also runs on a Pixel Tablet and an HDR-capable Chromebook |

Ordering rationale (owner asked for a codec-engineer / social-media-video-team ordering): first, know exactly what is in the file and see it correctly (container, syntax, HDR display) — the daily "what did the encoder/packager emit" question; second, block-level truth, which is the codec engineer's core need and the hardest technical risk; third, quality measurement, which the video team needs for ladder and encoder decisions and which depends on correct decoding and colour handling from phases 1–2; fourth, parity features.

---

## 0.9 Glossary seed (chapters add to this)

**Access unit**, **Annex B**, **AVC/H.264**, **AV1**, **bits per block (estimated)** — entropy-decoder cost in 1/8-bit units (CABAC and range coders have no per-block byte boundary; only slice/tile/OBU sizes are exact), **block**, **cell**, **CABAC**, **CAVLC**, **CDEF**, **CLL (MaxCLL/MaxFALL)**, **CMAF**, **coded picture coordinates**, **conformance window**, **CTU/CU/PU/TU**, **DPB**, **emulation prevention byte (EPB)**, **EOTF/OETF/OOTF**, **FlatBuffers**, **GOP**, **HDR10 / HDR10+ / HLG / PQ / Dolby Vision**, **HEVC/H.265**, **hidden frame (AV1)**, **ISOBMFF**, **libplacebo**, **loop restoration**, **macroblock**, **MDCV**, **mode info (MI)**, **motion vector (MV)**, **NAL unit**, **OBU**, **order hint**, **parameter set (VPS/SPS/PPS)**, **plane (Y/U/V)**, **POC**, **QP / qindex**, **random access point (RAP)**, **RBSP**, **reference list (L0/L1)**, **SAO**, **SEI**, **superblock**, **temporal unit**, **tone mapping**, **VMAF / PSNR / SSIM / UVQ**, **Y4M**.

---

## 0.10 Chapter list and ownership

| # | Chapter file | Content | Primary research inputs |
|---|---|---|---|
| 0 | `00-backbone.md` | this backbone, reprinted (sections `0.n`) | — |
| 1 | `01-introduction.md` | vision, personas, goals/non-goals, success criteria, how to read, document map | backbone, Appendix A |
| 2 | `02-glossary.md` | all terms (merged from chapters) | all |
| 3 | `03-competitive-reference.md` | what commercial tools do; feature matrix; must/should/nice; UX conventions adopted | Appendix A |
| 4 | `04-requirements.md` | FR/NFR with acceptance tests, grouped by area | reports 01, 05, backbone |
| 5 | `05-architecture.md` | layers, data flow, process/thread model, ADRs in full | backbone, 02, 05, 06 |
| 6 | `06-data-model.md` | complete FlatBuffers schemas incl. codec extension tables; coordinate/unit rules; cache format; export formats | Appendix D, backbone |
| 7 | `07-bitstream-and-decoders.md` | `io`, `BitReader`, syntax parsers per codec, `index`, plugin ABI, each plugin (aom, de265 incl. patch spec, ffmpeg, hm), playback decoders | reports 02, 06 |
| 8 | `08-container.md` | ISOBMFF reader design, registry generation, derived views, IVF/Annex B/OBU/Y4M, later EBML/TS/MXF, DASH/HLS, validation | Appendix B.2 |
| 9 | `09-metrics.md` | plugin model, normalisation, kernels, libvmaf host, UVQ host, sidecar protocol, pooling, BD-rate, exports, HDR domains | Appendix B.3 |
| 10 | `10-rendering-and-hdr.md` | VideoSurface, libplacebo pipeline, overlays, picking, HDR output per platform, HDR analysis measurements, per-plane/diff views | Appendix B.4 |
| 11 | `11-gui.md` | panels, layouts, interaction model, keyboard map, colour language, states, Android/ChromeOS adaptations | reports 01, 05 |
| 12 | `12-cli-and-api.md` | `ovt` subcommands, output schemas, exit codes; C API; Python module | backbone |
| 13 | `13-platforms-build-ci.md` | build system, presets, third_party scripts, Android project, macOS signing, Linux packaging, CI matrix | reports 05, 07 |
| 14 | `14-testing.md` | corpora, oracles, golden tests, fuzzing, performance tests, acceptance | reports 02, 03, 04, 06 |
| 15 | `15-licensing-and-governance.md` | licences, patents, decoder plugin distribution, DCO, trademark, third-party notices | Appendix E |
| 16 | `16-roadmap-and-work-items.md` | phases, milestones, WI list with dependencies | all chapters |
| 17 | `17-risks-and-open-questions.md` | risks with mitigations; decisions for the owner | all |
| A–F | appendices | commercial feature inventory, library survey tables, spec syntax cross-reference, full schema listing, URL index | reports |

---

## 0.11 Cross-chapter decisions (D1–D32)

These rulings settle cross-chapter questions once; chapters cite them as `Dn`. Where a decision names a chapter as owner, that chapter's text is normative and the others mirror it.

**D1.** FR/NFR ids are defined ONLY in chapter 04. A per-chapter table (chapters 08–13, 15) MUST NOT invent or redefine FR ids: it is a "Requirements realised by this chapter" table whose rows cite a chapter-04 id with chapter 04's exact requirement sentence and name the sections/WIs that realise it. Requirements first identified in a design chapter were given chapter-04 ids in the `FR-<AREA>-1nn` ranges.

**D2.** Diagnostic code format (normative; chapter 6 V-029 and chapter 5 §5.7): `<FAMILY>(-<QUALIFIER>)?-<nnnn>` where FAMILY ∈ {CNT, SYN, DEC, VQ, MDL, IO, BR, VER, ANA, RND, GUI, CLI}, QUALIFIER is 2–8 uppercase alphanumerics (e.g. `CNT-TS-nnnn`, `SYN-H264-nnnn`, `DEC-H265-nnnn`), nnnn is four digits. Bare `CNT-nnnn` means ISOBMFF/QuickTime; `CNT-ISOBMFF-nnnn`/`CNT-ISO-nnnn` spellings are replaced by `CNT-nnnn`. EVERY code used anywhere must appear in exactly one registry table: CNT-* in 08 §8.5; SYN-*, DEC-*, IO-*, BR-* in 07 §7.9; VQ-* in 09 §9.11.4; MDL-*, VER-* in 06; ANA-* in 07 §7.9.5 (ANA-LRU in 12 §12.3.10); RND-* in 10 §10.10; GUI-* in 11 §11.7; CLI-* in 12 §12.1.3. Registry row: code | severity | message template | when raised | spec reference.

**D3.** CLI: chapter 12 is the owner. Canonical output selector is `--format {text,json,fb,csv,...}` as 12 §12.1.2 defines; `--json` is defined (in §12.1.2) as an alias of `--format json`, so other chapters may keep `--json`. Exit codes: 12 §12.1.3 table is canonical and is NOT renumbered; chapters 04 and 08 and 14 are aligned to it. Every option another chapter uses on a subcommand must be defined in that subcommand's grammar in 12, or the other chapter's text uses an existing option. `validate`, `import`, `fbdiff`, `hdr-scan`, `render-frame` are not subcommands: use `ovt index --validate` / `ovt analyze --validate`, `ovt cache import`, `ovt dump --diff`, `ovt hdr --measure`, `ovt render`.

**D4.** C API ownership rules (chapter 12 §12.3 is the owner; chapter 0 §0.6.1 and chapter 8 §8.8 mirror it): (a) Any `ovt_Buffer`/pointer returned through an out-parameter is owned by the caller and released with the single function `ovt_buffer_free` (or the type-specific `ovt_<type>_release` where one exists); (b) `ovt_Picture` returned by `ovt_decoder_next` is reference-counted: the caller owns one reference and MUST call `ovt_picture_release`; it stays valid across further decoder calls; Python wraps it in a capsule whose destructor calls `ovt_picture_release` (zero-copy is safe). (c) No exceptions cross the C ABI; every C entry point is `noexcept` and converts to `ovt_Status`. (d) Every ABI struct begins with `uint32_t struct_size`.

**D5.** Resource budgets and pins (owners: 13 for toolchains, 05/06 for caches, 10/14 for render): first frame ≤ 500 ms; index of 1 h 1080p ≤ 2 s; overlay redraw ≤ 16 ms at 4K on desktop, ≤ 33 ms on Android/ChromeOS GLES; analysis ≤ 1.5 s (libde265) / ≤ 3 s (libaom) per 60-frame 1080p GOP on M1, ≤ 5 s on Pixel Tablet; PSNR+SSIM ≥ 200 fps 1080p; VMAF ≥ 60 fps 4 threads on M1; disk cache cap 20 GiB desktop / 2 GiB Android; plane LRU 4 frames (2 on Android); FrameAnalysis LRU 64 frames (16 on Android) — §0.7.3; RSS ≤ 2 GiB desktop / ≤ 1 GiB Android. Budget numbers are owned by chapter 14 §14.9.3 (D19). Pins: C++20; CMake ≥ 3.25; Clang ≥ 16 (Xcode ≥ 16.0), GCC ≥ 13; Qt 6.8.3 (LTS; 6.10 is NOT LTS — say "a later Qt 6.x" where Wayland HDR is concerned); NDK r27c; minSdk 28; targetSdk 36; Python ≥ 3.11; FFmpeg ≥ 7.1; libaom ≥ 3.9 (oracle pin 3.12.1); libde265 1.0.15; libvmaf ≥ 3.0.0 (the "VMAF v1 model" claim is unverified: say "libvmaf 3.x with models vmaf_v0.6.1, vmaf_v0.6.1neg, vmaf_4k_v0.6.1; newer bundled models when present"); libplacebo ≥ 7; HM-18.0; Flatpak app id `org.openvideotools.Studio`; Android package `org.openvideotools.studio`.

**D6.** AV1 reference slots: in the neutral model `FrameAnalysis.refs[]` (`table RefSlot`) is indexed by **reference type** — `refs[0]` is the INTRA placeholder, `refs[1..7]` are LAST…ALTREF (spec numbering), `RefSlot.index` is that position; `CellGrid.ref0/ref1` hold `RefSlot.index` (sentinel −1 = unset; never 0 for a cell); `RefSlot.slot` records the DPB slot `ref_frame_idx[index − 1]` (0–7) the reference type was mapped to (−1 for H.26x and for INTRA). Chapter 06 §6.3.1 defines this; chapter 07 maps libaom accordingly.

**D7.** `FrameInfo.size_bytes` = sum of the file spans of all units of the access unit/temporal unit INCLUDING start codes / length prefixes / OBU headers, and including preceding AUD/SEI/parameter sets that belong to the AU (chapter 07 §7.4.2 S7). V-031 and chapter 06 are aligned.

**D8.** Per-chapter screenshot/GUI test harness: chapter 14 §14.10.2 is canonical; chapter 11 §11.9 references it. Keyboard map: chapter 11 §11.4.3 is canonical; UX-2 (chapter 03) and FR-GUI-008 (chapter 04) are aligned to it. Settings storage: JSON file at the platform config dir (`~/.config/ovt/settings.json`, `~/Library/Application Support/ovt/settings.json`, Android app files dir) — no QSettings.

**D9.** Alignment: temporal alignment uses NCC with confidence = S(o*) − S(o2) in [0,2], `min_confidence` default 0.05; sign convention: offset o means distorted frame i corresponds to reference frame i + o. All chapters use this sign.

**D10.** ssim_x264 kernel: specify exactly the x264 `x264_pixel_ssim_wxh` algorithm (4×4 sums, 8×8 windows stepping 4, SSIM constants on 64 pixels, result averaged over (w/4−1)(h/4−1) windows); tiny_ssim (libvpx) differs and is NOT an oracle for it. Name the libvpx variant `ssim_tiny` only if kept, with its own definition; otherwise drop tiny_ssim from oracles.

**D11.** UVQ: state that the published google/uvq models are TensorFlow SavedModels; export path is tf2onnx (fallback: keras2onnx/manual re-implementation); keep the ONNX Runtime host unchanged.

**D12.** Pixel Tablet: SDR panel (no HDR). Android HDR acceptance uses an HDR-capable Android device or Chromebook; chapter 14 §14.9.2 hardware table is canonical.

**D13.** The FlatBuffers schema in chapter 06 is the only schema; chapter 0 §0.5 reproduces excerpts of it and defines nothing of its own (render/GUI state types such as `DisplayTarget`, `ViewState`, `OverlaySet` are C++ types owned by chapter 10 §10.2). Every `Table.field` reference in any chapter must exist in 06.

**D14.** Repository layout: `cmake/` is a top-level directory (chapter 0 §0.4). Produces paths are always repo-rooted (`core/src/...`, never `src/...`). Fuzz targets: `core/tests/fuzz/`; perf: `tests/perf/` harness, benchmark names `bench_*` (chapter 14 §14.9.3 canonical).

**D15.** Every WI block MUST have all template lines (Goal, Depends on, Produces, Interface, Steps, Acceptance tests, Size, Notes/pitfalls); Interface may be "none (build/CI only)" but must exist; acceptance tests are Given/When/Then).

**D16.** Section references use the long form "chapter N §N.n.m" (always with the chapter number inside the § number). Chapter 08 §8.3 groups get numbered headings (§8.3.1 Group A …) and 14496-12 clause references are written "14496-12 §8.6.1.2".

**D17.** libaom inspection: one `aom_codec_decode` per frame (not per temporal unit) when driving the inspect path; `user_priv` is not used for hints (CONFIG_INSPECTION's decoder_inspect writes an `Av1DecodeReturn` through it) — chapter 7 §7.6.1 states which fields are available from stock `insp_mi_data`/`insp_frame_data` (no plane buffers, no superblock_size) versus from the OVT patch.

**D18.** V-002: AV1 coded size may be any size (frame_width_minus_1 arbitrary); cells on the right/bottom edge may extend beyond the coded picture; AV1 render size is independent of coded size (not a crop) — chapter 6 V-002 and §6.2 define this.

**D19.** Performance budgets have one source: chapter 14 §14.9.3 and `tests/perf/budgets.json`. Chapter 0 §0.7.4/D5 restate headline numbers for orientation; chapters 08, 09 (§9.11.3) and 10 cite "chapter 14 §14.9.3" and repeat no numbers except as "(see `budgets.json`)". The Android analysis budget is 5 s per 60-frame 1080p GOP on `pixel-tablet` (FR-AND-014), not a multiple of the M1 budget.

**D20.** Diagnostic codes outside the D2 families are renamed: `HDR-CLL-0001` → `VQ-HDR-0002`; `HDR-0001` → `DEC-HDR-0001`; `MET-ALIGN-0001` → `VQ-ALIGN-0001`; `MET-MODEL-0001` → `VQ-MODEL-0001`; `MET-BD-0003` → `VQ-BD-0003`; `PLG-ABI-0001`/`PLG-DUP-0002`/`PLG-SIG-000x`/`PLG-PERM-000x` → `ANA-PLG-0001…` (plugin-host family, registry in chapter 07 §7.9). Every code has a registry row in its owning chapter.

**D21.** CLI (chapter 12 owner): the raw-input geometry option is `--raw-format WxH:pix_fmt[:fps]` (chapter 12 §12.1.5); `ovt dump --raw` keeps its meaning (raw plane output). The decoded-picture-hash flag is `--hash-sei` everywhere (`--hash`/`--picture-hash` are aliases in §12.2.0 only). Chapter 04 FR-CNT-014 / FR-CLI-012 / FR-CLI-013 and chapters 08/09/14 follow.

**D22.** Public headers: C API headers live in `core/include/ovt/*.h` and are C11; C++ helper headers live in `core/include/ovt/cxx/*.hpp` (`ovt/cxx/result.hpp`, `ovt/cxx/jobs.hpp`, `ovt/cxx/log.hpp`, `ovt/cxx/cancel.hpp`, …) and carry no ABI promise. WI-1.512, chapter 05, chapter 07 §7.1.6, chapter 12 §12.3 and chapter 0 §0.4/§0.6 use these paths.

**D23.** CMake presets: chapter 13 §13.2.4 owns and defines every preset name used anywhere — configure/build presets `macos-arm64-{debug,release}`, `macos-x86_64-release`, `linux-x64-{debug,release}`, `linux-x64-asan-ubsan`, `macos-arm64-asan`, `linux-x64-tsan`, `linux-x64-fuzz`, `android-arm64-{debug,release}`, `android-x86_64-debug`; test presets `pr`, `nightly`, `sanitizers`, `perf`. Chapters 14 and 16 use exactly these names. The corpora manifest is `tests/corpora/manifest.json` (chapter 14 §14.2.4); there is no `.yaml`.

**D24.** One `-Werror` warning set: chapter 13 §13.2.5 (`ovt::flags`) is canonical and chapter 14 §14.8.3 references it. Catch2: ordinary test executables link `Catch2::Catch2WithMain`; the golden runner links `Catch2::Catch2` with its own `main` (chapter 14 §14.2 and WI-0.550 state this).

**D25.** Scheduling: chapter 16 owns milestones, the WI chapter owns the `**Depends on:**` line, and both change together. Every dependency's milestone ordinal is ≤ the dependent WI's (chapter 16 §16.0/§16.2.5 rule; `MA.n` milestones take the ordinal of the first core milestone in their title). In particular: header-parser WIs (WI-1.108 …) do not depend on `ovt syntax` (WI-1.504) or on golden fixtures (WI-0.567) — the golden suite WI-1.572 depends on them; the licence gate WI-0.602 depends on WI-0.601, WI-0.000 and WI-0.010 only (not on the release workflow WI-0.036) and `tools/license_policy.toml`'s `[directories]` table is produced by WI-0.601; WI-0.608's acceptance test uses the gate's own collector; WI-1.512's tests use a synthetic job, not the index builder; WI-1.409 depends on WI-1.408 only (value text moves to WI-2.433); WI-4.104 depends on WI-0.010 only; `test_model_roundtrip` is produced by WI-0.093; a core-phase WI does not depend on an Android-track (`MA.n`) WI that runs in parallel with it (WI-0.606 generates the Android branding keys without waiting for WI-0.050).

**D26.** Fixtures: chapter 14 §14.3 is the single fixture registry. Every corpus file or synthetic fixture named in any acceptance test (chapters 04, 06–13, 16) appears there — manifest sources table, named-fixture table or synthetic recipe table — with its source id and `fast`-subset membership; fixture names follow the corpus-id rule `<source>_<name>` of chapter 14 §14.2.3. Where a fixture cannot exist, the citing chapter names an existing one. The synthetic HLG recipe is `syn_hlg_1080p`.

**D27.** Every tool or file consumed by a WI is produced by exactly one WI's **Produces** line (`tools/ref_annexb.py`, `tools/extract_samples.py`, `tools/yuview_csv_lint.py`, `tools/release/manifest_schema.py`, `cmake/ovt-config.cmake.in`, the WI-0.551 fixture server, the TSDuck oracle for WI-4.205, the `docs/spec/` commit in WI-0.000, …); the owning chapter decides which WI (chapter 13 for cmake/release, chapter 14 for tools/oracles, chapters 07/08 for codec/container helpers) and chapter 16 §16.4 mirrors it.

**D28.** New work items are added only by the specification editor following chapter 16 §16.2.6 (next free id in the chapter's range; new ranges opened by the editor — first: ids 4.470 to 4.479 of phase 4, chapter 11; row in §16.4 first, then the block, then the §4.17 rows). The specification is self-versioned per chapter 16 §16.2.8: front-matter `MAJOR.MINOR.PATCH`, `docs/spec/CHANGELOG.md`, every WI text change recorded, implementation PRs state the spec version they were built against.

**D29.** Reference hardware: the ids and specifications of chapter 14 §14.9.2 are canonical — `m1-air-8gb` (MacBook Air M1, 8 GB, macOS 15), `linux-x64-ref` (Ubuntu 24.04), `pixel-tablet` (SDR), `android-hdr`, `chromebook-hdr`. Chapters 02 and 04 mirror them; no other chapter states RAM or OS versions for these machines.

**D30.** Decoder facts at the pinned versions (chapters 06/07/14): libaom v3.12.1 dispatches `aom_codec_decode()` to `decoder_inspect()` iff `user_priv != NULL`, and the inspection callback is honoured only on that path; `insp_mi_data` has no `sb_bits` and the inspect JSON has no `sb` key — AV1 `Block.bits_x8` is accounting-derived only; inspect JSON keys are the names of `examples/inspect.c` (`motion_mode`, `intrabc`, `palette`, `uv_palette`, `cdef_level`, `cdef_strength`, `cfl_alpha_idx`, `cfl_alpha_sign`, `referenceFrameMap`, …, listed in chapter 06 §6.8.3). FFmpeg 7.1: no AV1 VideoToolbox hwaccel (AV1 is software `libdav1d`/`libaom` on macOS, MediaCodec on Android); MediaCodec 10-bit buffer output is not P010-mapped (surface path or software fallback). HM `Pel` is `int16`: the HM plugin packs 8-bit pictures to byte planes (V-023 holds). FFmpeg h264 `init_qp` includes `qp_bd_offset`: `QpY = qp − QpBdOffsetY` is stored (V-028 holds). HRD: the initial arrival/removal recursion uses the first access unit of the *previous* buffering period (H.264 C.1.2 / H.265 C.2.2). FFmpeg configure uses `--disable-shared`, never `--enable-shared=no`. WI-1.130 out-parameter buffers are caller-owned (D4).

**D31.** Scene detection is exactly FFmpeg `scdet` (`libavfilter/vf_scdet.c`): SAD over **all** planes at their own dimensions, `mafd = sad · 100 / (count · 2^bitdepth)`, score = `clip(min(mafd_t, |mafd_t − mafd_{t−1}|), 0, 100)`, threshold 10.0 by default, no downscaling; chapter 09 §9.8.2 and chapter 04 FR-VQ-021 state this so the scdet oracle is bit-exact.

**D32.** Cache root (chapter 06 §6.7 owner; chapters 11/12 follow): `~/Library/Caches/org.openvideotools.ovt/` on macOS, `$XDG_CACHE_HOME/ovt/` on Linux, the app cache directory (`Context.getCacheDir()/ovt`) on Android.
