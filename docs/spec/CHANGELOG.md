# Specification changelog

Newest first. Format and versioning rules: chapter 16 §16.2.8.

## 1.0.0-rc21 — 2026-11-12

MINOR-level: spec-amendment pass 2 folds in the accepted answers of 32 `spec-question` issues on chapter 7 (plus the chapter 0, 4, 6 and 12 sentences they touch). New diagnostic codes are registered (D2); no public C header, schema or CLI grammar changes — the §0.6.2 comments are aligned with the merged ABI header.

- 7 §7.2.4 — `BR-0011` for `me(v)` codeNum outside Table 9-4; `BR-0006` also for `trailing_bits(0)` and `me(v)` with `chroma_array_type > 3`; `nb_bits > bits_left()` is `BR-0001` — WI-1.102, WI-1.103 (#67, #85)
- 7 §7.2.2 — `unread_bytes_after_trailing()` returns `bits_left() / 8` — WI-1.102 (#68)
- 7 §7.2.5, §7.9.3 — `SYN-GEN-0002` is raised by the unit parser from `epb_positions`; `rbsp_from_nal` throws `std::length_error` over 4 GiB — WI-1.104 (#76)
- 7 §7.3.11 — Dolby oriented code is `0x00000800` u(32); provider code read for every country with a table row; HDR10+ `application_version > 1` falls through; HDR Vivid oriented code `0x0005` (owner, 2026-11-05) — WI-1.120 (#89)
- 7 §7.7.1 — CBS AV1 positions count from the OBU header; `trace_diff.py` subtracts the header as §14.4.1 does — WI-1.126, WI-0.555 (#94)
- 7 §7.3.2, WI-1.106 — `ovt::syntax::Severity` declared in `syntax_builder.hpp`; `SyntaxNodeBuffer` defined in `core/src/syntax/common/`; root node `unit`, builder behaviour details — WI-1.106 (#96, #97)
- 7 §7.9.3, WI-1.107 — `SYN-GEN-0004` forbidden byte pattern; `SYN-GEN-0105` also for AV1 length overruns — WI-1.107 (#109)
- 7 §7.1.2, §7.1.3, 12 §12.3.6 — `open_fd` size sentinel 0 or `UINT64_MAX`; fd closed on failure with ownership; C wrapper `dup()`s; C opens use `Pread`; `IoError::code` returned; FIFOs/sockets rejected — WI-1.101 (#110)
- 0 §0.6.2, 7 §7.5.1, §7.6 — ABI header self-contained (`OVT_STATUS_TYPES_DEFINED`), `OVT_GRID_*` names, `OVT_PLUGIN_EXPORT`, `pull()` comment — WI-2.150 (#211)
- 7 §7.5.1, §7.9.5, 12 §12.3.11 — one plugin search order (add_search_path, `OVT_PLUGIN_PATH` replaces unless trailing `:`, platform defaults merged; macOS `Contents/PlugIns/ovt/` per FR-MAC-006); ownership check allows root; duplicate id detected by file name before `dlopen`; `default_for()` rule — WI-2.150 (#212)
- 7 WI-2.162 — AT2 checks AMP modes over all pictures (frame 0 is intra) — WI-2.162 (#215)
- 7 §7.6.2 — `deblk_bS_v` getter semantics and PB-edge gating of the bS mapping — WI-2.162, WI-2.164 (#216)
- 7 §7.5.3, §7.6, §7.9.4 — plugin diagnostics ride on the next `FrameAnalysis`; diagnostics-only `FrameAnalysis` on `OVT_E_UNSUPPORTED`; `DEC-H265-0002` for libde265 warnings — WI-2.151, WI-2.163 (#249)
- 7 §7.5.3, §7.9.5, WI-2.163 — `ANA-0011` for frames the decoder does not output; AT1 streams chosen accordingly — WI-2.151, WI-2.163 (#251)
- 7 §7.5.2, §7.5.3, §7.9.5 — Session takes `StreamIndex` bytes and the file; ActiveSets snapshot derived from `StreamIndex.parameter_sets`; `SessionCache` seam; `ANA-0002`…`ANA-0010` registered; A6 RefSlot fill AV1-only until WI-1.121 — WI-2.151 (#253)
- 4 FR-SYN-019, 7 §7.7.6 — not every HEVC_v1 stream carries a hash SEI; the nine without verify through `*_yuv.md5` — WI-2.163 (#255)
- 7 §7.6.3, §7.9.4, 12 §12.3.11, 0 §0.6.2 — `ovt_plugin_describe` defined; `DEC-H264-0001`; which frame carries a decode-error diagnostic — WI-2.169 (#261)
- 7 §7.6.3 — analysis decoder uses `FF_THREAD_SLICE` only (frame threading makes side data nondeterministic) — WI-2.170 (#302)
- 7 §7.6.3, §7.9.5, 6 V-018 — `ANA-H264-0002` registry text and trigger; field flag follows `AV_FRAME_FLAG_INTERLACED`, validator exempts the FFmpeg plugin — WI-2.170 (#303)
- 7 §7.3.4, §7.9.3 — `SYN-H264-0002` (range), `SYN-H264-0003` (reserved value); `SYN-H264-0020` only when the assumed count was used; MVC `layer_id` = `view_id` at parse time; SPS node nesting — WI-1.108 (#320)
- 7 §7.3.6, §7.9.3 — `SYN-AV1-0004`…`0006` for reserved values, unknown metadata and tile lists — WI-1.116 (#321)
- 7 WI-1.116 — AT3 uses operating point 3 of `av1-1-b8-22-svc-L2T2` — WI-1.116 (#322)
- 7 §7.3.5, §7.9.3 — `SYN-H265-0004` for extensions kept opaque in phase 1 — WI-1.112 (#327)
- 7 §7.6.3, WI-4.102, 4 FR-BLK-010 — WI-4.102 applies the FFmpeg patch series to the plugin flavour regardless of `OVT_FFMPEG_GPL` — WI-4.100, WI-4.102 (#331)
- 7 §7.6.3 — `AVH264MacroblockParams` arrays in luma4x4BlkIdx order, mvd in quarter samples, coded-picture origin, `AV_VIDEO_BLOCK_PARAMS_NONE` — WI-4.100 (#332)
- 7 §7.6.1, WI-2.155 — `create()` has no message channel until ABI v2; AT2 reworded — WI-2.155 (#335)
- 7 §7.6.3 — h264 patch internals at n7.1.1 (one `H264MBParams` table, recorded while parsing, export from `output_frame()`, `vblock_data_dump` FATE tests) — WI-4.101 (#351)
- 7 §7.6.1, 6 §6.2.2, §6.3.1, V-006 — aom plugin reads header values from `AV1_COMMON`; accounting forces one thread; `mi_rows/mi_cols` from `mi_params`; no `MotionVector` for IntraBC; CDEF index per 64×64 unit; `blocks[0]` exempt from V-006; frame Diagnostics replace `derived` — WI-2.156 (#360)
- 7 §7.5.1, WI-4.107, 12 §12.3.11 — H.265 fast path is `libovt_plugin_ffmpeg_hevc` (id `ffmpeg_hevc`), opt-in only — WI-4.107 (#370)
- 7 WI-4.107 — export in `hevc_frame_end()` with output waiting; per-PU vectors and per-CU params; libde265 internals oracle for AT1 — WI-4.107 (#371)

## 1.0.0-rc19 — 2026-11-12

MINOR-level: spec-amendment pass 1 folds in the accepted answers of 41 `spec-question` issues on chapters 13, 14 and 15 (plus the chapter 7, 9, 11 and glossary sentences they touch). Acceptance-line wording changes for WI-0.012, WI-0.019, WI-0.556, WI-0.557, WI-0.565 and WI-0.569 describe what their merged PRs already do, so no WI is re-opened. Issue numbers are cited in the text.

- 13 §13.7.1, 14 §14.8.2, 14 WI-0.559 — the `clang-tidy` job lives in `static-analysis.yml` and is listed by job name; header filter leaves `core/include/` out; `core/tests/.clang-tidy` relaxes six checks; `check_ovt_flags.py` detects `ovt::flags` through the `OVT_FLAGS_APPLIED=1` marker — WI-0.559 (#29)
- 15 §15.4.2 — gate reads non-SPDX `VERSIONS.md` cells through parenthesis stripping and `[versions_overrides]`/`[versions_skip]`; `--pip` reads pip-licenses JSON or requirements with `importlib.metadata`/`[pip_overrides]` (`W-PIP-UNRESOLVED`); `--require-all` semantics; `[allow.any]` ids in `permissiveness`; `E-TARGET` — WI-0.602 (#31)
- 13 WI-0.003 — Python/Kotlin FlatBuffers code is generated in every configuration; the options gate only the consumers — WI-0.003 (#32)
- 13 WI-0.018 — `install_qt.py` prints `QT_ROOT_DIR=<dest>/6.8.3` — WI-0.018 (#39)
- 14 §14.2.4 — `unverified: true` manifest field, fetcher and licence-gate behaviour, member storage by basename, `.ok` record — WI-0.551, WI-0.552 (#41)
- 14 §14.4.12, 9 §9.4.4 — x264 sanity check uses a lossy QP without B-frames against x264's own reconstruction; `x264_ssim` takes Y4M — WI-0.554, WI-0.565 (#49, #306)
- 13 §13.2.4, 14 §14.8.1 — `asan-env-macos` test preset (`detect_leaks=0:detect_container_overflow=0`) for the macOS ASan presets — WI-0.558 (#55)
- 15 §15.6, WI-0.603 — notices before `manifest.toml`: component set from gate inventory rows with targets, licence files staged by `ovt_notices`, output under `build/<preset>/`, `OVT_NOTICES_REQUIRED` — WI-0.603 (#57)
- 14 §14.3.5, §14.3.2 — CENC fixtures are `mpegff_video_cenc_128iv` and `mpegff_video_cenc_roll_frag1s`; `gpac`/`bento4` ids point at files that exist — WI-0.551, WI-1.213 (#59)
- 13 §13.2.1, 14 §14.2.1 — `ovt_core_objects` OBJECT library, how `ovt_core` and the module tests link it, third-party deps on the objects — WI-1.100 (#62)
- 15 §15.5.5, WI-0.607 — `Date:` and `Issue:` header lines, `invalid <field>` message, ageing rule details — WI-0.607 (#66)
- 13 §13.2.1, §13.4.3, WI-0.019 — Android library is `libovt_studio_<ABI>.so` — WI-0.019 (#71)
- 14 WI-0.557 — regressions directory is `regressions/<name>/` — WI-0.557 (#90)
- 14 §14.4.0, §14.4.3, §14.4.4 — per-stream `block-grid` shape (`frames[]` with `fields`, `mbs`, `slices`, `mvs`, `qp_blocks`, `symbols`), JM arrays in read order, `hm_cu_dump` prints raw getter values and `hm_grid.py` maps them (chroma DM mode resolved, CBF at TU depth) — WI-0.555, WI-0.563 (#93)
- 14 §14.4.5 — oracle image patches `inspect` so the first accounting symbol of each block is printed — WI-0.554, WI-0.566 (#103)
- 13 §13.2.2 — `OVT_SANITIZER` defaults to `fuzz` under `OVT_ENABLE_FUZZ`, else `asan-ubsan` — WI-0.035 (#106)
- 14 WI-0.556, 2 glossary — golden comparison direction (golden = lower bound), `to_canonical` over `StreamIndex`/`SyntaxNode`, `SyntaxTree` defined, a/b split recorded — WI-0.556 (#111)
- 13 §13.3.1 — `OVT_SYSTEM_DE265` semantics and the libde265 probe tests under `plugins/ovt-plugin-de265/tests/` — WI-0.014 (#114)
- 13 §13.3.3 — plugin flavour passes `--disable-decoders --enable-decoder=h264,hevc` and `--disable-hwaccels …` — WI-0.012 (#128)
- 13 WI-0.012 AT2 — symbol-leak test on a module linked like the plugin — WI-0.012, WI-2.169 (#129)
- 14 §14.4.13 — `bjontegaard` 1.3.0 does not raise for cubic on 6 points; refusal tested against OVT alone — WI-0.555 (#142)
- 13 §13.3.7 — dav1d pinned to the videolan release tarball with its hash (owner decision 2026-11-05); libplacebo stays on the GitLab archive — WI-0.011, WI-0.015 (#44)
- 13 §13.3.7 — one `VERSIONS.md` row per ONNX Runtime prebuilt archive; MoltenVK and ONNX Runtime hashes filled — WI-0.017 (#150)
- 13 §13.2.2 — `OVT_SYSTEM_LIBAOM` inspection probe builds and runs (`try_run`); not supported when cross-compiling — WI-0.013 (#152)
- 14 §14.4.2, WI-0.562 — `gst_parse_oracle` prints JSON Lines folded by the WI-0.567 normaliser; struct-of-arrays, union, entry points, interim acceptance inputs — WI-0.562, WI-0.567 (#153)
- 14 §14.3.1, §14.3.2 — fast-subset names absent upstream keep their ids with stand-ins or `unverified` entries; `post` steps not yet implemented; `ytugc` uses the 20 s H.264 re-encodes; per-corpus sizes are upper bounds; `h265-cu-golden` and `av1-grid-golden` member lists — WI-0.552 (#160, #161)
- 14 §14.3.3 — pattern fixtures are `pat_*.y4m` from `gen_hdr_patterns.py`; no `--kind pattern` — WI-0.553, WI-1.413 (#173)
- 14 §14.3.3 recipe 60 — `syn_raw_<ctag>` family = the 22 §8.6.5 tags with distinct layouts; siting-only tags `same_as` — WI-0.553 (#174)
- 14 §14.3.1, §14.3.2, §14.12, 9 §9.6 — `fast` budget kept; 19 named streams moved to `slow`; golden tags defined by tag membership; Givens on moved streams run nightly; UVQ fast clips are `ytugc_clip01` and `03` — WI-0.552 (#179)
- 14 §14.4.3a, WI-0.563 — `ffmpeg_mvs [--qp <qp.csv>] <in>`; MVs enabled through `export_side_data` — WI-0.563 (#181)
- 14 WI-0.569 — OSS-Fuzz local test uses the `projects/ovt` flow, not `--external` — WI-0.569 (#267)
- 13 §13.3.4 — libaom patches live in `plugins/ovt-plugin-aom/patches/` (`OVT_AOM_PATCH_DIR`) — WI-2.154 (#274)
- 7 §7.6.1, WI-2.154 — patch series applied with `tools/apply_patches.py --check`, not `git apply --3way` — WI-2.154 (#275)
- 7 §7.6.4, WI-4.104, 13 §13.3.7 — HM built from OVT's own CMake list without patches (TLibCommon, TLibDecoder, Utilities; no TLibVideoIO; `ENC_DEC_TRACE` untouched); HM row pins the commit, git rows verified through `_COMMIT`; BSD-3-Clause and the libmd5 note — WI-4.104 (#345)
- 14 §14.10.3, 11 WI-1.465 — test IPC wire format (Unix socket, request/response JSON shape, `click` argument forms, `wait-idle [timeout_ms]`) — WI-0.568, WI-1.465 (#353)
- 13 §13.4.1–§13.4.3, WI-0.050 — Qt Gradle plugin 1.2 is the only native build (no `externalNativeBuild`, no `ndk.abiFilters`); `src/main/cpp` holds only `ovt_jni`; `QtQuickView` loads `AndroidShell.qml` — WI-0.050 (#355, #356)
- 15 §15.2, 13 §13.3.7 — Qt Gradle plugin licence is `LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0` (build-only tool) — WI-0.050 (#357)
- 13 §13.6.2, WI-0.070 — the AppImage `AppRun` serves the CLI through `--ovt-cli` or an `ovt` symlink and appends the bundled plugin dir — WI-0.070 (#374)
- 14 §14.2.3, WI-0.565 — metric golden naming (pair ids, `.json.gz`), Produces list, Netflix reference value `76.6689`, picture caps, Daala 10-bit skip, UVQ goldens split into part b — WI-0.565 (#306)

## 1.0.0-rc18 — 2026-11-09

PATCH-level: WI-3.300 folds in the accepted answer of `spec-question` #156 and aligns chapter 9 with the backbone and chapter 13. No requirement sentence changes.

- 0 §0.6.3, 9 §9.2.2, 9 §9.2.4, 9 §9.5.4, 9 §9.10, 2 glossary — the multi-plane view a metric plugin scores is `ovt_MetricPlanes` (with `struct_size`, D4), not `ovt_PlaneView`, which `ovt/decode.h` (chapter 6, WI-1.237) defines as one plane with another layout; `ovt_MetricPlanes` is owned by chapter 9 and declared only in `metric_v1.h`; chapter 9's copy of `ovt_MetricApi` carries the backbone's `struct_size` — WI-3.300
- 9 WI-3.300 Produces and step 3 — the ABI header carries `ovt_MetricPlanes`; metric plugins are discovered in `OVT_PLUGIN_DIR` (chapter 13 §13.2.6, same rules as decoder plugins, chapter 12 §12.3.11) and `$OVT_METRIC_PATH`; there is no `share/ovt/metrics/` — WI-3.300

## 1.0.0-rc17 — 2026-11-09

PATCH-level: WI-0.060 implementation detail. No requirement sentence or acceptance line changes.

- 13 §13.5.2 — the bundle script installs into `Contents/` and then moves the install tree to the bundle layout (`GNUInstallDirs` names are configure-time cache variables), passes every plugin to `macdeployqt` as `-executable`, and runs `fix_rpaths.py --fix` before the verification — WI-0.060

## 1.0.0-rc16 — 2026-11-08

PATCH-level: owner decision (Karl, 2026-11-08) on the golden size cap. No requirement sentence or WI scope changes.

- 16 §16.2.7 and `CLAUDE.md` §5 — the cap on committed goldens is 2 MB gzip-compressed (the size of the file as committed), not 2 MB uncompressed, so the AV1 and HEVC grid goldens fit; golden runners read `.json.gz`

## 1.0.0-rc15 — 2026-11-07

PATCH-level: WI-0.605 records how branch protection is implemented now that the repository is public. No requirement sentence changes.

- 13 §13.7.3, 15 WI-0.605 — protection of `main` is a repository ruleset managed by `tools/github/apply_protection.sh`; required checks, code-owner review and signed tags follow in later PRs (accepted `asan-ubsan` failures and missing teams block them today); AT1 is pending until the teams exist — WI-0.605

## 1.0.0-rc14 — 2026-11-07

PATCH-level: two recipe commands made reproducible after the WI-0.553 c2 `--check` on the oracle image. No requirement sentence, acceptance line or WI scope changes.

- 14 §14.3.3 reproducibility rule 2 and recipes 68 `syn_mp4_cenc_widevine`, 69 `syn_mp4_piff` — Bento4 `mp4fragment`/`mp4encrypt` write the wall-clock time into `mvhd` (two runs differed in bytes 64 and 68), so both recipes end with the new `tools/mp4_pin_times.py`, which zeroes the `mvhd`/`tkhd`/`mdhd` creation and modification times; `generate.py` rejects (exit 4) a recipe whose last Bento4 step is not followed by it — WI-0.553

## 1.0.0-rc13 — 2026-11-07

Owner decision (follow-up to the PR #132 review, which noted the msmpeg4 decoders still in the core): the core FFmpeg flavour also leaves out the Microsoft pre-standard variants of MPEG-4 Part 2. MINOR-level change (configure flag list); no WI is re-opened.

- 13 §13.3.3, 15 §15.5.2 POL-PAT-2 — core flavour adds `msmpeg4v1`, `msmpeg4v2`, `msmpeg4v3`, `wmv1`, `wmv2` to `--disable-decoder=` (all five share FFmpeg's `msmpeg4dec` component, which then is not built; none has a hwaccel); the `h263` decoder and parser stay; `OVT_PATENT_CODECS_IN_CORE=ON` keeps them — WI-0.012

## 1.0.0-rc12 — 2026-11-06

PATCH-level: a recipe command fixed after the WI-0.553 c2 run on the oracle image. No requirement sentence, acceptance line or WI scope changes.

- 14 §14.3.3 recipe 21 `syn_mp4_frag_cmaf` — `MP4Box -dash 2000 -rap` on the 2 s `syn_h265_amp_sao_on` (IDRs at frames 0 and 30) wrote only `cmaf_1.m4s`, and the `cat` of three segments failed. The recipe now encodes its own HEVC from `src_720p_8bit.y4m` with an IDR every 20 frames and segments at 600 ms, which gives three 667 ms segments; the `cmfc`/`styp`/`moof` intent is unchanged — WI-0.553

## 1.0.0-rc11 — 2026-11-06

PATCH-level: a recipe command fixed after the WI-0.553 c2 run on the oracle image. No requirement sentence, acceptance line or WI scope changes.

- 14 §14.3.3 recipe 33 `syn_ts_cc_errors` — the clean TS loops the 60-frame source once (`-stream_loop 1`, 120 frames, 4 s). At 2 s, `ts_inject_errors.py --pcr-gap 1` (gap from first PCR + 1 s to + 2 s) found no PCR after the gap and stopped with "the stream is too short"; the 1 s gap and the 7 CC errors are unchanged — WI-0.553

## 1.0.0-rc10 — 2026-11-06

PATCH-level: recipe commands fixed after the WI-0.553 c2 run on the oracle image. No requirement sentence, acceptance line or WI scope changes.

- 14 §14.3.3 recipe 19 `syn_av1_superres_scaled_refs` — `--resize-denominator=12` → `9`: with `--superres-denominator=12` the combined scale was 2.25x, over AV1's 2x reference-scaling limit, and aomenc pass 2 failed ("Can't find at least one reference frame with valid size"); 9 × 12 gives 1.6875x and keeps `superres`, `reference_scaling` and `render_size`. The row states the R × S ≤ 128 rule, which generate.py now enforces — WI-0.553
- 14 §14.3.3 recipe 68 `syn_mp4_cenc_widevine` — the `mp4encrypt --pssh` system ID is written as 32 hex digits without dashes (`edef8ba979d64acea3c827dcd51d21ed:<file>`); Bento4 requires `arg[32] == ':'` and rejected the dashed UUID ("invalid argument syntax for --pssh") — WI-0.553
- 14 §14.3.3 reproducibility rule 6 (new) — a recipe whose `{corpus:ID}` input is a `manual_download` corpus missing from the cache (recipe 71 `syn_mkv_dv_p5` ← `hdr_dovi_p5`) is skipped with a logged reason and stays `unverified`, instead of failing the whole run — WI-0.553

## 1.0.0-rc9 — 2026-11-06

PATCH-level: an aomenc option name in two recipe commands, found by the WI-0.553 c2 run on the oracle image. No requirement sentence, acceptance line or WI scope changes.

- 14 §14.3.3 recipes 19 `syn_av1_superres_scaled_refs` and 62 (`syn_av1_tools_all` …) — aomenc 3.12.1 has no `--superres-denom`; the option is `--superres-denominator=12` (same value) — WI-0.553

## 1.0.0-rc8 — 2026-11-06

PATCH-level: a recipe command fixed after the WI-0.553 c2 run on the oracle image (an option name HM 18.0 does not know, and slice sizes that produced one slice per picture). No requirement sentence, acceptance line or WI scope changes.

- 14 §14.3.3 recipe 12 `syn_h265_multislice_wp` — HM 18.0 `TAppEncoder` has no `--DependentSliceMode`/`--DependentSliceArgument`; dependent slice segments are now `--SliceSegmentMode=1 --SliceSegmentArgument=40`, and slices shrink from 300 to 100 CTUs (`--SliceArgument=100`): a 1280x720 picture has 240 64x64 CTUs, so 300-CTU slices gave one slice per picture and the `multiple_slices` feature was never produced — WI-0.553

## 1.0.0-rc7 — 2026-11-06

Owner decision on PR #132 (WI-0.012): the core FFmpeg flavour leaves out every pool-covered decoder, not only AVC/HEVC. MINOR-level change (configure flag list); no WI is re-opened.

- 13 §13.3.3, 15 §15.5.2 POL-PAT-2, 02 glossary "VP9 / VVC / MPEG-2" — core flavour adds `vvc`, `aac`, `aac_fixed`, `aac_latm`, `aac_at`, `aac_mediacodec`, `mpeg4`, `mpeg4_mediacodec`, `vc1`, `vc1image`, `wmv3`, `wmv3image` to `--disable-decoder=` and `mpeg4_videotoolbox`, `mpeg4_vaapi`, `vc1_vaapi`, `wmv3_vaapi` to `--disable-hwaccel=`; `OVT_PATENT_CODECS_IN_CORE=ON` keeps them; POL-PAT-2 no longer says parsers and bitstream filters are disabled (§13.3.3 keeps them on) — WI-0.012

## 1.0.0-rc6 — 2026-11-06

Amendment from the pre-public audit, owner-approved wording change. Editorial except one MINOR-level item: the WI-2.483 Produces path follows the panel rename below (WI-2.483 is not started, so no WI is re-opened). No requirement sentence, acceptance line or interface changes.

- 00 §0.0 — the one-sentence goal names the feature areas OVT covers instead of saying it "reproduces the functionality of" two named commercial products — none
- 01 §1.2, §1.4 — commercial analyzers named only as examples of the tool class; per-product price figures and sub-product lists removed; trademark and non-affiliation note added; the live-monitoring non-goal no longer names a product — none
- 90 Appendix A — reduced to factual feature comparison with links to public pages: quoted vendor text, descriptions of vendors' panels, menus, key bindings and CLI flags, the UI-lineage claim and reseller price tables removed; trademark, non-affiliation and sourcing rules added; A.5 replaced by product-neutral usability lessons — none
- 11 design principles, §11 panel purposes — references to competitors' UI elements removed; no behaviour changes — none
- 03 — trademark and independence note; §3.1 and §3.2 describe feature areas only (UI skeleton, mode list, CLI flag and price rows removed; "reproduces both idioms" and the UI-lineage claim removed); §3.5 conventions stated as OVT's own with no competitor provenance, UX-2 justified on its own terms (one key per overlay mode, default order from §11.4.2, all rebindable). Coordinator ruling: no binding is tied to a competitor; the F-key assignments, FR-GUI-008 and its acceptance lines are unchanged — none
- 02 — "Binding mode" entry removed; "Primary benchmark" reworded — none
- 04 GUI rationale — no longer cites competitor users' expectations — none
- 01 §1.3 goal G1 — "Cover every feature area marked 'must' in chapter 3 §3.3" instead of "Replicate the must-have functionality of" two named products — none
- 02, 11 §11.3.10, 16, 01 — the per-stage block sample panel is renamed **Block Samples** (was a competitor's panel name); WI-2.483 Produces `app/qml/panels/BlockSamplesPanel.qml`; the glossary entries "Block Samples" and "Bar-chart-as-timeline" no longer name competitors — WI-2.483
- 10 FR-PLN-007 row, 03 rows 41 and B2, 11 §11.3.2 — compare modes described only with OVT's own `CompareMode` vocabulary; the informative mapping to other products' mode names is removed — none

## 1.0.0-rc5 — 2026-11-06

MINOR-level amendment (CI job behaviour) from the pre-public audit requested by the owner; no WI is re-opened.

- 13 §13.7.1 — `build-linux-x64` and `build-macos-arm64` upload their build artefacts only while the repository is private, because a public Actions artefact is a binary distribution and the CI bundles carry no third-party notices — WI-0.030

## 1.0.0-rc4 — 2026-11-06

MINOR-level amendment (new optional manifest fields) made by the implementation of WI-0.552; no WI is re-opened.

- 14 §14.2.4 — manifest entries may carry `upstream_path` (path at the origin) and `note` (pending steps, SPEC-ASSUMPTION remarks); a `vcs` entry carries `rev`, the pinned commit, and the fetcher resolves it to the Git LFS media URL (github.com origins only); an `unverified` entry may have no `url` when the file is still to be chosen or does not exist at its origin; `tools/manifest_add.py` fills hashes — WI-0.552

## 1.0.0-rc3 — 2026-11-06

Accepted answers of `spec-question` #119 and #123 (coordinator, under the owner's level-3 autonomy). MINOR-level changes (recipe commands of WI-0.553, licence-gate interface of WI-0.561); no WI is re-opened.

- 14 §14.3.3 — reproducibility adjustments of the synthetic recipes: pinned encoder thread counts, `MP4Box -for-test`, `mkvmerge --no-date --deterministic`, seeded lavfi sources, fixed Bento4 IV, `-strict -1` for 10-bit Y4M, `-framerate 30` for raw Annex B, tool paths and arguments of recipes 2, 16, 22, 27, 28, 31, 41, 56 and 68; `check_fast_subset.py` greps `docs/spec/` (#119) — WI-0.553
- 14 §14.3.4, WI-0.561 — `--corpora` is its own mode, `--manifest` names the corpora manifest there, rule-1 failures exit 4, the dependency check and the corpora check are separate steps of the `license-gate` job (#123) — WI-0.561

## 1.0.0-rc2 — 2026-11-04

Accepted answers of the `spec-question` issues raised by the M0.1 work items (owner decisions of 2026-11-04). MINOR-level changes (WI Produces/acceptance lines); no WI is re-opened, because every affected WI is merged and already implements the amended line.

- 00 front matter — licence of `docs/spec/` decided: CC-BY-4.0 (#20) — none
- 12 §12.3.2 — `ovt_version()`/`ovt_version_string()` return the product version (`VERSION`, 0.x) while `OVT_API_VERSION` stays 1.0.0; `ovt --version` prints both (#4) — WI-0.000, FR-GEN-010
- 13 §13.2.6, WI-0.008 — the CMake package version is the C API version, so `find_package(ovt 1)` matches a 0.x product (#13) — WI-0.008
- 13 §13.3.2, WI-0.002 — the manifest listing is the end state; WI-0.002 commits the nine Goal ports only, later WIs add theirs (#8) — WI-0.002
- 13 §13.3.7, WI-0.010 — MoltenVK archives live on the `v<version>-artifacts` tag; `versions.cmake` is generated into the build tree; `<dep>` id mapping for names with spaces (#15) — WI-0.010
- 13 §13.7.1, §13.7.3, WI-0.030 — Linux test step is the `linux-x64-release` test preset; `build-macos-arm64` runs on the self-hosted Mac Mini and uploads `ovt-macos-arm64`; lint steps skip with a notice until their tool lands; markdownlint scoped to touched files outside `docs/spec/`; no branch protection on a Free private repository (#27, #25) — WI-0.030, WI-0.604
- 13 WI-0.000 — `ci.yml` skeleton carries a placeholder `skeleton` job (#2); acceptance tests 2 and 3 run against a plain configure until the presets exist (#3); ADR table is §0.2 (#3) — WI-0.000
- 13 WI-0.001 — acceptance test 2 runs with the vcpkg toolchain overridden until WI-0.002 (#6) — WI-0.001
- 14 WI-0.550 — no `tests` job in `ci.yml`; the `tools/` pytest suite is the `tools_pytest` CTest test run by the `pr` step (#11) — WI-0.550, WI-0.030
- 14 WI-4.599 — `Depends on: none` (ordering by milestone M4.4), matching the §16.4 row (#17) — WI-4.599
- 15 §15.1, WI-0.600 — `docs/spec/` is CC-BY-4.0, `LICENSES/CC-BY-4.0.txt` added; placeholder plugin `CMakeLists.txt` stubs and `plugins/ovt-plugin-aom/PATENTS` keep every licence text used for REUSE (#20) — WI-0.600
- 15 §15.4.2 — `[directories]` gains `.github`, `plugins` and the repository root, all LGPL-2.1-or-later (#22) — WI-0.601, WI-0.602
- 15 §15.7.2, WI-0.604 — enforcement contact `conduct@ovt.dev`; `docs/dev/dco.md` linked from the hook, PR template and `CONTRIBUTING.md` instead of the DCO app's fixed message; required `DCO` check pending until branch protection exists (#25) — WI-0.604, WI-0.605
- 16 §16.3.3 — the WI-0.550 overlap row names the `tools_pytest` test instead of a `tests` job (#11) — WI-0.550

## 1.0.0-rc1 — 2026-11-04

- all chapters — initial release candidate for owner review, committed to `docs/spec/` — none
