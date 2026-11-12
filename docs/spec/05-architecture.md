# 5. Architecture

This chapter describes the structure of OVT: its layers, components, data flow, process and threading model, and the sixteen architecture decisions (ADRs) that bind every other chapter. Chapters 6–13 specify each component in detail; this chapter is the map.

## 5.1 Architectural principles

1. **Core produces data; front-ends display it.** Every fact shown anywhere in the GUI exists first as a field in a FlatBuffers record produced by `libovt-core` (chapter 6) and is reachable from the `ovt` command line (chapter 12). Front-ends never parse bitstreams, decode pixels or compute metrics themselves.
2. **Codec-neutral core, lossless codec extensions.** Views (grids, arrows, heatmaps, trees) are written once against the neutral model (`CellGrid`, `Block`, `MotionVector`, `SyntaxNode`). Every codec-specific syntax element is still preserved in a typed extension table so nothing is lost.
3. **Instrument upstream decoders; never re-implement them.** Block-level truth comes from the software decoders that already compute it (libaom, libde265, FFmpeg's H.264 decoder), through the smallest possible read-only hooks, behind a C plugin ABI. Header-level syntax (parameter sets, slice/frame headers, SEI/metadata) is parsed by OVT's own position-recording parsers because no upstream API records bit offsets.
4. **Two decode paths.** A fast playback path (hardware decoders where available) for scrubbing and viewing, and a GOP-local analysis path (instrumented software decoders) for the frame under inspection.
5. **Correct colour everywhere.** One colour description structure travels with every picture from the container/bitstream through metrics normalisation to the renderer; libplacebo owns the display transform.
6. **Hostile input is the normal case.** Every byte read from a file goes through bounds-checked readers; every parser has a fuzz target; a malformed stream produces diagnostics, never a crash.
7. **Deterministic and reproducible.** Identical input produces byte-identical records; every result carries the configuration that produced it (plugin id and version, normalisation config, model version).
8. **Small, upstreamable patches.** Any modification to a third-party decoder is split into patches of ≤ 500 changed lines each (NFR-026) and ≤ 2 000 lines in total per decoder, behind a build flag, documented in `plugins/<plugin>/patches/` (or `third_party/patches/` for libraries without a plugin), restricted to read-only accessors, side-data export or copies of intermediate buffers (never a change of decoding results), and accompanied by an upstream submission work item.

## 5.2 Layers and components

```
┌──────────────────────────────────────────────────────────────────────────────┐
│ Front-ends (GPL-3.0-or-later)                                                 │
│  ovt (CLI)        ovt-studio (Qt 6 Quick GUI)        Android shell (Kotlin)   │
│  python/ovt (pybind11)                                                        │
├──────────────────────────────────────────────────────────────────────────────┤
│ libovt-render (LGPL-2.1-or-later)                                             │
│  VideoSurface: libplacebo frame pipeline, compare modes, overlays, picking,   │
│  HDR output per platform, HDR measurements. No toolkit types in its API.      │
├──────────────────────────────────────────────────────────────────────────────┤
│ libovt-core (LGPL-2.1-or-later) — C API in core/include/ovt/*.h               │
│  io · bitreader · container · demux · syntax · index · decode · analysis ·    │
│  model · color · metrics · cache · jobs · log                                 │
├──────────────────────────────────────────────────────────────────────────────┤
│ Plugins (separate shared libraries, C ABI, own licences)                      │
│  ovt-plugin-aom · ovt-plugin-de265 · ovt-plugin-ffmpeg · ovt-plugin-hm        │
│  metric plugins (built-in: ovt-kernels, libvmaf host, uvq host; sidecars)     │
├──────────────────────────────────────────────────────────────────────────────┤
│ Third-party: FFmpeg · libaom · dav1d · libde265 · HM · libvmaf · libplacebo · │
│  zimg · FlatBuffers · zstd · ONNX Runtime · Highway · Qt 6 · fmt · spdlog     │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 5.2.1 `libovt-core` modules

| Module | Responsibility | Key types | Specified in |
|---|---|---|---|
| `io` | Random-access file abstraction: mmap on desktop, `pread` on a file descriptor on Android (Storage Access Framework), page cache for the hex view. | `ovt::io::RandomAccessFile` | chapter 7 §7.1, chapter 8 §8.8 |
| `bitreader` | Bounds-checked bit reading for every descriptor used by H.264, H.265, AV1 and ISOBMFF; position recording; emulation-prevention removal and raw↔RBSP offset mapping. | `ovt::BitReader`, `ovt::ParseError`, `BitSpan` | chapter 7 §7.2 |
| `container` | Format sniffing; own ISOBMFF reader producing `ContainerTree` with byte/bit spans; IVF, Annex B, raw OBU, Y4M, raw YUV; later EBML, MPEG-TS, DASH/HLS. Derived views (tracks, sample table, edit list, fragments, encryption, HDR summary). | `ContainerTree`, `BoxNode`, `Field`, `TrackInfo`, `SampleTable` | chapter 8 |
| `demux` | libavformat wrapper that turns container samples into codec units (NAL/OBU) for decoders, with sample indices and codec private data. | `ovt::demux::Demuxer` | chapter 8 §8.7 |
| `syntax` | Position-recording header parsers for H.264, H.265, AV1 producing `SyntaxNode` trees; parameter-set activation; SEI/metadata payload parsers. | `SyntaxBuilder`, per-codec parsers | chapter 7 §7.3 |
| `index` | Builds `StreamIndex`: units → frames, types, RAPs, POC/order hint, display order, declared references, DPB simulation, per-frame sizes. Incremental and cancellable. | `StreamIndex`, `FrameInfo`, `Unit` | chapter 7 §7.4 |
| `decode` | Playback path: libavcodec with hardware acceleration (VideoToolbox, MediaCodec, VA-API) and software fallback; produces `Picture` with `ovt_ColorDescription`. | `ovt_Decoder`, `Picture` | chapter 7 §7.6.3 |
| `analysis` | Plugin discovery and hosting; GOP-local decode scheduling; `FrameAnalysis` production; plane blob store. | `ovt_Analysis`, `ovt_AnalysisDecoderApi` | chapter 7 §7.5–§7.6 |
| `model` | Generated FlatBuffers code, builders, validators (`V-nnn` rules, reported as `MDL-VAL-0nnn`), exporters (JSON, YUView CSV, aom-inspect JSON). | `ovt.fb.*` | chapter 6 |
| `color` | `ovt_ColorDescription` construction from container/bitstream metadata; H.273 tables; CPU nits math for tests and CLI. | `ovt_ColorDescription` | backbone §0.5.6, chapter 10 §10.3 |
| `metrics` | Metric plugin host, normalisation stage (zimg), alignment, kernels (Highway SIMD), libvmaf host, UVQ host (ONNX Runtime), sidecar protocol, pooling, BD-rate, exporters. | `ovt_MetricApi`, `MetricsResult` | chapter 9 |
| `cache` | On-disk cache of `StreamIndex` and `FrameAnalysis` (zstd) keyed by source hash; LRU eviction; size cap. | `ovt_Cache` | chapter 6 §6.7 |
| `jobs` | Thread pool, job handles, cancellation, progress reporting. | `ovt_Job` (C, `core/include/ovt/jobs.h`), `ovt::jobs::Pool`, `ovt::jobs::Cancel`, `ovt::Cancelled` (C++, `core/include/ovt/cxx/jobs.hpp`, `ovt/cxx/cancel.hpp`) | chapter 7 §7.1.6 (C++ API), chapter 12 §12.3 (`ovt_Job`), this chapter §5.5 (rules); implemented by WI-1.512 |
| `log` | Structured logging (stderr / `--log-file`), never data output. | `ovt_log_fn` (C, `core/include/ovt/log.h`), `ovt::log` (C++, `core/include/ovt/cxx/log.hpp`) | chapter 7 §7.1.6 (API and line format), this chapter §5.7.1 |

**Header rule (D22, chapter 0 §0.11).** Every header directly under `core/include/ovt/*.h` (and `ovt/abi/*.h`) is a C11 C API header and is installed. Every C++ header of `libovt-core` — the helpers `ovt/cxx/result.hpp`, `ovt/cxx/jobs.hpp`, `ovt/cxx/cancel.hpp`, `ovt/cxx/log.hpp` and the C++ module interfaces (`ovt/cxx/bitreader.hpp`, `ovt/cxx/rbsp.hpp`, `ovt/cxx/io/…`, `ovt/cxx/syntax/…`, `ovt/cxx/index/…`, `ovt/cxx/analysis/…`, `ovt/cxx/model/…`) — lives under `core/include/ovt/cxx/` with the `.hpp` extension, is never included from a C header, carries no ABI promise and is not installed (chapter 12 §12.3.1 rule 1, chapter 13 §13.2.6).

### 5.2.2 Plugins

| Plugin | Codec / role | Source of truth | Licence | Upstream status |
|---|---|---|---|---|
| `ovt-plugin-aom` | AV1 block analysis, symbol accounting, reconstruction planes (prediction/pre-filter planes and hidden-frame planes only with the OVT snapshot patch) | libaom built with `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1`; `AV1_SET_INSPECTION_CALLBACK` (honoured only when `aom_codec_decode()` gets a non-NULL `user_priv`, chapter 7 §7.6.1); `insp_mi_data`; per-block bits from accounting | BSD-2 + AOM patent licence | Semi-public API (source-tree header `av1/decoder/inspection.h`, not installed); a ≤ 300-line extension of `ifd_inspect` plus a snapshot patch are proposed upstream (chapter 7 §7.6.1) |
| `ovt-plugin-de265` | HEVC Main/Main10 block analysis, prediction/residual/coefficient planes | libde265 + read-only internals header exposing `de265_image` metadata arrays | LGPL-3.0+ | Three patches of ≤ 500 lines each, upstream PR mandatory (chapter 7 §7.6.2) |
| `ovt-plugin-ffmpeg` | H.264 MVs and per-MB QP (phase 2); all-codec playback decoders (phase 1, WI-2.171); phase 4: MB type/intra/ref_idx/CBP export | FFmpeg `export_side_data=mvs+venc_params`; hwaccel | LGPL-2.1+ (GPL if built with `--enable-gpl`) | Public API in phase 2; small side-data patch proposed upstream in phase 4 (chapter 7 §7.6.3) |
| `ovt-plugin-hm` | HEVC bit-exact oracle, RExt/SCC | HM `TComDataCU` public getters, no patch | BSD-3 | No patch |
| built-in metric plugins | PSNR, SSIM (two variants), identity, maps; libvmaf host; UVQ host | own kernels; libvmaf; ONNX Runtime | LGPL (OVT), BSD+Patent, Apache-2.0 | — |
| sidecar metric plugins | ColorVideoVDP, HDR-VDP-3, research metrics | Python processes over the sidecar protocol | per package | — |

dav1d is used only through FFmpeg's `libdav1d` wrapper for AV1 playback. There is no dav1d fork (ADR-008).

### 5.2.3 Front-ends

- **`ovt` CLI** (chapter 12): one subcommand per capability, machine-readable output, same core as the GUI.
- **`ovt-studio`** (chapter 11): Qt 6 Quick application; panels bound to core FlatBuffers through `QAbstractItemModel` adapters; video via `libovt-render` inside a `QQuickRhiItem` (SDR, tone-mapped) or a dedicated HDR window/surface.
- **Android shell** (chapter 11 §11.5, chapter 13 §13.4): a Kotlin `Activity` embedding `QtQuickView`, a `SurfaceView` sibling for HDR output, Storage Access Framework file access. Same Qt Quick code as the desktop with tablet/Chromebook layouts.
- **Python module** (chapter 12 §12.4): pybind11 over the C API; numpy views of grids and planes.

## 5.3 Data flow

### 5.3.1 Open a file (phase 1 behaviour)

```mermaid
sequenceDiagram
  participant UI as ovt-studio / ovt
  participant IO as io
  participant C as container
  participant D as demux (libavformat)
  participant S as syntax
  participant X as index
  participant P as decode (playback)
  participant R as libovt-render
  UI->>IO: ovt_file_open(path | fd)
  UI->>C: ovt_container_open → ContainerTree (lazy, no mdat reads)
  C-->>UI: tracks, box tree, HDR summary
  UI->>D: open same file via custom AVIOContext
  UI->>X: ovt_stream_build_index(job)
  X->>C: unit map for the selected track (byte ranges)
  X->>S: parse every header unit → SyntaxNode (with bit spans)
  X-->>UI: StreamIndex (frames, types, RAPs, POC, sizes, refs) — progress events
  UI->>P: ovt_decoder_open(prefer_hw) ; seek(display_index 0) ; next()
  P-->>R: Picture + ovt_ColorDescription
  R-->>UI: rendered frame (tone-mapped or HDR surface)
```

Targets (NFR, orientation only — the normative numbers are those of chapter 14 §14.9.3 and `tests/perf/budgets.json`, D19): index of a 1-hour 1080p H.264 file in ≤ 2 s; first frame on screen ≤ 500 ms after open on the reference machine `m1-air-8gb` (chapter 14 §14.9.2).

### 5.3.2 Inspect a frame at block level (phase 2)

1. The GUI requests `ovt_analysis_frame(stream, decode_index=N, plane_stage_mask, job)`.
2. `analysis` checks the cache (`<cache_root>/<source_hash>/frames/<N as 8-digit zero-padded decimal>.fa.zst`, chapter 6 §6.7.1); on hit it returns immediately.
3. Otherwise it finds the nearest random access point `R ≤ N` from `StreamIndex`, creates (or reuses) the codec's analysis plugin instance, pushes all parameter sets active at `R`, then pushes units of frames `R..N` in decode order, pulling `FrameAnalysis` records as they complete. Records for frames `R..N-1` are cached too (they were paid for).
4. Planes requested in `plane_stage_mask` are kept in the plane blob store (LRU of 4 frames, §0.7.3); grids and block trees for the LRU of 64 `FrameAnalysis` records stay in memory; everything is written to the cache.
5. The GUI builds an `OverlaySet` from the `FrameAnalysis` and asks `libovt-render` to draw it over the playback picture of the same frame (`FrameId` equality) — or over the analysis plugin's reconstruction plane when a per-stage view is selected.

AV1 hidden frames: `decode_index` counts them; the GUI timeline shows shown frames by `display_index` and offers a "decode order" toggle that reveals hidden frames (chapter 11 §11.4).

### 5.3.3 Run metrics (phase 3)

1. The user picks a reference (raw/Y4M or decoded stream) and one or more distorted streams, metrics, and a `NormalizationConfig`.
2. `metrics` decodes both through the playback path (software decoders for determinism), aligns temporally (±K frame search) and spatially, converts both into each metric's working domain via zimg, and streams aligned frame pairs to each metric plugin.
3. Per-frame `FrameScore`s (and map blobs) are accumulated into a `MetricsResult`; pooling and per-scene statistics are computed at the end; the configuration is embedded verbatim.
4. The GUI plots the series linked to the timeline and overlays maps on the picture; the CLI writes CSV/JSON/FlatBuffers.

## 5.4 Process model

Phase 1–3: everything runs **in-process** (ADR-013). One decoder or plugin instance is owned by one thread at a time. Because every plugin output is a FlatBuffer and every plane is a blob with an id, an **out-of-process plugin host** can be added later (phase 4 option) without changing any consumer: the host would run the same `ovt_AnalysisDecoderApi` in a child process and ship buffers over shared memory. Sidecar metric plugins (chapter 9 §9.7) are already separate processes.

The CLI is a separate process by nature and is the recommended way to run long batch jobs.

**Untrusted input and isolation.** Media files are hostile input (principle 6), and in phases 1–3 the instrumented upstream decoders (libaom, libde265, FFmpeg, HM) run **in the OVT process** with the user's privileges: a memory-safety bug in a decoder can crash or compromise the GUI/CLI process. Mitigations in scope: OVT's own parsers are bounds-checked and fuzzed (chapter 7 §7.2, chapter 14 §14.7); decoder plugins are built with the pinned, patched upstream versions and run under ASan/UBSan in CI; plugin entry points are `noexcept` and a plugin error degrades to "block analysis unavailable" (§5.7). Not in scope before phase 4: process isolation or sandboxing of decoders, and memory limits per decoder. Users who analyse untrusted files at scale run the CLI in a container or a separate OS account; the out-of-process plugin host above is the planned mitigation and is tracked as open item OI-18 in chapter 17 §17.4.

## 5.5 Threading model

| Rule | Detail |
|---|---|
| T1 | Core objects (`ovt_Container`, `ovt_Stream`, `ovt_Analysis`, `ovt_Decoder`, `ovt_Metric`) are **not** thread-safe. Exactly one thread uses an instance at a time. |
| T2 | `ovt::jobs::Pool` (chapter 7 §7.1.6) owns `N = hardware_concurrency()` worker threads, capped at 16 (`--threads` / settings). Long operations (index, analysis, metrics, decode-ahead) run as jobs; the GUI thread never blocks on them. |
| T3 | A job checks its `ovt::jobs::Cancel` token (C API: `ovt_Job` cancellation flag) at least every 50 ms of work and reports progress as `(done, total, phase)`; the token is passed by reference down every long call chain (container expansion, index building, decode loops, metrics). |
| T4 | Decoders and plugins may be internally multi-threaded (libde265 WPP/tile threads, libaom row-MT, FFmpeg frame threads). The host passes `threads = min(n, 8)` at `create` time and never calls one plugin instance from two threads; plugin entry points are `noexcept` at the ABI and the host's `try`/`catch` around them is only a best-effort guard (chapter 7 §7.5.1). libaom's inspection callback runs on a libaom decode thread: the plugin copies data there and hands it to its own thread. |
| T5 | Rendering happens on the GUI/render thread; uploads of planes are triple-buffered; `FrameAnalysis` → `OverlaySet` conversion runs on a job thread and hands immutable GPU-ready buffers to the render thread. |
| T6 | Per-stream analysis runs on one dedicated job at a time (serialised queue per stream) to keep the plugin instance single-owner; different streams (A/B compare) use different instances and may run in parallel. |
| T7 | The cache is process-safe via per-file atomic rename; no cross-process locking in phase 1 (two OVT processes on the same file may duplicate work but never corrupt). |

## 5.6 Memory model and caching

- `FrameAnalysis` grids and block trees: LRU of 64 frames in memory per stream (≈ 5 MB per 1080p frame, 21 MB per 4K frame).
- Planes: LRU of 4 frames across all stages per stream (a 4K 10-bit frame with recon + pred + residual + two pre-filter stages ≈ 125 MB).
- Disk cache: `<cache_root>/<source_hash>/` with `index.fb`, `frames/<decode_index>.fa.zst`, `meta.json`; `<cache_root>` is `~/Library/Caches/org.openvideotools.ovt/` on macOS, `$XDG_CACHE_HOME/ovt/` (default `~/.cache/ovt/`) on Linux and the app cache directory (`Context.getCacheDir()/ovt`) on Android, overridable by `OVT_CACHE_DIR` (chapter 6 §6.7.1 is the single definition); default cap 20 GiB desktop, 2 GiB Android, LRU by last access; `ovt cache clear` and GUI setting (`cache.max_size`). Cache entries are invalidated when the schema major (`schema_version`) or the plugin id/version change (chapter 6 §6.7.3; a different OVT version alone does not invalidate).
- Budgets (NFR): ≤ 2 GB RSS per open 4K stream on desktop; ≤ 1 GB on Android (chapter 0 §0.7.3).

## 5.7 Error handling and diagnostics

- C API functions return `ovt_status`; `OVT_OK` with an empty buffer means "nothing yet" where documented. Human-readable detail is attached to the returned record as `Diagnostic{severity, code, message, span, spec_ref}`.
- Internal C++ uses `ovt::Result<T>` (a `std::expected`-like type; defined in chapter 7 §7.1.6, `core/include/ovt/cxx/result.hpp`) and `ovt::ParseError` exceptions **only inside parser call chains**, caught at the module boundary; the only other exception type is `ovt::Cancelled` (`core/include/ovt/cxx/cancel.hpp`), thrown by `Cancel::throw_if_requested()` and caught at the job boundary (→ `OVT_E_CANCELLED`). No exception crosses the C ABI.
- Diagnostic codes are stable strings of the form `<FAMILY>(-<QUALIFIER>)?-<nnnn>`: FAMILY is one of `CNT` (container, chapter 8 §8.5), `SYN` (syntax parsing and index building, chapter 7 §7.9), `DEC` (decoders, plugins, HRD, chapter 7 §7.9), `VQ` (metrics, chapter 9 §9.11.4), `MDL` (data model, validator, cache, chapter 6 §6.9.1), `IO` (file access, chapter 7 §7.9), `BR` (bit reader, chapter 7 §7.2.4), `VER` (verify mode, chapter 6 §6.9.1), `ANA` (analysis host, chapter 7 §7.9; the single code `ANA-LRU-0001` is registered in chapter 12 §12.3.10 next to the C function that raises it), `RND` (rendering, chapter 10 §10.10), `GUI` (chapter 11 §11.7), `CLI` (CLI layer: argument parsing, output formats, exit codes — chapter 12 §12.1.3); QUALIFIER is 2–8 uppercase alphanumerics naming a codec or sub-area (`H264`, `H265`, `AV1`, `GEN`, `HRD`, `TS`, `EBML`, …) and `nnnn` is four digits. A bare `CNT-nnnn` (no qualifier) is an ISOBMFF/QuickTime code; `CNT-TS-nnnn` and `CNT-EBML-nnnn` are the MPEG-TS and Matroska qualifiers; the spellings `CNT-ISOBMFF-nnnn`/`CNT-ISO-nnnn` are not used (D2). Examples: `CNT-0001`, `CNT-TS-1001`, `SYN-H265-0007`, `SYN-GEN-0101`, `DEC-HRD-0010`, `MDL-VAL-0002`, `BR-0001`, `CLI-0001`. Each owning chapter carries the registry table for its family (code, severity, message template, when raised, spec reference); rule V-029 (chapter 6 §6.9) checks the format on every emitted record; codes are never reused or renumbered.

### 5.7.1 Logging

Logging is separate from diagnostics: a `Diagnostic` is data attached to a record (it travels into JSON output and the GUI panel); a log line is operational text for a developer. `ovt::log` (chapter 7 §7.1.6) writes to stderr by default and to the file named by `--log-file` (chapter 12 §12.1.2); `OVT_LOG_LEVEL` and the `--log-level` option select `trace|debug|info|warn|error` (default `warn` in the CLI, `info` in the GUI's Log panel). Line format: `<ISO-8601 UTC time with milliseconds> <LEVEL, 5 wide> <module> [<diagnostic code when the line mirrors a Diagnostic>] <message>`. Stdout never carries log lines (chapter 12 §12.1.3 determinism); the GUI keeps a 10 000-line ring buffer (chapter 11 §11.3.16).
- Partial results are first-class: a `StreamIndex` with diagnostics is still returned; the GUI shows a badge count and a Diagnostics panel with jump-to-location.
- Plugin failures (`OVT_E_PLUGIN`, crashes in the future out-of-process mode) degrade the UI to "block analysis unavailable for this frame" with the reason; playback keeps working.

## 5.8 Configuration

- User settings (GUI): one plain JSON file `settings.json` in `QStandardPaths::AppConfigLocation` — macOS `~/Library/Application Support/ovt/settings.json`, Linux `$XDG_CONFIG_HOME/ovt/settings.json` (default `~/.config/ovt/settings.json`), Android the app's private files directory (`<filesDir>/settings.json`) — read and written by the GUI's own JSON settings store, **not** `QSettings` (D8); chapter 11 §11.2.5 is the single definition of the file, its schema and the override `OVT_CONFIG_DIR`. Keys (informative, chapter 11 §11.2.5 is normative): cache directory and cap, plugin directories, thread count, default tone-mapping, colour maps, keyboard overrides, metric defaults, telemetry (none; no network access by default).
- Project/session files: `.ovtproj` JSON (chapter 11 §11.2.6).
- Environment variables honoured by core and CLI: `OVT_CACHE_DIR`, `OVT_PLUGIN_PATH` (colon-separated), `OVT_LOG_LEVEL`, `OVT_THREADS`.
- No network access is initiated by core or GUI except: explicit user action to download metric model assets (UVQ weights) and decoder packs, with SHA-256 verification (chapter 9 §9.6, chapter 15 §15.5).

## 5.9 Extension points

| To add… | Implement… | Register by… | Chapter |
|---|---|---|---|
| a codec analysis decoder | `ovt_analysis_decoder_v1()` in a shared library, filling the neutral model + a codec extension table | placing `libovt_plugin_<id>.{dylib,so}` in a plugin directory; `ovt plugins` lists it | chapter 7 §7.5–§7.6 |
| a metric | `ovt_metric_v1()` (in-process) or a sidecar adapter | plugin directory or `metrics.d/*.json` sidecar manifest | chapter 9 §9.2, §9.7 |
| a container format | `ovt::container::Reader` subclass producing `ContainerTree` | registry entry in `core/src/container/format_sniff.cpp` with a sniff function | chapter 8 §8.6 |
| a codec's header parser | `ovt::syntax::<Codec>Parser` using `SyntaxBuilder` | `core/src/syntax/registry.cpp`; `Codec` enum value in `ovt_model.fbs` | chapter 7 §7.3 |
| a hand-written ISOBMFF box decoder | `BoxDecoder{fourcc, decode}` | the generated registry of `tools/gen_box_registry.py` (`core/src/container/isobmff/decoders/`) | chapter 8 §8.2.9–§8.2.10 |
| an overlay type | a builder from `FrameAnalysis` to `OverlaySet` layers | `render/src/overlays/registry.cpp` + GUI toggle entry | chapter 10 §10.5, chapter 11 §11.4 |
| a CLI subcommand | a `Command` subclass in `cli/src/cmd/<name>.cpp` | the subcommand table in `cli/src/main.cpp` | chapter 12 §12.2 |
| a GUI panel | a QML component + `QAbstractItemModel` adapter | `app/qml/panels/` + dock registry | chapter 11 §11.3 |

## 5.10 Architecture decision records

The ADR table in the Backbone (§0.2) is normative; the titles below are an informative summary and §0.2 wins on any difference in wording. Each ADR is expanded as its own file under `docs/adr/ADR-0nn-<slug>.md` in the repository using the template: Context · Decision · Consequences · Alternatives considered · References · Status (Accepted / Superseded by). The sixteen ADRs are:

| ID | Title |
|---|---|
| ADR-001 | C++20 for all native code; C ABI for public interfaces |
| ADR-002 | LGPL-2.1-or-later core and renderer; GPL-3.0-or-later front-ends; BSD-2 plugin ABI headers; AGPL/SSPL/BUSL banned |
| ADR-003 | Layered core → render → front-ends; CLI parity with GUI |
| ADR-004 | Qt 6 Quick GUI; Android via QtQuickView in a native Activity |
| ADR-005 | libplacebo owns the frame display pipeline |
| ADR-006 | Own ISOBMFF reader with offsets; libavformat for demux; GPAC et al. as oracles |
| ADR-007 | Own position-recording header parsers for H.264/H.265/AV1 |
| ADR-008 | Block-level truth from instrumented upstream decoders behind a C plugin ABI: libaom, libde265, FFmpeg; no dav1d fork |
| ADR-009 | Two decode paths: hardware playback, GOP-local software analysis |
| ADR-010 | FlatBuffers for all analysis records; JSON and YUView CSV as exports |
| ADR-011 | Metric plugin system with shared normalisation; libvmaf, own kernels, ONNX Runtime for UVQ, sidecars |
| ADR-012 | CMake + presets + vcpkg + third_party scripts; Gradle for Android |
| ADR-013 | In-process plugins in phase 1; IPC-ready data model |
| ADR-014 | Android HDR via SurfaceView dataspace, not Vulkan colourspaces; ChromeOS HDR spike |
| ADR-015 | Runtime-loaded analysis decoder plugins; the default build keeps the pool-covered FFmpeg decoders in the core (owner decision 2026-11-12), a patent-pool-free split build stays supported |
| ADR-016 | DCO governance; trademark; distribution channels (no Mac App Store) |

## 5.11 Deployment views

**macOS.** `ovt-studio.app` bundle containing `libovt-core.dylib`, `libovt-render.dylib`, Qt frameworks, libplacebo, MoltenVK, FFmpeg libs, `PlugIns/ovt/libovt_plugin_*.dylib`; `ovt` CLI inside `Contents/MacOS` and symlinked by Homebrew. Notarised DMG. User plugins in `~/Library/Application Support/ovt/plugins/` (chapter 7 §7.5.1 search order).

**Linux.** Flatpak (`org.openvideotools.Studio`, KDE runtime) and AppImage; distro packages split as `ovt`, `ovt-studio`, `ovt-plugins-free` (aom), `ovt-plugins-avc-hevc` (ffmpeg h264/hevc, de265). User plugins in `$XDG_DATA_HOME/ovt/plugins`.

**Android / ChromeOS.** One AAB with arm64-v8a and x86_64 ABIs; native libraries in `lib/<abi>/`; decoder plugins either bundled or installed as a "codec pack" split/APK into app-private storage; model assets (UVQ) downloaded on demand. Play Store and F-Droid builds are reproducible from the published tree.

## 5.12 Quality attributes summary

| Attribute | Mechanism | Verified by |
|---|---|---|
| Robustness | `BitReader` bounds checks, fuzz targets per parser, ASan/UBSan CI | chapter 14 §14.7–§14.8 |
| Performance | budgets in chapter 14 §14.9.3 / `tests/perf/budgets.json` (headline numbers in §0.7.4); GOP-local analysis; GPU overlays; caches | chapter 14 §14.9 benchmarks |
| Correctness | oracle golden tests (FFmpeg trace_headers, JM/HM traces, aom inspect, MP4Box, libvmaf CLI, UVQ CLI); decoded-picture MD5 | chapter 14 §14.4–§14.6 |
| Maintainability | upstream-first, patches ≤ 500 lines each (NFR-026), pinned versions table, one build entry point | chapter 13, chapter 7 §7.6 |
| Portability | C++20, Qt, libplacebo, CMake presets; no platform code outside `platform/` directories and the Android shell | chapter 13 CI matrix |
| Extensibility | plugin ABIs, registries in §5.9 | chapter 7 §7.5, chapter 9 §9.2 |
| Licensing hygiene | directory-based licence split, SPDX headers, CI licence gate | chapter 15 |

### Glossary additions (chapter 5)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **Analysis path / playback path** — the two decode paths of ADR-009: GOP-local instrumented software decoding for the inspected frame (chapter 7 §7.5) versus hardware-accelerated decoding for viewing (chapter 7 §7.6.3).
- **Job** — a unit of long-running work submitted to `ovt::jobs::Pool`, with a cancellation token and progress reporting (§5.5, chapter 7 §7.1.6); exposed to the C API as `ovt_Job`.
- **Semi-public API** — (a) in OVT: the C++ headers under `core/include/ovt/cxx/*.hpp` (§5.2.1 header rule, D22) that the GUI, CLI and tests may use but that carry no ABI promise; (b) of a third-party library: a header that is part of its source tree but not installed (libaom's `av1/decoder/inspection.h`), which OVT includes from the pinned source tree, never from a vendored copy (§5.2.2, chapter 7 §7.6.1).
