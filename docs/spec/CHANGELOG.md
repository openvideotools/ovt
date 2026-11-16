# Specification changelog

Newest first. Format and versioning rules: chapter 16 §16.2.8.

## 1.0.0-rc37 — 2026-11-16

PATCH-level: spec-amendment pass 17 folds in the accepted answers of `spec-question` issues #589 from WI-2.582 (PR #590), #591 from WI-4.109 (PR #594), #592 from WI-1.206 (PR #600), #593 from WI-4.207 (PR #595), #596 and #597 from WI-2.168, #603 from WI-1.572 (PR #604), #606 from WI-4.106 (PR #607), #605 from WI-2.165 (PR #608) and #612 from the corpora-cache fix (PR #613, issue #611). No new diagnostic code; no public C header, plugin ABI or CLI grammar change. The additive `VerifyFrame`/`VerifyReport` fields of #606 are appended to the §6.3 schema block by the WI-4.106 PR itself (the block must equal `core/schema/ovt_model.fbs`); this pass carries only the chapter 7 and chapter 12 prose that names them.

- 14 §14.12 WI-2.582 AT1, AT3 — the HM grid comparison covers what the `hm-grid` goldens hold (`cbf` bit 0 only until libde265 patch 0002, WI-2.165; `partition_id` and more `hm-cutree` goldens after the `hm_cu_dump` follow-up); the block tree is compared on the two existing `hm-cutree` goldens; `SLIST_A_Sony_5` excluded (#254); AT3 is a manual record blocked on WI-0.560 part b and the device run — the block-tree narrowing and the exclusion were accepted by the owner on 2026-11-16 — WI-2.582 (#589)
- 7 §7.8 WI-4.109 — AV1 MD5 side files: one digest per shown frame (first 32 hex digits of each line) over the cropped output planes as `aomdec --rawvideo` writes them; the MaxBR / MaxCpbSize checks split into WI-4.109 part b with their own `SYN-<codec>-006x` code (FR-SYN-020) — WI-4.109 (#591)
- 8 §8.3.2 `stsd`, `tx3g` — `CNT-1010` is raised by `check_stsd_entry_count()` after the children are walked, not by the prefix decoder; `tx3g` `displayFlags` bit names listed — WI-1.206 (#592)
- 8 §8.10 WI-4.207 steps 1–2 — timer checks 1.3/1.5/1.6/2.6 run on the file's PCR axis with their start and end points defined, one error per exceeded interval, not evaluated below two usable PCRs; whole-file `CNT-TS-1007`/`2007` keep the no-section-at-all cases; 2.3 counts PCR intervals with 2.3a, 2.3b or both; timer errors in the `TsReport` only — WI-4.207 (#593)
- 14 §14.3.3 recipe 26, 7 §7.8 WI-2.168 step 1 — `syn_h265_gop60_1080p_nowpp` (x265 `--no-wpp`) added for the thread sweep; the budgeted bench input stays `syn_h265_gop60_1080p` — WI-2.168 (#596)
- 7 §7.6.2 mitigation (4), WI-2.168 AT2 and notes — libde265 built with `OVT_DE265_OPT_FLAGS` (`-O3`, `-march=armv8-a` on Android arm64) for the preset's build type, no fast-math; non-WPP streams parse/reconstruct on one thread with deblocking and SAO on the workers; AT2 records in the de265 README and `docs/acceptance/wi/WI-2.168.md` — WI-2.168 (#597)
- 14 §14.4.1, §14.4.2, WI-1.572 — `trace_headers` names are mapped by `cbs_name_map.json` at compare time and compared as base names, byte arrays expanded from the Bytes node; the `gstparse` check is per member key (OVT-missing keys must be zero, derived values from OVT's parsed structs, test-side `gst_map.json`); `ovt_syntax_golden_run` stands in for `ovt syntax` until WI-1.504; split into parts a (H.264), b (HEVC), c (AV1, after `gen_av1.py` writes the two goldens) and d (hand tests, fuzz, AT2) — WI-1.572 (#603)
- 7 §7.6.4, WI-4.106 Produces, 12 §12.2.12 output — the `VER-0001..0004` Diagnostics of a frame live in `VerifyFrame.diagnostics`, run-level ones in `VerifyReport.diagnostics`; the mismatch heat map is the Picture-store blob `mismatch_map_blob` (U8, one byte per 4×4 luma cell, bit p = plane p differs) with `mismatch_cells_w`/`mismatch_cells_h`; planes of different geometry → one `VER-0001` at (0,0), grids of different dimensions → one `VER-0002`; fields appended by WI-4.106 (chapter 6 §6.1 P7) — WI-4.106 (#606)
- 7 §7.6.2 `cu_bits_x8` row, WI-2.165 step 5 and AT2 — the slice bit total sums the CTU `bits_x8` per slice segment and must equal the slice segment data bits × 8 exactly (CU bits are `coding_unit()` only and omit split, SAO and termination bits, so the former ±1 % CU sum could not hold); the `grid.bits_x8` remainder follows V-020; split into parts a (steps 1–4, AT3) and b (step 5, AT1–AT2) — WI-2.165 (#605)
- 13 §13.7.1 `corpora-cache`, §13.7.2 Corpora row, 14 §14.2.4 — the `corpora-cache` job (weekly, on a `main` push touching the manifest or fetcher, manual dispatch) refreshes the `fast` set under the key the PR jobs restore; the `all` set is saved by nightly's own `setup-ovt` step; cache key `corpora-<set>-<os>-<manifest hash>`; only `main` runs save, pull-request runs restore only — WI-0.552 part c (#612)
- 7 §7.3.4 rows 22–23, WI-1.109 notes, §7.9 `SYN-H264-0071`, WI-4.111 step 1 — WI-1.109 part d (PR #598) parses the SVC, MFC and MVCD subset-SPS extensions, the three VUI extensions and `prefix_nal_unit_svc()`; only `seq_parameter_set_3davc_extension()` (profile 139) stays as bytes with `SYN-H264-0071` and moves to WI-4.111 with `slice_header_in_3davc_extension()`; no part e — WI-1.109 part d (#552)
- 8 §8.2.10 inputs, steps 1, 6, 7, update procedure, WI-1.203 Produces — as merged (PR #579) no upstream snapshot is committed: `INPUTS.sha256` pins the FileFormatConformance and MP4RA commits and file hashes, the generator (and the CI `registry-check` step) clones them into the git-ignored `upstream/` and verifies every hash before reading; steps 1 and 6 point at the inputs paragraph; MP4RA descriptions stay off until the owner rules (`Refs #574`) — WI-1.203 (#574)

## 1.0.0-rc36 — 2026-11-16

PATCH-level: spec-amendment pass 16 folds in the accepted answers of `spec-question` issue #574 from WI-1.203 (PR #579): points 2–7 under the Level-3 triage, point 1 half-ruled: the owner accepted BSD-3-Clause-Clear for the FileFormatConformance snapshots on 2026-11-16; the unlicensed MP4RA CSVs stay uncommitted and unused until he rules, so the issue stays open. No new diagnostic code; no public C header, FlatBuffers schema, plugin ABI or CLI grammar change.

- 8 §8.2.10 steps 2–4 and snippet — one `"*"` row per fourcc plus `*/<container>` rows for differing definitions (~600 rows); class containers expand to fourccs, `file` is the top-level token, bare classes leave `allowed_parents` empty; sample entries are `ContainerWithPrefix`; strict child-box heuristic; `0xFF` / `0xFFFFFF` unknown masks; `has_trailing_data_ok` list; `spec_ref` sources; extras with `spec_ref`, `kind` override and several entries per fourcc — WI-1.203 (#574 points 2–6)
- 8 §8.2.3 step 12 — a `ContainerWithPrefix` without a hand decoder stays a leaf; the children offset is relative to the reader start after version/flags; one mutable `BoxContext` per child list; a `"*"` decoder decorates every more specific row of its fourcc — WI-1.203, WI-1.204 (#574 point 7)
- 8 §8.2.10 inputs — the FileFormatConformance snapshots are committed under BSD-3-Clause-Clear (new `tools/license_policy.toml` row, test-input use only); the MP4RA CSVs (no stated licence) are not committed and contribute no descriptions until the owner rules — WI-1.203 (#574 point 1, FFC part owner 2026-11-16)

## 1.0.0-rc35 — 2026-11-15

PATCH-level: spec-amendment pass 15 folds in the accepted answers of `spec-question` issues #558 from WI-1.230 (PR #518), #567 from WI-1.205 (PR #571), #568 from WI-1.111 (PR #573), #569 from WI-4.206 (PR #572), #570 from WI-1.114 (PR #575) and #566 from WI-3.590 (PR #576). New diagnostic codes are registered (D2: `SYN-H265-0051`, `SYN-H265-0052`); no public C header, FlatBuffers schema, plugin ABI or CLI grammar change. The test-corpus manifest schema (`tests/corpora/manifest.schema.json`) gains the optional `expected.demux` object in PR #518.

- 8 §8.7 WI-1.230 AT1 — for a manifest entry carrying `expected.demux = {packets, error}` the parity sweep asserts exactly that packet count, `file_pos` parity for those packets and then a read error of that class (`mpegff_FX_VY_9436R_3_qhd`: 216 of 483 samples, libavformat CENC/seig limitation); an acceptance-test narrowing for one entry, listed for the owner's check-in — WI-1.230 (#558)
- 8 §8.3 field rules, builder note and §8.3.1 notation — derived text of a `String` (fourcc) field is a sibling `<field>.derived` (`kind = String`, empty `span`), `compatible_brands.derived` for the array; non-`String` kinds keep it in `svalue` (#415); the two sentences no longer disagree — WI-1.205, WI-1.203 (#567 point 2)
- 8 §8.3.1 `hdlr` — a QuickTime counted `name` is two fields, `name_length u8` then `name str_fixed(name_length)` — WI-1.205 (#567 point 3)
- 8 §8.10 WI-1.205 AT1 — `mvhd`/`tkhd`/`mdhd` on `mpegff_a1_foreman_qcif`, `elst.entries[0].media_time` on `mpegff_hvc1_only` (the former has no `edts`); an acceptance-test substitution listed for the owner's check-in — WI-1.205 (#567 point 1)
- 7 §7.3.9 rule 10 — a prefix NAL (14) does not start a picture; it belongs to the base-layer slice that follows — WI-1.111 (#568 point 1)
- 7 §7.3.9 field pairing — both fields of a complementary pair are reference or both non-reference (H.264 3.29/3.30) — WI-1.111 (#568 point 4)
- 7 §7.8 WI-1.111 AT2 and notes — AT2 uses `jvt_sp1_bt_a` (`MR9_BT_B` has no mmco 5; an acceptance-test substitution listed for the owner's check-in); JM 19.0 + FFmpeg `-debug pict` per-picture POC oracle in `jm_poc_expected.h`; an SPS changing inside a picture is held to the next picture without a diagnostic; identical repeated parameter sets keep their `generation` — WI-1.111 (#568 points 2, 3, 5, 6)
- 8 §8.6.7 and WI-4.206 notes — PES reassembly starts at the first `payload_unit_start_indicator` with a `00 00 01` payload on every non-PSI PID (PES before the PMT count, as ffprobe counts them); damaged PES keep their bytes with `complete = false`; file-based TR 101 290 2.3a/2.3b/2.4/2.5 semantics on carried values (`CNT-TS-2003`…`2006`); ffmpeg's 100 ms PCR interval makes `syn_ts_clean.ts` report `CNT-TS-2003` legitimately; `bitrate_per_pid` time axis and memory bound — WI-4.206 (#569)
- 7 §7.3.5 row 14, §7.9, WI-1.114 ATs — `CpbCnt = cpb_cnt_minus1[0] + 1` from the SPS named by `bp_seq_parameter_set_id` (as HM and `cbs_h265`); new `SYN-H265-0051` (SEI without its SPS/VPS, parsed as bytes) and `SYN-H265-0052` (trailing bytes); AT1 on `jctvc_VPSID_A_VIDYO_2` (+ `HRD_A` checksum), AT2 on `syn_h265_hdr10plus`, AT3 on `jctvc_HRD_A_Fujitsu_3` and `jctvc_CONFWIN_A_Sony_1` — three acceptance-test substitutions listed for the owner's check-in — WI-1.114 (#570)
- 14 §14.12 WI-3.590 AT3 — the 1e-9 conservation check runs on block SSEs over the map's block grid (plus each map value equal to its block PSNR to float32 precision); the read-back from the float32, `psnr_max`-clipped map values conserves within 1e-6 on content with no clipped block, since the map stores PSNR, not MSE (§9.4.7); an acceptance-test rewording whose read-back tolerance is relaxed, listed for the owner's check-in — WI-3.590 (#566)

## 1.0.0-rc34 — 2026-11-15

PATCH-level: spec-amendment pass 14 folds in the accepted answers of `spec-question` issues #550 from WI-0.035 (Android emulator smoke, PR #548), #551 and #552 from WI-1.109 part c (PR #554) and #553 from WI-4.105 part c1 (PR #543). New diagnostic codes are registered (D2: `SYN-H264-0033`, `SYN-H264-0071`); no public C header, FlatBuffers schema, plugin ABI or CLI grammar change.

- 13 §13.4.4 item 9 — Qt Quick accessibility is not exposed through `QtQuickView` (Qt ≤ 6.10); `MainActivity` mirrors the QML root's `productName` onto the view's accessibility node so TalkBack and UI Automator see one labelled node — WI-0.035, WI-0.050 (#550, PR #548)
- 17 §17.1 — new risk R-23: screen readers cannot reach QML controls on Android under the `QtQuickView` embedding; mitigation and trigger listed — (#550)
- 7 §7.3.4 row 20, §7.9 — bytes after an `end_of_seq` / `end_of_stream` NAL header are kept as `trailing_data` and reported with the new Info `SYN-H264-0033`; `SYN-H264-0003` stays for reserved values — WI-1.109 (#551)
- 7 §7.3.4 rows 22–23, §7.9, WI-1.109 block — SVC / MVCD / 3D-AVC subset-SPS extensions, VUI extensions and `prefix_nal_unit_svc()` are kept as bytes with the new Info `SYN-H264-0071` until WI-1.109 part d (waits for the H.264 Annex G–J text); the WI block records the four-way split — WI-1.109 (#552)
- 7 §7.6.4 mapping and verify mode, WI-4.106 notes — the `hm` plugin reports empty `HevcRps` lists for BLA pictures (HM resets the slice-header RPS and no HM patch is allowed; every entry would be "no reference picture"); other plugins keep the signalled lists; verify mode does not compare `HevcRps` of BLA pictures — WI-4.105, WI-4.106 (#553)

## 1.0.0-rc33 — 2026-11-15

PATCH-level: spec-amendment pass 13 folds in the accepted answers of `spec-question` issues #546 from WI-1.118 part c (AV1 tile groups) and #547 from WI-0.035/WI-0.050 (Android native-library packaging, PR #548). No new diagnostic code; no public C header, FlatBuffers schema, plugin ABI or CLI grammar change. Also corrects the rc32 entry's count ("seventeen" → "eighteen").

- 7 §7.3.6 row 29 — a tile group with `tg_end ≥ NumTiles` or `tg_start > tg_end` is ParseError `SYN-GEN-0003`; the read values stay in the partial tree, no tile is visited, and the frame is reported truncated (`SYN-AV1-0101`) as after a `SYN-AV1-0020` overrun — WI-1.118 (#546)
- 13 §13.4.2, §13.4.5, §13.4.6 W^X paragraph, WI-0.050 notes — the app module uses `useLegacyPackaging = true` (libraries extracted at install) because Qt 6.8.3's `QtLoader` reads only `ApplicationInfo.nativeLibraryDir` (direct-from-APK loading arrives in Qt 6.9.0); no `android:extractNativeLibs` in the source manifest; user-plugin W^X rule unchanged; Play policy check before a `play` release (WI-0.056) — WI-0.050 (#547, PR #548)

## 1.0.0-rc32 — 2026-11-15

MINOR-level: spec-amendment pass 12 folds in the accepted answers of eighteen `spec-question` items on chapters 6, 7, 8, 9, 10, 13 and 14 (#159, #366 ranking wording, #406, #419 accumulation wording, #431, #433, #434, #449, #450, #470, #471, #477, #478, #480, #486, #490, #499, #510). #470, #471, #480, #486 and #499 were accepted by the owner on 2026-11-14 ("OK all"); the rest under the Level-3 spec-question triage. No public C header, FlatBuffers schema, plugin ABI or CLI grammar change; §7.5.3 adds a priming `pull()` to the host/plugin contract text and §10.2.2 pins field semantics of `ExternalTexture`/`VulkanDeviceInfo` without changing a declaration.

- 14 §14.3.3 recipe 45 — `syn_h265_multi_pps` clones differ in `num_extra_slice_header_bits` / `output_flag_present_flag`, not `init_qp_minus26` (decoding unchanged, formally non-conforming) — WI-0.553 (#159)
- 8 §8.6.6 step 4 — ranking prefers the larger picture (width × height); chroma and bit-depth tie-breaks at equal geometry — WI-1.229 (#366)
- 13 §13.2.2, §13.3.3 — `OVT_FFMPEG_CORE_STATIC` (fuzz-only static core FFmpeg for OSS-Fuzz) — WI-0.569 (#406)
- 14 §14.4.3, 7 §7.6.3 — phase-2 QP test rebuilds QP_Y from `trace_headers` + `mb_qp_delta` (the `jm-trace` `qp` key is unreliable, normaliser fix owed); I_PCM macroblocks report `qp = 0` in the public-API FFmpeg mapping, test accepts either — WI-2.583 (#431, #433)
- 8 §8.6.7, §8.10 rows, WI-4.205 — `PSIT` root for PSI/SI sections; `CNT-TS-1007`/`2007` whole-file forms until WI-4.207; `0x3F` extension descriptor (HEVC timing/HRD) decoded, `0x7F` DVB extension as hex — WI-4.205 (#434)
- 7 §7.5.3 — priming `pull()` with the stage mask after `create()`; a plugin treats any `pull()` mask as a standing request for later pictures — WI-2.166, WI-2.151 (#449)
- 6 §6.6.1, 7 §7.6.2 — `Residual = PreDeblock − Prediction` after clipping (so `Prediction + Residual == PreDeblock`), PCM/bypass convention and `bit_depth` recorded there; patch 0003 copy points in `decode_some()` and `run_postprocessing_filters_*` — WI-2.166 (#450)
- 10 §10.3.3, §10.4.1, §10.10 `RND-0040`, WI-1.406 AT2 — at libplacebo v7.349.0 the NATIVE EDR target uses `{R16G16B16A16_SFLOAT, BT2020_LINEAR_EXT}` via `pl_swapchain_colorspace_hint`; AT2 checks `DisplayTarget.hdr_capable` — WI-1.406 (#470)
- 10 §10.5.1 — `viridis` CC0; `cividis` from pnnl/cmaputil under `LicenseRef-Battelle-cmaputil` (owner decision), viridis fallback until the licence entry lands — WI-2.431 (#471)
- 10 §10.2.2, WI-1.404 — Metal export via a self-created `VkImage` + `vkExportMetalObjectsEXT`; host Vulkan device ≥ 1.2 with `timelineSemaphore`/`hostQueryReset`, `queue_index` must be 0, held layout `SHADER_READ_ONLY_OPTIMAL`; `handle2` carries the `GLsync` for `GlTexture` — WI-1.404 (#477)
- 10 §10.6.4, WI-1.410 AT1 — B converted inside `set_compare()`; RND-0010 behaviour per mode; SideBySide/Split geometry; difference/divider output on non-sRGB targets; overlay blend on linear intermediates; overlays in Single/Flicker-A only; `region_psnr` readback in `measure()`, finite values unclamped — WI-1.410 (#478)
- 7 §7.6.1, WI-2.159 — `OVT_AOM_SNAPSHOTS` from `INSPECTION_FRAME_SNAPSHOTS`; mvd in `MB_MODE_INFO` under `CONFIG_INSPECTION`; patch 0002 carries the three filter snapshots only (Prediction/Residual and hidden-frame recon to a follow-up part); AT1 every frame, AT2/AT3 on corpus `aom` vectors instead of Argon — WI-2.159 (#480)
- 13 §13.7.1, WI-0.035 — emulator smoke runs `ovt --version` now and `ovt info` once the CLI has it (`OVT_SMOKE_REQUIRE_INFO`) — WI-0.035 (#486)
- 10 §10.3.4, §10.7.5, §10.7.8 — out-of-gamut rule (0.2 % relative tolerance, container peak), band fractions counted per pixel, grey = PQ of CIE Y, §10.7.8 wins over §10.3.4, false colour wins over the gamut overlay, conformance window placed in the viewport, no dithering — WI-4.453 (#490)
- 14 §14.4.5, WI-2.581 — suite driven through `PluginRegistry` until `ovt analyze --plugin aom`; first accounting symbol re-added until the goldens come from the patched oracle; masked layers; IVF/Section 5 only; bench row via WI-0.560 part b — WI-2.581 (#499)
- 9 §9.4.4 step 3 — `ssim_x264` plane accumulation is `float` in x264's order (`ssim_end4` groups of four, `float` plane total, `double(total) / count`); only the per-window division is `double`; a `double` plane mean is not allowed (≈ 1.5e-6 off the unmodified oracle on the Netflix pair) — WI-3.308 (#419, PR #447)
- 10 WI-1.405 AT1 — frame 0 built from `sdr709_ramp.expected.json` through `PictureHost::set_picture()`, reference from a 1:1 `Interop::None` surface, until WI-2.171 — WI-1.405 (#510)

## 1.0.0-rc31 — 2026-11-14

PATCH-level: spec-amendment pass 11 folds in the accepted answers of `spec-question` issues #528 from WI-1.110 part a (H.264 SEI, PR #530), #533 from WI-1.118 part a (AV1 film grain, PR #535) and #529 from WI-1.109 part a (H.264 slice header). Three diagnostic codes are registered and two widened (D2, additive); `parse_slice_header()` takes a `ParameterSets` instead of the not-yet-existing `ActiveSets` (D22 semi-public C++ helper header); no public C header, FlatBuffers schema, plugin ABI or CLI grammar change.

- 7 §7.3.4 rows 7 and 9, §7.9 table, WI-1.110 steps and AT2; `corrupt_conditions.json` — each SEI payload is parsed through a bounded view of the message's own `BitReader` (one reader per `sei_rbsp`, absolute node offsets) instead of a sub-BitReader; `SYN-H264-0031` Warning `"pic_timing SEI without an active SPS; parsed as bytes"` and `SYN-H264-0032` Info `"SEI payload has trailing bytes after its last element"` registered with `not_in_table` reasons; AT2 runs against `CVSE3_Sony_H` (committed golden) instead of `CVSEFDFT3_Sony_E` (`slow`, no golden), clock timestamps from `trace_headers` over crafted NAL units — WI-1.110 (#528)
- 7 §7.3.6 row 27, §7.9 table, WI-1.118 AT2; `corrupt_conditions.json` — the film grain conformance checks of AV1 §6.8.20 (`film_grain_params_ref_idx` not in `ref_frame_idx[]`, `num_y_points` > 14, `num_cb_points`/`num_cr_points` > 10, point values not increasing, 4:2:0 with exactly one chroma list empty) raise `SYN-AV1-0015` and parsing continues with the values as read (point arrays sized by the f(4) range); `load_grain_params` copies every saved element including `apply_grain` but keeps `update_grain` and `film_grain_params_ref_idx` as read; AT2's slot-copy case runs on hand-assembled frame headers (no public vector) — WI-1.118 (#533)
- 7 §7.3.7, §7.9 table, WI-1.109 interface and step 4; `corrupt_conditions.json` — `parse_slice_header(BitReader&, SyntaxBuilder&, const NalHeader&, const ParameterSets&)` with `ParameterSets {pps, sps, subset_sps, on_activate}` owned by WI-1.111's `ActiveSets` (mirrors the H.265 resolution of #391); `SYN-H264-0010` also covers a PPS whose SPS or subset SPS is missing; `slice_header_in_scalable_extension()` stays with WI-4.111, until then SVC/3D-AVC enhancement-layer slices are kept as bytes with Info `SYN-H264-0070` and `parse_slice_header()` throws `SYN-H264-0002` for them — WI-1.109 (#529)

## 1.0.0-rc30 — 2026-11-14

PATCH-level: spec-amendment pass 10 folds in the accepted answer of `spec-question` issue #418 from WI-1.115 (H.265 activation). Two diagnostic codes are registered (D2, additive); no public C header, FlatBuffers schema, plugin ABI or CLI grammar change.

- 7 §7.3.8 H.265 row, §7.9 table; `corrupt_conditions.json` — `SYN-H265-0021` Error `"%s %d changed within a picture"` (VPS/SPS/PPS; new content held back until the next picture, mirrors `SYN-H264-0021`) and `SYN-H265-0022` Warning `"SPS %d refers to missing VPS %d"` (once per SPS generation); `SYN-H265-0022` gets a bit-edit condition (`sps_video_parameter_set_id = 1`), `SYN-H265-0021` a `not_in_table` reason — WI-1.115 (#418)

## 1.0.0-rc29 — 2026-11-14

PATCH-level: spec-amendment pass 9 folds in the accepted answers of `spec-question` issues #495 and #498 from the HM plugin work (WI-4.105 part b, PR #496). No public C header, FlatBuffers schema (`predictor` is already a free string), plugin ABI or CLI grammar change.

- 6 §6.3.1 `MotionVector.predictor` HEVC row; 7 §7.6.4 HM mapping — the `:<source>` suffix is optional: a decoder that does not expose the merge/AMVP candidate's neighbour (HM) writes `merge[<idx>]` / `amvp[<flag>]` and says so in its README; consumers parse the prefix up to `:` — WI-4.105 (#495)
- 6 §6.2.3 — the ±32767 clamp with `MDL-GEN-0003` is exercised by the conformance stream `MVCLIP_A_qualcomm_3` (the sentence "conformance streams never need this" was wrong); rule unchanged — WI-4.105 (#498)

## 1.0.0-rc28 — 2026-11-14

MINOR-level: spec-amendment pass 8 folds in the accepted answers of `spec-question` issues #466 (owner OK 2026-11-14), #473 and #512 (internal to libovt-render), #513 (WI-1.117: new code `SYN-AV1-0015` registered, D2; an additive `parse_frame_header` overload in the semi-public C++ helper header, D22; AT2 oracle substitution listed for the owner's check-in) the #509 review nits, and the corpus placeholder answer #517 (follow-up of #507 and #160; ships with `tests/corpora/manifest.json`, `tests/golden/av1-index.json`, the regenerated `tests/corpora/LICENSES.md` and the one-condition demux sweep change, because `tools/check_fast_subset.py` ties the §14.3.2 names to manifest entries). No public C header, FlatBuffers schema, plugin ABI or CLI grammar change; the `MeasureConfig` setter of #512 item 5 is recorded as a later work item that waits for the owner.

- 10 WI-2.431 step 6 and AT2, WI-4.452 AT3 — the overlay-white 203-nit check reads the `ScRgbLinear16F` (1.0 ± 0.005) and `Pq10` (203 cd/m² ± 1 %) targets in WI-2.431; the literal `M` readout moves to WI-4.452 as its AT3, since `M` does not exist before that work item — WI-2.431, WI-4.452 (#466, owner OK 2026-11-14)
- 10 §10.7.1, §10.7.3, `RND-0070` row, WI-4.452 — `M` at full crop size (`s` only selects the histogram grid); histogram by compute atomics into a storage buffer, no float-blend fallback (`RND-0070` registered but never raised, no compute → no measurements); `RGBA32F (max, sum, min, 0)` reduction targets; HLG `M.hdr.max_luma` = source Lw, `min_luma` = inferred black; `Q.a` filled by WI-4.453 — WI-4.452, WI-4.453 (#473)
- 10 §10.7.6, §10.7.7, §10.7.8 export line, WI-4.454 — scopes are `Rgba16F` log-count textures (`log2(1 + count)`, no `R32F` in `OffscreenFormat`); waveform plots CIE Y in nits or the RGB parade, brightest row at the top, `floor` + clamp; edge clamp/drop rule and black-pixel skip; gamut tables defined (12 values per gamut via XYZ D65, PQ); export LUT `round(255·v/v_max)`; two texture sets per readback slot, synchronous export; CIE locus from colour-science 0.4.7 (BSD-3-Clause, attribution row); `MeasureConfig`/`set_measure_config` deferred to a later WI (public header) with defaults stated — WI-4.454, WI-1.412 (#512)
- 7 §7.3.6 rows 12, 14, 15, §7.3.7, §7.9, WI-1.117 AT2 and notes; `corrupt_conditions.json` — `SYN-AV1-0015` Error for frame size above the sequence maximum, `found_ref` into an empty slot and more than 64 tile columns/rows (parsing continues with the values as read); `parse_frame_header` overload taking `const ObuHeader&` for `buffer_removal_time`'s layer ids; AT2 uses `trace_headers` + §5.9.9 for `av1-1-b8-03-sizeup` (which never enters `frame_size_with_refs`) and hand-assembled bits for `frame_size_with_refs`, `syn_av1_superres_scaled_refs` to follow — WI-1.117 (#513)
- 14 §14.2.4, §14.3.1, §14.3.2; 8 WI-1.213 AT1, WI-4.212 AT1, WI-4.214 AT1 — a test that enumerates the manifest skips `unverified` and absent `manual_download` entries also under `OVT_REQUIRE_CORPORA=1` (a named fixture still fails); the nine entries with no upstream file are deleted (`aom_av1_1_b8_16_intra_only`, `aom_av1c_vs_00`…`03`, `bento4_cenc_subsample`, `bento4_playready`, `bento4_frag_cenc`, `gpac_eac3_atmos`) and their acceptance tests move to `mpegff_video_cenc_128iv` and test-built boxes over `syn_mp4_cenc_cbcs` (oracle substitutions for the owner's check-in); `hdr_hlg_bt2020` is nightly-only (`slow`, `needs-corpus`); the `cta`/`dashif` placeholders stay for the Mini to fill; `av1-1-b8-24-monochrome` exists after all — WI-0.552, WI-1.213, WI-4.212, WI-4.214 (#517)
- 6 §6.8.1, `CHANGELOG.md` rc27 header — `canonicalize()` learns the fixed-type palette rule in the #493 follow-up; issue count corrected to twenty-four — WI-0.088 (#509 review nits)

## 1.0.0-rc27 — 2026-11-14

MINOR-level: spec-amendment pass 7 folds in the accepted answers of twenty-four `spec-question` issues raised by work items in flight (#415, #416, #417, #419, #421, #425, #426, #429, #430, #432, #435, #436, #437, #442, #443, #444, #457, #460, #461 points 1–4, #465, #467, #469, #488, #493) and the WI-1.119 frame-count oracle change ruled in the #453 review. New diagnostic codes are registered (D2): `SYN-AV1-0103`, `CNT-HLS-0001`..`CNT-HLS-0010`. No public C header, FlatBuffers schema, plugin ABI or CLI grammar change (the `ovt_jni_*` lines are internal glue, §13.4.4; `docs/spec/schemas/ovtproj.schema.json` gains two optional keys). Acceptance-test wording changes listed for the owner's check-in (oracle substitutions, not narrowings): WI-3.308 AT1 (#419), WI-1.570 AT1 (#437), WI-1.119 step 4/AT1 (#453 review); WI-2.484 AT1 (#457) and the WI-4.211 corpus source (#429) were approved or are substance-preserving. WI-A.450 AT3 (#444) is a narrowing the owner approved on 2026-11-14 ("OK all four"), and the exemption of the HM plugin from the §0.6.2 never-crash rule (#442) was approved by the owner on 2026-11-14 ("OK HM exemption"); both are folded. Nothing is left waiting for the owner.

- 6 §6.9 V-032, WI-0.088, WI-0.094 — V-032 is a Warning on buffers written by a newer schema minor (vtables longer than the validator knows); `fuzz_json_import.cpp` and the `to_json` fuzz call move to WI-0.088; WI-0.094 step 2 asserts through the object-API rebuild, `canonicalize()` assertions guarded until WI-0.085 — WI-0.085, WI-0.086, WI-0.088, WI-0.094 (#435)
- 6 §6.8.1, WI-0.088; 12 §12.1.4; 14 §14.3 — JSON export is OVT's own reflection printer in the `GenerateText` layout over a verified buffer (not `GenerateText`); shortest round-trip decimals in ECMAScript layout, `nan`/`inf` tokens quoted with `strict_numbers` (the CLI always sets it), 64-bit integers beyond 2^53 as strings, odd `bool` bytes as integers, absent fields omitted and empty vectors `[]`; unexpandable nested buffers (unknown type stays bytes, failing verifier throws, fixed-type palettes throw); non-UTF-8 strings throw; importer uses `Parser::Deserialize`, returns the canonical buffer, writes the file identifier for root types only; goldens `tiny16_hevc.index.json`, `tiny_metrics.json`; fuzz targets `fuzz_json_import`/`fuzz_json_export`; `fbs_to_jsonschema.py` `<field>_decoded` follow-up noted — WI-0.088, WI-1.505, WI-2.521 (#493)
- 7 §7.3.10.4, §7.9.3 — `SYN-AV1-0103` Info "order hints disabled; poc = decode_index", once per frame; `corrupt_conditions.json` `not_in_table` reason — WI-1.119 (#426)
- 7 WI-1.119 step 4, AT1 — frame count vs FFmpeg `trace_headers` (`OBU_FRAME` + `OBU_FRAME_HEADER`) and shown frames vs `aomdec --summary` instead of libaom `inspect`, which misses `show_existing_frame` headers and needs `CONFIG_INSPECTION=1` that CI lacks; `inspect` optional manual oracle; expected counts 5/4 and 4/4 — WI-1.119 (#453 review ruling, #488)
- 8 §8.2.5, §8.2.8 — `Field` has no `derived` member: `svalue` carries derived/enum text for non-`String` kinds, array elements are children named `<name>[<index>]`, descriptors are not stored; no schema change — WI-1.202 (#415)
- 8 §8.5, WI-4.209, 17 OI-16 — `CNT-HLS-0001`..`0010` parse-level codes; HLS `Manifest` mapping (one period, adaptation set per media type, timescale 1 000 000, `start_time` running sum, `number` from `#EXT-X-MEDIA-SEQUENCE`, `#EXT-X-MAP` as `is_init`, `#EXT-X-KEY` as distinct keys) — WI-4.209 (#416)
- 8 §8.6.7, WI-4.211; 14 §14.3.3 — bmx is BSD-3-Clause; recipe 77 `syn_mxf_op1a_xdcam` (alias `op1a_xdcam.mxf`) replaces the sourceless corpus file in AT1 — WI-4.211, WI-0.553 (#429)
- 9 §9.4.4, §9.4.6, WI-3.308 — `sha256_match` defined by sample equality (no digest computed); `identity` outputs/options/`finish` without `frame`; `VQ-DIFF-0001` through the `DiagSink` hook; `ssim_x264` `count == 0` → `OVT_E_INVALID_ARG`, `planes` default `["y"]`, window-centre map attribution; AT1 oracle = synthetic 10-bit pairs of `run_x264_ssim.py synth` — WI-3.308, WI-3.311, WI-3.316 (#419)
- 10 §10.4.3, WI-A.400; 13 §13.4.4, WI-0.051, WI-0.052 — `SurfaceView` at default z-order below the Qt UI (chapter 4 wins; no `setZOrderMediaOverlay`); `RANGE_FULL` for the PQ dataspace; SDR surfaces `ADATASPACE_BT709`, log name `SRGB`; `setDesiredHdrHeadroom` is API 35, `hdrSdrRatio` forwarding API 34+; dataspace set before and after `eglCreateWindowSurface`; JNI lines `ovt_jni_set_display_caps`, `ovt_jni_file_size`, `ovt_jni_info_json`; WI-0.051 overlay JSON built from the C API until WI-1.500 — WI-0.051, WI-0.052, WI-A.400, WI-4.458 (#436, #417)
- 11 §11.3.17, `schemas/ovtproj.schema.json` — `N`/`Shift+N` in ascending `decode_index` wrapping; `decode_index` alone until WI-2.480; box anchors via `boxRequested(path)`; optional `bit_offset`/`bit_length` on field anchors — WI-2.487 (#421)
- 11 WI-2.484 AT1 — in-memory `StreamIndex` of `syn_h265_bpyramid`'s shape until the H.265 indexer and a GUI corpus loader exist — WI-2.484 (#457, owner OK 2026-11-14)
- 9, 10, 11, 14 Produces lines — Android Kotlin paths `org/ovt/` → `org/openvideotools/studio/` (backbone D5 wins) — WI-A.400, WI-A.401, WI-A.450, WI-A.452, WI-A.454, WI-A.455, WI-1.577, WI-3.318, WI-4.458 (#443)
- 13 §13.5.2 — `fix_rpaths.py --fix`, then `strip.sh`, then `fix_rpaths.py` verifying the final bytes; bundle notices for shipping targets only (`license_gate.py --only-targets`) — WI-0.060 (#425)
- 13 §13.7.1, WI-0.033 — `fuzz-smoke` gate over `fuzz-smoke-shard` × 4 with canary input; `fuzz-nightly` shards, `-merge=1`, staged artefact; separate `fuzz-corpus-pr` job with the write permissions and `tools/fuzz_corpus_pr.py` limits — WI-0.033, WI-0.557 (#430)
- 14 §14.4.0, WI-1.570 — `units` golden family (per FFmpeg packet, `ffprobe -show_packets` + `trace_headers` types, `gen_units.py`); AT1 per access unit plus per-NAL sizes against `ref_annexb.py`; `MP4Box -dnal` half moves to WI-1.571 — WI-1.570, WI-1.571 (#437)
- 11 WI-A.450 AT3 — same live native session handle and unchanged `OvtSession.openCount` after Activity recreation; the `source_hash` comparison moves to the WI that adds the index API — WI-A.450 (#444, owner OK 2026-11-14)
- 0 §0.6.2, 7 §7.6.4 — `ovt-plugin-hm` is exempt from the never-crash rule (oracle for conformant streams, never default, CLI/GUI warning, no fuzz target; out-of-process HM is a possible follow-up WI); HM's `printf` redirected to stderr by a forced-include compile flag, not a patch; `Pel` is a 32-bit `Int` with `RExt__HIGH_BIT_DEPTH_SUPPORT=1` — WI-4.104, WI-4.105 (#442, owner OK 2026-11-14)
- 7 §7.6.2 — de265 mapping: CU `level = 2` (tree depth, chapter 6 wins); tile scan and tile index from the new patch-0001 getters `de265_internals_get_CTB_addr_ts`/`_tile_id`, `Tile` partitions after the slice-segment partitions; `grid.cbf` bit 0 = `cbf_luma` and `rqt_root_cbf` 1/255 until patch 0002; `HevcFilterParams` per-slice-segment vectors only — WI-2.164, WI-2.165 (#461 points 1–4; point 5 waits for an oracle or the owner)
- 9 §9.5.3, WI-3.309 AT5 — libvmaf's integer VIF extractor is named `vif` (`integer_vif` is a provided-feature name only); `vif` outputs fixed to `integer_vif_scale0..3` (float values renamed on export); `cambi` NoReference only, `full_ref=true` → `OVT_E_UNSUPPORTED`; `vmaf_psnr` has no `finish` score (no APSNR getter), export takes `apsnr_*` from OVT `psnr`; feature ids need no chunk overlap — WI-3.309, WI-3.310 (#460)
- 10 §10.6.1, WI-1.408 — plane views: the plane is declared with three components mapped to R, G, B and the `PL_HOOK_RGB_INPUT` hook (texture input) replicates Y and normalises the bit depth itself; U/V always full range; `raw_code_values` turns dithering off; the U/V tint is deferred to a later work item (public header) — WI-1.408 (#465)
- 10 §10.9.2 items 4 and 6 — one comparison rule for every 8-bit render golden: a pixel differs when any channel is more than 2 codes off, at most 0.1 % differing pixels, tighter local assertions allowed; goldens in `tests/golden/render/<name>.png`, rewritten by `OVT_UPDATE_GOLDEN=1`, read and written by `render/tests/png_io.h` (stored-deflate PNG, no zlib until WI-1.412) — WI-1.408, WI-2.431, WI-2.432–WI-2.436 (#469, #465)
- 10 WI-2.431 notes — the overlay layer holds premultiplied colour (straight-alpha fragments blended "over" into a transparent layer), `overlay.repr.alpha = PL_ALPHA_PREMULTIPLIED`, §10.5 unchanged — WI-2.431 (#467)
- 14 §14.3.2, `tests/golden/av1-index.json` — the slow-moved list states the fast size after #507 (279.0 MB) and keeps `ytugc_clip02`'s figures with it; the `av1-index.json` skip entry of the deleted Annex B id goes — WI-0.552 (#508 review nits)
- 15 §15.4.2 — targets under a `tests`/`test` directory need no `[targets]` row (own licence and link graph still checked) — WI-0.032 (#432)

## 1.0.0-rc26 — 2026-11-14

PATCH-level corpus bookkeeping (#507, follow-up of #160): the five `tests/corpora/manifest.json` ITU entries that returned 404 at their origin are replaced by the real streams, the AV1 Annex B fixture that never existed is dropped, and the hashes of the three remaining `unverified` ITU entries are recorded. Nothing that passed before changes: none of the old ids was ever fetchable. Ships with the manifest, `tests/golden/h26x-index.json` and `tools/tests/test_manifest_add.py` in one PR, because `tools/check_fast_subset.py` ties the §14.3.2 names to manifest entries. No public interface change.

- 14 §14.3.2 — `jvt`: `src19td.IBP.264` and `CVFC1_SVA_C` leave the list (`CVFC1_Sony_C` takes the `h264-ps-golden` tag; `h264-slice-golden` is 38); `ALPHACONFORMANCEG` moves to `slow` (29 MB zip, over the `fast` budget); `jctvc`: `DBLK_A_SONY_4` → `DBLK_A_SONY_3`, `INITQP_B_Sony_1` → `INITQP_B_Main10_Sony_1`, `LTRPSPS_A_FJ_1` → `LTRPSPS_A_Qualcomm_1`; `aom`: `av1-1-b8-01-size-16x16-annexb` removed, `syn_av1_annexb` (recipe 18) is the Annex B fixture — WI-0.552, WI-1.108, WI-1.109, WI-2.164 (#507, #160)
- 4 FR lines 288 and 342, 7 WI-1.108 Produces — the same id renames — WI-1.108, WI-2.164 (#507)

## 1.0.0-rc25 — 2026-11-14

MINOR-level: spec-amendment pass 6 folds in the accepted answers of `spec-question` issues #394 (libvmaf host, ruled 2026-11-13), #407 (SSIM, Reviewer A), #408 (AV1 accounting, ruled 2026-11-14), #402 (timeline arrows, Reviewer A), two follow-up rulings on the closed #323 (box walker) and the acceptance-test changes of #391 point 4 and #395 point 6 that the owner approved on 2026-11-13. One new diagnostic code is registered (D2: `CNT-0111`). No public header, schema or CLI grammar change; the `finish` doc comment in `core/include/ovt/abi/metric_v1.h` ("optional sequence-level FrameScore") is unchanged and rides the next comment-only header batch (owner's check-in list). Acceptance-test wording changes listed for the owner's check-in: WI-2.157 AT2 (oracle = libaom `Accounting` records + inspect.c printing rule, #408; stricter, not narrower) and the test-stream swap to `av1-1-b8-22-svc-L1T2` (#408 point 4).

- 9 §9.2.2 rule 3 — a plugin whose score of frame `t` needs frame `t+1` emits it on the next `score` call and the last one from `finish`; a `finish` `FrameScore` with `frame` present is a late per-frame score, one without `frame` is the sequence-level score; libvmaf ids have no sequence-level score — WI-3.300, WI-3.309 (#394 point 2)
- 9 §9.2.3, §9.5.2, WI-3.309 step 6 — libvmaf chunk overlap is `lookbehind = 1`, `lookahead = 1` with both overlap scores discarded, never shifted (`integer_motion2(0) = 0` per context makes a one-frame lookbehind insufficient); `metrics_vmaf_chunking_identity` on 1/2/3/5/8 chunks — WI-3.309 (#394 point 1)
- 9 §9.5.2 — `VmafConfiguration.n_threads = 0` per context, parallelism from OVT's chunks; the `clamp(threads / chunks, 1, 8)` rule is dropped; short clips use fewer threads than requested and a missed §14.9.3 budget is fixed through the chunk formula; `n_subsample = spec.subsample` stays (1 until the job wires `subsample` through) — WI-3.309 (#394 point 3)
- 9 §9.5.3 — libvmaf ids are function tables internal to libovt-core registered through `PluginRegistry::register_builtin`, not exported `ovt_metric_vmaf_<id>()` symbols — WI-3.309 (#394)
- 9 §9.5.4 — the `VmafPicture` copy is clipped to libvmaf's `w >> 1` × `h >> 1` chroma planes, dropping the last chroma column/row of odd-sized pictures as the `vmaf` CLI does — WI-3.309 (#394)
- 9 §9.6.5 — UVQ's `finish` result is marked as the sequence-level `FrameScore` (`frame` absent) per the §9.2.2 rule 3 convention — WI-3.312 (#394)
- 9 §9.4.3, §9.4.7, WI-3.307 — `border = "libvmaf"` (default) is libvmaf's down-scale by `f = max(1, round(min(W,H)/256))` plus a valid 11-tap convolution with libvmaf's window table and `L = 255` constants; `"valid"` is full-resolution Wang; `descriptor.version = "ssim 1.0 border=libvmaf"`; `planes` default `["y"]` with NaN for planes not computed; no `finish` score; map blocks without a window centre hold NaN — WI-3.307 (#407)
- 6 §6.8.3, 7 §7.6.1, §7.7.5, WI-2.157 — `inspect` prints the context marker instead of the first record of each context (OVT writes every triple; comparison per `(x4,y4,id)` through inspect.c's rule); frame-level and out-of-grid accounting records are left out of `symbols[]` and counted in `Block[0].bits_x8` (V-024 holds); padding leaves `bits_x8 = 0`; the tile-group count decides `DEC-AV1-0002`, `Block.bits_x8` kept, `grid.bits_x8` cleared, no byte-count comparison; multi-tile-group vector `av1-1-b8-22-svc-L1T2`; WI-2.157 step 5 and AT2 reworded — WI-2.157, WI-2.161, WI-0.090 (#408)
- 11 §11.3.1 — timeline arrows from `declared_refs` are drawn as L0, solid, until the index carries list membership — WI-1.455 part c (#402)
- 8 §8.2.3, §8.5, §8.9.3 — the step 2.6 `size < 8` recovery excludes `uuid` (`CNT-0106`, break; `CNT-0107` for `8 ≤ size < header_length + 16`); node bound of 1 000 000 `BoxNode`s per walk with the new `CNT-0111` and trailing-bytes nodes; the bound test runs in memory — WI-1.201, WI-1.224 (#323 follow-ups)
- 7 WI-1.113 AT2 — RPS compared with the committed gstparse goldens (IDR + 5 pictures); the 8-picture HM comparison is WI-1.572's — WI-1.113 (#391 point 4, owner OK 2026-11-13)
- 7 WI-4.102 step 4, AT2 — slot-level `ref0` check (`L0[ref_idx_l0]` equals JM; POC check moves to #253's WI); JM comparison on the 17 streams with goldens — WI-4.102 (#395 point 6, owner OK 2026-11-13)

## 1.0.0-rc24 — 2026-11-13

MINOR-level: spec-amendment pass 5 folds in the accepted answers of five new `spec-question` issues (#385, #388, #389, #391 points 1–3, #395 points 1–5), the three public-header rulings pass 4 left open (#209 item 4 `ovt_log_write`, #366 item 3 `raw_format == NULL`, #318 no `Pool` export) and the WI-0.089 AT3 rewording. New diagnostic codes are registered (D2: `SYN-H265-0011`, `SYN-H265-0061`, `DEC-AV1-0001`, `DEC-AV1-0002`). Public-header text changes (listed for the owner's check-in): the additive `ovt_log_write` declaration in §12.3.4, the `raw_format` NULL semantics comment in §12.3.7, the `plane()` `*size` comment in `core/include/ovt/abi/analysis_decoder_v1.h`, and the `PpsTable` signature in §7.3.7 (`ovt/cxx`). No schema or CLI grammar change.

- 7 §7.3.4 rows 2–3 — `pic_width_in_mbs_minus1`, `pic_height_in_map_units_minus1` ≤ 65535 (arithmetic-safety bound, `uint64` derived values); `delta_scale` se −128..127; violations `SYN-H264-0002` — WI-1.108 (#388)
- 7 §7.3.7, §7.9.3 — H.265 `parse_slice_segment_header` takes `const PpsTable&` (declared in `ovt/cxx/syntax/h265/slice_segment_header.hpp`); `SYN-H265-0011` missing PPS; `SYN-H265-0061` dependent segment without a preceding independent one; `corrupt_conditions.json` `not_in_table` reasons — WI-1.113, WI-1.115 (#391 points 1–3; point 4 waits for the owner)
- 7 §7.6.1, §7.9.5, WI-2.158 — film-grain note is the Info `Diagnostic` `DEC-AV1-0001` "film grain present (not applied)" (§7.6.1/§6.6.1 rule 5 win over the stale step text); accounting note is `DEC-AV1-0002`; in-place libaom strides with `plane()` `*size = stride_bytes × (height − 1) + width × sample_bytes` (comment in `analysis_decoder_v1.h`) — WI-2.157, WI-2.158 (#389)
- 7 §7.6.3 — I_16x16 `tx_log2 = 2` (V-027 allows (2, 3, 255)); interim `mb_field_decoding_flag` 0/1 on interlaced frames until WI-4.110; `P_8x8ref0` reported as `P_8x8`; slice type inferred from MBs; per-picture ref slots until #253 — WI-4.102 (#395 points 1–5; point 6 waits for the owner)
- 8 §8.6.6, 12 §12.3.7 — `ovt_container_open_format(…, "raw", NULL, …)` runs the sidecar/heuristics `resolve()`; no `@<header_bytes>` suffix — WI-1.229, WI-1.511 (#366 item 3, ruling 2026-11-13)
- 9 §9.4.2, §9.4.7, WI-3.306 — fixed 8-entry `psnr` layout `[y, u, v, jvet, ffmpeg_avg, mse_y, mse_u, mse_v]` with NaN for absent/unselected planes, `finish` `[apsnr_y, apsnr_u, apsnr_v, apsnr_jvet]`; `min(…, psnr_max)` clipping on every frame incl. `ffmpeg_avg` (FFmpeg oracle may differ near-lossless); NaN → JSON `null` / empty CSV cell; `MapSink` seam until WI-3.316; `map_blob_id` names the first selected component — WI-3.306, WI-3.312, WI-3.316 (#385)
- 10 §10.10, WI-2.430, 12 §12.3.4 — additive `ovt_log_write(level, module, code, msg)` in `ovt/log.h` as the emit entry point for separate libraries and plugins (#209 item 4, ruling 2026-11-13); `ovt::jobs::Pool` stays unexported, `std::thread` chunks final (#318 closed) — WI-1.400, WI-1.512, WI-2.430
- 6 WI-0.089, `docs/acceptance/wi/WI-0.089.md` — AT3 reworded to the YUView 2.14 run on the Mac Mini (plain file arguments, `tiny16_16x16.yuv`, no exit code, screenshot); the manual record filled in — WI-0.089 (#233 item 5)

## 1.0.0-rc23 — 2026-11-13

MINOR-level: spec-amendment pass 4 folds in the accepted answers of 28 `spec-question` issues on chapters 8, 9, 10 and 11 (plus the chapter 0, 4, 6 and 14 sentences they touch). New diagnostic codes are registered (D2: `CNT-OBU-0003`, `CNT-OBU-0004`, `CNT-DASH-0001`…`0009`); D11 and §9.6/§14.4.14 move the UVQ reference to the pinned PyTorch google/uvq (`c204ecc0`, UVQ 1.5 default) with a `torch.onnx.export` path; no schema or CLI grammar change; the public-header questions of #209 item 4 (`ovt_log_write`), #318 (`ovt::jobs::Pool` export) and #366 item 3 (raw header offset in the C API) stay open for the owner.

- 8 §8.2.3, §8.2.5, §8.9.3, WI-1.201 — four-escape fourcc display form; a trailing-bytes node on every walk stop; box-relative spans on `BoxNode`, file-relative for tree-level findings, `CNT-0105` on the parent, `CNT-0003` on `moov`; 13 broken files committed under CC0, the truncation sweep in memory; registry stand-in until WI-1.203 — WI-1.201 (#323)
- 8 §8.2.4, §8.9.2, 14 WI-0.564 — `header_length` excludes FullBox bytes, the sample-entry fixed part and `uuid` `usertype`; mp4box.js `hdr_size` normalised (−4/−8/−16) by `mp4boxjs.py`; WI-0.564 AT1 runs over every fetchable `fast` ISOBMFF entry; oracle limits listed in `KNOWN_DIFFS.md` — WI-0.555, WI-0.564 (#134, #226)
- 8 §8.5, WI-1.200 — `diag_codes.inc` X-macro gains a fifth `spec_ref` column; `CNT-DASH-` family; `ovt_File` forward-declared and defined internally by WI-1.200; `ovt_container_list_diagnostics` unsupported until the `DiagnosticRegistry` table lands — WI-1.200 (#138, #349)
- 8 §8.6.1 — Annex B sniff returns a heuristic codec guess (`H264`/`H265`/`Ambiguous`), resolved by the stream reader — WI-1.200 (#138)
- 8 §8.6.2, WI-1.225 — `header_size < 32` ignored; partial frame header → `CNT-IVF-0003`; zero timebase → `timescale 0`; last-sample duration; `is_sync` by a minimal OBU/VP9/VP8 walk; AT1 `offset == pos + 12`; C entry points wait for WI-1.217 — WI-1.225 (#280)
- 8 §8.6.3 — empty NAL not emitted; `CNT-ESB-0004` for SPS/PPS/VPS/AUD only; `CNT-ESB-0002` counting rule; `CNT-ESB-0001` for garbage after a boundary — WI-1.226 (#278)
- 8 §8.5, §8.6.4 — `CNT-OBU-0003` (length overrun, clamped) and `CNT-OBU-0004` (malformed header/leb128/length, incl. `obu_length` 0) — WI-1.227 (#279)
- 8 §8.6.6, 4 FR-CNT-014, WI-1.229 — ranking prefers the larger frame size among `frame_count ≥ 2`; packed 4:2:2 names deferred; C API parses the explicit string only, `resolve()` carries sidecar/heuristics/overrides; small defaults (chroma_format, LE `p0xx`, H.273 2, `dts = i × fps_den`); header offset in the C API left to the owner — WI-1.229 (#366)
- 8 §8.6.7, WI-4.204 — MPEG-TS resync rule, 188/192/204 phase detection and the M2TS prefix, `MPTS`/`TSPK`/`ADPT` tree, continuity-counter rule, per-PID statistics internal until WI-4.207, 10 000-entry diagnostics cap — WI-4.204 (#337)
- 8 §8.7.1 — `ovt::demux::Status = ovt::Result<void>`; `read_packet()` returns `Result<std::optional<Packet>>`, `nullopt` = end of stream — WI-1.230 (#316)
- 8 §8.7.3, §8.7.4, WI-1.231 — `scratch` holds only the rewritten last OBU and `unit_bytes()` serves it; `temporal_delimiters_expected` parameter; LengthPrefixed edge cases → `CNT-9001`; `UnitSpan` defined in `demux/unit_split.h`; `tools/extract_samples.py` walks `stsz/stsc/stco/avcC/hvcC` itself (no MP4Box) — WI-1.231 (#338)
- 8 WI-4.208, 6 §6.4 — injected `SidxResolver` callback, `CNT-DASH-0003` without one; `ManifestPeriod.start/duration` in milliseconds; template `start_time` rule — WI-4.208 (#349)
- 6 §6.6.2 — each open stream owns its `PictureStore`; `blob_id`s of two streams never collide — WI-0.087, WI-2.151 (#157)
- 9 FR-VQ-009, FR-VQ-010 (and the chapter 4 copy), §9.6.1–§9.6.5, WI-3.314, 0 D11, 14 §14.4.14 — map/label outputs qualified to model versions that produce them; google/uvq pinned at `c204ecc0` (PyTorch, UVQ 1.5 default: ContentNet/DistortionNet/AggregationNet, `values = [mos]`, no maps or labels; 1.0 keeps the seven values, grid maps and YouTube-8M labels); `torch.onnx.export` opset 17 replaces tf2onnx; `uvq_inference.py --model_version` oracle; swscale bicubic resize (1.5 stretches to 1920×1080, 1.0 letterboxes 1280×720 then bilinear 496×496), `fps=…:round=up` sampling, `F = 1`/`F = 5`, black-frame padding; additive manifest keys; release upload is the owner's — WI-3.314, WI-3.315, WI-0.554, WI-0.565 (#47, #246)
- 9 §9.2.4 — resolved `NormalizationConfig` form: defaults/consts expanded, objects materialised, nullable → `null`, other default-less properties absent — WI-3.300 (#290)
- 9 WI-3.3xx AT2 — x264 reconstruction encoded with `--bframes 0` — (#306)
- 10 §10.1.3, §10.2.1, §10.2.5, §10.2.7, §10.4.5, §10.10, WI-1.400 — `render_api.h` with `OVT_RENDER_API`; `"ovt_model_generated.h"` include; `set_frame()` stays `void`, `RND-0001` via `last_error()`; `build(buf, size, …)` verifying overload; explicit heatmap ranges linear in natural units; log codes reach stderr/`last_error()` until an exported `ovt_log_write` exists (owner) — WI-1.400, WI-1.401, WI-2.430 (#209, #317)
- 10 §10.3.3, §10.10, WI-1.402 — `RND-0020` only for codes outside the tables, transfer 11/12 and ICtCp with a non-PQ/HLG transfer (→ PQ); codes 0/2 silent; notices returned by the pure functions — WI-1.402 (#237)
- 10 §10.5.1, WI-2.430 — only `Slice`/`Tile` rects are partition boundaries, shared edges once; 4 chunks on `std::thread` workers (`ovt::jobs::Pool` not exported; owner) — WI-2.430 (#317, #318)
- 11 §11.2.2, WI-1.451 — 17 panel ids; `Layout.leftSize/rightSize/bottomSize`; preset slash = stacked groups with fractions; `layouts.json` `{"layouts_version": 1, "current": …}`; View ▸ Reset Layout — WI-1.451 (#229)
- 11 §11.2.5, WI-1.450 — settings directory resolved from `GenericConfigLocation`/`GenericDataLocation`/`AppDataLocation` + `/ovt`, not `AppConfigLocation`; `recent_files.json` — WI-1.450, WI-1.454 (#117, #243)
- 11 §11.3.1, WI-1.455 — range = inclusive positions in the current x order; Δpts includes the last frame's presentation distance; hatch and GOP labels in part c, 55 % opacity in part b — WI-1.455 (#240, #257)
- 11 §11.4.3, WI-1.466 — action ids `action.<menu id>`; example `action.navigate.next` → `J`; whole-file, same-scope conflict rule — WI-1.466 (#222)
- 11 §11.7, §11.8, WI-1.454 — `GUI-RAW-0001` expected size rule; Open-as roles; Welcome sample streams (≤ 5 `fast` entries, `OVT_CORPORA_MANIFEST`, consent dialog, `sampleConsented`); core hookup via `fileOpened`; file chooser follow-up — WI-1.454, WI-1.464 (#243, #309)
- 11 WI-1.452 — `SelectionIndex` interface and app-side struct mirrors; sample span from the adapter; `setChainLength`; EPB test alternative until the golden exists — WI-1.452 (#219)
- 11 WI-1.453 — `DiagnosticsModel::NavigationSink`; `DiagnosticLocation`; jobs over the C `ovt_Job` API with an own `QThreadPool` — WI-1.453 (#137)

## 1.0.0-rc22 — 2026-11-13

MINOR-level: spec-amendment pass 3 folds in the accepted answers of 22 `spec-question` issues on chapters 6 and 12 (plus the chapter 0, 4, 7, 13 and 14 sentences they touch) and three review follow-ups. New diagnostic codes are registered (D2: `MDL-CSV-0001`…`0004`, `CLI-0010`, `CLI-0011`); the public-header text of §12.3.4 and §12.3.14 is aligned with the merged `ovt/log.h` and `ovt/cache.h`; no schema or CLI grammar change.

- 6 §6.3.3 — nested buffers are built with `ForceVectorAlignment(size, 1, nested.GetBufferMinAlignment())`; `ext_as` verifies with nested-relative alignment — WI-0.084, WI-0.085 (#98)
- 6 §6.6.2, §6.9 V-023 — plugins emit the plugin-local `decode-index/stage/plane` form; the host prefixes the source hash; V-023 applies to host-stored records; `tb-` ids have no leading zeros — WI-2.151, WI-2.155, WI-2.163 (#250, #98)
- 6 §6.6.3, WI-0.087 — `PlaneBlob`; pin per frame; evictions count frames; `OVT_E_NOMEM` only under the byte cap; 0.6 × cap trim; `hard_cap_bytes` default 2 GiB; `adopt` keeps the stride; `ovt/color.h` created by WI-0.087 — WI-0.087 (#157, #156)
- 6 §6.7.1, §6.7.3, §6.7.4, 12 §12.3.14 — `reports/<kind>.fb.zst`; I3 uses `ovt::model::kModelSchemaMajor`; `reports/` evicted after `maps/` — WI-0.091, WI-0.092 (#288, #80)
- 6 §6.7.3, §6.7.4, WI-0.092 — `Cache::open_entry(hash, codec, SelectedPlugin)` applies I1–I7, `Cache::entry()` and the C object API stay raw; every eviction step targets 0.9 × cap; `MDL-CCH-0002` once per `Cache`; cache state shared per root per process; AT3 arithmetic — WI-0.092 (#295, #296; PR #301 review)
- 6 §6.8.1, 12 §12.1.4, WI-1.501 — `[ubyte]` as arrays of numbers, hash-like fields (real `.fbs` names) as hex, the five nested fields replaced in place; schema generator rules; AT1 skip rule and permissive `result` for free-form commands — WI-0.088, WI-1.501 (#285, #286)
- 6 §6.8.2, §6.9.1, WI-0.089, 14 WI-2.584 — example QP row 33; `MDL-CSV-0001`…`0004`; importer size limits; exporter choices (a)–(e); own CC0 sample CSVs; AT3 and the manual YUView step recorded as pending; `v1.2` in chapter 14 — WI-0.089 (#233)
- 6 §6.8.3, WI-0.090 — luma TXBs only; `tileCols`/`tileRows` rounded up; `AomJsonOptions::ovt_version`; AT1 without accounting keys — WI-0.090 (#269)
- 6 §6.9 — V-001 requires `OVTA` (plain or size-prefixed); V-002 HEVC multiple of 8; V-011 grid leaf has no child of any listed kind; 64 violations per rule cap; V-032 canonical form = object API `Pack` layout — WI-0.085, WI-0.086 (#311, #175, #271)
- 6 §6.10, WI-0.093 — `edge_flags_v = [10]`, bin `128+33`, chroma QPs per Table 8-10, `data_bit_offset = 32` in the fixture; fixtures stored not size-prefixed; `Mv` is (row, col); inspect.json details; two-part delivery — WI-0.093 (#164)
- 6 WI-0.080, WI-0.083, 13 WI-0.003 — secondary roots verify with `Verifier::VerifyBuffer<T>(id)`; `schema_version.hpp` plain header in `ovt::model`, `bfbs_resources.cpp` committed with generated `.inc` includes; `--gen-name-strings` in every flatc flag list — WI-0.080, WI-0.083 (#35, #80, #98)
- 6 WI-0.084 — `BlobIdParts`, `mv_to_neutral_checked`, `mv_from_neutral` throws, `label_for` fields and golden key, `leaf_at` rule — WI-0.084 (#98)
- 6 WI-0.085 — no `CopyTable`; structural diff reports every difference in field-id order; AT2 wording — WI-0.085 (#271)
- 6 WI-0.086, 12 §12.2.4, §12.2.8, §12.3.8 — violations are `MDL-VAL-0nnn` with the message beginning `V-nnn`; hash-less overload; `validate_metrics_result` later — WI-0.086 (#311)
- 12 §12.1.2, §12.1.3, §12.2.15, 4 FR-CLI-003 — `--time` footer on stderr for `fb`; `CLI-0010`, `CLI-0011`; CLI-0001 coverage and did-you-mean; usage errors always one human line; `--version` formats; `completion` ignores the TTY default; alias pairs; log-level precedence and JSON log names; `--progress` step; CSV number rules; `--plugin-dir` replaces `OVT_PLUGIN_PATH`; `$schema` at `ovt.dev` — WI-1.500 (#100, #101)
- 12 §12.2.13, §12.3.14 — kinds of `cache list`/`clear --kind`; hits/misses are process-lifetime; `--source` unsupported until WI-1.121; `ovt_cache_enabled()`, `ovt_cache_max_bytes()`; `set_max_bytes(0)` keeps the stored cap — WI-0.091, WI-0.092, WI-1.509 (#297, #300; PR #294 review)
- 12 §12.3.1, §12.3.9, 0 §0.6.1 — by-value embedded structs are frozen; §12.3.9 layouts authoritative, backbone aligned; `ovt_picture_ref/release` to the first decoder WI — WI-0.087 (#156)
- 12 §12.3.4, 7 §7.1.6 — silent default logger; `Level::Fatal`/`Off`; `OVT_LOG_*` C macros; `OVT_LOG_CODE`; `format_line()`/`stderr_sink()` — WI-1.512 (#69)
- 12 WI-1.500 — cancel hook, `JsonWriter` result callback, `ProgressReporter`; wiring by WI-1.502/WI-1.505 — WI-1.500 (#100)
- 7 §7.3.2 — `set_enum` turns a `UInt` node into `Enum` — WI-1.106 (#97; PR #102 review)
- 7 §7.6.1 — libaom built with `-DCONFIG_SVT_AV1=0` (owner, 2026-11-08) — WI-2.154

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

## 1.0.0-rc20 — 2026-11-12

Owner decision (Karl, 2026-11-12, answering chapter 17 Q3 after PR #132): the default build keeps the pool-covered FFmpeg decoders in the core flavour, so the official default download plays AVC/HEVC without the codec pack. By §16.2.8 rule 1 this is MAJOR-level (ADR-015 wording changes); because the specification is still a 1.0 release candidate awaiting the owner's acceptance, the rc counter advances and the major number stays. The runtime-loaded plugin mechanism, the codec pack (now the analysis decoders) and the patent-pool-free split build (`OVT_PATENT_CODECS_IN_CORE=OFF`) are unchanged. No WI is re-opened: WI-0.012 is merged and the implementation PR of this entry flips the default and its tests.

- 0 §0.2 ADR-015, 5 §5.10, 17 Q3 and R-14 — the default build keeps AVC/HEVC (and VVC, AAC, MPEG-4 Part 2, VC-1) playback decoders in the core; the analysis decoders stay runtime-loaded plugins; the split build stays supported; app-store, paid or commercial distribution of the default build needs the owner's legal check — WI-0.012
- 13 §13.2.2 — `OVT_PATENT_CODECS_IN_CORE` defaults to ON (was OFF); the licence-manifest example records `true` — WI-0.012
- 13 §13.3.3 — the core row lists the `--disable-decoder=`/`--disable-hwaccel=` lists under `OVT_PATENT_CODECS_IN_CORE=OFF` instead of as the default; rationale sentence updated (the plugin flavour is the instrumented *analysis* decoder) — WI-0.012
- 13 §13.4 items 3 and 4 — the Android default download carries the core's playback decoders, including `h264_mediacodec`/`hevc_mediacodec` hardware playback without the codec pack; the Play base module (split build) keeps MediaCodec-via-plugin as its path — WI-0.012
- 15 §15.5.2 POL-PAT-2, POL-PAT-3; 4 §4.15.8 NFR-033 (and its chapter 15 copy) — requirement sentences follow the new default; NFR-033 now binds the *analysis* decoder components, its acceptance line is unchanged — NFR-033, WI-0.602, WI-0.603
- 13 §13.4 item 3, §13.4.8 Play bullet, 15 §15.5.2 POL-PAT-2, 15 §15.8 Google Play and PyPI rows, 12 §12.4.1 Python wheel table, `docs/licensing.md` — coordinator decision on the owner's behalf (2026-11-12, for the weekly check-in): the Google Play base module and the PyPI wheel are split builds (`OVT_PATENT_CODECS_IN_CORE=OFF`) until the owner's legal check; the F-Droid/GitHub APK and the other official artefacts carry the default — WI-0.012
- 13 §13.6.2 AppImage paragraph, 13 §13.2.6 CPack components, 13 WI-0.012 Goal line, 90 appendix patent-risk note — wording follows the new default (the AppImage bundles the default core flavour; the `core` component excludes the analysis plugins; WI-0.012 is not re-opened) — WI-0.012
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
