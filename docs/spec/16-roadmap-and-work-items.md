# 16. Roadmap and work items

> **Status:** normative. This chapter is where an implementing agent picks up work. It restates the five phases and the Android track of chapter 0 §0.8 with concrete, testable exit demos (§16.1), fixes the team workflow (§16.2), resolves every place where two chapters specified the same deliverable (§16.3), lists all 380 work items (including WI-4.470, added under §16.2.6) with concrete dependencies and a milestone (§16.4), names the critical path and the parallelisable work (§16.5), gives the per-phase gate checklist (§16.6) and parks future work (§16.7).
>
> **Precedence.** Where this chapter and a module chapter disagree about *which* WI delivers a file or *when* it is scheduled, this chapter wins (it was written last, with all chapters in view). Where they disagree about *what* a file contains, the module chapter wins. Chapter 0 §0.8 phase themes and exit criteria are not changed, only sharpened.
>
> **Reading rule for agents.** Before starting a WI: read §16.2 once, then the WI's row in §16.4, then the WI block in its chapter (the "Chapter §ref" column), then the chapter sections named in the WI's **Interface** line. If the WI appears in §16.3, read that row first — it may remove work from the WI.

## 16.0 Conventions used in this chapter

| Term | Meaning |
|---|---|
| Phase `P` | One of 0, 1, 2, 3, 4 (sequential for the core) or A (Android/ChromeOS track, runs in parallel from phase 0 onward). A WI's phase is the first character after `WI-` **except** for the WIs re-phased in §16.3.11 and §16.3.12. |
| Milestone `Mp.n` / `MA.n` | A group of WIs that is complete when every WI in it is Done (§16.2.2). Milestones inside a phase are ordered; `MA.n` milestones run concurrently with the core milestones named in their title; for dependency ordering (§16.2.5 check) an `MA.n` milestone has the ordinal of the first core milestone in its title (its WIs may be picked from then on) and a core WI may depend on an `MA.n` WI only when that `MA.n` starts no later than the core WI's milestone. Duration estimates assume ≥ 4 implementing agents working in parallel and are planning aids, not requirements. |
| Phase gate | The set of checks in §16.6 for a phase. A phase is **exited** only when its gate is green and the exit demo of §16.1 has been executed and recorded in `docs/acceptance/phase-<n>.md` (chapter 14 §14.11). The next phase's milestones MAY start before the gate (work overlaps), but no release is cut and no phase is declared done before the gate. |
| Primary / superseded / reduced WI | See §16.3. A **superseded** WI is not implemented; its useful acceptance tests moved to the primary. A **reduced** WI is implemented with a smaller scope stated in §16.3. |
| Critical path | The longest dependency chain (in milestone ordinals) that ends at a phase gate; slipping any WI on it slips the gate (§16.5). |
| Hardware-bound WI | A WI whose acceptance tests cannot run on a hosted CI runner (§16.5.3). |
| Size `S` / `M` / `L` | Effort for one implementing agent, stated once here and used by every `**Size:**` line in chapters 06–15 and by §16.4: `S` ≤ 1 day, `M` ≤ 3 days, `L` ≤ 1 week (chapter 14 §14.12 states the same) (a WI larger than `L` is split per §16.2.6). |
| FRs column (§16.4) | The requirement ids of chapter 4 that the WI realises; the inverse of chapter 4 §4.17. `—` marks infrastructure WIs that realise no requirement directly. |

Dependency notation in §16.4 lists **only WI ids**. Where the source chapter wrote prose ("chapter 7 plugin host"), the ids were resolved by reading the producing chapter; the resolution is also applied to the chapter text (the `**Depends on:**` lines), so both say the same thing.

---

## 16.1 Phase overview

The ordering follows the owner's request for a codec-engineer / social-media-video-team ordering (chapter 0 §0.8, chapter 1 G7): **first** know exactly what is in the file and see it correctly (container, syntax, HDR display) — the daily "what did the encoder/packager emit" question; **second** block-level truth, the codec engineer's core need and the hardest technical risk (instrumented upstream decoders); **third** quality measurement, which the video team needs for ladder and encoder decisions and which depends on correct decoding and colour handling from phases 1–2; **fourth** parity features. Android/ChromeOS is a parallel track so that every phase demo also runs on a tablet; it starts in phase 0 because the HDR-on-ChromeOS question (ADR-014) is the project's largest unverified assumption and must be answered in week one.

Each phase below states: theme, **entry criterion** (what must be true to start), **exit demo** (a script a human or an agent with device access executes; every line is observable and recorded in `docs/acceptance/phase-<n>.md`), and the milestones. The exit demo is the sharpened form of the §0.8 exit criterion; the machine-checkable parts of it are repeated as the gate checklist in §16.6.

### 16.1.1 Phase 0 — Foundations

**Theme.** Repository, CMake presets, vcpkg, `third_party` scripts, CI matrix (macOS, Linux, Android), FlatBuffers schemas and model library, `io`, `BitReader`, logging, jobs, licence gate, corpora fetcher, oracle image, test/fuzz/perf harnesses, Android shell, **ChromeOS/Android HDR hardware spike**.

**Entry criterion.** The owner has created the empty repository and named the default branch `main`; the spec (`docs/spec/*.md`, with `docs/spec/CHANGELOG.md` and `docs/adr/`) is committed — by WI-0.000 (chapter 13), whose Produces line carries the verbatim copy; until WI-0.000 merges, agents read the specification from the editor's copy of the same version (§16.2.8).

**Exit demo (all lines MUST pass; commands run from a clean clone on the reference machines of chapter 14 §14.9.2):**

| # | Command / action | Expected observable result |
|---|---|---|
| 1 | `git clone <repo> ovt && cd ovt && tools/bootstrap.sh` | exit 0; prints the host tool versions; no manual step needed on macOS 15 and Ubuntu 24.04 |
| 2 | `cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release && ctest --preset pr` | all tests pass; 0 failed; the only skipped tests are labelled `needs-corpus` when offline |
| 3 | Same with `linux-x64-release` and `linux-x64-asan-ubsan` | green; the sanitizer run reports no finding |
| 4 | `python3 tools/fetch_corpora.py --tags fast && python3 tools/fetch_corpora.py --verify-only` | exit 0 both times; total ≤ 314 572 800 bytes; altering one `sha256` in the manifest and re-running gives exit 2 and the file is re-downloaded |
| 5 | `python3 tools/license_gate.py --policy tools/license_policy.toml --versions third_party/VERSIONS.md --manifest build/<preset>/license_manifest.json` (chapter 13 §13.7.1 `license-gate` arguments); then add a fake `AGPL-3.0-only` row to `third_party/VERSIONS.md` and re-run | exit 0, then exit 1 naming the row |
| 6 | `cmake --preset android-arm64-release && cmake --build --preset android-arm64-release && ./gradlew -p android :app:assembleFossDebug` | two APKs (`arm64-v8a`, `x86_64`), each ≤ 120 MB |
| 7 | Install the APK on the Pixel Tablet and on the HDR Chromebook; open the app | the Qt window titled `OVT Studio` appears inside `QtQuickView`; the PQ test pattern of WI-0.052 is visible through `HdrSurfaceView` |
| 8 | `adb logcat \| grep OVT_HDR` on the Chromebook | prints `dataspace=BT2020_PQ` (log tag `OVT_HDR`, line `dataspace=<name>`, emitted by WI-0.052's `ovt_jni_surface_attach` — the same format WI-1.577 later asserts), **or** the documented fallback diagnosis string; `docs/platforms/chromeos-hdr.md` contains the PASS/FAIL verdict with device, OS version and photometer/eyeball readings (WI-0.055) |
| 9 | `ctest -L fuzz-regress` ; `build/linux-x64-fuzz/fuzz/ovt_fuzz_bitreader -max_total_time=60` (target `ovt_fuzz_bitreader` of chapter 13 §13.2.1, built by the `linux-x64-fuzz` preset of chapter 13 §13.2.4) | green; no crash, no sanitizer report |
| 10 | `flatc --cpp --python --kotlin core/schema/*.fbs` ; `ctest -R test_model_roundtrip` (test executable produced by WI-0.093, D25) | compiles; round-trip test passes; `ovt::model::bfbs(Model)` root table is `ovt.fb.FrameAnalysis` |
| 11 | `tools/perf_run.py --only bench_smoke` on the M1 runner | result file with `hardware_id == "m1-air-8gb"` uploaded as a nightly artefact |
| 12 | Branch protection: open a PR without `Signed-off-by` | the `dco-check` and the required checks of chapter 13 §13.7.3 block the merge |

**Milestones.** See table below (generated from §16.4). M0.1–M0.5 are sequential for a single agent but M0.2/M0.3 and M0.4/MA.0 overlap heavily with ≥ 4 agents.

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **M0.1** | Repository, build and governance skeleton | week 1 | WI-0.000, WI-0.001, WI-0.002, WI-0.004, WI-0.006, WI-0.007, WI-0.008, WI-0.010, WI-0.550, WI-0.600, WI-0.601, WI-0.604, WI-0.608 |
| **M0.2** | Core primitives, schemas and tooling | weeks 2–3 | WI-0.003, WI-0.011, WI-0.018, WI-0.019, WI-0.080, WI-0.081, WI-0.082, WI-0.083, WI-1.100 (re-phased), WI-1.102 (re-phased), WI-1.103 (re-phased), WI-1.104 (re-phased), WI-1.512 (re-phased), WI-0.551, WI-0.554, WI-0.558, WI-0.559, WI-0.602, WI-0.603, WI-0.607 |
| **M0.3** | Model library, third_party builds and CI matrix | weeks 3–4 | WI-0.084, WI-0.085, WI-0.087, WI-0.089, WI-0.090, WI-0.093, WI-1.101 (re-phased), WI-1.105 (re-phased), WI-1.106 (re-phased), WI-1.107 (re-phased), WI-0.012, WI-0.013, WI-0.014, WI-0.015, WI-0.016 (sup.), WI-0.017, WI-0.030, WI-0.031, WI-0.032, WI-0.033, WI-0.005 (sup.), WI-0.552, WI-0.553, WI-0.555, WI-0.557, WI-0.561, WI-0.605 |
| **M0.4** | Validator, cache, oracles, perf harness | weeks 5–6 | WI-0.086, WI-0.088, WI-0.091, WI-0.092, WI-0.094, WI-0.095, WI-0.040, WI-0.041 (sup.), WI-0.037 (sup.), WI-0.606, WI-0.556, WI-0.560, WI-0.562, WI-0.563, WI-0.564, WI-0.565, WI-0.566, WI-0.567, WI-0.568, WI-0.569 |
| **M0.5** | Packaging skeletons and phase-0 gate | week 7 | WI-0.060, WI-0.061, WI-0.063, WI-0.070, WI-0.071, WI-0.072, WI-0.073, WI-0.036, WI-0.062, WI-0.609 |

### 16.1.2 Phase 1 — See the stream

**Theme.** Own ISOBMFF reader (tree + offsets + tracks + sample table), IVF/Annex B/OBU/Y4M/raw, libavformat demux, H.264/HEVC/AV1 header parsers + `SyntaxTree`, `StreamIndex`, CLI `ovt info/probe/syntax/index/frames/dump/extract/cache`, playback decode (HW + SW), libplacebo `VideoSurface` with HDR output on macOS, Qt GUI skeleton: container tree, syntax tree ↔ hex, timeline, picture panel with zoom/pan, Y/U/V plane views, HDR metadata panel.

**Entry criterion.** Phase-0 gate green (§16.6.1) **and** WI-1.100–1.107 and WI-1.512 Done (they are phase-0 deliverables, §16.3.11).

**Exit demo:**

| # | Command / action | Expected observable result |
|---|---|---|
| 1 | For every `fast` ISOBMFF corpus file `f`: `ovt probe f -f json --expand all` | `ctest -L golden -R isobmff` green: box fourcc/offset/size/fields equal the MP4Box, mp4ff-info, mediainfo and mp4box.js goldens |
| 2 | `ovt syntax f --trace` for one `fast` stream per codec | `ctest -R syntax_golden` green: element names, values and RBSP bit positions equal `trace_headers`; values equal `gstparse` |
| 3 | `ovt index syn_av1_hidden_altref_showexisting.ivf -f json` | lists hidden frames with `shown=false` and `show_existing_of` set; the frame count equals the number of `frame` entries in the `inspect` golden |
| 4 | `ovt index syn_h264_1h_1080p.mp4` on the M1 | wall time ≤ 2 000 ms (`bench_index_h264_1h_1080p`); second run logs `cache hit` and is ≥ 5× faster |
| 5 | GUI: `ovt-studio`, open the `cta` HEVC HDR10 CMAF track | first picture visible ≤ 500 ms after the file dialog closes (`bench_first_frame_*`, `--test-ipc wait-idle` timing) |
| 6 | Click `moov/trak/mdia/minf/stbl/stsd/hvc1/hvcC` in the container tree | hex view scrolls to the box and highlights exactly its bytes (offset and size shown in the status bar equal `ovt probe` output) |
| 7 | Expand `hvcC`, click `general_profile_idc` | hex view highlights the exact bits; the bit span equals the `trace_headers` golden span |
| 8 | Click the SPS node in the syntax tree, then `pic_width_in_luma_samples` | hex highlights the field bits; Selection Info shows the value and spec reference `H.265 7.3.2.2.1` (node `spec_ref` format of chapter 7 §7.3.1) |
| 9 | Scrub the timeline bar chart | picture updates within 500 ms per seek; frame type colours follow chapter 11 §11.4.5; the unit list follows the selection |
| 10 | Switch Picture panel to Y, then U, then V plane on a 4:2:0 10-bit frame | three planes with correct dimensions (U/V half size), nearest-neighbour magnification at zoom ≥ 8× shows the sample grid and the status bar / Selection Info shows the sample value under the cursor (the in-picture value text of FR-PLN-003 arrives with the label pass WI-2.433 in M2.1) |
| 11 | On an XDR MacBook with HDR output enabled: open `syn_h265_main10_pq_hdr.mp4` | chapter 14 §14.10.5 steps 1–4 pass (1000-nit patch visibly brighter than UI white, BT.2390 roll-off, SDR toggle matches the mpv reference by eye) |
| 12 | On an SDR display (or `--force-sdr`) | render parity suite green (`test_render_parity`, p99 ΔE_ITP ≤ 1.0 vs the mpv capture) |
| 13 | HDR panel → Metadata tab | shows `colr`/`mdcv`/`clli` values and the SEI values side by side, with conflicts flagged (`DEC-HDR-0001`, chapter 7 §7.9.4) on the deliberately inconsistent `syn_h265_main10_pq_hdr_mismatch` file |
| 14 | Android: steps 5–10 on the Pixel Tablet, file opened via `content://` from the Files app | same results; the Chromebook additionally shows hover tooltips with a mouse and resizes in free-form mode without Activity recreation |
| 15 | `ovt dump f --md5` for every `fast` file | equals `ffmpeg -f framemd5` (software path); on macOS `prefer_hw` reports `videotoolbox` |
| 16 | HLG: open `syn_hlg_1080p` (chapter 14 §14.3.3 recipe; x265 `--transfer arib-std-b67 --colorprim bt2020`) on the XDR MacBook and on an SDR display (`--force-sdr`) | HDR panel shows `transfer = 18 (HLG)` with no MDCV/CLLI rows; HDR output applies the HLG OOTF with system gamma `1.2 + 0.42·log10(Lw/1000)` for the display peak (chapter 10 §10.3); the SDR run passes the render parity suite (`test_render_parity`, HLG scene, p99 ΔE_ITP ≤ 1.0 vs the mpv capture) — FR-HDR-003 |

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **M1.1** | Syntax parsers, container core, skeletons | weeks 1–2 | WI-1.108, WI-1.109, WI-1.110, WI-1.111, WI-1.112, WI-1.113, WI-1.114, WI-1.115, WI-1.116, WI-1.117, WI-1.118, WI-1.119, WI-1.120, WI-1.200, WI-1.201, WI-1.202, WI-1.203, WI-1.204, WI-1.225, WI-1.226, WI-1.227, WI-1.228, WI-1.229, WI-1.231, WI-1.237, WI-1.500, WI-1.501, WI-1.400, WI-1.401, WI-1.402, WI-1.413, WI-1.450, WI-1.466, WI-2.150 (re-phased) |
| **M1.2** | Index, box decoders, demux, playback decode, surfaces | weeks 3–4 | WI-1.121, WI-1.122, WI-1.123, WI-1.124, WI-1.125, WI-1.129, WI-1.205, WI-1.206, WI-1.207, WI-1.208, WI-1.209, WI-1.210, WI-1.211, WI-1.212, WI-1.213, WI-1.214, WI-1.215, WI-1.216, WI-1.230, WI-2.169 (re-phased), WI-1.403, WI-1.404, WI-1.406, WI-1.407, WI-1.408, WI-1.411, WI-1.451, WI-1.452, WI-1.453 |
| **M1.3** | Derived views, CLI subcommands, picture host, panels | weeks 5–6 | WI-1.130, WI-1.217, WI-1.218, WI-1.219, WI-1.220, WI-1.221, WI-1.222, WI-1.223, WI-1.224, WI-1.232, WI-1.233, WI-1.234, WI-1.235, WI-2.171 (re-phased), WI-2.172 (re-phased), WI-1.502, WI-1.503, WI-1.504, WI-1.505, WI-1.506, WI-1.507, WI-1.508, WI-1.509, WI-1.128 (sup.), WI-1.126, WI-1.127 (sup.), WI-1.405, WI-1.409, WI-1.410, WI-1.412, WI-1.415, WI-1.454, WI-1.455, WI-1.456, WI-1.457, WI-1.458, WI-1.459, WI-1.460, WI-1.570, WI-1.572, WI-1.573, WI-1.574 |
| **M1.4** | HDR GUI, goldens, C API, phase-1 gate | weeks 7–8 | WI-1.461, WI-1.462, WI-1.463, WI-1.464, WI-1.465, WI-1.510, WI-1.511, WI-1.236, WI-1.238, WI-1.239, WI-1.414 (sup.), WI-1.571, WI-1.575, WI-1.576 |

### 16.1.3 Phase 2 — Look inside blocks

**Theme.** Plugin host; `ovt-plugin-aom` (libaom inspection + accounting); `ovt-plugin-de265` (internals patch + upstream PR); `ovt-plugin-ffmpeg` (H.264 MVs/QP via the public API); overlays: partitions, prediction modes, MVs, QP map, bits heatmap, ref-index map; Selection Info; per-stage planes; GOP/reference graph; statistics panel; CLI `ovt analyze` (FlatBuffers/JSON/YUView CSV/aom-inspect) and `ovt plugins`.

**Entry criterion.** Phase-1 gate green (§16.6.2); WI-2.150 Done (it is a phase-1 deliverable).

**Exit demo (for each of `syn_h264_weightedpred_ltr`, `syn_h265_amp_sao_on`, `syn_av1_tiles_cdef_lr`):**

| # | Command / action | Expected observable result |
|---|---|---|
| 1 | `ctest -L golden -R "analysis\|xplugin"` | green: libaom vs `inspect`, libde265 vs HM grid, FFmpeg plugin vs JM QP / ffprobe side data |
| 2 | GUI: open the sample, press F1, F2, F3, F5, F6, F7 in turn (coding flow/partitions, prediction, motion vectors, QP map, bits heatmap, reference indices; chapter 11 §11.4.2) | each toggle redraws within one frame; panning at 4K stays at 60 fps on the M1 (`bench_overlay_4k_mv_arrows` p95 ≤ 16 ms) |
| 3 | Click a block | Selection Info lists every extension-table field with its spec name; values equal `ovt analyze <f> --frames N --format json` for that `block_id` |
| 4 | Press `Tab`/`Shift+Tab` (level cycling; `[`/`]` are difference gain) | selection moves CTU→CU→PU→TU (HEVC), MB→partition (H.264), SB→block→TXB (AV1) and the overlay outline follows |
| 5 | Block Samples: switch Reconstruction / Prediction / Residual / Pre-deblock | planes change; `Reconstruction` equals the playback frame bit-exactly (`ovt dump --stage recon --md5` equals `ovt dump --md5`) |
| 6 | Reference / GOP graph | `declared_refs` edges shown; the AV1 hidden ALTREF appears as an unshown node; clicking an edge selects the referenced frame |
| 7 | Statistics panel | per-frame bits, QP histogram, block-size distribution; CSV export equals `ovt analyze --format csv` |
| 8 | `ovt analyze f --frames 7 --format fb` twice | byte-identical files; `threads=1` vs `threads=4` byte-identical |
| 9 | `ovt analyze f --format yuview` → open in YUView 2.x | statistics overlay identical to OVT's (manual, screenshot recorded); `tools/yuview_csv_lint.py` passes |
| 10 | `ovt plugins --check` with a plugin of wrong ABI version in `--plugin-dir` | exit 6, diagnostic `ANA-PLG-0001` (plugin-host family, chapter 7 §7.9; D20) |
| 11 | `tools/perf_run.py --only "bench_analysis_*"` | de265 ≤ 1 500 ms, aom ≤ 3 000 ms, ffmpeg ≤ 1 000 ms, cached ≤ 30 ms on the M1 |
| 12 | Android: steps 2–3 on the Pixel Tablet at 1080p | overlays toggle within one frame; a 60-frame 1080p GOP analysis completes within 5 s (FR-AND-014; budget `bench_analysis_*` on `pixel-tablet`, chapter 14 §14.9.3 / `tests/perf/budgets.json`, D19) |

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **M2.1** | Plugin host, libaom plugin, overlay base | weeks 1–2 | WI-2.151, WI-2.152, WI-2.153, WI-2.154, WI-2.155, WI-2.156, WI-2.157, WI-2.158, WI-2.162, WI-2.163, WI-2.170, WI-2.430, WI-2.431, WI-2.432, WI-2.433, WI-2.520, WI-2.523, WI-2.480 |
| **M2.2** | libde265 plugin, overlay passes, analyze CLI, goldens | weeks 3–4 | WI-2.159, WI-2.160, WI-2.161 (sup.), WI-2.164, WI-2.165, WI-2.166, WI-2.167, WI-2.434, WI-2.435, WI-2.436, WI-2.437, WI-2.439, WI-2.521, WI-2.174 (sup.), WI-2.481, WI-2.482, WI-2.173 (sup.), WI-2.580 (sup.), WI-2.581, WI-2.583 |
| **M2.3** | Block panels, statistics, exports, dual view, phase-2 gate | weeks 5–6 | WI-2.483, WI-2.484, WI-2.485, WI-2.486, WI-2.487, WI-2.488, WI-2.489, WI-2.522, WI-2.438, WI-2.582, WI-2.584 |

### 16.1.4 Phase 3 — Measure quality

**Theme.** Normalisation stage (alignment, scale, zimg colour), PSNR/SSIM kernels + maps, libvmaf host, UVQ via ONNX Runtime, metrics panel (timeline plot, heatmap overlay, A/B/diff views), pooling, CSV/JSON export, BD-rate, CLI `ovt metrics` / `ovt bdrate` / `ovt hdr`.

**Entry criterion.** Phase-2 gate green (§16.6.3) — in particular `Reconstruction` planes are bit-exact (phase-2 demo step 5), because metrics are computed on them.

**Exit demo:**

| # | Command / action | Expected observable result |
|---|---|---|
| 1 | `ctest -L golden -R metrics` | green, including `vmaf_cli_parity` **bit-exact** on all `netflix` vectors and `uvq_parity` within 0.01 MOS |
| 2 | `ovt metrics -r netflix_src01_hrc00_576x324 -d netflix_src01_hrc01_576x324 -m vmaf --align none --format vmaf-json -o o.json` ; `python3 tests/oracles/normalize/vmaf_json_compare.py o.json netflix_src01_hrc01_576x324.vmaf_v0.6.1.vmaf.json --tol 1e-6` (comparator produced by WI-3.530 and used by WI-3.591; `vmaf-json` is the libvmaf `--json` layout of chapter 12 §12.1.4, not the `ovt` envelope) | exit 0: every common key equal at `%.6f` (`fps` and `version` are ignored by the comparator) |
| 3 | GUI: load ref/dist pair | metrics panel shows per-frame VMAF/PSNR/SSIM curves linked to the timeline, pooled table, per-block PSNR heatmap overlay; A/B and Difference views switch with one key; CSV/JSON export equals the CLI's |
| 4 | `syn_align_drop7` (distorted input with 7 leading frames dropped, chapter 14 §14.3.3) | alignment reports `offset_frames == +7` (D9 sign: distorted *i* ↔ reference *i + 7*) with `confidence ≥ 0.5` (the assertion of chapter 9 §9.3.1 test `metrics_align_leading_drop`; the flagging threshold `min_confidence` stays 0.05, D9) and `status == "ok"`; scores after alignment equal the aligned CLI run |
| 5 | 10-bit PQ pair | normalisation report shows the conversion; PSNR uses peak 1023; VMAF equals the recorded `vmaf` CLI run on the same 10-bit Y4M bit-exactly |
| 6 | `ovt bdrate --anchor a.json --test b.json --interpolator pchip` (and `cubic`, `akima`) on the committed RD sets | `|Δ| ≤ 1e-6` vs the `bjontegaard` package |
| 7 | `ovt hdr syn_h265_main10_pq_hdr.mp4 --measure --compare-declared` | MaxCLL/MaxFALL within 1 % of `nits_reference.py`; `VQ-HDR-0002` (chapter 9 §9.11.4; D20) when declared ≠ measured |
| 8 | `tools/perf_run.py --only "bench_metrics_*"` | PSNR+SSIM ≥ 200 fps, VMAF ≥ 60 fps (4 threads), normalisation PQ ≥ 120 fps on the M1 |
| 9 | Android: PSNR/SSIM/VMAF on a 720p pair on the Pixel Tablet | results equal the desktop results bit-exactly |

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **M3.1** | Metrics core: normalisation, alignment, kernels | weeks 1–2 | WI-3.300, WI-3.301, WI-3.302, WI-3.303, WI-3.304, WI-3.305, WI-3.306, WI-3.307, WI-3.308, WI-3.314, WI-3.590, WI-3.592 |
| **M3.2** | libvmaf, UVQ, pooling, BD-rate, metrics CLI | weeks 3–4 | WI-3.309, WI-3.311, WI-3.312, WI-3.313, WI-3.315, WI-3.316, WI-3.318, WI-3.530, WI-3.310, WI-3.531, WI-3.532, WI-3.533, WI-3.591, WI-3.593 |
| **M3.3** | Metrics GUI, documentation, phase-3 gate | weeks 5–6 | WI-3.450, WI-3.451, WI-3.452, WI-3.319 |

### 16.1.5 Phase 4 — Deepen & extend

**Theme.** H.264 MB-type/intra export patch; HM oracle plugin + `ovt verify`; residual/coefficient views; HRD/buffer graph; conformance checks (decoded picture hash SEI, level limits); Matroska and TS trees; DASH/HLS virtual file; Python package; Linux Wayland HDR; sidecar metric protocol + ColorVideoVDP; HDR scopes (false colour, nits waveform, vectorscope); XPSNR and HDR-domain metrics.

**Entry criterion.** Phase-3 gate green (§16.6.4).

**Exit demo:**

| # | Command / action | Expected observable result |
|---|---|---|
| 1 | `test_xplugin_h264_ffmpeg_vs_jm` (full mode) on the `jvt` fast subset | every `AvcMb` field equals the JM trace |
| 2 | `ovt verify --hash-sei` (D21) on `jctvc` (incl. RExt) and on the corrupted set `tests/golden/h265/corrupt/*.bit` (made by `tools/corrupt.py`, chapter 14 WI-4.596) | 100 % match on `jctvc`; the corrupted picture is localised by `decode_index` |
| 3 | Block Samples → Coefficients for a selected TU | values equal HM `TraceDec.txt` for that TU (3 TUs spot-checked and recorded) |
| 4 | HRD/buffer graph on `jctvc_HRD_A_Fujitsu_3`, then on a copy with halved `bit_rate_value_minus1` | no underflow, then an underflow flagged |
| 5 | Open an MKV and a TS built from `syn_*` | trees match `mediainfo --Details=1` offsets; MKV element ids/sizes match `mkvinfo -v -v -z` |
| 6 | Open the `dashif` MPD and the `apple` master playlist | init + first 3 segments appear as one stream; segment provenance shown per sample |
| 7 | `pip install ./python && python -c "import ovt; print(ovt.probe('x.mp4').tracks)"` ; `pytest python/tests` | prints the tracks; tests green on macOS and Linux |
| 8 | Linux: KDE Plasma 6.2+ Wayland session on an HDR monitor | checklist §14.10.5 steps 1–4 pass through `wp_color_manager_v1` |
| 9 | `ovt metrics --metric cvvdp` (sidecar) on a 720p pair | a score is produced; the protocol round-trip test passes |
| 10 | HDR scopes on the PQ clip | false colour thresholds align with the 100/203/1000-nit steps; nits waveform and vectorscope render; MaxCLL readout within 1 % |
| 11 | Feature matrix audit | every "must-have" row of chapter 3 §3.3 for the three codecs is demonstrated and ticked in `docs/acceptance/phase-4.md` (WI-4.599) |

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **M4.1** | Oracles and codec depth (FFmpeg patch, HM plugin, verify) | weeks 1–3 | WI-4.100, WI-4.101, WI-4.102, WI-4.103, WI-4.104, WI-4.105, WI-4.106, WI-4.107, WI-4.108, WI-4.109, WI-4.110, WI-4.111, WI-4.112, WI-4.543, WI-4.595, WI-4.596 |
| **M4.2** | Matroska, MPEG-TS, DASH/HLS, HEIF, container depth | weeks 3–5 | WI-4.200, WI-4.201, WI-4.202, WI-4.203, WI-4.204, WI-4.205, WI-4.206, WI-4.207, WI-4.208, WI-4.209, WI-4.210, WI-4.211, WI-4.212, WI-4.213, WI-4.214, WI-4.215, WI-4.597 |
| **M4.3** | Python package, Linux HDR, HDR scopes and dynamic metadata | weeks 5–7 | WI-4.540, WI-4.541, WI-4.542, WI-4.545, WI-4.546, WI-4.216, WI-4.310 (sup.), WI-4.544, WI-4.450, WI-4.451, WI-4.452, WI-4.453, WI-4.454, WI-4.455, WI-4.456, WI-4.457, WI-4.459, WI-4.470 |
| **M4.4** | Metric extensions, sidecar, phase-4 gate | weeks 7–9 | WI-4.300, WI-4.301, WI-4.302, WI-4.303, WI-4.304, WI-4.305, WI-4.306, WI-4.307, WI-4.308, WI-4.309, WI-4.598, WI-4.599 |

### 16.1.6 Track A — Android/ChromeOS

**Theme.** Gradle project, Qt Quick embedding, `SurfaceView` HDR sibling, `content://` I/O, x86_64 + arm64 ABIs, tablet/Chromebook layouts (resizable window, keyboard/mouse, hover), Play + F-Droid packaging. Starts in phase 0 and ships with each phase.

**Entry criterion.** MA.0 starts when WI-0.019 (Qt skeleton) is Done. MA.n (n ≥ 1) starts with the corresponding core phase.

**Exit demo (per phase, in addition to the "Android" lines of the core phase demos):**

| # | Action | Expected observable result |
|---|---|---|
| 1 | Resize the window on the Chromebook in free-form mode | layout keeps its state; `onCreate` count stays 1 in logcat; `HdrSurfaceView` re-layouts within 1 px of the Picture panel |
| 2 | External keyboard: press every shortcut of chapter 11 §11.4.3 | each performs the desktop action |
| 3 | Hover a block with a mouse | tooltip with block id and type appears |
| 4 | Open a file via `content://` from the Files app and via a "Share to OVT" intent | opens; recent list shows the display name; reopens after process restart |
| 5 | `tools/repro_check.sh android` | F-Droid reproducible build produces an APK whose `classes.dex` and native libraries hash equal the release pipeline's |
| 6 | HDR device (HDR Chromebook or HDR Android tablet) | §14.10.5 steps 1–3 pass through the `SurfaceView` sibling; the Pixel Tablet (SDR) shows the documented fallback |

| Milestone | Title | Duration | Work items |
|---|---|---|---|
| **MA.0** | Android shell and HDR hardware spike (track A, runs with M0.3–M0.4) | weeks 3–6 | WI-0.050, WI-0.051, WI-0.052, WI-0.054, WI-0.055, WI-0.034, WI-0.035, WI-0.056, WI-A.402 (sup.) |
| **MA.1** | Android GUI shell, HDR surface, instrumentation (track A, runs with M1.3–M1.4) | weeks 5–8 | WI-A.450, WI-A.451, WI-A.452, WI-A.453, WI-A.455, WI-A.400, WI-A.401, WI-A.403, WI-4.458, WI-A.404, WI-A.454, WI-A.456, WI-A.457 (sup.), WI-1.577 |
| **MA.2** | Android analysis performance and plugin store (track A, runs with M2.3: it needs the overlay benchmarks of WI-2.438) | weeks 5–6 | WI-2.168, WI-A.405, WI-A.406, WI-0.053 |
| **MA.3** | Android metrics build and parity (track A, runs with M3.3) | weeks 5–6 | WI-3.317, WI-3.594 |

---

## 16.2 Team workflow for implementing agents

This section is normative for every agent that implements a WI. It is deliberately mechanical.

### 16.2.1 Picking a work item

1. Open §16.4. A WI is **eligible** when (a) its milestone is the current or an earlier milestone of its phase, (b) every id in its "Depends on" column is marked Done in the tracker (the repository's issue board; one issue per WI, titled with the WI id, created idempotently from this table by `tools/check_wi_refs.py --make-issues` of WI-0.007 when M0.1 starts and re-run after every §16.4 change), (c) it is not marked superseded, and (d) nobody else holds it (assign the issue to yourself before starting).
2. Prefer the eligible WI with the lowest milestone ordinal; among equals, prefer one on the critical path (§16.5.1), then the smallest size.
3. Read, in this order: the WI's §16.3 row if any; the WI block in its chapter; every chapter section named in the WI's **Interface** line; the backbone sections it cites; chapter 14 §14.1.2 (Definition of Done). Do not start coding before this reading is complete.
4. If a dependency is Done but its artefact is missing or does not match its chapter's **Produces** line, stop and file a bug against that WI (not a spec question) before continuing.
5. Hardware-bound WIs (§16.5.3) MUST only be picked by an agent with access to the named device; the issue carries the label `needs-hardware:<id>`.

### 16.2.2 Definition of Done

A WI is Done when chapter 14 §14.1.2 items 1–8 hold, **plus**:

9. The PR description lists every acceptance test of the WI with the automated test name or the path of the manual record `docs/acceptance/wi/WI-<phase>.<nnn>.md`.
10. Every path in the WI's **Produces** line exists (or the PR states which paths §16.3 removed from the WI).
11. The §16.4 row still describes the WI (if the PR changed scope, the same PR edits this chapter — §16.2.5).
12. The issue is moved to Done by the reviewer, not the author.

CI enforces chapter 14 items 1–5, 7 and 8 mechanically; the reviewer checks items 6 and 9–12.

### 16.2.3 Branches, commits and pull requests

| Rule | Value |
|---|---|
| Branch name | `wi/<phase>.<nnn>-<kebab-slug>`, e.g. `wi/1.121-stream-index-builder`. One branch per WI. A split WI (§16.2.6) uses `wi/1.121a-…`, `wi/1.121b-…`. |
| Base branch | `main` (always releasable; chapter 13 §13.8). Never branch from another WI branch unless the dependency is still open — then rebase onto `main` once it merges, before review. |
| Commit message | First line ≤ 72 chars, starts with the WI id: `WI-1.121: build StreamIndex frame table`. Body explains *why*. `Signed-off-by:` trailer (DCO, chapter 15 §15.7). The `tools/check_wi_refs.py` hook rejects commits without a `WI-` or `#issue` reference. |
| PR size | ≤ 1 500 changed lines excluding generated code, goldens, patches under `third_party/patches/` and `plugins/*/patches/`. A WI of size L whose diff exceeds this MUST be split (§16.2.6) or delivered as a stack of ≤ 3 PRs merged in order, each green on CI. |
| PR title | `WI-<phase>.<nnn>: <imperative title from the chapter>` (or `WI-1.121 (1/3): …` for a stack). |
| PR body | Template: Goal (copied), Acceptance tests table (test name or manual record per line), Deviations from the spec (or "none"), Spec questions raised (issue links), `changelog.d/<pr>.<type>.md` added. |
| Review | ≥ 1 maintainer approval; 2 for `core/include/ovt/abi/`, `core/schema/`, `LICENSES/`, `tools/license_policy.toml` (chapter 15 §15.7.3, WI-0.605). Squash or rebase merge, linear history. |
| Required checks | chapter 13 §13.7.3: `lint`, `license-gate`, `build-linux-x64`, `build-macos-arm64`, `asan-ubsan`, `fuzz-smoke`, `clang-tidy`, `android-build`, `android-emulator-smoke`, `python-typecheck`, `abi-check` (when triggered), and the status check `DCO` (GitHub app, chapter 13 §13.7.3 / chapter 15 WI-0.604; not `dco-check`). |

### 16.2.4 When the spec is ambiguous or wrong

Agents MUST NOT guess silently. The procedure:

1. Search the spec first: the glossary (chapter 2), the backbone, the chapter's Interface sections and chapter 17 §17.2 (open questions already known). Many "ambiguities" are answered elsewhere.
2. If still ambiguous, open an issue labelled `spec-question` titled `spec: <chapter §> — <one-line question>`, quoting the conflicting sentences with file and line, and proposing one answer with its consequence. Link it from the WI issue. The spec editor answers within one working day (the owner, for questions chapter 17 lists as owner decisions); a question still unanswered after two working days is settled by the proposed answer, which the editor writes into the spec under §16.2.8.
3. **Continue working** on the answer you proposed unless the ambiguity blocks the interface other WIs consume (a public header, a schema, a CLI grammar, a file path). In that case stop that part of the WI and pick other steps or another WI.
4. Record the assumption in the PR body under "Deviations / assumptions" **and** in the code as a comment `// SPEC-ASSUMPTION(#<issue>): …`. The reviewer merges only if the assumption is harmless or the owner answered.
5. When the owner answers, the answer is written into the spec chapter in the same PR that implements it (chapter 14 DoD item 5). `// SPEC-ASSUMPTION` comments are removed when resolved; CI (`tools/check_wi_refs.py`) counts them and the nightly report lists the open ones.
6. A contradiction between two chapters is resolved by the precedence rule at the top of this chapter; if §16.3 does not cover it, treat it as a spec question.

### 16.2.5 Keeping this chapter true

Any PR that changes a WI's scope, dependencies, size or produced paths MUST edit the WI block in its chapter **and** the row in §16.4 in the same PR. The reviewer checks that `tools/check_wi_refs.py --table docs/spec/16-roadmap-and-work-items.md` passes (it verifies that every WI id in §16.4 exists exactly once in the chapters, that every dependency id exists, that no superseded WI is referenced as a dependency by a non-superseded WI or cited by a chapter 4 §4.17 row, that the `Depends on` column equals the chapter's `**Depends on:**` line, that the `FRs` column is the exact inverse of chapter 4 §4.17, and — the **milestone-order rule** — that every dependency's milestone ordinal is ≤ the dependent WI's (phases ordered 0 < 1 < 2 < 3 < 4; `MA.n` milestones take the ordinal of the first core milestone in their title, §16.0). WI-0.007 implements these checks as `tools/check_wi_refs.py --table`).

### 16.2.6 Splitting a work item; adding a new work item

A WI MAY be split when (a) its estimated diff exceeds the PR size limit, (b) two agents can work on independent **Steps** of it concurrently, or (c) a hardware-bound step blocks the rest. Rules: the split is announced in the WI issue with the step numbers per part; parts are named `WI-x.nnna`, `WI-x.nnnb` (letters, never new numbers — numbering is owned by this chapter); every acceptance test of the original is assigned to exactly one part; the original WI is Done only when all parts are Done. A WI MUST NOT be split so that a part has no acceptance test.

**Adding a genuinely new work item** (D28). A new WI is created only by the specification editor, never by an implementing agent, and only when a requirement, review finding or owner decision needs work that no existing WI can absorb. Procedure: (1) the id is the next free number in the owning chapter's range for that phase (chapter ranges: 06 `0.08x/0.09x`, 07 `x.1xx`, 08 `x.2xx`, 09 `x.3xx`, 10/11 `x.4xx`, 12 `x.5xx`, 13 `0.0xx`, 14 `x.5[5-9]x`, 15 `0.6xx`; a new sub-range is opened by the editor and recorded here — the first one is **ids 4.470 to 4.479 of phase 4** for chapter 11 panels, opened for WI-4.470 "HRD buffer graph panel"); (2) the §16.4 row is written first (phase, milestone, concrete dependencies, FRs), then the WI block in the owning chapter with every template line (D15), then the chapter 4 §4.17 rows it realises; (3) the milestone-order rule of §16.2.5 must hold after the addition; (4) the change is recorded in `docs/spec/CHANGELOG.md` (§16.2.8) and the spec version is bumped. The checker rejects a block without a row and a row without a block.

### 16.2.7 Daily mechanics

- Run `ctest --preset pr` locally before pushing; run `ctest -L golden` when touching any parser, decoder plugin, exporter or metric.
- Never commit corpus media, goldens > 2 MB gzip-compressed, measured on the file as committed (use `.gz`/zstd as chapter 14 §14.4 prescribes), or generated FlatBuffers code.
- Determinism: run the WI's output twice; if bytes differ, the WI is not Done (chapter 0 §0.7.5).
- When a third-party pin must move, that is a separate PR touching only `third_party/VERSIONS.md` and the affected script, with the golden suites re-run (chapter 13 §13.3.7).

### 16.2.8 Spec change procedure and self-versioning

The specification is versioned independently of the software (D28). Rules:

1. The version in `docs/spec/00-front-matter.md` is `MAJOR.MINOR.PATCH`; a release candidate for owner review carries a `-rcN` suffix (`1.0-rc1` = `1.0.0-rc1`), and the owner's acceptance turns it into the plain version. **PATCH** = editorial or cross-reference fixes that change no requirement sentence, acceptance line, interface or WI scope; **MINOR** = a changed or added FR/NFR acceptance line, interface signature, schema field, CLI option, WI scope/Produces/Depends-on, or a new WI; **MAJOR** = a change to an ADR, a D-decision or the repository layout.
2. Every change is recorded in `docs/spec/CHANGELOG.md` (newest first) as `## <version> — <date>` followed by one line per change: `<chapter §> — <what changed> — <affected WI/FR ids>`. WI text changes name the WI id; a changed acceptance line names the FR id.
3. A PR that changes spec text is a spec PR: it edits only `docs/spec/**`, bumps the version per rule 1 and adds the changelog lines; it is reviewed by the spec editor (CODEOWNERS entry for `docs/spec/`). Implementation PRs that must adjust a WI block or a §16.4 row (§16.2.5) carry the same changelog line and a PATCH or MINOR bump in the same PR.
4. Every implementation PR states in its body the spec version it was built against (`Spec-Version: 1.0.0`). A MINOR bump that changes an FR acceptance line or a WI's Steps/Acceptance **re-opens** every WI that claims that FR (chapter 4 §4.17) or is that WI, unless the WI's PR already passes the new acceptance line; the editor lists the re-opened ids in the changelog entry.
5. `tools/check_wi_refs.py --spec-version` fails CI when `docs/spec/**` changed in a PR without a version bump and a changelog entry.

---

## 16.3 Overlap resolution

Several chapters specified the same deliverable twice because they were written in parallel. For each overlapping group this section names **one primary WI** (the one with the more complete specification, or the one in the chapter that owns the file per chapter 0 §0.4/§0.10) and states exactly what each secondary WI is reduced to. The same sentence is written into the secondary WI's block as an `**Overlap:**` line. Rules:

- A **superseded** WI is not implemented. Its issue is closed as `superseded`; its acceptance tests named below are added to the primary's PR checklist. It keeps its row in §16.4 (marked) so that dependency references in the chapters stay resolvable.
- A **reduced** WI is implemented with exactly the scope stated here. If its chapter block names a file that the primary owns, the file is **not** created twice; the reduced WI consumes the primary's artefact.
- File path and function name conflicts are decided per group; the decision is binding for both WIs.

### 16.3.1 Corpora manifests, fetcher and golden runner

| Deliverable | Primary | Secondary → resolution |
|---|---|---|
| `tools/fetch_corpora.py`, `tests/corpora/manifest.json`, `manifest.schema.json`, exit codes 0/2/3/4, `--tags/--ids/--sources/--verify-only` grammar (chapter 14 §14.2.4) | **WI-0.551** (ch 14) | **WI-0.040** (ch 13) reduced to the CMake side: `cmake/OvtCorpora.cmake`, `OVT_CORPORA_DIR`, tests skip (not fail) with the message `corpus subset 'fast' missing: run tools/fetch_corpora.py --tags fast`, and `tests/corpora/README.md`. Its `--set mini` is spelled `--tags fast`; `tests/corpora/mini.json` is not written (one manifest). Acceptance test 1 of WI-0.040 moves to WI-0.551; test 2 stays. |
| Manifest population, `fast` subset ≤ 300 MiB, `LICENSES.md` | **WI-0.552** (ch 14) | **WI-1.237** (ch 08) reduced to the container-specific entries (tags `container-smoke`, `container-full`, `expected` blocks per §8.9.3) in the single manifest; no per-directory manifests, no `--lane` flag (use `--tags`). Chapter 8 §8.9.3's manifest shape is replaced by §14.2.4's. |
| Golden runner: `ovt_add_golden_test(...)`, canonical families, `fb_normalize`, `OVT_UPDATE_GOLDEN=1` | **WI-0.556** (ch 14) with WI-0.550 | **WI-0.041** (ch 13) superseded; its acceptance tests 1–2 (unified diff on mismatch; `ctest -L golden -j8` without temp-file collisions) move to WI-0.556. The CLI-level golden runner of WI-1.510 (ch 12) remains separate: it drives `ovt` binaries, not libraries. |

### 16.3.2 Fuzz scaffold

| Deliverable | Primary | Secondary → resolution |
|---|---|---|
| `ovt_add_fuzz_target(name)`, fuzz CMake, `linux-x64-fuzz`/`oss-fuzz` presets, `fuzz_bitreader`, seeds tooling, regress mode, `.github/workflows/fuzz-smoke.yml` | **WI-0.557** (ch 14) | **WI-0.005** (ch 13) superseded (test 1 moves to WI-0.557; `tools/fuzz_smoke.sh` not written). **WI-0.033** (ch 13) reduced to the nightly lane and required-check wiring. |
| BitReader / RBSP fuzz targets | WI-0.557 owns `fuzz_bitreader` | **WI-1.105** (ch 07) reduced to `fuzz_rbsp` + the BitReader seed corpus; registered with `ovt_add_fuzz_target`; no workflow step. |
| Header-parser fuzz targets (`fuzz_h264_units`, `fuzz_h265_units`, `fuzz_av1_obus`, `fuzz_index_builder`) | **WI-1.129** (ch 07) unchanged except: CI step comes from WI-0.557/WI-0.033 | — |
| Container fuzz targets incl. splitters and generated per-box fuzzers | **WI-1.238** (ch 08) | The splitter targets `fuzz_container_{annexb,obu,ivf,y4m}` listed in WI-0.557 **move to WI-1.238** (they need chapter 8 parsers that do not exist in phase 0). WI-1.238 registers everything through `ovt_add_fuzz_target`. |
| Fuzz source directory | `core/tests/fuzz/` (chapter 0 §0.4; chapters 07/08/13) | Chapter 14 §14.2.2's `tests/fuzz/` is read as `core/tests/fuzz/`; WI-0.557's produced paths move accordingly. **Spec fix applied in chapter 14 §14.2.2** (fuzz tree moved under `core/tests/`). |

### 16.3.3 Test framework, sanitizers and static analysis

| Deliverable | Primary | Secondary → resolution |
|---|---|---|
| Catch2 v3 framework, `main.cpp` with logger hook, CTest labels, `testPresets` (`pr`, `nightly`, `sanitizers`), the `tools_pytest` CTest test (chapter 13 §13.7.1 has no separate `tests` job: it is the test step of WI-0.030's `build-*` jobs, issue #11) | **WI-0.550** (ch 14) | **WI-0.004** (ch 13) reduced to `cmake/OvtTesting.cmake` sanitizer flag plumbing (`ovt_enable_sanitizers()`, `OVT_SANITIZED`, the `canary` test). Its test 2 (`ctest -L unit`) moves to WI-0.550. |
| Sanitizer presets `linux-x64-asan-ubsan`, `linux-x64-tsan`, `macos-arm64-asan` | WI-0.004 (presets exist in `CMakePresets.json` from WI-0.001; flags from WI-0.004) | **WI-0.558** (ch 14) reduced to suppression files, `tools/check_suppressions.py`, `docs/dev/sanitizers.md`, propagation of flags into third_party builds. |
| CI jobs `asan-ubsan`, `tsan` (nightly), `macos-asan` (nightly), `coverage` (NFR-028), `abi-check` | **WI-0.031** (ch 13) | `sanitizers.yml` of WI-0.558 is not written; WI-0.031 owns the jobs. |
| `.clang-tidy`, `tools/check_ovt_flags.py` (the `-Werror` set itself is the `ovt::flags` target of chapter 13 §13.2.5, D24), `static-analysis.yml` (PR `clang-tidy-diff`, nightly whole-tree, cppcheck) | **WI-0.559** (ch 14) | **WI-0.007** (ch 13) reduced to pre-commit hooks, `clang-format`, `check_versions_md.py`, `check_wi_refs.py`. **WI-0.031** does not own the `clang-tidy` job. |

### 16.3.4 Licence gate

| Deliverable | Primary | Secondary → resolution |
|---|---|---|
| `tools/license_gate.py`, `tools/license_policy.toml` (completed), SPDX parser, report JSON, exceptions — the `[directories]` table of `tools/license_policy.toml` is **produced by WI-0.601** (M0.1, read by `check_spdx_dirs.py`) and WI-0.602 completes the file; WI-0.602 depends on WI-0.601, WI-0.000 and WI-0.010 only (not on WI-0.036: the gate reads `build/<preset>/license_manifest.json` from WI-0.000 and `VERSIONS.md` from WI-0.010; release collection consumes the gate report later — D25) | **WI-0.602** (ch 15) | **WI-0.561** (ch 14) reduced to the corpora rules of §14.3.4 implemented as `tools/license_gate.py --corpora` (a module inside the same tool) plus the `tests/corpora/LICENSES.md` generator; no `tools/licence_gate.py`, no `licence_allowlist.json` (rules live in `license_policy.toml`). **Spelling:** the file is `license_gate.py` (American, chapter 15); chapter 14 §14.1.2 item 8 and §14.11 corrected in editing. |
| CI wiring with all inputs | **WI-0.032** (ch 13) | WI-0.561's `licence-gate.yml` is not written. |
| Branch protection / required checks | **WI-0.605** (ch 15) | No overlap; WI-0.605 lists WI-0.032's job as a required check. |

### 16.3.5 Performance harness

| Deliverable | Primary | Secondary → resolution |
|---|---|---|
| Google Benchmark harness, `tools/perf_run.py`, `tools/perf_compare.py`, `tests/perf/budgets.json`, baselines, `perf-nightly.yml`, runner registration (M1, Linux, Pixel Tablet, Chromebook) | **WI-0.560** (ch 14) | **WI-0.037** (ch 13) superseded (its tests move: results are artefacts compared against `tests/perf/baselines/`, not a `perf-results` branch; a > 20 % regression opens a `perf-regression` issue). |
| `render_bench` | **WI-1.415**, **WI-2.438** (ch 10) reduced: Google Benchmark binaries registered in the harness; benchmark names `bench_overlay_4k_mv_arrows`, `bench_overlay_build` (§14.9.3); thresholds in `tests/perf/budgets.json`, not `render/tests/perf_thresholds.json` | — |
| Container perf | **WI-1.239** (ch 08) reduced to `bench_container_open_moov_1h` and sibling benchmarks; perf files come from `tests/corpora/synthetic/generate.py` (WI-0.553), not `tools/make_perf_files.py` | — |
| Index perf | WI-1.125's `perf_index_h264_1h.cpp` is registered as `bench_index_h264_1h_1080p`; `tools/make_long_stream.sh` is replaced by the `syn_h264_1h_1080p` recipe of WI-0.553 | — |

### 16.3.6 Android shell

| Deliverable | Primary | Secondary → resolution |
|---|---|---|
| Gradle project, `MainActivity.kt`, `QtQuickView` embedding, both ABIs, `foss`/`play` flavours, `ovt_jni.cpp`, `bootstrap_android.sh` | **WI-0.050** (ch 13) | **WI-A.450** (ch 11) reduced to `OvtSession.kt` and the §11.5.1 manifest entries; its test 2 (ChromeOS free-form) is covered by WI-0.054. |
| `content://` → fd: C++ `open_fd` + C API | **WI-1.101** (ch 07) reduced to the C++/C side (`FileOpener.kt` not written) | **WI-0.051** (ch 13) owns `FilePicker.kt` (SAF UI, `ACTION_VIEW`, drag-and-drop, display name) and `ovt_jni_open_fd`; WI-1.101's step 3 moves there. **WI-A.452** (ch 11) reduced to the GUI flows (recent list, export, `AND-0001` copy action, Espresso tests); no `Files.kt`/`file_fd_bridge.cpp`. |
| HDR `SurfaceView` sibling: `HdrSurfaceView.kt`, `hdr_surface.cpp`, dataspace, PQ test pattern (plain GLES) | **WI-0.052** (ch 13) | **WI-A.400** (ch 10) reduced to extending those files with geometry callbacks, visibility rule (R-ARCH-3) and lifecycle tests; no `HdrSurface.kt`/`hdr_surface_jni.cpp`. **WI-A.401** (ch 10) unchanged: it replaces the GLES test pattern with the libplacebo EGL target. **WI-A.404** (ch 10) extends the `DisplayCaps.kt` stub of WI-0.052. **WI-A.454** (ch 11) unchanged (Picture-panel integration). |
| ChromeOS/Android HDR hardware spike | **WI-0.055** (ch 13, phase 0 week one) | **WI-A.402** (ch 10) superseded; §10.4.4's pass/fail criteria become WI-0.055's Interface; WI-4.459 depends on WI-0.055. |
| ChromeOS adaptations (manifest, free-form, `onConfigurationChanged`) | **WI-0.054** (ch 13) | WI-A.451 (layouts), WI-A.453 (input parity), WI-A.455 (state) are GUI-side and unchanged. |
| Plugin store / codec pack | **WI-0.053** (ch 13) — scheduled in **MA.2** (needs WI-2.150, WI-2.520) | — |
| Signing, F-Droid metadata, reproducibility | **WI-0.056** (ch 13) | **WI-A.456** (ch 11) unchanged (icons, About/licences); depends on WI-0.056, WI-0.603. |
| Android CI | **WI-0.034** (build), **WI-0.035** (PR emulator smoke) (ch 13); **WI-1.577** (ch 14) nightly instrumentation `android-nightly.yml` + device farm | **WI-A.457** (ch 11) superseded by WI-1.577; `bench_gui_scrub` (chapter 14 §14.9.3) runs through WI-0.560's Android runner. |

### 16.3.7 third_party build scripts

| Script | Primary | Secondary → resolution |
|---|---|---|
| `third_party/libaom.cmake` | **WI-0.013** (ch 13) | **WI-2.154** (ch 07) reduced to the extension-patch hook, `LIBAOM_SOURCE_DIR` export and the inspection-callback smoke program; its test 2 (broken patch fails the build naming it) stays. |
| `third_party/libde265.cmake` + patch step | **WI-0.014** (ch 13) | **WI-2.162** (ch 07) reduced to patch 0001 content (header + implementation) and the smoke program; it replaces WI-0.014's placeholder patch. |
| `third_party/ffmpeg.cmake` (two flavours) | **WI-0.012** (ch 13) | **WI-2.169** (ch 07) reduced to the `ovt-plugin-ffmpeg` analysis skeleton. |
| `third_party/hm.cmake` | **WI-4.104** (ch 07, phase 4; depends on WI-0.010 only — the superseded WI-0.016 is not a dependency) | **WI-0.016** (ch 13) superseded (`-fPIC` note moves). |
| `third_party/libvmaf.cmake`, `third_party/libplacebo.cmake` | **WI-0.015** (ch 13) | **WI-3.309** (ch 09) reduced to the libvmaf host plugins; Meson Android cross files belong to WI-0.011. **WI-1.400** (ch 10) reduced to the render skeleton. |
| MoltenVK, ONNX Runtime | **WI-0.017** (ch 13) | WI-1.400 step 1 uses WI-0.017's MoltenVK (prebuilt), not a vcpkg port. |
| Qt skeleton target | **WI-0.019** (ch 13) minimal target with `--self-test` | **WI-1.450** (ch 11) extends the same `app/CMakeLists.txt`, `main.cpp`, `Main.qml` in place. |

### 16.3.8 CLI subcommands and APIs

Chapter 12 owns the `ovt` grammar (chapter 0 §0.10). Subcommand sources live in `cli/src/cmd/<name>.cpp` (chapter 12 convention); the `cli/src/cmd_<name>.cpp` paths of chapters 07/08 are not used.

| Subcommand / API | Primary | Secondary → resolution |
|---|---|---|
| `ovt probe` | **WI-1.503** (ch 12) | **WI-1.235** (ch 08) reduced to the core functions the CLI calls (`ovt_container_expand_all`, diagnostics-registry dump, `--bytes` hex helper of the byte service) and the `docs/spec` cross-reference; tests 1–2 move to WI-1.503, test 3 (`≥ 150 codes`) stays on the core function. |
| `ovt units`, `ovt syntax`, `ovt index` | **WI-1.504**, **WI-1.505** (ch 12) | **WI-1.128** (ch 07) superseded. `ovt units` does not exist in §12.2; its content is `ovt syntax` list mode (`-f csv|json` without a tree). Tests 1–3 of WI-1.128 move to WI-1.504/1.505. WI-1.126 depends on WI-1.504. |
| `ovt analyze`, `ovt plugins` | **WI-2.521**, **WI-2.520** (ch 12) | **WI-2.174** (ch 07) superseded; the YUView CSV writer is WI-0.089 (ch 06), not `core/src/export/yuview_csv.cpp`. |
| `ovt metrics` | **WI-3.530** (ch 12) | **WI-3.312** (ch 09) reduced to `core/src/metrics/export/{csv,json,fb}.cpp` and the `JobSpec` JSON parser; the grammar of §12.2.9 wins over the grammar block in WI-3.312 (contradiction resolved in editing: WI-3.312 no longer carries a grammar; chapter 12 §12.2.9 gained `--opt`, `--group-by`, `--display-model`, `--frames`). **Cycle removed:** WI-3.310 depended on WI-3.312 which depended on WI-3.310; WI-3.310 now depends on WI-3.530. |
| `ovt bdrate`, `ovt_bdrate` | **WI-3.531** (ch 12) for CLI + C function | **WI-3.313** (ch 09) owns the maths in `core/src/metrics/bd/` and `tools/oracles/run_bjontegaard.py`; WI-3.531 drops `core/src/metrics/bdrate.cpp`. |
| `ovt hdr` | **WI-3.532** (ch 12) incl. sequence scan (`--frames A-B`, selector grammar of chapter 12 §12.1.2; per-frame and sequence MaxCLL/MaxFALL) and `core/src/color/hdr_measure.cpp` | **WI-4.457** (ch 10) reduced to `ovt render` (PNG dump via the GPU surface); the earlier `hdr-scan` subcommand name and `hdr_scan.schema.json` are dropped. GPU measurement (WI-4.452) feeds the GUI scopes; the CLI uses the CPU path. |
| Python package | **WI-4.540** (skeleton), **WI-4.541** (analysis), **WI-4.542** (metrics) (ch 12) | **WI-0.095** (ch 06) owns `python/ovt/fb/` (generated) and `python/ovt/model.py`; WI-4.540 reuses it (no `ovt/_fb/` copy). **WI-4.310** (ch 09) superseded by WI-4.542. **WI-4.216** (ch 08) reduced to extending `python/ovt/_container.py` with the derived views and adding `--csv` to `ovt probe`. |
| `ovt_Job`, `ovt::jobs::Pool`, `log` module | **WI-1.512** (ch 12) — expanded: no chapter 07 WI produces `jobs::Pool` or `core/src/log/`; WI-1.512 delivers both and is scheduled in **M0.2** | WI-1.125, WI-1.453, WI-3.300 depend on WI-1.512. |
| C API headers | **WI-1.511** (ch 12) owns header text (§12.3) | **WI-1.130** (ch 07) implements `ovt_stream_unit_bytes`/`ovt_stream_unit_syntax` and must match §12.3; WI-1.511 depends on WI-1.130, WI-1.224, WI-1.234, WI-1.512. |
| validation subcommand | — | Chapter 12 §12.2 has no `validate` subcommand. **WI-0.086** (ch 06) is reduced: no `cli/src/cmd/validate.cpp`; validation is exposed as `ovt index --validate` (WI-1.505) and `ovt analyze --validate` (WI-2.521); WI-0.086's acceptance tests run through `core/tests/model/test_validator.cpp`. **Spec fix applied in chapter 12 §12.2.4/§12.2.8** (`--validate` flag) and chapter 06 §6.9/WI-0.086. |
| Plugin ABI header `analysis_decoder_v1.h` | **WI-2.150** (ch 07) writes it verbatim from chapter 0 §0.6.2 / chapter 7 §7.5.1 | No earlier WI publishes it; WI-2.150 is a phase-1 deliverable (§16.3.12). |

### 16.3.9 Golden tests and oracles

Chapter 14 owns oracles, normalisers, goldens and golden suites (§0.10). Module chapters keep the hand-written unit tests and the plugin-internal tests.

| Area | Primary | Secondary → resolution |
|---|---|---|
| `trace_headers` goldens and glue | **WI-0.555** (normalisers), **WI-0.567** (goldens), **WI-1.572** (suite), **WI-0.556** (CTest glue) | **WI-1.126** (ch 07) reduced to `tools/trace_names*.json` and `tools/trace_diff.py` as a normaliser consumed by WI-0.555's `syntax-elements` family; `ovt_add_trace_golden_test` and `regen_golden.py` are not written (goldens are generated in the oracle image). |
| GStreamer codecparsers cross-check | **WI-0.562** (oracle program) + WI-1.572 | **WI-1.127** (ch 07) superseded (no in-process GStreamer link; its field-map YAML becomes `tools/gen_gst_fields.py`). Its test 2 (SKIPPED when absent) is moot: goldens are committed. |
| ISOBMFF goldens | **WI-0.554/0.555** (oracle image, normalisers), **WI-0.564** (goldens, `KNOWN_DIFFS.md`), **WI-1.571** (suite) | **WI-1.236** (ch 08) reduced to `tools/golden.py coverage` (hand-decoder coverage vs FileFormatConformance `file_features`, ≥ 95 %) and the container field maps `tools/oracle_maps/*.json` consumed by WI-0.555; its test 2 (tkhd.track_ID regression) moves to WI-1.571. |
| libaom `inspect` parity | **WI-2.581** (ch 14) + WI-0.566 | **WI-2.161** (ch 07) superseded. |
| ffprobe side-data parity | **WI-2.583** (ch 14) + WI-0.567 | **WI-2.173** (ch 07) superseded. |
| HM grid parity | **WI-2.582** (ch 14) + WI-0.563/0.567 | WI-2.164 keeps its unit tests. |
| `vmaf` CLI parity | **WI-3.591** (ch 14) | **WI-3.310** (ch 09) reduced to `core/src/metrics/export/libvmaf_json.cpp` (`ovt_metrics_export_vmaf_json`) and `tools/oracles/make_10bit.py`; WI-3.530's `vmaf_json.cpp` calls that exporter. |
| libplacebo/mpv render parity | **WI-1.575** (ch 14; ΔE_ITP gates) | **WI-1.414** (ch 10; PSNR dB gates) superseded. **Metric contradiction** (40/45 dB PSNR vs p99 ΔE_ITP ≤ 1.0/0.5): chapter 14 wins; chapter 10 §10.9.2 item 3 and WI-1.414 corrected in editing. |
| GUI test tooling | **WI-0.568** (`imgdiff.py`, scaffolding, IPC client), **WI-1.576** (model tests, screenshots, scripts) (ch 14) | **WI-1.465** (ch 11) reduced to the in-app `--test-ipc` server (`app/src/testipc/`) and the `gui` job of `.github/workflows/ci.yml`; `tests/gui/script_runner.cpp` and the JSON scripts are replaced by WI-1.576's Python scripts. WI-1.576 does not implement the server. WI-0.568 depends on WI-0.019 only (not on a phase-1 app WI). |
| `FrameAnalysis` validator | **WI-0.086** (ch 06; V-001…V-032) | **WI-2.580** (ch 14) superseded; `ovt analyze --validate` → WI-2.521; cross-plugin consistency §14.6.2 → **WI-2.153** (ch 07, plugin conformance harness, scope grows). |

### 16.3.10 FlatBuffers CMake integration

| Deliverable | Decision |
|---|---|
| `OvtFlatBuffers.cmake` | One file at `cmake/OvtFlatBuffers.cmake` (chapter 13 directory convention, beside `OvtTesting.cmake`), one function `ovt_add_flatbuffers_schema(<target> <fbs>...)` (chapter 06 name). **WI-0.003** (ch 13) delivers the first version (code generation for C++/Python/Kotlin, `ovt_schemas` target, `tools/check_schema_compat.py`) against stub schemas; **WI-0.083** (ch 06) extends the same file with `.bfbs` embedding, `schema_version.h` and the `python/ovt/fb/` layout. WI-0.083's `core/cmake/OvtFlatBuffers.cmake` path and WI-0.003's `ovt_add_fbs` name are not used. |

### 16.3.11 Phase mismatch: `io`, `BitReader`, RBSP, `SyntaxBuilder`, unit scanners, jobs/log

Chapter 0 §0.8 and chapter 13 place `io`, `BitReader`, logging and jobs in phase 0 ("WI-0.1xx"); chapter 7 numbered them WI-1.100–1.107 and chapter 12 numbered the jobs C API WI-1.512. **Resolution: the numbering is kept (no renumbering); the following WIs are phase-0 deliverables and are scheduled in phase-0 milestones:** WI-1.100 (M0.2), WI-1.101 (M0.3), WI-1.102 (M0.2), WI-1.103 (M0.2), WI-1.104 (M0.2), WI-1.105 (M0.3), WI-1.106 (M0.3), WI-1.107 (M0.3), WI-1.512 (M0.2, expanded to `jobs::Pool` + `log`). Every reference to "WI-0.1xx" or "chapter 07 phase-0 WIs" in other chapters resolves to these ids (edits applied). The phase-0 gate (§16.6.1) includes their acceptance tests.

### 16.3.12 Phase mismatch: plugin loading and playback decoders

Chapter 0 §0.8 lists "playback decode (HW + SW)" in phase 1; chapter 7 numbered the playback decoder WI-2.171 (inside `ovt-plugin-ffmpeg`), which depends on WI-2.169 and WI-2.150. **Resolution: WI-2.150 (M1.1), WI-2.169 (M1.2), WI-2.171 (M1.3) and WI-2.172 (M1.3) are phase-1 deliverables; numbering kept.** The phase-1 gate requires WI-2.171's acceptance tests. WI-1.507 (`ovt dump`), WI-1.574 (MD5 suite) and WI-1.456 (Picture panel) depend on WI-2.171. Related: **WI-4.458** (Android display probe) is a track-A phase-1 deliverable (MA.1) because WI-A.404 needs it; **WI-0.053** (Android plugin store) is a phase-2 deliverable (MA.2); **WI-0.016** is absorbed into phase 4 (WI-4.104); **WI-1.463** delivers only the macOS NATIVE host in phase 1 (the Linux host is WI-4.450, phase 4).

### 16.3.13 Other binding decisions taken here

| Topic | Decision |
|---|---|
| Python generated FlatBuffers path | `python/ovt/fb/` (chapters 06); chapter 12's `ovt/_fb/` is not used. |
| CLI source layout | `cli/src/cmd/<name>.cpp` (chapter 12). |
| Perf benchmark naming | §14.9.3 names are canonical; chapter 10/08/07 perf test names map onto them as stated in §16.3.5. |
| Render parity metric | p99 ΔE_ITP (chapter 14 §14.4.15). |
| Manifest shape | chapter 14 §14.2.4 (`entries[]` with `id`, `licence_spdx`, `redistributable`, `tags`); chapter 8 §8.9.3 and chapter 13 WI-0.040 shapes are not used. |
| Licence gate filename | `tools/license_gate.py`. |
| Fuzz directory | `core/tests/fuzz/`. |
| Oracle normaliser location | `tests/oracles/normalize/` (chapter 14); chapter 8's `tools/golden/normalize_*.py` are not written. |
| Parser WIs vs golden suites (D25) | Header-parser WIs (WI-1.108–1.120, M1.1) depend on their inputs only (BitReader, RBSP, `SyntaxBuilder`, T.35 registry); they are tested by their unit tests. The golden suite WI-1.572 (M1.3) depends on the parsers and on WI-0.567 (goldens), not the reverse; `ovt syntax --trace` (WI-1.504, M1.3) is a consumer, not a dependency. The acceptance tests of WI-1.108–WI-1.120 are unit tests whose expected values are hard-coded from a recorded oracle output; corpus-wide `trace_headers`/JM/HM/GStreamer parity is WI-1.572 (headers) and WI-1.573 (POC/RefPicList/index). |
| Zoom/pan vs label pass (D25) | WI-1.409 (M1.3) depends on WI-1.408 only; its step "value text at zoom ≥ 24" and the `labels_zoom32.png` golden move to WI-2.433 (M2.1). In phase 1 the sample value under the cursor is shown in the status bar / Selection Info (FR-PLN-003 is completed in M2.1). |
| Jobs API tests (D25) | WI-1.512 (M0.2) tests cancellation with a synthetic `MockJob` (`core/tests/jobs/mock_job.cpp`), not with the index builder (WI-1.121, M1.2). |
| `test_model_roundtrip` (D25) | Produced by WI-0.093 (chapter 06); §16.1.1 #10 and §16.6.1 #9 run it. |
| Branding vs Android track | WI-0.606 (M0.4) does not depend on WI-0.050 (MA.0, parallel track): it generates the `android/gradle.properties` keys unconditionally and WI-A.456 (MA.1) verifies the Android branding. |

---

## 16.4 Resolved dependency table

One row per work item (380 rows, including WI-4.470 added under §16.2.6). "Chapter §ref" names the chapter and the section that holds the WI block. "Phase" is the **scheduled** phase; where it differs from the phase digit in the id, the numbered phase is given in parentheses (§16.3.11, §16.3.12). "Depends on" lists concrete WI ids only — every prose or wildcard dependency of the chapters has been resolved (the same resolution is written into the chapter's `**Depends on:**` line) and every id below exists in §16.4. Ids mentioned in a chapter's dependency line only to state ownership (for example "the Linux host is added by WI-4.450") are not dependencies and are not listed. Ranges in the chapters (`WI-1.205–1.216`) are expanded. Superseded WIs keep their row so that references stay resolvable; nobody implements them. Rows are sorted by the phase digit of the id, then by id.

Scheduled WIs per phase: 0 → 90, 1 → 119, 2 → 49, 3 → 30, 4 → 63, A → 29 (total 380 including WI-4.470; 14 superseded, 56 reduced). The `FRs` column is **derived** from chapter 4 §4.17 (inverse mapping; a superseded WI shows `—`); the milestone-order rule of §16.2.5 holds for every row.

| WI | Chapter §ref | Title | Size | Phase | Milestone | Depends on (concrete ids only) | FRs (chapter 4 §4.17) |
|---|---|---|---|---|---|---|---|
| WI-0.000 | 13 §13.10 | Repository skeleton and top-level CMake | M | 0 | M0.1 | none | FR-GEN-001, FR-GEN-002, FR-GEN-010, NFR-030 |
| WI-0.001 | 13 §13.10 | `CMakePresets.json` and custom vcpkg triplets | S | 0 | M0.1 | WI-0.000 | FR-GEN-008, FR-GEN-102, NFR-019 |
| WI-0.002 | 13 §13.10 | vcpkg manifest, submodule and binary caching | M | 0 | M0.1 | WI-0.001 | NFR-022 |
| WI-0.003 | 13 §13.10 | FlatBuffers code generation target and schema install (reduced, §16.3) | S | 0 | M0.2 | WI-0.002 | — |
| WI-0.004 | 13 §13.10 | Test infrastructure, CTest labels and sanitizer presets (reduced, §16.3) | S | 0 | M0.1 | WI-0.002 | NFR-028 |
| WI-0.005 | 13 §13.10 | libFuzzer harness scaffold (superseded by WI-0.557) | S | 0 | M0.3 | WI-0.004 | — |
| WI-0.006 | 13 §13.10 | Bootstrap scripts and developer docs | M | 0 | M0.1 | WI-0.002 | FR-GEN-101, NFR-027, NFR-034 |
| WI-0.007 | 13 §13.10 | Pre-commit hooks, clang-format/clang-tidy configs, lint CI inputs (reduced, §16.3) | S | 0 | M0.1 | WI-0.000 | NFR-029, NFR-036 |
| WI-0.008 | 13 §13.10 | Install/export verification and downstream consumer test | S | 0 | M0.1 | WI-0.002 | — |
| WI-0.010 | 13 §13.10 | `third_party` common infrastructure and `VERSIONS.md` parser | M | 0 | M0.1 | WI-0.002 | NFR-022 |
| WI-0.011 | 13 §13.10 | Meson wrapper (`ovt_tp_meson`) | M | 0 | M0.2 | WI-0.010 | — |
| WI-0.012 | 13 §13.10 | FFmpeg two-flavour build script | L | 0 | M0.3 | WI-0.011, WI-0.010 | — |
| WI-0.013 | 13 §13.10 | libaom inspection build script | S | 0 | M0.3 | WI-0.010 | — |
| WI-0.014 | 13 §13.10 | libde265 patched build script | S | 0 | M0.3 | WI-0.010 | — |
| WI-0.015 | 13 §13.10 | libvmaf and libplacebo Meson scripts | M | 0 | M0.3 | WI-0.011 | — |
| WI-0.016 | 13 §13.10 | HM oracle build script (optional) (superseded by WI-4.104) | S | 0 | M0.3 | WI-0.010 | — |
| WI-0.017 | 13 §13.10 | MoltenVK and ONNX Runtime acquisition scripts | S | 0 | M0.3 | WI-0.010 | — |
| WI-0.018 | 13 §13.10 | Qt installer script (`tools/install_qt.py`) | S | 0 | M0.2 | WI-0.006 | — |
| WI-0.019 | 13 §13.10 | Qt GUI skeleton target (`ovt_studio`) building on all presets (reduced, §16.3) | M | 0 | M0.2 | WI-0.018, WI-0.002 | — |
| WI-0.030 | 13 §13.10 | CI: lint and build jobs for Linux and macOS | M | 0 | M0.3 | WI-0.007, WI-0.002, WI-0.010, WI-0.011, WI-0.012, WI-0.013, WI-0.014, WI-0.015 | FR-GEN-008, FR-GEN-103, NFR-028 |
| WI-0.031 | 13 §13.10 | CI: sanitizer, clang-tidy and ABI jobs (reduced, §16.3) | S | 0 | M0.3 | WI-0.030, WI-0.004 | NFR-015, NFR-028 |
| WI-0.032 | 13 §13.10 | CI: licence gate job wiring | S | 0 | M0.3 | WI-0.030, WI-0.602 | NFR-031 |
| WI-0.033 | 13 §13.10 | CI: fuzz smoke (60 s per target) and nightly fuzz (reduced, §16.3) | S | 0 | M0.3 | WI-0.557, WI-0.030 | NFR-012 |
| WI-0.034 | 13 §13.10 | CI: Android build job | M | A (numbered 0) | MA.0 | WI-0.050, WI-0.018 | FR-CLI-108 |
| WI-0.035 | 13 §13.10 | CI: Android emulator smoke | M | A (numbered 0) | MA.0 | WI-0.034 | — |
| WI-0.036 | 13 §13.10 | CI: release workflow and artefact collection | M | 0 | M0.5 | WI-0.030, WI-0.034, WI-0.060, WI-0.070, WI-0.602, WI-0.603, WI-0.608 | FR-GEN-008, FR-GEN-104 |
| WI-0.037 | 13 §13.10 | CI: nightly perf job on self-hosted M1 (superseded by WI-0.560) | S | 0 | M0.4 | WI-0.030, WI-0.560 | — |
| WI-0.040 | 13 §13.10 | Corpus access from CMake (`OVT_CORPORA_DIR`, skip helper) (reduced, §16.3) | M | 0 | M0.4 | WI-0.006, WI-0.551 | — |
| WI-0.041 | 13 §13.10 | Golden test runner and corpus-driven test registration (superseded by WI-0.556) | S | 0 | M0.4 | WI-0.040 | — |
| WI-0.050 | 13 §13.10 | Android Gradle project skeleton with Qt embedding | L | A (numbered 0) | MA.0 | WI-0.019, WI-0.018, WI-0.001 | FR-AND-001 |
| WI-0.051 | 13 §13.10 | SAF file picking → `ovt_file_open_fd` | M | A (numbered 0) | MA.0 | WI-0.050, WI-1.101 | FR-AND-005 |
| WI-0.052 | 13 §13.10 | `HdrSurfaceView` sibling and dataspace plumbing | M | A (numbered 0) | MA.0 | WI-0.050 | FR-AND-002 |
| WI-0.053 | 13 §13.10 | Android plugin store and codec pack skeleton (reduced, §16.3) | M | A (numbered 0) | MA.2 | WI-0.050, WI-2.150, WI-2.520 | FR-AND-014, FR-AND-015, FR-AND-016 |
| WI-0.054 | 13 §13.10 | ChromeOS adaptations and large-screen manifest settings | S | A (numbered 0) | MA.0 | WI-0.050, WI-0.052 | FR-AND-007 |
| WI-0.055 | 13 §13.10 | ChromeOS/Android HDR hardware spike (week one) (reduced, §16.3) | M | A (numbered 0) | MA.0 | WI-0.052 | FR-AND-004 |
| WI-0.056 | 13 §13.10 | Android signing configuration, F-Droid metadata and reproducibility check | M | A (numbered 0) | MA.0 | WI-0.050, WI-0.034 | FR-AND-018, FR-GEN-104 |
| WI-0.060 | 13 §13.10 | macOS bundle script and `macdeployqt` integration | M | 0 | M0.5 | WI-0.019, WI-0.012, WI-0.017 | FR-MAC-001, FR-MAC-008 |
| WI-0.061 | 13 §13.10 | macOS signing, entitlements, notarisation and DMG | M | 0 | M0.5 | WI-0.060, WI-0.030 | FR-MAC-005, FR-MAC-006, NFR-019 |
| WI-0.062 | 13 §13.10 | Homebrew tap: cask, codec-pack cask and formula | S | 0 | M0.5 | WI-0.061, WI-0.036 | FR-MAC-007 |
| WI-0.063 | 13 §13.10 | macOS universal2 merge script (MAY) | S | 0 | M0.5 | WI-0.060 | FR-MAC-001 |
| WI-0.070 | 13 §13.10 | Linux AppImage script | M | 0 | M0.5 | WI-0.019, WI-0.012 | FR-LNX-005 |
| WI-0.071 | 13 §13.10 | Flatpak manifest and Flathub preparation | M | 0 | M0.5 | WI-0.070, WI-0.010 | FR-LNX-004 |
| WI-0.072 | 13 §13.10 | Distro packaging skeletons (Debian, Fedora/RPM Fusion, Arch) | M | 0 | M0.5 | WI-0.008, WI-0.012 | FR-LNX-004, FR-LNX-005 |
| WI-0.073 | 13 §13.10 | Linux runtime dependency checker and Vulkan CI setup | S | 0 | M0.5 | WI-0.015 | FR-LNX-001, FR-LNX-006 |
| WI-0.080 | 6 §6.11 | Write `ovt_model.fbs` and make it compile | M | 0 | M0.2 | WI-0.010 | FR-GEN-003 |
| WI-0.081 | 6 §6.11 | Write `ovt_container.fbs` | S | 0 | M0.2 | WI-0.080 | — |
| WI-0.082 | 6 §6.11 | Write `ovt_metrics.fbs` | S | 0 | M0.2 | WI-0.080 | — |
| WI-0.083 | 6 §6.11 | CMake integration for generated code and schema versioning (reduced, §16.3) | M | 0 | M0.2 | WI-0.080, WI-0.081, WI-0.082 | — |
| WI-0.084 | 6 §6.11 | Model helper library: grid, tree, units, labels | M | 0 | M0.3 | WI-0.083 | FR-BLK-002, FR-BLK-003, FR-BLK-016 |
| WI-0.085 | 6 §6.11 | Canonical builder and byte-identity tooling | M | 0 | M0.3 | WI-0.084, WI-0.093 | FR-GEN-009, NFR-017 |
| WI-0.086 | 6 §6.11 | `FrameAnalysis` / `StreamIndex` validator (V-001…V-032) (reduced, §16.3) | L | 0 | M0.4 | WI-0.085, WI-0.093 | FR-BLK-002, FR-BLK-003, FR-SYN-013 |
| WI-0.087 | 6 §6.11 | `PictureStore`: plane blobs, LRU, pinning | M | 0 | M0.3 | WI-0.084 | FR-AND-013, FR-GUI-029, NFR-009 |
| WI-0.088 | 6 §6.11 | JSON exporter/importer (reflection based) | L | 0 | M0.4 | WI-0.085 | FR-BLK-022 |
| WI-0.089 | 6 §6.11 | YUView statistics CSV exporter and importer | M | 0 | M0.3 | WI-0.084 | FR-BLK-022, FR-BLK-024 |
| WI-0.090 | 6 §6.11 | libaom `inspect`-compatible JSON exporter | M | 0 | M0.3 | WI-0.084, WI-0.093 | — |
| WI-0.091 | 6 §6.11 | Disk cache: layout, `meta.json`, read/write, zstd | L | 0 | M0.4 | WI-0.087, WI-0.010 | NFR-010 |
| WI-0.092 | 6 §6.11 | Cache invalidation and size-cap eviction | M | 0 | M0.4 | WI-0.091, WI-0.086 | NFR-010 |
| WI-0.093 | 6 §6.11 | Golden fixtures: hand-built `FrameAnalysis` records and round-trip tests | M | 0 | M0.3 | WI-0.084 | — |
| WI-0.094 | 6 §6.11 | Schema evolution and robustness tests (compatibility, fuzzing) | M | 0 | M0.4 | WI-0.086, WI-0.093 | — |
| WI-0.095 | 6 §6.11 | Python access to the schemas | S | 0 | M0.4 | WI-0.083, WI-0.088 | — |
| WI-0.550 | 14 §14.12 | Set up the test framework skeleton (reduced, §16.3) | M | 0 | M0.1 | WI-0.001, WI-0.002 | — |
| WI-0.551 | 14 §14.12 | Implement `tools/fetch_corpora.py` and the manifest schema | M | 0 | M0.2 | none | — |
| WI-0.552 | 14 §14.12 | Populate the full corpora manifest and the `fast` subset | L | 0 | M0.3 | WI-0.551 | — |
| WI-0.553 | 14 §14.12 | Synthetic stream generator | L | 0 | M0.3 | WI-0.551, WI-0.554 | — |
| WI-0.554 | 14 §14.12 | Oracle Docker image and build scripts | L | 0 | M0.2 | none | — |
| WI-0.555 | 14 §14.12 | Oracle normalisers and canonical JSON families | L | 0 | M0.3 | WI-0.554 | — |
| WI-0.556 | 14 §14.12 | Golden test runner (C++ side) and `fb_normalize` | M | 0 | M0.4 | WI-0.550, WI-0.083 | — |
| WI-0.557 | 14 §14.12 | Fuzz harness, first targets and CI smoke (reduced, §16.3) | M | 0 | M0.3 | WI-0.550, WI-1.102, WI-1.100 | FR-GEN-007, NFR-012, NFR-013 |
| WI-0.558 | 14 §14.12 | Sanitizer presets and suppressions (reduced, §16.3) | S | 0 | M0.2 | WI-0.550, WI-0.004, WI-0.010 | NFR-015 |
| WI-0.559 | 14 §14.12 | clang-tidy, `-Werror` set and cppcheck jobs | S | 0 | M0.2 | WI-0.550 | NFR-016 |
| WI-0.560 | 14 §14.12 | Performance harness and reference-hardware runners | M | 0 | M0.4 | WI-0.550, WI-0.552, WI-0.553 | FR-GEN-103, FR-SYN-016, NFR-001, NFR-002, NFR-004, NFR-006, NFR-008, NFR-009 |
| WI-0.561 | 14 §14.12 | Corpora and dependency licence gate (reduced, §16.3) | S | 0 | M0.3 | WI-0.551, WI-0.602 | — |
| WI-0.562 | 14 §14.12 | GStreamer codecparsers oracle program | M | 0 | M0.4 | WI-0.554 | — |
| WI-0.563 | 14 §14.12 | HM getter dumper (`hm_cu_dump`) and JM/HM trace builds | M | 0 | M0.4 | WI-0.554 | — |
| WI-0.564 | 14 §14.12 | mp4box.js oracle script and ISOBMFF golden set | S | 0 | M0.4 | WI-0.554, WI-0.555 | — |
| WI-0.565 | 14 §14.12 | Metric oracle goldens (vmaf CLI, FFmpeg psnr, x264 SSIM kernel, Daala, bjontegaard, UVQ) | M | 0 | M0.4 | WI-0.554, WI-0.555, WI-0.553 | — |
| WI-0.566 | 14 §14.12 | libaom `inspect` goldens and AV1 index goldens | S | 0 | M0.4 | WI-0.554, WI-0.555, WI-0.553 | — |
| WI-0.567 | 14 §14.12 | `trace_headers`, `gstparse`, JM and HM goldens for H.264/HEVC fast corpora | M | 0 | M0.4 | WI-0.562, WI-0.563, WI-0.555 | — |
| WI-0.568 | 14 §14.12 | Screenshot diff tool and GUI test scaffolding (reduced, §16.3) | M | 0 | M0.4 | WI-0.550, WI-0.019 | — |
| WI-0.569 | 14 §14.12 | OSS-Fuzz onboarding files and triage docs | S | 0 | M0.4 | WI-0.557 | — |
| WI-0.600 | 15 §15.9 | Licence texts, `LICENSE`, `REUSE.toml` and `docs/licensing.md` | S | 0 | M0.1 | WI-0.000 | FR-GEN-107, NFR-032 |
| WI-0.601 | 15 §15.9 | SPDX headers on all files and `tools/spdx_header.py` / `tools/check_spdx_dirs.py` | S | 0 | M0.1 | WI-0.600 | FR-GEN-105, NFR-032 |
| WI-0.602 | 15 §15.9 | `tools/license_gate.py` and `tools/license_policy.toml` | M | 0 | M0.2 | WI-0.601, WI-0.000, WI-0.010 | FR-GEN-106, NFR-031, NFR-033 |
| WI-0.603 | 15 §15.9 | `tools/gen_notices.py` and `ovt_notices` target | M | 0 | M0.2 | WI-0.602, WI-0.010 | NFR-031, NFR-033 |
| WI-0.604 | 15 §15.9 | `DCO`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, DCO CI check | S | 0 | M0.1 | WI-0.000 | — |
| WI-0.605 | 15 §15.9 | `MAINTAINERS.md`, `CODEOWNERS`, branch protection and review rules | S | 0 | M0.3 | WI-0.604, WI-0.030 | FR-GEN-108 |
| WI-0.606 | 15 §15.9 | Trademark checklist, branding single source and rename dry-run | S | 0 | M0.4 | WI-0.000, WI-0.019 | — |
| WI-0.607 | 15 §15.9 | Decoder patch header checker and plugin distribution manifest validation | S | 0 | M0.2 | WI-0.602 | FR-MAC-006, NFR-026 |
| WI-0.608 | 15 §15.9 | `docs/licensing.md` "Plugins and the GPL" position paper and channel listings text | S | 0 | M0.1 | WI-0.600 | — |
| WI-0.609 | 15 §15.9 | Legal review hand-off package | S | 0 | M0.5 | WI-0.600, WI-0.608, WI-0.601, WI-0.602, WI-0.603, WI-0.604, WI-0.605, WI-0.606, WI-0.607 | — |
| WI-1.100 | 7 §7.8 | Implement `ovt::io::RandomAccessFile` (mmap and pread backends) | M | 0 (numbered 1) | M0.2 | WI-0.000 | FR-GEN-011 |
| WI-1.101 | 7 §7.8 | Add fd-based open and the Android content URI path (reduced, §16.3) | S | 0 (numbered 1) | M0.3 | WI-1.100 | FR-AND-005 |
| WI-1.102 | 7 §7.8 | Implement `ovt::BitReader` core descriptors and `ParseError` | M | 0 (numbered 1) | M0.2 | none | FR-SYN-001, FR-SYN-002, FR-SYN-013, NFR-016 |
| WI-1.103 | 7 §7.8 | Add AV1 descriptors to `BitReader` | S | 0 (numbered 1) | M0.2 | WI-1.102 | FR-SYN-002 |
| WI-1.104 | 7 §7.8 | Implement `rbsp_from_nal` and raw↔RBSP offset mapping | S | 0 (numbered 1) | M0.2 | none | FR-SYN-003 |
| WI-1.105 | 7 §7.8 | BitReader and RBSP fuzz targets and sanitizer CI hook (reduced, §16.3) | S | 0 (numbered 1) | M0.3 | WI-1.102, WI-1.103, WI-1.104 | — |
| WI-1.106 | 7 §7.8 | Implement `SyntaxBuilder` and the `SyntaxNode` writer | M | 0 (numbered 1) | M0.3 | WI-1.102, WI-0.080 | FR-GEN-003, FR-SYN-007, FR-SYN-008, FR-SYN-013 |
| WI-1.107 | 7 §7.8 | Implement unit scanners (Annex B, length-prefixed, OBU low-overhead and Annex B) | M | 0 (numbered 1) | M0.3 | WI-1.100, WI-1.103 | — |
| WI-1.108 | 7 §7.8 | H.264 NAL header, SPS (with VUI/HRD/scaling lists), PPS parsers | L | 1 | M1.1 | WI-1.104, WI-1.106 | FR-SYN-004 |
| WI-1.109 | 7 §7.8 | H.264 slice header (+ ref list modification, pred weight table, dec ref pic marking), AUD, SPS ext, subset SPS, prefix NAL, data partitions | L | 1 | M1.1 | WI-1.108 | FR-SYN-004 |
| WI-1.110 | 7 §7.8 | H.264 SEI framing and payload parsers | M | 1 | M1.1 | WI-1.108, WI-1.120 | FR-SYN-004, FR-SYN-011, FR-SYN-012 |
| WI-1.111 | 7 §7.8 | H.264 activation, POC (types 0/1/2) and frame-boundary detection | M | 1 | M1.1 | WI-1.109 | FR-DEC-002, FR-SYN-009, FR-SYN-010 |
| WI-1.112 | 7 §7.8 | H.265 NAL header, VPS, PTL, SPS (VUI/HRD/scaling/range/SCC), PPS (tiles/range/SCC) | L | 1 | M1.1 | WI-1.104, WI-1.106 | FR-SYN-005 |
| WI-1.113 | 7 §7.8 | H.265 slice segment header, `st_ref_pic_set`, pred weight table, entry points | L | 1 | M1.1 | WI-1.112 | FR-SYN-005 |
| WI-1.114 | 7 §7.8 | H.265 SEI payload parsers | M | 1 | M1.1 | WI-1.112, WI-1.110, WI-1.120 | FR-SYN-005, FR-SYN-011, FR-SYN-012 |
| WI-1.115 | 7 §7.8 | H.265 activation, POC, frame boundary, RPS/DPB and RefPicList derivation | M | 1 | M1.1 | WI-1.113 | FR-DEC-003, FR-SYN-009, FR-SYN-010 |
| WI-1.116 | 7 §7.8 | AV1 OBU header, sequence header, temporal delimiter, metadata, padding, tile list parsers | L | 1 | M1.1 | WI-1.103, WI-1.106, WI-1.120 | FR-SYN-006, FR-SYN-011 |
| WI-1.117 | 7 §7.8 | AV1 `uncompressed_header` part 1 | L | 1 | M1.1 | WI-1.116, WI-1.119 | FR-SYN-006 |
| WI-1.118 | 7 §7.8 | AV1 `uncompressed_header` part 2, frame OBU, tile group sizes | L | 1 | M1.1 | WI-1.117 | FR-SYN-006 |
| WI-1.119 | 7 §7.8 | AV1 reference slot model, OrderHints, `set_frame_refs`, frame/temporal-unit boundaries | M | 1 | M1.1 | WI-1.116 | FR-DEC-004, FR-SYN-009, FR-SYN-010 |
| WI-1.120 | 7 §7.8 | ITU-T T.35 and `user_data_unregistered` registry with HDR10+/CEA-708/x264/x265/DV detection | S | 1 | M1.1 | WI-1.102 | FR-SYN-011 |
| WI-1.121 | 7 §7.8 | `StreamIndex` builder core | L | 1 | M1.2 | WI-1.107, WI-1.111, WI-1.115, WI-1.119, WI-1.110, WI-1.114, WI-1.231 | FR-DEC-001, FR-DEC-005, FR-DEC-011 |
| WI-1.122 | 7 §7.8 | H.264 DPB simulation (sliding window, MMCO, ref list init/modification) | L | 1 | M1.2 | WI-1.121 | FR-DEC-002 |
| WI-1.123 | 7 §7.8 | H.265 RPS/RefPicList integration into the index | M | 1 | M1.2 | WI-1.121, WI-1.115 | FR-DEC-003 |
| WI-1.124 | 7 §7.8 | AV1 slot model integration (hidden frames, show_existing, operating points) | M | 1 | M1.2 | WI-1.121, WI-1.119 | FR-DEC-004 |
| WI-1.125 | 7 §7.8 | Incremental/cancellable indexing, lazy header trees, performance test | M | 1 | M1.2 | WI-1.121, WI-1.512 | FR-SYN-016, NFR-001 |
| WI-1.126 | 7 §7.8 | `trace_headers` diff tool and golden-test infrastructure (reduced, §16.3) | M | 1 | M1.3 | WI-1.106, WI-1.504, WI-0.555 | — |
| WI-1.127 | 7 §7.8 | GStreamer codecparsers cross-check harness (optional test dependency) (superseded by WI-0.562, WI-1.572) | M | 1 | M1.3 | WI-1.109, WI-1.113, WI-1.118 | — |
| WI-1.128 | 7 §7.8 | CLI hooks: `ovt units`, `ovt syntax`, `ovt index` (superseded by WI-1.504/WI-1.505; see chapter 16 §16.3) | M | 1 | M1.3 | WI-1.121, WI-1.125, WI-1.500 | — |
| WI-1.129 | 7 §7.8 | Fuzz targets for the H.264, H.265 and AV1 header parsers | S | 1 | M1.2 | WI-1.121 | — |
| WI-1.130 | 7 §7.8 | C API for unit bytes and on-demand syntax; Python/CLI smoke (reduced, §16.3) | S | 1 | M1.3 | WI-1.125 | FR-SYN-014 |
| WI-1.200 | 8 §8.10 | Create the container module skeleton and format sniffer | M | 1 | M1.1 | WI-1.100, WI-1.102, WI-0.083, WI-1.512 | FR-GEN-004 |
| WI-1.201 | 8 §8.10 | Implement the ISOBMFF box header walk with spans and truncation handling | M | 1 | M1.1 | WI-1.200, WI-0.553 | FR-CNT-001, FR-CNT-101 |
| WI-1.202 | 8 §8.10 | Implement `FieldBuilder`, `BoxContext` propagation and the decoder registration macro | M | 1 | M1.1 | WI-1.201 | — |
| WI-1.203 | 8 §8.10 | Write `tools/gen_box_registry.py` and generate `box_registry.gen.cpp` | M | 1 | M1.1 | WI-1.202 | FR-CNT-003 |
| WI-1.204 | 8 §8.10 | Implement lazy traversal, `ovt_container_expand` and `mfro`/`mfra` location | M | 1 | M1.1 | WI-1.203 | FR-CNT-004, FR-GEN-011 |
| WI-1.205 | 8 §8.10 | Hand decoders, group A (file and movie level) | M | 1 | M1.2 | WI-1.202 | FR-CNT-002 |
| WI-1.206 | 8 §8.10 | Hand decoders, group B (`stsd` and sample entries) | M | 1 | M1.2 | WI-1.205 | FR-CNT-002 |
| WI-1.207 | 8 §8.10 | Hand decoders, group C part 1 (codec configuration records) | L | 1 | M1.2 | WI-1.206, WI-1.108, WI-1.112, WI-1.116 | FR-CNT-002 |
| WI-1.208 | 8 §8.10 | Hand decoders, group C part 2 (colour, HDR, Dolby Vision, misc. sample-entry children) | M | 1 | M1.2 | WI-1.206 | FR-CNT-002 |
| WI-1.209 | 8 §8.10 | Hand decoders, group D part 1 (time-to-sample, chunk and size tables) | M | 1 | M1.2 | WI-1.204 | FR-CNT-002 |
| WI-1.210 | 8 §8.10 | Hand decoders, group D part 2 (sample groups, subsamples, auxiliary info) | M | 1 | M1.2 | WI-1.209 | FR-CNT-002 |
| WI-1.211 | 8 §8.10 | Hand decoders, group E part 1 (movie extends and movie fragments) | M | 1 | M1.2 | WI-1.204 | FR-CNT-002 |
| WI-1.212 | 8 §8.10 | Hand decoders, group E part 2 (random access and segment index boxes) | M | 1 | M1.2 | WI-1.211 | FR-CNT-002 |
| WI-1.213 | 8 §8.10 | Hand decoders, group H (encryption) and `senc` | M | 1 | M1.2 | WI-1.210, WI-1.211 | FR-CNT-002, FR-CNT-008 |
| WI-1.214 | 8 §8.10 | Hand decoders, group F (metadata and HEIF item boxes) | M | 1 | M1.2 | WI-1.208 | FR-CNT-002 |
| WI-1.215 | 8 §8.10 | Hand decoders, group G (user data, iTunes `ilst`, QuickTime `keys`, `©xyz`, `chpl`, `kind`) | M | 1 | M1.2 | WI-1.214 | FR-CNT-002 |
| WI-1.216 | 8 §8.10 | Hand decoders, groups K and M (`uuid` boxes and QuickTime media boxes) | M | 1 | M1.2 | WI-1.213, WI-1.206 | FR-CNT-002 |
| WI-1.217 | 8 §8.10 | Build `TrackInfo` from the parsed `moov` | S | 1 | M1.3 | WI-1.205, WI-1.206, WI-1.207, WI-1.208, WI-1.209, WI-1.210, WI-1.211, WI-1.212, WI-1.213, WI-1.214, WI-1.215, WI-1.216 | FR-CNT-005, FR-GEN-005 |
| WI-1.218 | 8 §8.10 | Implement `SampleTable` expansion for `moov` tables | M | 1 | M1.3 | WI-1.217 | FR-CNT-006 |
| WI-1.219 | 8 §8.10 | Implement `FragmentTimeline` and fragment sample appending | L | 1 | M1.3 | WI-1.218, WI-1.211, WI-1.212 | FR-CNT-006 |
| WI-1.220 | 8 §8.10 | Implement `EditList` and the `elst ↔ tfdt` sanity check | S | 1 | M1.3 | WI-1.219 | — |
| WI-1.221 | 8 §8.10 | Implement the CENC per-sample encryption map and hex shading ranges | M | 1 | M1.3 | WI-1.213, WI-1.219 | FR-CNT-008 |
| WI-1.222 | 8 §8.10 | Implement brand and profile checks | M | 1 | M1.3 | WI-1.220, WI-1.221 | — |
| WI-1.223 | 8 §8.10 | Implement `HdrMetadata` (box-level + bitstream probe) | M | 1 | M1.3 | WI-1.208, WI-1.218, WI-1.231, WI-1.110, WI-1.114, WI-1.116 | FR-CNT-102, FR-HDR-001 |
| WI-1.224 | 8 §8.10 | Implement the validation driver, diagnostics table and broken-corpus coverage test | M | 1 | M1.3 | WI-1.218, WI-1.219, WI-1.220, WI-1.221, WI-1.222, WI-1.223 | FR-CNT-009, FR-CNT-101, FR-GEN-006, NFR-013, NFR-036 |
| WI-1.225 | 8 §8.10 | Implement the IVF reader | S | 1 | M1.1 | WI-1.200 | FR-CNT-010 |
| WI-1.226 | 8 §8.10 | Implement the Annex B start-code splitter | S | 1 | M1.1 | WI-1.200 | FR-CNT-011 |
| WI-1.227 | 8 §8.10 | Implement raw AV1 OBU stream detection and reader (Section 5 and Annex B) | S | 1 | M1.1 | WI-1.200 | FR-CNT-012 |
| WI-1.228 | 8 §8.10 | Implement the Y4M reader | S | 1 | M1.1 | WI-1.200 | FR-CNT-013 |
| WI-1.229 | 8 §8.10 | Implement raw YUV opening: format grammar, heuristics, sidecar and C API | S | 1 | M1.1 | WI-1.228 | FR-CNT-014, FR-CNT-105, FR-PLN-008 |
| WI-1.230 | 8 §8.10 | Implement the libavformat `Demuxer` with the custom `AVIOContext` | M | 1 | M1.2 | WI-1.200, WI-0.012 | — |
| WI-1.231 | 8 §8.10 | Implement the unit splitter (length-prefixed, Annex B, AV1 OBU) | S | 1 | M1.1 | WI-1.226, WI-1.227 | FR-CNT-007 |
| WI-1.232 | 8 §8.10 | Implement track ↔ stream matching, `sample_index` mapping and `seek_to_sample` | S | 1 | M1.3 | WI-1.230, WI-1.231, WI-1.218, WI-1.219 | FR-CNT-103, FR-GEN-005 |
| WI-1.233 | 8 §8.10 | Extract `codec_private` for plugin `create()` and integrate with the plugin host | S | 1 | M1.3 | WI-1.232, WI-2.150 | — |
| WI-1.234 | 8 §8.10 | Implement the byte service: page cache, `ovt_container_bytes`, `ovt_container_box_at`, `ovt_stream_unit_bytes` | S | 1 | M1.3 | WI-1.204, WI-1.231 | FR-CNT-104 |
| WI-1.235 | 8 §8.10 | Add `ovt probe` container subcommand hooks (reduced, §16.3) | S | 1 | M1.3 | WI-1.224, WI-1.232, WI-1.234, WI-1.500 | — |
| WI-1.236 | 8 §8.10 | Build the golden-test harness and oracle normalisers (reduced, §16.3) | M | 1 | M1.4 | WI-1.235 | — |
| WI-1.237 | 8 §8.10 | Write corpus manifests, fetcher integration and licence tags (reduced, §16.3) | S | 1 | M1.1 | WI-0.551, WI-0.552 | — |
| WI-1.238 | 8 §8.10 | Add fuzz targets for every container parser and generated per-decoder fuzzers (reduced, §16.3) | M | 1 | M1.4 | WI-1.224, WI-1.229, WI-1.231, WI-1.230 | FR-CNT-106 |
| WI-1.239 | 8 §8.10 | Add performance tests and the perf corpus generator (reduced, §16.3) | S | 1 | M1.4 | WI-1.218, WI-1.230 | FR-CNT-018 |
| WI-1.400 | 10 §10.11 | Create `libovt-render` skeleton and libplacebo build integration (reduced, §16.3) | M | 1 | M1.1 | WI-0.001, WI-0.015, WI-0.017 | — |
| WI-1.401 | 10 §10.11 | Implement `Picture` upload and `pl_frame` construction | M | 1 | M1.1 | WI-1.400, WI-1.413 | FR-PLN-002 |
| WI-1.402 | 10 §10.11 | H.273 → libplacebo colour mapping | S | 1 | M1.1 | WI-1.400 | FR-HDR-001, FR-HDR-005, FR-MAC-009 |
| WI-1.403 | 10 §10.11 | Target frames, tone/gamut mapping, dithering options | M | 1 | M1.2 | WI-1.401, WI-1.402 | FR-HDR-003, FR-HDR-004, FR-HDR-008, FR-PLN-101 |
| WI-1.404 | 10 §10.11 | Offscreen interop export (Metal texture, shared Vulkan device, GL texture) | L | 1 | M1.2 | WI-1.403 | FR-GUI-030 |
| WI-1.405 | 10 §10.11 | Qt `PictureHost` item using the offscreen texture (EMBEDDED mode) | M | 1 | M1.3 | WI-1.404, WI-1.450 | FR-GUI-030, FR-LNX-006 |
| WI-1.406 | 10 §10.11 | macOS EDR native target (MoltenVK swapchain on CAMetalLayer) | L | 1 | M1.2 | WI-1.403 | FR-HDR-003, FR-MAC-002, FR-MAC-003, FR-MAC-009 |
| WI-1.407 | 10 §10.11 | Display capability probe (macOS first, with Linux/Android stubs) | S | 1 | M1.2 | WI-1.406 | — |
| WI-1.408 | 10 §10.11 | Plane modes Y/U/V/gray and raw code values | M | 1 | M1.2 | WI-1.403 | FR-PLN-001, FR-PLN-005 |
| WI-1.409 | 10 §10.11 | Zoom, pan, nearest-neighbour magnification and sample-value text | S | 1 | M1.3 | WI-1.408 | FR-GUI-007, FR-PLN-003 |
| WI-1.410 | 10 §10.11 | Compare modes (SideBySide, Split, Overlay, Flicker, Difference) | L | 1 | M1.3 | WI-1.403 | FR-PLN-007 |
| WI-1.411 | 10 §10.11 | HDR static metadata signalling and `pl_swapchain_colorspace_hint` | S | 1 | M1.2 | WI-1.406 | FR-HDR-011, FR-MAC-003 |
| WI-1.412 | 10 §10.11 | PNG export (`HdrPng16`, `SdrPng8`, `ViewPng8`) | M | 1 | M1.3 | WI-1.403, WI-1.410 | FR-PLN-010 |
| WI-1.413 | 10 §10.11 | Test pattern generator and measurement expectations | S | 1 | M1.1 | none | — |
| WI-1.414 | 10 §10.11 | libplacebo/mpv parity test harness (superseded by WI-1.575; see chapter 16 §16.3) | S | 1 | M1.4 | WI-1.412, WI-1.413 | — |
| WI-1.415 | 10 §10.11 | Render benchmarks (`bench_render_*`) and GPU timers (reduced, §16.3) | S | 1 | M1.3 | WI-1.403, WI-0.560 | — |
| WI-1.450 | 11 §11.10 | Application skeleton, RHI selection, `Platform` singleton, settings (reduced, §16.3) | M | 1 | M1.1 | WI-0.001, WI-0.018, WI-0.019 | FR-GUI-001, FR-GUI-002, FR-GUI-023, FR-GUI-025, NFR-008, NFR-021, NFR-025 |
| WI-1.451 | 11 §11.10 | Dock manager (`SplitView`-based) with presets and serialisation | L | 1 | M1.2 | WI-1.450 | FR-GUI-001, FR-GUI-002 |
| WI-1.452 | 11 §11.10 | `SelectionModel` and the derivation rules | M | 1 | M1.2 | WI-1.450, WI-0.083 | FR-GUI-004, FR-GUI-011, FR-GUI-101 |
| WI-1.453 | 11 §11.10 | Jobs model, progress UX and diagnostics model | M | 1 | M1.2 | WI-1.450, WI-1.512 | FR-GEN-006, FR-GUI-012, FR-GUI-022, FR-GUI-026 |
| WI-1.454 | 11 §11.10 | File open flow, project model and `.ovtproj` | M | 1 | M1.3 | WI-1.452, WI-1.217, WI-1.121 | FR-GUI-027, FR-MAC-008, FR-PLN-008 |
| WI-1.455 | 11 §11.10 | Timeline panel | L | 1 | M1.3 | WI-1.452, WI-1.453 | FR-DEC-009, FR-GUI-003, FR-GUI-004, FR-GUI-005, FR-GUI-017, FR-GUI-020 |
| WI-1.456 | 11 §11.10 | Picture panel (EMBEDDED mode): zoom/pan, planes, probe, selection rect | L | 1 | M1.3 | WI-1.405, WI-1.452 | FR-DEC-009, FR-DEC-014, FR-GUI-007, FR-GUI-009, FR-HDR-007, FR-PLN-004, FR-PLN-006 |
| WI-1.457 | 11 §11.10 | Syntax tree panel | M | 1 | M1.3 | WI-1.452, WI-1.130, WI-1.108, WI-1.112, WI-1.116 | FR-GUI-012, FR-GUI-013, FR-GUI-102, FR-SYN-015, FR-SYN-017 |
| WI-1.458 | 11 §11.10 | Hex view panel | M | 1 | M1.3 | WI-1.452, WI-1.101, WI-1.234 | FR-GUI-014, FR-SYN-014 |
| WI-1.459 | 11 §11.10 | Unit list panel | S | 1 | M1.3 | WI-1.455 | — |
| WI-1.460 | 11 §11.10 | Container tree panel with derived tabs | L | 1 | M1.3 | WI-1.452, WI-1.218, WI-1.219, WI-1.220, WI-1.221, WI-1.223 | FR-GUI-013, FR-GUI-015 |
| WI-1.461 | 11 §11.10 | Stream Info panel | S | 1 | M1.4 | WI-1.455, WI-1.460 | FR-BLK-021 |
| WI-1.462 | 11 §11.10 | HDR panel skeleton (Metadata + Display tabs) and Display Capabilities panel | M | 1 | M1.4 | WI-1.456, WI-1.407, WI-1.411 | FR-GUI-016, FR-HDR-002, FR-HDR-005 |
| WI-1.463 | 11 §11.10 | HDR output toggle and NATIVE-mode host (desktop) (reduced, §16.3) | M | 1 | M1.4 | WI-1.406, WI-1.456 | FR-HDR-007, FR-HDR-101 |
| WI-1.464 | 11 §11.10 | Welcome page, empty states, first-run banners | S | 1 | M1.4 | WI-1.454 | — |
| WI-1.465 | 11 §11.10 | Interaction script runner, screenshot harness and CI wiring (reduced, §16.3) | M | 1 | M1.4 | WI-1.456, WI-0.568 | — |
| WI-1.466 | 11 §11.10 | Keyboard map, Go to dialog, Preferences dialog | S | 1 | M1.1 | WI-1.450 | FR-GUI-006, FR-GUI-008, FR-GUI-023, FR-GUI-024, NFR-023, NFR-035 |
| WI-1.500 | 12 §12.6 | CLI skeleton with CLI11, global options, exit codes, envelope writers | M | 1 | M1.1 | WI-0.000, WI-0.083 | FR-CLI-001, FR-CLI-003, FR-CLI-010, FR-CLI-011, FR-CLI-012, FR-CLI-015, FR-CLI-101, FR-CLI-102, FR-CLI-103, FR-CLI-105, FR-CLI-106, FR-CLI-107, FR-CLI-108, FR-CLI-109, FR-GEN-006, FR-LNX-007, NFR-025, NFR-035 |
| WI-1.501 | 12 §12.6 | JSON Schema generation from FlatBuffers schemas | S | 1 | M1.1 | WI-1.500 | FR-CLI-002, NFR-027 |
| WI-1.502 | 12 §12.6 | `ovt info` | S | 1 | M1.3 | WI-1.500, WI-1.217, WI-1.121, WI-1.223 | — |
| WI-1.503 | 12 §12.6 | `ovt probe` | M | 1 | M1.3 | WI-1.502, WI-1.204, WI-1.224, WI-1.235 | FR-CLI-004 |
| WI-1.504 | 12 §12.6 | `ovt syntax` with `--trace` | M | 1 | M1.3 | WI-1.500, WI-1.106, WI-1.130, WI-1.121 | FR-CLI-005 |
| WI-1.505 | 12 §12.6 | `ovt index` | S | 1 | M1.3 | WI-1.500, WI-1.121, WI-1.125, WI-0.091 | FR-CLI-006 |
| WI-1.506 | 12 §12.6 | `ovt frames` | S | 1 | M1.3 | WI-1.505 | — |
| WI-1.507 | 12 §12.6 | `ovt dump` | M | 1 | M1.3 | WI-1.505, WI-2.171 | FR-CLI-007, FR-DEC-008 |
| WI-1.508 | 12 §12.6 | `ovt extract` | M | 1 | M1.3 | WI-1.505 | FR-CLI-013, FR-SYN-018 |
| WI-1.509 | 12 §12.6 | `ovt cache` | S | 1 | M1.3 | WI-1.500, WI-0.091, WI-0.092, WI-0.088 | — |
| WI-1.510 | 12 §12.6 | CLI golden-test harness and determinism check | S | 1 | M1.4 | WI-1.502, WI-1.509, WI-1.503, WI-1.504, WI-1.505, WI-1.506, WI-1.507, WI-1.508 | FR-CLI-004, FR-CLI-104, FR-GEN-009, NFR-017 |
| WI-1.511 | 12 §12.6 | C API headers, visibility, version script and `api_example.c` | M | 1 | M1.4 | WI-1.130, WI-1.224, WI-1.234, WI-1.512 | FR-API-001, FR-API-002, FR-API-003, FR-API-005, FR-API-009, FR-API-010, FR-GEN-001, NFR-011, NFR-027 |
| WI-1.512 | 12 §12.6 | `ovt_Job` implementation and cancellation plumbing (reduced, §16.3) | S | 0 (numbered 1) | M0.2 | WI-0.000 | FR-API-004, FR-API-006, FR-CLI-011, NFR-007, NFR-011 |
| WI-1.570 | 14 §14.12 | Golden suite: BitReader and splitters | M | 1 | M1.3 | WI-0.556, WI-0.557, WI-1.107, WI-1.226, WI-1.227, WI-1.231 | FR-SYN-001 |
| WI-1.571 | 14 §14.12 | Golden suite: ISOBMFF reader | L | 1 | M1.4 | WI-0.564, WI-1.221, WI-1.224 | FR-CNT-003 |
| WI-1.572 | 14 §14.12 | Golden suite: H.264/HEVC/AV1 header parsers | L | 1 | M1.3 | WI-0.567, WI-0.566, WI-1.109, WI-1.110, WI-1.113, WI-1.114, WI-1.118, WI-1.120 | FR-SYN-007, FR-SYN-008 |
| WI-1.573 | 14 §14.12 | Golden suite: StreamIndex | M | 1 | M1.3 | WI-1.572, WI-1.121, WI-1.122, WI-1.123, WI-1.124 | — |
| WI-1.574 | 14 §14.12 | Decoded-picture MD5 suite and HW/SW tests | M | 1 | M1.3 | WI-2.171, WI-0.552 | FR-DEC-007 |
| WI-1.575 | 14 §14.12 | Render parity suite (mpv/libplacebo) and `measure()` tests | M | 1 | M1.4 | WI-1.403, WI-1.412, WI-1.413, WI-0.568 | FR-PLN-101 |
| WI-1.576 | 14 §14.12 | GUI model tests, screenshots and interaction scripts (phase 1 scope) (reduced, §16.3) | L | 1 | M1.4 | WI-0.568, WI-1.465, WI-1.455, WI-1.457, WI-1.458, WI-1.460, WI-1.462 | FR-GUI-101 |
| WI-1.577 | 14 §14.12 | Android instrumentation smoke (phase 1) (reduced, §16.3) | M | A (numbered 1) | MA.1 | WI-0.050, WI-0.051, WI-A.452, WI-A.454, WI-0.553 | FR-AND-013, FR-AND-014, FR-GUI-021 |
| WI-2.150 | 7 §7.8 | Plugin discovery, loading and ABI check (reduced, §16.3) | M | 1 (numbered 2) | M1.1 | WI-0.000, WI-1.100 | FR-BLK-001, FR-GEN-007 |
| WI-2.151 | 7 §7.8 | GOP-local analysis driver (`ovt_analysis_open/frame/plane/close`) | L | 2 | M2.1 | WI-2.150, WI-1.121, WI-1.124, WI-0.091 | FR-BLK-016, FR-DEC-010, NFR-004, NFR-014 |
| WI-2.152 | 7 §7.8 | Cache integration for `FrameAnalysis` and planes | M | 2 | M2.1 | WI-2.151, WI-0.091, WI-0.092 | FR-DEC-010, NFR-014 |
| WI-2.153 | 7 §7.8 | Plugin conformance harness (reduced, §16.3) | M | 2 | M2.1 | WI-2.151 | FR-BLK-001 |
| WI-2.154 | 7 §7.8 | `third_party/libaom.cmake` with inspection build (reduced, §16.3) | S | 2 | M2.1 | WI-0.013, WI-0.010 | — |
| WI-2.155 | 7 §7.8 | `ovt-plugin-aom` skeleton: ABI glue, temporal-unit batching, callback plumbing | M | 2 | M2.1 | WI-2.154, WI-2.150 | — |
| WI-2.156 | 7 §7.8 | `ovt-plugin-aom`: `insp_mi_data` → CellGrid / Block tree / MotionVector / RefSlot | L | 2 | M2.1 | WI-2.155 | FR-BLK-006, FR-BLK-007, FR-BLK-011, FR-BLK-012, FR-BLK-013 |
| WI-2.157 | 7 §7.8 | `ovt-plugin-aom`: accounting → `SymbolCost` and `bits_x8` | M | 2 | M2.1 | WI-2.156 | FR-BLK-008, FR-BLK-015 |
| WI-2.158 | 7 §7.8 | `ovt-plugin-aom`: planes (reconstruction, prediction, pre-filter) → `PlaneRef` | M | 2 | M2.1 | WI-2.155 | FR-BLK-014 |
| WI-2.159 | 7 §7.8 | `ovt-plugin-aom`: extension patch `0001-inspection-extend-mi-data` | L | 2 | M2.2 | WI-2.156, WI-2.158 | FR-BLK-006, NFR-026 |
| WI-2.160 | 7 §7.8 | Upstream the libaom inspection extension | S | 2 | M2.2 | WI-2.159 | — |
| WI-2.161 | 7 §7.8 | `ovt-plugin-aom` golden tests vs `inspect` JSON (superseded by WI-2.581) | M | 2 | M2.2 | WI-2.157, WI-2.154 | — |
| WI-2.162 | 7 §7.8 | `third_party/libde265.cmake` and patch 0001 (internals API) (reduced, §16.3) | L | 2 | M2.1 | WI-0.014, WI-0.010 | NFR-026 |
| WI-2.163 | 7 §7.8 | `ovt-plugin-de265` skeleton: ABI glue, push/decode/pull with decode-index tracking | M | 2 | M2.1 | WI-2.162, WI-2.150 | — |
| WI-2.164 | 7 §7.8 | `ovt-plugin-de265`: CTB/CB/PB/TU/intra/deblock/SAO → model | L | 2 | M2.2 | WI-2.163 | FR-BLK-004, FR-BLK-005, FR-BLK-011, FR-BLK-012, FR-BLK-013 |
| WI-2.165 | 7 §7.8 | `ovt-plugin-de265`: patch 0002 (persist PU/TU syntax, CU bit positions) | L | 2 | M2.2 | WI-2.164 | FR-BLK-004, FR-BLK-015 |
| WI-2.166 | 7 §7.8 | `ovt-plugin-de265`: patch 0003 (pre-deblock / pre-SAO snapshots, prediction, residual, coefficients) | L | 2 | M2.2 | WI-2.163 | FR-BLK-014, FR-BLK-023 |
| WI-2.167 | 7 §7.8 | Upstream libde265 patch 0001 (PR) and track | S | 2 | M2.2 | WI-2.162 | — |
| WI-2.168 | 7 §7.8 | `ovt-plugin-de265`: Android/arm64 performance measurement and mitigations | M | A (numbered 2) | MA.2 | WI-2.164 | FR-AND-014 |
| WI-2.169 | 7 §7.8 | `third_party/ffmpeg.cmake` and `ovt-plugin-ffmpeg` analysis skeleton (H.264) (reduced, §16.3) | M | 1 (numbered 2) | M1.2 | WI-0.012, WI-2.150 | FR-BLK-009 |
| WI-2.170 | 7 §7.8 | `ovt-plugin-ffmpeg`: AVMotionVector / AVVideoEncParams → model, n/a handling | M | 2 | M2.1 | WI-2.169 | FR-BLK-009, FR-BLK-011, FR-BLK-012, FR-BLK-013 |
| WI-2.171 | 7 §7.8 | `ovt-plugin-ffmpeg`: playback decoders with hwaccel selection and software fallback (reduced, §16.3) | L | 1 (numbered 2) | M1.3 | WI-2.169, WI-1.230, WI-1.232, WI-1.121 | FR-AND-015, FR-DEC-006, FR-DEC-007, FR-DEC-008, FR-DEC-014, FR-LNX-003, FR-MAC-004, NFR-002, NFR-006 |
| WI-2.172 | 7 §7.8 | `ovt-plugin-ffmpeg`: fill `ovt_ColorDescription` from context and side data (reduced, §16.3) | M | 1 (numbered 2) | M1.3 | WI-2.171 | FR-HDR-001 |
| WI-2.173 | 7 §7.8 | `ovt-plugin-ffmpeg` golden tests vs ffprobe (superseded by WI-2.583) | S | 2 | M2.2 | WI-2.170 | — |
| WI-2.174 | 7 §7.8 | CLI hooks: `ovt analyze` and `ovt plugins` (superseded by WI-2.520, WI-2.521) | M | 2 | M2.2 | WI-2.151, WI-2.152 | — |
| WI-2.430 | 10 §10.11 | `OverlayBuilder` CPU side: cell attribute, heatmap and partition data | M | 2 | M2.1 | WI-1.401, WI-0.083 | FR-BLK-017 |
| WI-2.431 | 10 §10.11 | Overlay layer, LUTs and the heatmap/fill/hatch passes | M | 2 | M2.1 | WI-2.430, WI-1.403 | FR-HDR-014 |
| WI-2.432 | 10 §10.11 | Grid line pass with viewport/level culling | M | 2 | M2.1 | WI-2.431 | — |
| WI-2.433 | 10 §10.11 | SDF glyph atlas and label pass | M | 2 | M2.1 | WI-2.431 | FR-PLN-003 |
| WI-2.434 | 10 §10.11 | MV arrow pass (vertex pulling) and intra direction strokes | L | 2 | M2.2 | WI-2.431 | FR-BLK-018 |
| WI-2.435 | 10 §10.11 | Id buffer and `pick()` | M | 2 | M2.2 | WI-2.434 | FR-PLN-002 |
| WI-2.436 | 10 §10.11 | Deblocking-edge, ref-index fill and selection/probe passes | S | 2 | M2.2 | WI-2.432 | — |
| WI-2.437 | 10 §10.11 | GPU overlay cache and `set_overlays` lifecycle | S | 2 | M2.2 | WI-2.434 | FR-BLK-017, FR-BLK-018 |
| WI-2.438 | 10 §10.11 | Overlay performance benchmarks (`bench_overlay_*`) and budget enforcement (reduced, §16.3) | S | 2 | M2.3 | WI-2.434, WI-2.433, WI-2.432, WI-1.415 | FR-BLK-018, NFR-003 |
| WI-2.439 | 10 §10.11 | Colour-blind-safe overlay palette and legend data export | S | 2 | M2.2 | WI-2.431 | FR-GUI-019, FR-GUI-020, NFR-023, NFR-024 |
| WI-2.480 | 11 §11.10 | Analysis orchestration and `FrameAnalysisStore` | M | 2 | M2.1 | WI-2.150, WI-2.151, WI-2.152, WI-1.453 | FR-GUI-029, FR-PLN-005 |
| WI-2.481 | 11 §11.10 | Overlay modes F1–F12, toggles, Legend panel | M | 2 | M2.2 | WI-2.480, WI-2.431, WI-2.432, WI-2.433, WI-2.434, WI-2.435, WI-2.436, WI-2.439 | FR-GUI-019 |
| WI-2.482 | 11 §11.10 | Selection Info panel with reflection-based extension display and level cycling | M | 2 | M2.2 | WI-2.480, WI-0.083 | FR-BLK-019, FR-GUI-009, FR-GUI-011, FR-GUI-102 |
| WI-2.483 | 11 §11.10 | Block Samples panel | M | 2 | M2.3 | WI-2.482, WI-2.523, WI-2.158, WI-2.166 | FR-BLK-023, FR-GUI-010 |
| WI-2.484 | 11 §11.10 | Reference / GOP graph panel | M | 2 | M2.3 | WI-1.455 | FR-DEC-011, FR-GUI-018 |
| WI-2.485 | 11 §11.10 | Statistics panel (Qt Graphs 2D) | M | 2 | M2.3 | WI-2.480 | FR-BLK-020, FR-BLK-021, FR-GUI-031 |
| WI-2.486 | 11 §11.10 | Compare modes UI and B-side selection | M | 2 | M2.3 | WI-1.410, WI-1.456 | FR-PLN-009 |
| WI-2.487 | 11 §11.10 | Bookmarks & Comments panel | S | 2 | M2.3 | WI-1.454, WI-1.455 | FR-GUI-103 |
| WI-2.488 | 11 §11.10 | Export menu (PNG/PGM/Y4M/CSV/JSON/unit bytes) and Copy-as-JSON everywhere | S | 2 | M2.3 | WI-1.412, WI-0.088, WI-0.089, WI-1.508, WI-2.485 | FR-AND-017, FR-GUI-028 |
| WI-2.489 | 11 §11.10 | Dual view (two `VideoSurface` instances, synchronised and unlockable) | M | 2 | M2.3 | WI-2.486, WI-2.481, WI-1.405 | FR-GUI-021 |
| WI-2.520 | 12 §12.6 | `ovt plugins` and plugin search-path implementation | M | 2 | M2.1 | WI-1.511, WI-2.150 | FR-API-009 |
| WI-2.521 | 12 §12.6 | `ovt analyze` and `ovt dump` non-recon stages | L | 2 | M2.2 | WI-2.520, WI-1.507, WI-2.151, WI-2.523, WI-0.089, WI-0.090 | FR-BLK-022, FR-BLK-024, FR-CLI-007, FR-CLI-008 |
| WI-2.522 | 12 §12.6 | "Copy as command" in the GUI | S | 2 | M2.3 | WI-2.481, WI-2.482, WI-2.521 | — |
| WI-2.523 | 12 §12.6 | `ovt_analysis_range` callback API and LRU plane store exposure | S | 2 | M2.1 | WI-2.151, WI-2.152 | — |
| WI-2.580 | 14 §14.12 | Validator implementation and rule tests (superseded by WI-0.086) | M | 2 | M2.2 | WI-0.086, WI-2.151 | — |
| WI-2.581 | 14 §14.12 | Golden suite: `ovt-plugin-aom` vs `inspect` | M | 2 | M2.2 | WI-0.566, WI-0.086, WI-2.157 | — |
| WI-2.582 | 14 §14.12 | Golden suite: `ovt-plugin-de265` vs HM grid | M | 2 | M2.3 | WI-0.567, WI-0.563, WI-0.086, WI-2.164 | — |
| WI-2.583 | 14 §14.12 | Golden suite: `ovt-plugin-ffmpeg` (H.264 MVs/QP) vs `ffmpeg_mvs` and JM QP | M | 2 | M2.2 | WI-0.567, WI-0.086, WI-2.170 | — |
| WI-2.584 | 14 §14.12 | Overlay performance and GUI tests (phase 2 scope) | M | 2 | M2.3 | WI-0.560, WI-1.576, WI-2.438, WI-2.481, WI-2.482, WI-2.485 | — |
| WI-3.300 | 9 §9.12 | Create the metrics schema, C API skeleton and plugin ABI header | M | 3 | M3.1 | WI-0.082, WI-0.083, WI-1.512 | FR-API-010, FR-VQ-001 |
| WI-3.301 | 9 §9.12 | Implement the normalisation pipeline (crop, bit depth, range, pairing) without zimg | M | 3 | M3.1 | WI-3.300, WI-2.171 | FR-VQ-002, FR-VQ-004, FR-VQ-024 |
| WI-3.302 | 9 §9.12 | Integrate zimg for scaling, chroma upsampling, range and colour conversion | M | 3 | M3.1 | WI-3.301 | FR-VQ-024 |
| WI-3.303 | 9 §9.12 | Implement working-domain conversions (linear_light, pu21, ictcp) and display-model presets | M | 3 | M3.1 | WI-3.302 | FR-HDR-009 |
| WI-3.304 | 9 §9.12 | Implement temporal alignment | M | 3 | M3.1 | WI-3.301 | FR-VQ-003 |
| WI-3.305 | 9 §9.12 | Implement spatial alignment and crop detection | S | 3 | M3.1 | WI-3.304 | FR-VQ-002 |
| WI-3.306 | 9 §9.12 | Implement the PSNR kernel with combinations, APSNR and maps | M | 3 | M3.1 | WI-3.300 | FR-VQ-005, FR-VQ-011 |
| WI-3.307 | 9 §9.12 | Implement the Gaussian SSIM kernel | M | 3 | M3.1 | WI-3.306 | FR-VQ-006 |
| WI-3.308 | 9 §9.12 | Implement the x264 4×4 SSIM, identity and the kernel plugin registration | S | 3 | M3.1 | WI-3.306 | FR-PLN-009, FR-VQ-006, FR-VQ-023 |
| WI-3.309 | 9 §9.12 | Build libvmaf via ExternalProject and expose the libvmaf host plugins (reduced, §16.3) | L | 3 | M3.2 | WI-3.300, WI-0.015, WI-0.011 | FR-VQ-007, FR-VQ-008 |
| WI-3.310 | 9 §9.12 | Bit-exact parity test against the `vmaf` CLI and libvmaf-schema JSON export (reduced, §16.3) | M | 3 | M3.2 | WI-3.309, WI-3.306, WI-3.307, WI-3.530 | FR-VQ-007, FR-VQ-014, NFR-018 |
| WI-3.311 | 9 §9.12 | Implement pooling, scene detection and frame-type grouping | M | 3 | M3.2 | WI-3.300, WI-3.306, WI-1.121 | FR-VQ-012, FR-VQ-021 |
| WI-3.312 | 9 §9.12 | Implement the metrics exporters and the `JobSpec` JSON parser (core side of `ovt metrics`) | M | 3 | M3.2 | WI-3.301, WI-3.302, WI-3.303, WI-3.304, WI-3.305, WI-3.306, WI-3.307, WI-3.308, WI-3.309, WI-3.311 | FR-VQ-014 |
| WI-3.313 | 9 §9.12 | Implement BD-rate / BD-metric with three interpolators and `ovt bdrate` (reduced, §16.3) | M | 3 | M3.2 | WI-3.312 | FR-VQ-017 |
| WI-3.314 | 9 §9.12 | UVQ model export tool and preprocessing document | L | 3 | M3.1 | none | FR-VQ-009 |
| WI-3.315 | 9 §9.12 | UVQ host plugin on ONNX Runtime | L | 3 | M3.2 | WI-3.314, WI-3.303, WI-0.017, WI-1.217 | FR-VQ-009, FR-VQ-010, NFR-021 |
| WI-3.316 | 9 §9.12 | Map store, FlatBuffers export and C API for GUI | M | 3 | M3.2 | WI-3.306, WI-3.312 | FR-VQ-010, FR-VQ-011, FR-VQ-022 |
| WI-3.317 | 9 §9.12 | Performance test suite and Android build of the metrics module | M | A (numbered 3) | MA.3 | WI-3.306, WI-0.560, WI-3.307, WI-3.308, WI-3.309, WI-3.310, WI-3.311, WI-3.312, WI-3.313, WI-3.314, WI-3.315 | FR-VQ-015, NFR-005 |
| WI-3.318 | 9 §9.12 | Example third-party metric plugin and plugin developer guide | S | 3 | M3.2 | WI-3.300, WI-3.312 | FR-VQ-001, FR-VQ-002 |
| WI-3.319 | 9 §9.12 | Documentation of metric differences and About-dialog notices | S | 3 | M3.3 | WI-3.307, WI-3.309, WI-3.315 | — |
| WI-3.450 | 11 §11.10 | Metrics job dialog and results model | L | 3 | M3.3 | WI-3.316, WI-3.300, WI-1.455 | — |
| WI-3.451 | 11 §11.10 | Metrics plot linked to the timeline and heatmap overlay | M | 3 | M3.3 | WI-3.450, WI-2.481 | FR-VQ-016, FR-VQ-022 |
| WI-3.452 | 11 §11.10 | BD-rate tab and run sets | S | 3 | M3.3 | WI-3.450, WI-3.313 | FR-VQ-018 |
| WI-3.530 | 12 §12.6 | `ovt metrics` and `ovt compare` | L | 3 | M3.2 | WI-1.507, WI-3.301, WI-3.302, WI-3.309, WI-3.311, WI-3.312, WI-3.316, WI-1.500 | FR-CLI-009, FR-CLI-016 |
| WI-3.531 | 12 §12.6 | `ovt bdrate` and `ovt_bdrate` (reduced, §16.3) | M | 3 | M3.2 | WI-3.530, WI-3.313 | — |
| WI-3.532 | 12 §12.6 | `ovt hdr` (reduced, §16.3) | M | 3 | M3.2 | WI-1.507, WI-1.402, WI-1.223 | FR-HDR-006 |
| WI-3.533 | 12 §12.6 | `ovt_metrics_push` in-memory path | S | 3 | M3.2 | WI-3.530 | — |
| WI-3.590 | 14 §14.12 | Golden suite: PSNR/SSIM kernels vs FFmpeg psnr, the x264 kernel oracle and libvmaf float_ssim | M | 3 | M3.1 | WI-0.565, WI-3.306, WI-3.307, WI-3.308 | NFR-018 |
| WI-3.591 | 14 §14.12 | Golden suite: libvmaf host bit-exact vs `vmaf` CLI | M | 3 | M3.2 | WI-0.565, WI-3.309, WI-3.310, WI-3.530 | FR-VQ-007 |
| WI-3.592 | 14 §14.12 | Golden suite: normalisation stage and alignment | M | 3 | M3.1 | WI-3.301, WI-3.302, WI-3.303, WI-3.304, WI-3.305 | — |
| WI-3.593 | 14 §14.12 | Golden suite: UVQ host and BD-rate | M | 3 | M3.2 | WI-0.565, WI-3.315, WI-3.313 | — |
| WI-3.594 | 14 §14.12 | Metrics GUI tests and Android metric parity | M | A (numbered 3) | MA.3 | WI-1.576, WI-1.577, WI-3.450, WI-3.451, WI-3.452, WI-3.317 | — |
| WI-4.100 | 7 §7.8 | FFmpeg patch part 1: `libavutil` block-params side data type | L | 4 | M4.1 | WI-2.169 | FR-BLK-010 |
| WI-4.101 | 7 §7.8 | FFmpeg patch part 2: `h264` decoder persistence and export | L | 4 | M4.1 | WI-4.100 | FR-BLK-010 |
| WI-4.102 | 7 §7.8 | `ovt-plugin-ffmpeg`: consume block params into `AvcMb` | M | 4 | M4.1 | WI-4.101, WI-2.170 | FR-BLK-010 |
| WI-4.103 | 7 §7.8 | Submit the FFmpeg patch set to ffmpeg-devel | S | 4 | M4.1 | WI-4.101 | — |
| WI-4.104 | 7 §7.8 | `third_party/hm.cmake` (HM-18.0 static libraries) (reduced, §16.3) | M | 4 | M4.1 | WI-0.010 | FR-DEC-012 |
| WI-4.105 | 7 §7.8 | `ovt-plugin-hm`: decoder wrapper and `TComDataCU` mapping | L | 4 | M4.1 | WI-4.104, WI-2.150 | FR-DEC-012 |
| WI-4.106 | 7 §7.8 | Verify mode: plugin-vs-plugin and decoded-picture-hash comparator | M | 4 | M4.1 | WI-4.105, WI-2.164, WI-1.114 | FR-DEC-012 |
| WI-4.107 | 7 §7.8 | Opportunistic FFmpeg HEVC `mvs`/`venc_params` export patch | L | 4 | M4.1 | WI-2.169 | — |
| WI-4.108 | 7 §7.8 | dav1d inspection API proposal (design only, no fork) | S | 4 | M4.1 | WI-2.159 | — |
| WI-4.109 | 7 §7.8 | Decoded-picture-hash and level-limit conformance checks in the index/analysis | S | 4 | M4.1 | WI-1.114, WI-2.164, WI-2.156 | FR-SYN-019, FR-SYN-020 |
| WI-4.110 | 7 §7.8 | H.264 field/MBAFF completeness: field ref lists, PAFF pairing, MBAFF grid mapping | M | 4 | M4.1 | WI-1.122, WI-4.102 | — |
| WI-4.111 | 7 §7.8 | SVC/MVC headers-only completeness and multi-layer indexing | M | 4 | M4.1 | WI-1.109, WI-1.121 | — |
| WI-4.112 | 7 §7.8 | HRD buffer model and `HrdTrace` builder | M | 4 | M4.1 | WI-1.121, WI-1.108, WI-1.112, WI-1.110, WI-1.114 | FR-DEC-013, FR-GUI-032 |
| WI-4.200 | 8 §8.10 | Generate the EBML/Matroska schema tables | S | 4 | M4.2 | WI-1.203 | FR-CNT-015 |
| WI-4.201 | 8 §8.10 | Implement the EBML reader core | M | 4 | M4.2 | WI-4.200 | — |
| WI-4.202 | 8 §8.10 | Build the Matroska model: tracks, colour/HDR, DV block additions, sample table | M | 4 | M4.2 | WI-4.201, WI-1.208, WI-1.223 | — |
| WI-4.203 | 8 §8.10 | Matroska goldens and demux integration | S | 4 | M4.2 | WI-4.202, WI-1.232 | FR-CNT-015 |
| WI-4.204 | 8 §8.10 | Implement MPEG-TS packet sync and packet/adaptation-field decoding | M | 4 | M4.2 | WI-1.200 | FR-CNT-016 |
| WI-4.205 | 8 §8.10 | Implement PSI/SI table parsing (PAT, PMT, CAT, descriptors; DVB SDT/EIT/TDT read-only) | M | 4 | M4.2 | WI-4.204 | — |
| WI-4.206 | 8 §8.10 | Implement PES reassembly, the TS `SampleTable` and the PCR/PTS/DTS timing table | M | 4 | M4.2 | WI-4.205, WI-1.226 | — |
| WI-4.207 | 8 §8.10 | Implement the TR 101 290 priority 1 and 2 check set and report | S | 4 | M4.2 | WI-4.206 | FR-CNT-016 |
| WI-4.208 | 8 §8.10 | Implement the DASH MPD parser and segment URL expansion | M | 4 | M4.2 | WI-1.200 | FR-CNT-017 |
| WI-4.209 | 8 §8.10 | Implement the HLS M3U8 parser (master and media playlists) | S | 4 | M4.2 | WI-4.208 | — |
| WI-4.210 | 8 §8.10 | Implement `VirtualFile` and segment provenance | M | 4 | M4.2 | WI-4.208, WI-4.209 | FR-CNT-017, NFR-021 |
| WI-4.211 | 8 §8.10 | MXF: libavformat-only placeholder and deferred KLV reader decision record | S | 4 | M4.2 | WI-1.230 | — |
| WI-4.212 | 8 §8.10 | CENC detail: `cbcs` pattern view, PlayReady/Widevine payload decoders in CLI, PIFF parity | S | 4 | M4.2 | WI-1.221, WI-1.216 | — |
| WI-4.213 | 8 §8.10 | HEIF derived items, MIAF checks and item payload view | M | 4 | M4.2 | WI-1.214, WI-1.222 | — |
| WI-4.214 | 8 §8.10 | Phase-4 hand decoders (rows marked phase 4 in §8.3.1–§8.3.10) | M | 4 | M4.2 | WI-1.216 | — |
| WI-4.215 | 8 §8.10 | TS and Matroska demux (Role B) integration for the analysis path | S | 4 | M4.2 | WI-4.202, WI-4.206, WI-1.233 | — |
| WI-4.216 | 8 §8.10 | Container panel exports and Python bindings for container views (reduced, §16.3) | S | 4 | M4.3 | WI-1.235, WI-1.503, WI-4.540 | — |
| WI-4.300 | 9 §9.12 | Port XPSNR as a separately built plugin | M | 4 | M4.4 | WI-3.306, WI-3.318 | FR-VQ-020 |
| WI-4.301 | 9 §9.12 | HDR domain metrics: PU21-PSNR/SSIM and ΔE-ITP with maps | M | 4 | M4.4 | WI-3.303, WI-3.306, WI-3.307 | — |
| WI-4.302 | 9 §9.12 | JVET HDR CTC metrics: wPSNR, PSNR-L100, DE100 | M | 4 | M4.4 | WI-4.301 | — |
| WI-4.303 | 9 §9.12 | SI/TI (P.910 2008 and 2023) | S | 4 | M4.4 | WI-3.303 | FR-VQ-020 |
| WI-4.304 | 9 §9.12 | Sidecar protocol host | L | 4 | M4.4 | WI-3.300, WI-3.303 | FR-VQ-019, NFR-020 |
| WI-4.305 | 9 §9.12 | `ovt-sidecar` Python package with ColorVideoVDP adapter | M | 4 | M4.4 | WI-4.304 | — |
| WI-4.306 | 9 §9.12 | HDR-VDP-3 adapter (Octave) | M | 4 | M4.4 | WI-4.305 | FR-VQ-019 |
| WI-4.307 | 9 §9.12 | CAMBI heatmap and PQ EOTF option | S | 4 | M4.4 | WI-3.309, WI-3.316 | FR-VQ-008 |
| WI-4.308 | 9 §9.12 | Convex hull and encoding-ladder table | S | 4 | M4.4 | WI-3.313 | FR-VQ-101 |
| WI-4.309 | 9 §9.12 | Per-GOP alignment UI hook and manual override API | S | 4 | M4.4 | WI-3.304, WI-3.316 | FR-VQ-003 |
| WI-4.310 | 9 §9.12 | Python bindings for metrics (superseded by WI-4.542) | M | 4 | M4.3 | WI-3.316, WI-4.540 | — |
| WI-4.450 | 10 §10.11 | Linux Vulkan native target over a child `QWindow(VulkanSurface)` | L | 4 | M4.3 | WI-1.406, WI-1.450, WI-1.463 | FR-HDR-003, FR-LNX-002 |
| WI-4.451 | 10 §10.11 | Wayland display-capability probe (`wp_color_manager_v1`) | M | 4 | M4.3 | WI-4.450, WI-1.407 | FR-LNX-002 |
| WI-4.452 | 10 §10.11 | Measurement texture, reductions and MaxCLL/FALL/histogram | L | 4 | M4.3 | WI-1.403, WI-1.413 | FR-HDR-006, FR-PLN-006 |
| WI-4.453 | 10 §10.11 | False colour and gamut-clip views | M | 4 | M4.3 | WI-4.452 | FR-HDR-010 |
| WI-4.454 | 10 §10.11 | Waveform, vectorscope, CIE xy scope textures | M | 4 | M4.3 | WI-4.452, WI-1.404 | FR-HDR-010 |
| WI-4.455 | 10 §10.11 | HDR10+ (ST 2094-40) parsing and dynamic tone mapping | M | 4 | M4.3 | WI-1.403, WI-1.114, WI-1.116, WI-1.223 | FR-HDR-012 |
| WI-4.456 | 10 §10.11 | Dolby Vision RPU reshaping (P5/P8) and UI labelling hook | M | 4 | M4.3 | WI-1.403, WI-2.172 | FR-HDR-013 |
| WI-4.457 | 10 §10.11 | Scope and HDR exports in the CLI (`ovt render`; `ovt hdr --measure` is WI-3.532) (reduced, §16.3) | M | 4 | M4.3 | WI-4.452, WI-1.412, WI-1.500, WI-3.532 | FR-HDR-006 |
| WI-4.458 | 10 §10.11 | Android display-capability probe and headroom listener (reduced, §16.3) | S | A (numbered 4) | MA.1 | WI-A.401, WI-1.407 | FR-AND-003 |
| WI-4.459 | 10 §10.11 | Manual HDR release checklist and documentation | S | 4 | M4.3 | WI-4.450, WI-1.406, WI-0.055 | — |
| WI-4.470 | 11 §11.10 | HRD buffer graph panel (Buffer panel: CPB fullness curve, cursor sync, underflow/overflow marks) | M | 4 | M4.3 | WI-4.112, WI-1.455, WI-2.483 | FR-GUI-032 |
| WI-4.540 | 12 §12.6 | Python package skeleton, pybind11 bindings, wheels (reduced, §16.3) | L | 4 | M4.3 | WI-1.511, WI-1.512 | FR-API-001, FR-API-007, FR-API-008, FR-API-010, FR-API-101, FR-API-103, FR-API-106 |
| WI-4.541 | 12 §12.6 | Python analysis bindings (`FrameAnalysis`, zero-copy grids, planes, YUView export) | M | 4 | M4.3 | WI-4.540, WI-2.523 | FR-API-002, FR-API-102 |
| WI-4.542 | 12 §12.6 | Python metrics and BD-rate bindings | M | 4 | M4.3 | WI-4.540, WI-3.530, WI-3.533, WI-3.531, WI-3.532 | FR-API-004, FR-API-007, FR-API-104 |
| WI-4.543 | 12 §12.6 | `ovt verify` (and the phase-4 `ovt hrd` wrapper) | M | 4 | M4.1 | WI-2.521, WI-4.105, WI-4.106, WI-4.112 | FR-CLI-014, FR-SYN-019, FR-SYN-020 |
| WI-4.544 | 12 §12.6 | `ovt doctor` | M | 4 | M4.3 | WI-2.520, WI-1.407, WI-1.400, WI-2.171 | — |
| WI-4.545 | 12 §12.6 | Python docs, examples CI and typing gate | S | 4 | M4.3 | WI-4.541, WI-4.542 | FR-API-105 |
| WI-4.546 | 12 §12.6 | Optional: Python console panel and "Open in terminal" (MAY) | L | 4 | M4.3 | WI-4.540, WI-1.451 | — |
| WI-4.595 | 14 §14.12 | Golden suite: full H.264 `AvcMb` vs JM trace | M | 4 | M4.1 | WI-2.583, WI-4.102 | — |
| WI-4.596 | 14 §14.12 | Golden suite: `ovt-plugin-hm` and `ovt verify --hash-sei` | M | 4 | M4.1 | WI-0.563, WI-4.105, WI-4.106, WI-0.553 | — |
| WI-4.597 | 14 §14.12 | Golden suite: Matroska, MPEG-TS, DASH/HLS virtual files | L | 4 | M4.2 | WI-4.203, WI-4.207, WI-4.210 | — |
| WI-4.598 | 14 §14.12 | Python package tests, sidecar protocol tests, HDR scopes tests | M | 4 | M4.4 | WI-4.540, WI-4.541, WI-4.542, WI-4.304, WI-4.305, WI-4.453, WI-4.454 | — |
| WI-4.599 | 14 §14.12 | Phase-4 acceptance replay and feature-matrix audit | M | 4 | M4.4 | none | FR-CLI-101, FR-GEN-012 |
| WI-A.400 | 10 §10.11 | HDR `SurfaceView` sibling in the Android shell (reduced, §16.3) | M | A | MA.1 | WI-0.050, WI-0.052, WI-A.450 | FR-AND-002, FR-HDR-101 |
| WI-A.401 | 10 §10.11 | EGL + dataspace native target (`AndroidWindow`) | L | A | MA.1 | WI-1.403, WI-A.400 | FR-AND-002, FR-HDR-003 |
| WI-A.402 | 10 §10.11 | ChromeOS HDR hardware spike (superseded by WI-0.055) | M | A | MA.0 | none | — |
| WI-A.403 | 10 §10.11 | Shared-context GL interop for EMBEDDED mode on Android | M | A | MA.1 | WI-1.404, WI-A.450 | — |
| WI-A.404 | 10 §10.11 | Kotlin display-capability and headroom plumbing (reduced, §16.3) | S | A | MA.1 | WI-A.400, WI-4.458 | FR-AND-003, FR-AND-012 |
| WI-A.405 | 10 §10.11 | Android performance tuning for overlays (GLES) | M | A | MA.2 | WI-2.438, WI-A.403 | — |
| WI-A.406 | 10 §10.11 | `AHardwareBuffer` + Vulkan path (optional, gated) | L | A | MA.2 | WI-A.405 | — |
| WI-A.450 | 11 §11.10 | Gradle project with `QtQuickView` embedding (reduced, §16.3) | M | A | MA.1 | WI-1.450, WI-0.050 | FR-AND-001 |
| WI-A.451 | 11 §11.10 | `Platform` singleton on Android and breakpoint layouts | M | A | MA.1 | WI-A.450, WI-1.451 | FR-AND-008, FR-AND-012 |
| WI-A.452 | 11 §11.10 | Storage Access Framework open/recent/export (reduced, §16.3) | M | A | MA.1 | WI-A.450, WI-1.454, WI-0.051 | FR-AND-005, FR-AND-006, FR-AND-017 |
| WI-A.453 | 11 §11.10 | Touch, hover, right-click and keyboard parity | M | A | MA.1 | WI-A.451, WI-1.456 | FR-AND-009, FR-AND-010, FR-AND-011, FR-GUI-024 |
| WI-A.454 | 11 §11.10 | HDR `SurfaceView` sibling integration in the Picture panel | M | A | MA.1 | WI-A.400, WI-A.401, WI-1.456 | — |
| WI-A.455 | 11 §11.10 | State preservation and lifecycle | S | A | MA.1 | WI-A.450, WI-1.454 | FR-AND-007 |
| WI-A.456 | 11 §11.10 | Play / F-Droid packaging hooks for the GUI (icons, adaptive icon, splash, about/licences) | S | A | MA.1 | WI-A.450, WI-0.603, WI-0.606, WI-0.056 | FR-AND-016, FR-AND-018 |
| WI-A.457 | 11 §11.10 | Android instrumentation CI (emulator) and perf run (superseded by WI-1.577) | S | A | MA.1 | WI-A.452, WI-A.453, WI-A.454, WI-A.455, WI-1.577 | — |

---

## 16.5 Critical path and parallelism

### 16.5.1 Critical path per phase gate

The chains below are the longest dependency chains (by milestone ordinal and WI size) ending at each gate. A WI on a chain is marked `[CP]` in the tracker and is picked first (§16.2.1). Durations are the sum of the WI sizes along the chain with one agent per WI (S = 1 d, M = 3 d, L = 5 d) and are lower bounds.

| Gate | Chain (→ = depends on the previous) | Bound |
|---|---|---|
| Phase 0 | WI-0.000 → WI-0.001 → WI-0.002 → WI-0.010 → WI-0.080 → WI-0.081/0.082 → WI-0.083 → WI-0.084 → WI-0.085 → WI-0.086 → WI-0.092 → WI-0.094 | ≈ 29 d |
| Phase 0 (Android) | WI-0.000 → WI-0.001 → WI-0.002 → WI-0.006 → WI-0.018 → WI-0.019 → WI-0.050 → WI-0.052 → WI-0.055 (hardware) → owner verdict in chapter 17 | ≈ 20 d + device access |
| Phase 0 (CI) | WI-0.002 → WI-0.010 → WI-0.011 → WI-0.012 (L) → WI-0.030 → WI-0.034 → WI-0.036 → WI-0.062 | ≈ 18 d |
| Phase 1 | WI-1.102 → WI-1.106 → WI-1.112 (L) → WI-1.113 (L) → WI-1.115 → WI-1.121 (L) → WI-1.125 → WI-1.130 → WI-1.504 → WI-1.126 → WI-1.572 (L) → gate | ≈ 32 d |
| Phase 1 (container/GUI) | WI-1.200 → WI-1.201 → WI-1.202 → WI-1.203 → WI-1.204 → WI-1.209 → WI-1.210 → WI-1.213 → WI-1.216 → WI-1.217 → WI-1.218 → WI-1.219 (L) → WI-1.220 → WI-1.221 → WI-1.222 → WI-1.224 → WI-1.503 → WI-1.571 (L) → WI-1.576 (L) | ≈ 50 d — **the phase-1 bottleneck**; split hand-decoder groups across agents (they only share WI-1.202) |
| Phase 1 (playback/HDR) | WI-0.012 (L) → WI-1.230 → WI-1.232 → WI-2.171 (L) → WI-1.507 → WI-1.574; and WI-1.400 → WI-1.401/1.402 → WI-1.403 → WI-1.406 (L) → WI-1.407/1.411 → WI-1.462/1.463 → §14.10.5 manual run on XDR | ≈ 22 d each |
| Phase 2 | WI-2.150 → WI-2.151 (L) → WI-2.152 → WI-2.523 → WI-2.521 (L) → WI-2.581/2.583 → gate; in parallel WI-0.014 → WI-2.162 (L) → WI-2.163 → WI-2.164 (L) → WI-2.165 (L) → WI-2.582 | ≈ 22 d / ≈ 21 d |
| Phase 2 (overlays) | WI-1.401 → WI-2.430 → WI-2.431 → WI-2.434 (L) → WI-2.435 → WI-2.481 → WI-2.482 → WI-2.483 → WI-2.584 | ≈ 24 d |
| Phase 3 | WI-3.300 → WI-3.301 → WI-3.302 → WI-3.303 → WI-3.315 (L) → WI-3.593; and WI-3.300 → WI-3.306 → WI-3.307 → WI-3.309 (L) → WI-3.312 → WI-3.530 (L) → WI-3.310 → WI-3.591 | ≈ 20 d / ≈ 27 d |
| Phase 4 | WI-2.169 → WI-4.100 (L) → WI-4.101 (L) → WI-4.102 → WI-4.110 → WI-4.595; and WI-4.104 → WI-4.105 (L) → WI-4.106 → WI-4.543 → WI-4.596 | ≈ 19 d / ≈ 17 d |
| Phase 4 (containers) | WI-1.203 → WI-4.200 → WI-4.201 → WI-4.202 → WI-4.203; WI-1.200 → WI-4.204 → WI-4.205 → WI-4.206 → WI-4.207; WI-4.208 → WI-4.209 → WI-4.210 → WI-4.597 (L) | ≈ 17 d |
| Phase 4 (Python) | WI-1.511 → WI-4.540 (L) → WI-4.541 → WI-4.545; WI-3.533 → WI-4.542 → WI-4.598 | ≈ 15 d |
| Track A | WI-0.050 (L) → WI-A.450 (L) → WI-A.451 → WI-A.453; WI-0.052 → WI-A.400 → WI-A.401 (L) → WI-4.458 → WI-A.404; WI-A.452/A.454 → WI-1.577 | ≈ 20 d + device access per phase |

### 16.5.2 Work that can start in parallel at the beginning of each phase

Each list names WIs whose dependencies are all satisfied at phase start (or within the first milestone) and that touch disjoint files, so independent agents can take them without merge conflicts. At least eight per phase.

| Phase | Independent starters (≥ 8) |
|---|---|
| 0 (after WI-0.000/0.001/0.002, ≈ day 3) | WI-0.006 (bootstrap), WI-0.007 (hooks), WI-0.008 (install test), WI-0.010 (third_party infra), WI-0.550 (test framework), WI-0.551 (fetcher), WI-0.554 (oracle image), WI-0.600 (licence texts), WI-0.604 (DCO docs), WI-0.608 (plugins position paper), WI-1.100 (`RandomAccessFile`), WI-1.102 (`BitReader`), WI-1.104 (RBSP), WI-1.512 (jobs/log), WI-0.018 (Qt installer), WI-0.559 (clang-tidy set), WI-3.314 (UVQ export tool — Python only, no dependencies; MAY be pulled forward), WI-1.413 (test pattern generator — no dependencies) |
| 1 | WI-1.108 (H.264 PS), WI-1.112 (H.265 PS), WI-1.116 (AV1 OBU/sequence), WI-1.120 (T.35 registry), WI-1.200 (container skeleton), WI-1.225/1.226/1.227/1.228 (IVF, Annex B, OBU, Y4M readers), WI-1.500 (CLI skeleton), WI-1.400 (render skeleton), WI-1.450 (app skeleton), WI-1.466 (keyboard map), WI-2.150 (plugin registry), WI-0.568-dependent WI-1.465 later |
| 2 | WI-2.151 (analysis driver), WI-2.153 (conformance harness), WI-2.154 (libaom hook), WI-2.162 (de265 patch 0001), WI-2.170 (ffmpeg side data), WI-2.430 (overlay CPU side), WI-2.520 (`ovt plugins`), WI-2.480 (analysis orchestration), WI-2.484 (GOP graph — needs only WI-1.455), WI-2.487 (bookmarks), WI-2.439 (palette) |
| 3 | WI-3.300 (schema/API), WI-3.306 (PSNR), WI-3.314 (UVQ export, if not done), WI-3.318 (example plugin after 3.300), WI-3.590 goldens prep, WI-3.304 (alignment after 3.301), WI-3.309 (libvmaf host after 3.300), WI-3.450 (metrics dialog after 3.300/3.316 stubs), WI-3.319 (docs) |
| 4 | WI-4.100 (FFmpeg patch part 1), WI-4.104 (HM build), WI-4.107 (HEVC export patch), WI-4.108 (dav1d proposal), WI-4.111 (SVC/MVC), WI-4.200 (EBML tables), WI-4.204 (TS sync), WI-4.208 (MPD parser), WI-4.211 (MXF record), WI-4.300 (XPSNR), WI-4.303 (SI/TI), WI-4.304 (sidecar host), WI-4.450 (Linux Vulkan target), WI-4.452 (measurement texture), WI-4.540 (Python skeleton) |
| A (phase 0 start, after WI-0.019) | WI-0.050, then WI-0.051, WI-0.052, WI-0.054, WI-0.056, WI-0.034 in parallel; phase 1: WI-A.451, WI-A.452, WI-A.455, WI-A.400, WI-A.403, WI-A.456 |

### 16.5.3 Hardware-bound work items

These WIs have acceptance tests that cannot run on hosted CI. They carry the label `needs-hardware:<id>` and their manual results are recorded per chapter 14 §14.1.2 item 6. Hardware ids follow chapter 14 §14.9.2.

| Hardware | WIs |
|---|---|
| `m1-air-8gb` (M1 MacBook Air, self-hosted runner) | WI-0.037→WI-0.560 (runner registration), WI-0.060, WI-0.061, WI-0.063, WI-1.125 (index budget), WI-1.415, WI-2.438, WI-1.239, WI-2.171 (VideoToolbox), WI-1.406, WI-1.407, WI-3.317, WI-0.012 test 3 |
| macOS XDR / external HDR monitor | WI-1.406, WI-1.411, WI-1.463, WI-1.462, WI-1.575 (HDR target, nightly), WI-4.455, WI-4.456, WI-4.459, §14.10.5 runs at every phase gate |
| `pixel-tablet` (Google Pixel Tablet, SDR panel — D12: SDR acceptance only) | WI-0.051 (3 GB file), WI-0.052 (SDR fallback path), WI-0.055 (control device), WI-0.056, WI-A.401 test 2 (SDR fallback), WI-A.404 (SDR branch), WI-A.405, WI-2.168, WI-3.317, WI-1.577, WI-0.560 (Android runner) |
| `android-hdr` (HDR-capable Android tablet per chapter 14 §14.9.2; the Android HDR acceptance device) | WI-0.052 (dataspace path), WI-A.400, WI-A.401 test 1 (PQ output), WI-A.404 (headroom listener), WI-A.406, WI-4.458, WI-4.459, FR-AND-002/003 acceptance |
| `chromebook-hdr` (HDR-capable Chromebook, ARCVM) | WI-0.054, WI-0.055, WI-A.402→WI-0.055, WI-A.451 (free-form), WI-A.453 (mouse/keyboard), WI-4.459, §14.10.5 ChromeOS column |
| Linux HDR (Wayland compositor with `color-management-v1`, HDR monitor) | WI-4.450, WI-4.451, WI-4.459, WI-0.073 (Vulkan CI setup is hosted, the HDR check is not) |
| Linux discrete GPU (overlay budgets) | WI-2.438, WI-2.584 on `linux-x64-ref` |

Agents without the hardware MAY implement these WIs up to the hardware-dependent acceptance test and hand the PR to an agent with access; the PR stays in `needs-hardware` until the record exists.

---

## 16.6 Phase gates checklist

A gate is green when every automated line is green on the nightly run of the reference machines and every manual line has a dated record under `docs/acceptance/`. Automated lines reference the chapter 14 WI that owns the suite; the owning WI MUST be Done before the gate can be evaluated. "Budget" lines reference §14.9.3 benchmark names run by `tools/perf_run.py` (WI-0.560).

### 16.6.1 Phase 0 gate

| # | Check | Owner WI | Kind |
|---|---|---|---|
| 1 | `ctest --preset pr` green on `macos-arm64-release`, `linux-x64-release` | WI-0.550 | automated |
| 2 | `ctest --preset sanitizers` green (`linux-x64-asan-ubsan`, chapter 13 §13.2.4); nightly `tsan` (`linux-x64-tsan`) green | WI-0.004, WI-0.558, WI-0.031 | automated |
| 3 | `fuzz-smoke` 60 s per target without finding; `ctest -L fuzz-regress` green | WI-0.557, WI-1.105, WI-0.033 | automated |
| 4 | `clang-tidy` PR job and `-Werror` set green | WI-0.559 | automated |
| 5 | `license-gate` green; negative test (fake AGPL row) fails | WI-0.602, WI-0.032, WI-0.561 | automated |
| 6 | `fetch_corpora.py --tags fast` ≤ 300 MiB, exit 0; `--verify-only` exit 0 | WI-0.551, WI-0.552 | automated |
| 7 | Oracle image builds; `oracle-versions` prints every pin | WI-0.554 | automated (nightly) |
| 8 | Golden plumbing: `box-tree` golden of the committed 2 KiB MP4 passes; `fb_normalize` tests green | WI-0.556 | automated |
| 9 | Model: `test_model_roundtrip`, validator table test (32 mutators), cache tests, exporter round-trips green | WI-0.083–0.094 | automated |
| 10 | `io`/`BitReader`/RBSP/`SyntaxBuilder`/unit-scanner unit tests green; `fuzz_bitreader`, `fuzz_rbsp` 60 s clean | WI-1.100–1.107 | automated |
| 11 | `bench_smoke` result from `m1-air-8gb` uploaded; `perf_compare.py` exit 0 | WI-0.560 | automated (nightly) |
| 12 | `android-build` and `android-emulator-smoke` green; APK ≤ 120 MB per ABI | WI-0.034, WI-0.035 | automated |
| 13 | Reproducible APK (`diffoscope` identical except signature) | WI-0.056 | automated (nightly) |
| 14 | DMG notarised; AppImage runs on Ubuntu 22.04 and 24.04 | WI-0.061, WI-0.070 | automated (release workflow) |
| 15 | HDR spike verdict recorded (`docs/platforms/chromeos-hdr.md`, chapter 17 entry) | WI-0.055 | manual, owner sign-off |
| 16 | PQ test pattern visible on Pixel Tablet and Chromebook; logcat `OVT_HDR` line recorded | WI-0.052, WI-0.055 | manual |
| 17 | Branch protection applied; `apply_protection.sh --check` no drift | WI-0.605 | manual (once) |

### 16.6.2 Phase 1 gate

| # | Check | Owner WI | Kind |
|---|---|---|---|
| 1 | `ctest -L golden -R isobmff` green (five oracles, `fast` corpus) | WI-1.571 | automated |
| 2 | `ctest -R syntax_golden` green for `trace_headers` (positions) and `gstparse` (values), three codecs | WI-1.572 | automated |
| 3 | StreamIndex goldens green (frame types, POC/order hints, hidden frames, DPB refs) | WI-1.573 | automated |
| 4 | BitReader/splitter golden suite green | WI-1.570 | automated |
| 5 | Decoded-picture MD5 suite green (SW); HW path green on macOS runner | WI-1.574 | automated |
| 6 | Render parity: SDR p99 ΔE_ITP ≤ 1.0 (lavapipe CI); HDR p99 ≤ 0.5 (macOS nightly), PQ and HLG scenes | WI-1.575 | automated |
| 7 | CLI golden harness and determinism loop green for `info/probe/syntax/index/frames/dump/extract/cache` | WI-1.510 | automated |
| 8 | GUI model tests, 5 screenshot scenes, scripts `open_click_field_hex`, `timeline_scrub`, `hdr_panel` green | WI-1.576, WI-1.465 | automated |
| 9 | Container fuzz targets (incl. splitters, generated per-box) 60 s clean; header-parser fuzz targets clean, coverage ≥ 85 % of `core/src/syntax/**` | WI-1.238, WI-1.129 | automated |
| 10 | Budgets: `bench_index_h264_1h_1080p` ≤ 2 000 ms; `bench_index_h265/av1_10min` ≤ 400 ms; `bench_first_frame_*` ≤ 500 ms (HW) / 800 ms (SW); `bench_container_open_moov_1h` ≤ 150 ms | WI-1.125, WI-1.239, WI-2.171 | automated (nightly) |
| 11 | C API: every exported symbol starts with `ovt_`; `api_example.c` compiles with `-std=c11 -Wall -Werror`; `abi-check` baseline recorded | WI-1.511 | automated |
| 12 | Android instrumentation smoke green on emulator and Pixel Tablet | WI-1.577 | automated (nightly) |
| 13 | §14.10.5 steps 1–4 on XDR MacBook recorded | WI-1.463 | manual |
| 14 | Exit demo §16.1.2 lines 5–10, 13, 14 executed and recorded | — | manual |

### 16.6.3 Phase 2 gate

| # | Check | Owner WI | Kind |
|---|---|---|---|
| 1 | `golden_av1_inspect`: every grid field, `symbols`, `bits_x8` exact; `threads=1` = `threads=4` bytes | WI-2.581 | automated |
| 2 | de265 vs HM grid exact on `jctvc` fast subset | WI-2.582 | automated |
| 3 | ffmpeg plugin MVs (1/16-pel) and per-MB QP vs ffprobe/JM | WI-2.583 | automated |
| 4 | Plugin conformance harness: determinism, 1 000 corrupted streams without crash, cross-plugin consistency | WI-2.153 | automated |
| 5 | Validator: zero violations on every phase-2 golden `FrameAnalysis` | WI-0.086 | automated |
| 6 | Overlay perf: `bench_overlay_4k_mv_arrows` p95 ≤ 16 ms, `bench_overlay_build` ≤ 8 ms; 8K instance texture ≤ 48 MB | WI-2.438, WI-2.584 | automated (nightly) |
| 7 | Analysis budgets: de265 ≤ 1 500 ms, aom ≤ 3 000 ms, ffmpeg ≤ 1 000 ms, cached ≤ 30 ms | WI-2.151, WI-2.152 | automated (nightly) |
| 8 | Memory: `bench_memory_4k_stream` ≤ 2 GiB desktop | WI-2.152, WI-2.523 | automated (nightly) |
| 9 | GUI phase-2 tests (overlay toggles, Selection Info, Block Samples screenshots) green | WI-2.584 | automated |
| 10 | CLI goldens for `analyze` (json/csv/yuview/aom-inspect) and `plugins --check` negative test | WI-2.521, WI-2.520, WI-1.510 | automated |
| 11 | Upstream submissions recorded: libaom extension (WI-2.160), libde265 patch 0001 PR (WI-2.167) with URLs in `docs/dev/upstream.md` | WI-2.160, WI-2.167 | manual |
| 12 | YUView loads `--format yuview` output identically (screenshot) | WI-2.521 | manual |
| 13 | Android: overlays on the Pixel Tablet within 33 ms and a 60-frame 1080p GOP analysis within 5 s (FR-AND-014; `tests/perf/budgets.json`, D19); `bench_memory_4k_stream` ≤ 1 GiB | WI-2.168, WI-A.405 | manual/nightly |
| 14 | Exit demo §16.1.3 lines 2–7 recorded for the three sample streams | — | manual |

### 16.6.4 Phase 3 gate

| # | Check | Owner WI | Kind |
|---|---|---|---|
| 1 | PSNR/SSIM kernels vs FFmpeg `psnr`, the x264 `x264_pixel_ssim_wxh` kernel oracle and libvmaf `float_ssim` within stated tolerances (D10: `tiny_ssim` is not an oracle) | WI-3.590 | automated |
| 2 | libvmaf host **bit-exact** vs `vmaf` CLI JSON on every `netflix` pair × model × feature set, incl. subsample and 10-bit | WI-3.591 | automated |
| 3 | Normalisation and alignment goldens (`syn_align_drop7`: offset +7, confidence ≥ 0.5 as in chapter 9 `metrics_align_leading_drop` (flagging threshold `min_confidence` 0.05, D9); crop detection) | WI-3.592 | automated |
| 4 | UVQ parity within 0.01 MOS; BD-rate `|Δ| ≤ 1e-6` vs `bjontegaard` (three interpolators) | WI-3.593 | automated |
| 5 | Budgets: `bench_metrics_psnr_ssim_1080p` ≥ 200 fps, `bench_metrics_vmaf_1080p` ≥ 60 fps, `bench_metrics_normalize_1080p_pq` ≥ 120 fps | WI-3.317 | automated (nightly) |
| 6 | Metrics GUI tests; Android metric parity on a 720p pair: integer/fixed-point results (PSNR, libvmaf integer path) bit-exact, float metrics (SSIM, UVQ) within 1e-4 relative | WI-3.594 | automated (nightly) |
| 7 | CLI goldens for `metrics`, `bdrate`, `hdr` | WI-3.530–3.532, WI-1.510 | automated |
| 8 | `ovt hdr --measure` MaxCLL/MaxFALL within 1 % of `nits_reference.py` | WI-3.532 | automated |
| 9 | Example third-party metric plugin builds out of tree and is listed by `ovt plugins --metrics` | WI-3.318 | automated |
| 10 | Exit demo §16.1.4 lines 3–5 recorded | — | manual |

### 16.6.5 Phase 4 gate

| # | Check | Owner WI | Kind |
|---|---|---|---|
| 1 | Full `AvcMb` vs JM trace on `jvt` fast subset | WI-4.595 | automated |
| 2 | `ovt-plugin-hm` vs `hm_cu_dump`; `ovt verify --hash-sei` 100 % on `jctvc`, localises corruption | WI-4.596 | automated |
| 3 | Matroska, MPEG-TS (incl. TR 101 290 P1/P2 report), DASH/HLS virtual-file goldens | WI-4.597 | automated |
| 4 | Python package tests on macOS and Linux (wheels from `wheels.yml`); sidecar protocol round-trip; HDR scopes tests | WI-4.598 | automated |
| 5 | Conformance checks (picture hash SEI, level limits), HRD model and Buffer panel tests | WI-4.109, WI-4.106, WI-4.112, WI-4.470 | automated |
| 6 | HDR-domain metrics (PU21, ΔE-ITP, wPSNR, PSNR-L100, DE100), XPSNR, SI/TI goldens | WI-4.300–4.303 | automated |
| 7 | FFmpeg patch set submitted to ffmpeg-devel; dav1d proposal filed; URLs recorded | WI-4.103, WI-4.108 | manual |
| 8 | Linux Wayland HDR: §14.10.5 steps 1–4 on KDE Plasma 6.2+ | WI-4.450, WI-4.451 | manual |
| 9 | Manual HDR release checklist on all three platforms | WI-4.459 | manual |
| 10 | Phase-4 acceptance replay and feature-matrix audit: every chapter 3 §3.3 "must" row ticked | WI-4.599 | manual |

### 16.6.6 Track A per-phase gate additions

| Phase | Check | Owner WI |
|---|---|---|
| 0 | APK installs and shows the Qt window and the PQ pattern on both devices; spike verdict | WI-0.050, WI-0.052, WI-0.055 |
| 1 | Instrumentation smoke; `content://` open; free-form resize without recreation; keyboard/hover parity | WI-1.577, WI-A.452, WI-A.451, WI-A.453 |
| 2 | Overlays at 1080p within one frame; analysis ≤ 5 s per 60-frame 1080p GOP on `pixel-tablet` (FR-AND-014, D19; `tests/perf/budgets.json`); plugin store signature check | WI-A.405, WI-2.168, WI-0.053 |
| 3 | Metric parity bit-exact on 720p pair | WI-3.317, WI-3.594 |
| 4 | Scopes render on HDR Chromebook; reproducible F-Droid build; Play AAB validates | WI-4.459, WI-0.056, WI-A.456 |

---

## 16.7 Future work (not scheduled)

The following items are explicitly **not** in any phase. No WI may target them; the architecture must not preclude them (chapter 0 §0.0). Each is parked with the hook in the current design that keeps it possible and the chapter 17 question that would unpark it.

| Item | Why not now | Hook kept open | Unpark when |
|---|---|---|---|
| VP9 analysis | No instrumented upstream decoder with block export; FFmpeg/libvpx expose only MVs. Playback works via FFmpeg (vp9 decoder enabled). | `Codec` enum and extension-table union in `ovt_model.fbs` reserve `Vp9Block` (chapter 6 WI-0.096 reserved range); `caps()` in the plugin ABI is codec-neutral | Owner prioritises VP9 (chapter 17 §17.2) and a libvpx inspection patch ≤ 2 000 lines is feasible |
| VVC/H.266 | VTM is slow and GPL-incompatible packaging questions remain (chapter 15); no public conformance corpus policy decided | `Codec::H266` reserved; `SyntaxBuilder` is codec-agnostic; `ovt_container.fbs` already decodes `vvcC` as opaque | After phase 4; requires a VTM or OpenVVC oracle plan in chapter 14 |
| AV2 | Specification not final | OBU scanner takes a version parameter | AOM publishes v1 |
| MPEG-2 video | Legacy; playback only via FFmpeg | `Codec::Mpeg2` reserved | Platform persona demand |
| Windows and iOS builds | Owner non-target (chapter 1 §1.6) | No platform-specific code outside `render/src/platform/`, `app/src/platform/`, `core/src/analysis/plugin_paths_*.cpp`; CMake presets are per platform | Owner decision |
| Web (WASM) build | Non-target; libplacebo/Vulkan and plugin `dlopen` model do not map to the browser | FlatBuffers outputs are language-neutral; the Python reader (WI-0.095) shows the pattern for a JS reader | Owner decision |
| Live stream monitoring (UDP/SRT/RTMP, Stream Inspector class) | Non-goal (chapter 1 §1.4); file-based analysis only | `RandomAccessFile` is an interface; a ring-buffer backend is possible; the DASH/HLS `VirtualFile` (WI-4.210) already models growing inputs | Owner decision; would need a new chapter |
| Encoding / transcoding / editing | Non-goal | — | — |
| "True" Dolby Vision display mapping | Licensing; non-goal | RPU parsed and shown (WI-4.456); profile 5/8 → PQ via libplacebo and labelled | — |
| MXF own reader | Deferred by WI-4.211 decision record | libavformat demux covers playback | Broadcast persona demand |
| Arrow IPC exporter for research | No persona asked yet | WI-0.097 reserved in chapter 6 | Research persona request |
| Scripting console in the GUI | Optional MAY (WI-4.546) | — | Owner opt-in |
| Additional metrics (HDR-VDP-3 beyond the Octave adapter, FovVideoVDP, VMAF 4K/phone models tuning) | Sidecar protocol (WI-4.304) makes them plugins | Metric ABI v1 | Per request, no core change needed (G6) |
| Codec-pack distribution channels beyond Homebrew/Flatpak/F-Droid/Play (Snap, winget) | Platforms non-target or low demand | `tools/release/manifest.toml` lists artefacts | — |

Open questions that gate scheduling decisions (owner input required) are collected in chapter 17 §17.2; the risk register in chapter 17 §17.1 names the mitigations that already have WIs (HDR-on-ChromeOS → WI-0.055; libde265 patch upstreaming → WI-2.167; FFmpeg patch acceptance → WI-4.103 with the public-API fallback WI-2.170 already shipping in phase 2).

---

### Glossary additions (chapter 16)

| Term | Definition |
|---|---|
| **Milestone** | A named group of work items (`M<phase>.<n>` or `MA.<n>`) that is complete when every WI in it is Done; milestones within a phase are ordered by dependency and sized for 1–2 weeks of a team of ≥ 4 agents. |
| **Phase gate** | The checklist (§16.6) of automated suites, budgets and manual records that must be green before a phase is declared exited. |
| **Critical path** | The longest chain of dependent work items, measured in milestone ordinals and WI sizes, that ends at a phase gate; any slip on it slips the gate. |
| **Primary WI** | In an overlap group (§16.3), the single work item that owns the shared deliverable (files, functions, acceptance tests). |
| **Superseded WI** | A work item that is not implemented because a primary WI delivers its content; its row stays in §16.4 marked "(superseded by WI-…)" so dependency references remain valid. |
| **Reduced WI** | A work item implemented with a smaller scope than its chapter block states, as fixed by §16.3 and its `**Overlap:**` line. |
| **Hardware-bound WI** | A work item with at least one acceptance test that cannot run on a hosted CI runner and needs a named reference device (§16.5.3). |
| **Spec question** | An issue labelled `spec-question` raised when the specification is ambiguous or contradictory; the agent records the assumption it proceeds with (§16.2.4). |
| **Eligible WI** | A work item whose dependencies are all Done, whose milestone is current, which is not superseded and not assigned (§16.2.1). |
| **Fuzzy dependency** | A "Depends on" entry written as prose or a wildcard (`WI-0.0xx`, "chapter 7 plugin host") rather than concrete ids; all were resolved in §16.4 and in the chapter text. |
