# Open Video Tools (OVT) — Requirements and Architecture Specification

| | |
|---|---|
| **Status** | Release candidate for owner review |
| **Version** | 1.0-rc41 (2026-11-16) |
| **Audience** | The implementation team (coding agents and their human reviewers), the project owner, and future contributors |
| **Scope** | A free and open-source, cross-platform (macOS, Linux, Android/ChromeOS) video analysis suite for H.264/AVC, H.265/HEVC and AV1 with HDR (PQ/HLG) display, bitstream inspection, container inspection, block-level visualisation, per-plane views and video quality metrics |
| **Licence of this document** | CC-BY-4.0 (owner decision of 2026-11-04, issue #20; the rest of `docs/` stays CC-BY-SA-4.0, chapter 15 §15.1) |

## How to use this document

This specification is the single source of truth for building OVT. It is organised so that an implementer reads the general chapters once and then works from the chapter that owns their module and from the work item index in chapter 16.

1. **Everyone** reads chapter 1 (Introduction), chapter 2 (Glossary, as a reference) and chapter 5 (Architecture) before writing code.
2. **Module implementers** read their owning chapter (6–15) in full. Each owning chapter contains the interfaces (as code), the algorithms, the test oracles and the work items (WI) for that module.
3. **Pick work** from chapter 16, which lists every work item with its resolved dependencies, phase and milestone, and resolves the places where two chapters specified overlapping deliverables.
4. **The backbone** (chapter 0, reprinted after chapter 17 in the assembled document — it was written first and every chapter was authored against it; its sections are `0.n`) is the normative summary every chapter conforms to: architecture decisions, repository layout, core schema excerpts and public interface signatures. When a chapter and the backbone disagree, the chapter owner fixes the chapter; until then the backbone wins.
5. **Appendices** hold the commercial feature inventory, the library survey, specification cross-references, the schema listing and the URL index.

Chapter 17 lists the open questions the owner still has to answer; each has a default the specification assumes.

**Precedence when chapters disagree.** Chapter 4 wins on *what must be true* (requirement ids are defined only there); chapter 12 wins on CLI grammar and exit codes; chapter 11 wins on key bindings and the settings file; chapter 6 wins on the FlatBuffers schema; chapter 16 wins on scheduling (which WI delivers what, when); chapter 14 §14.9.3 and `tests/perf/budgets.json` hold the performance budgets (D19; chapter 0 §0.7.4 mirrors the headline numbers) and chapter 13 §13.1 the toolchain pins; chapter 14 §14.3 is the fixture registry (D26) and chapter 14 §14.9.2 the reference-hardware table (D29). Section references use the form "chapter N §N.n.m".

### Chapter ownership of identifiers

| Identifier | Defined in |
|---|---|
| `FR-<AREA>-nnn`, `NFR-nnn` | chapter 4 |
| `ADR-nnn` | chapter 5 (summary in chapter 0 §0.2) |
| `D<n>` cross-chapter decisions | chapter 0 §0.11 (D1–D32), cited by chapters |
| Diagnostic-code format `<FAMILY>(-<QUALIFIER>)?-<nnnn>` and the family list | chapter 5 §5.7 (D2); each family's registry: `CNT` chapter 8 §8.5, `SYN`/`DEC`/`IO`/`BR` chapter 7, `VQ` chapter 9, `MDL`/`VER`/`ANA` chapters 6/7, `RND` chapter 10, `GUI` chapter 11, `CLI` chapter 12 §12.1.3 |
| FR → WI traceability (and its inverse) | chapter 4 §4.17; chapter 16 §16.4 `FRs` column |
| Exit codes, `--format`, subcommand list | chapter 12 §12.1.3, §12.1.2, §12.2.0 |
| Keyboard map, settings file | chapter 11 §11.4.3, §11.2.5 |
| Performance budgets and version pins | chapter 14 §14.9.3 / `tests/perf/budgets.json` (D19; chapter 0 §0.7.4 and D5 mirror the headline numbers), chapter 13 §13.1 |
| Reference hardware ids, fixture registry (corpus ids `<source>_<name>`) | chapter 14 §14.9.2, §14.3 (D26, D29) |
| Specification version and changelog | this front matter, `docs/spec/CHANGELOG.md` (chapter 16 §16.2.8, D28) |
| FlatBuffers schemas, `V-nnn` validation rules | chapter 6 |
| `SYN-*`, `DEC-*`, `IO-*`, `BR-*` diagnostics, plugin ABI | chapter 7 |
| `CNT-nnnn` diagnostics | chapter 8 |
| Metric ids, `NormalizationConfig` | chapter 9 |
| Overlay colours and layouts | chapter 10 |
| Panels, shortcuts, `SelectionModel` | chapter 11 |
| `ovt` subcommands, C API, Python API | chapter 12 |
| CMake targets, presets, `third_party/VERSIONS.md` | chapter 13 |
| Definition of done, corpora, oracles | chapter 14 |
| `WI-<phase>.nnn` (defined in chapters 6–15, indexed in 16) | chapter 16 |
| `R-nn` risks, `Qn` open questions, `OI-nn` open items | chapter 17 (§17.1, §17.2, §17.4) |

### Revision history

| Version | Change |
|---|---|
| 0.9 | Draft v1 assembled from the chapter drafts and the backbone. |
| 0.9.2 | Review round 1 (6 reviewers, about 1 000 findings) applied; decisions D1–D18. |
| 0.9.3 | Review round 2 (4 reviewers, about 170 findings) applied; decisions D19–D32. |
| 1.0-rc1 (2026-11-04) | Final consistency pass (cross-chapter requests, dependency table, fixture registry, open items); release candidate for owner review. |
| 1.0-rc2 (2026-11-04) | Accepted answers of the M0.1 `spec-question` issues #2, #3, #4, #6, #8, #11, #13, #15, #17, #20, #22, #25 and #27 folded into chapters 12–16 (see `CHANGELOG.md`). |
| 1.0-rc3 (2026-11-06) | Accepted answers of `spec-question` #119 (synthetic generator reproducibility, chapter 14 §14.3.3) and #123 (licence gate `--corpora` mode, chapter 14 §14.3.4) folded in (see `CHANGELOG.md`). |
| 1.0-rc4 (2026-11-06) | WI-0.552 corpora manifest: `vcs.rev`, `upstream_path`, `note` fields; url-less `unverified` entries for names absent at their origin (see `CHANGELOG.md`). |
| 1.0-rc5 (2026-11-06) | CI build artefacts are uploaded only while the repository is private (chapter 13 §13.7.1; see `CHANGELOG.md`). |
| 1.0-rc6 (2026-11-06) | Editorial: goal sentence (chapter 0 §0.0) and chapter 1 §1.2 reworded as a factual feature-area comparison, with a trademark and non-affiliation note; chapter 3 and Appendix A trimmed to factual feature comparison; UX conventions stated as OVT's own; goal G1 reworded; "Block Presenter" panel renamed "Block Samples"; compare modes use OVT's own names only (see `CHANGELOG.md`). |
| 1.0-rc7 (2026-11-06) | WI-0.012 FFmpeg core flavour also disables the VVC, AAC/HE-AAC, MPEG-4 Part 2 and VC-1 decoders (owner decision on PR #132; see `CHANGELOG.md`). |
| 1.0-rc8 (2026-11-06) | Chapter 14 §14.3.3 recipe 12 uses HM 18.0's `SliceSegmentMode`/`SliceSegmentArgument` and 100-CTU slices with a segment every 40 CTUs, so 720p pictures carry several slices and dependent segments (see `CHANGELOG.md`). |
| 1.0-rc9 (2026-11-06) | Chapter 14 §14.3.3 recipes 19 and 62 spell the aomenc option `--superres-denominator` (see `CHANGELOG.md`). |
| 1.0-rc10 (2026-11-06) | Chapter 14 §14.3.3 recipe 19 uses `--resize-denominator=9` so resize × superres stays within AV1's 2x reference scaling, recipe 68 writes the `--pssh` system ID without dashes, and rule 6 skips recipes whose `manual_download` input is absent (see `CHANGELOG.md`). |
| 1.0-rc11 (2026-11-06) | Chapter 14 §14.3.3 recipe 33 loops its source once so the clean TS lasts 4 s and the 1 s PCR gap fits (see `CHANGELOG.md`). |
| 1.0-rc12 (2026-11-06) | Chapter 14 §14.3.3 recipe 21 encodes its own HEVC with an IDR every 20 frames and 600 ms segments, so the CMAF file has its three segments (see `CHANGELOG.md`). |
| 1.0-rc13 (2026-11-07) | WI-0.012 FFmpeg core flavour also disables the MS-MPEG4 v1–v3 and WMV7/WMV8 decoders, the Microsoft variants of MPEG-4 Part 2 (owner decision; see `CHANGELOG.md`). |
| 1.0-rc14 (2026-11-07) | Chapter 14 §14.3.3 rule 2 and recipes 68, 69: Bento4 outputs get their `mvhd`/`tkhd`/`mdhd` times pinned to 0 by `tools/mp4_pin_times.py` (see `CHANGELOG.md`). |
| 1.0-rc15 (2026-11-07) | WI-0.605: protection of `main` is a repository ruleset managed by `tools/github/apply_protection.sh`; required checks, code-owner review and signed tags follow in later PRs; acceptance test 1 is pending until the CODEOWNERS teams exist (see `CHANGELOG.md`). |
| 1.0-rc16 (2026-11-08) | Golden size cap is 2 MB gzip-compressed (chapter 16 §16.2.7, owner decision; see `CHANGELOG.md`). |
| 1.0-rc17 (2026-11-09) | WI-0.060: §13.5.2 describes how `tools/mac/bundle.sh` builds the bundle layout from the install tree, (see `CHANGELOG.md`). |
| 1.0-rc18 (2026-11-09) | WI-3.300 / `spec-question` #156: the multi-plane view scored by metric plugins is `ovt_MetricPlanes` with `struct_size` (chapter 9 §9.2.2, backbone §0.6.3); `ovt_PlaneView` of `ovt/decode.h` stays single-plane; metric plugins are searched in `OVT_PLUGIN_DIR` and `$OVT_METRIC_PATH` (see `CHANGELOG.md`). |
| 1.0-rc19 (2026-11-12) | Spec-amendment pass 1 (build, test infrastructure, licensing): accepted answers of 41 `spec-question` issues on chapters 13–15 (and the chapter 7, 9, 11 and glossary sentences they touch) folded in (see `CHANGELOG.md`). |
| 1.0-rc20 (2026-11-12) | Owner decision: the default build keeps the pool-covered FFmpeg decoders in the core (`OVT_PATENT_CODECS_IN_CORE=ON`); ADR-015, chapter 13 §13.2.2/§13.3.3/§13.4, chapter 15 POL-PAT-2/3, NFR-033, chapter 17 Q3 amended (see `CHANGELOG.md`). |
| 1.0-rc21 (2026-11-12) | Spec-amendment pass 2 (bitstream and decoders): accepted answers of 32 `spec-question` issues on chapter 7 (and the chapter 0, 4, 6 and 12 sentences they touch) folded in (see `CHANGELOG.md`). |
| 1.0-rc22 (2026-11-13) | Spec-amendment pass 3 (data model, CLI and C API): accepted answers of 22 `spec-question` issues on chapters 6 and 12 (and the chapter 0, 4, 7, 13 and 14 sentences they touch) plus the review nits of #102, #294 and #301 folded in (see `CHANGELOG.md`). |
| 1.0-rc23 (2026-11-13) | Spec-amendment pass 4 (container, metrics, rendering, GUI): accepted answers of 28 `spec-question` issues on chapters 8, 9, 10 and 11 (and the chapter 0, 4, 6 and 14 sentences they touch), including the UVQ 1.5 PyTorch pin in D11, §9.6 and §14.4.14 (see `CHANGELOG.md`). |
| 1.0-rc24 (2026-11-13) | Spec-amendment pass 5 (follow-ups): accepted answers of `spec-question` issues #385, #388, #389, #391 (points 1–3) and #395 (points 1–5), the public-header rulings left open by pass 4 (#209 item 4, #366 item 3, #318), and the WI-0.089 AT3 rewording after the Mac Mini run (see `CHANGELOG.md`). |
| 1.0-rc25 (2026-11-14) | Spec-amendment pass 6: accepted answers of `spec-question` issues #394 (libvmaf host), #407 (SSIM border policy), #408 (AV1 accounting), #402 (timeline arrows), two follow-ups on #323 (box walker: `uuid` with `size < 8`, node bound `CNT-0111`) and the owner-approved acceptance-test changes of #391 point 4 and #395 point 6 (see `CHANGELOG.md`). |
| 1.0-rc26 (2026-11-14) | Corpus fix (#507, #160 follow-up): five ITU conformance ids that were never fetchable are replaced by the streams that exist, the absent AV1 Annex B vector gives way to `syn_av1_annexb`, `ALPHACONFORMANCEG` moves to `slow` (see `CHANGELOG.md`). |
| 1.0-rc27 (2026-11-14) | Spec-amendment pass 7 (unblocking in-flight work items): accepted answers of `spec-question` issues #415, #416, #417, #419, #421, #425, #426, #429, #430, #432, #435, #436, #437, #443, #442, #444, #457, #460, #461, #465, #467, #469, #488, #493 and the WI-1.119 frame-count oracle change (see `CHANGELOG.md`). |
| 1.0-rc28 (2026-11-14) | Spec-amendment pass 8: accepted answers of `spec-question` issues #466, #473, #512, #513 and the corpus placeholder fix #517 (nine never-fetchable manifest entries deleted, manifest sweeps skip placeholders; see `CHANGELOG.md`). |
| 1.0-rc29 (2026-11-14) | Spec-amendment pass 9: accepted answers of `spec-question` issues #495, #498 (see `CHANGELOG.md`). |
| 1.0-rc30 (2026-11-14) | Spec-amendment pass 10: accepted answer of `spec-question` issue #418 (`SYN-H265-0021`, `SYN-H265-0022`; see `CHANGELOG.md`). |
| 1.0-rc31 (2026-11-14) | Spec-amendment pass 11: accepted answers of `spec-question` issues #528 (SEI payload bound, `SYN-H264-0031`, `SYN-H264-0032`, WI-1.110 AT2) #533 (film grain conformance → `SYN-AV1-0015`) and #529 (H.264 `ParameterSets`, `SYN-H264-0010` variants, `SYN-H264-0070`; see `CHANGELOG.md`). |
| 1.0-rc32 (2026-11-15) | Spec-amendment pass 12: accepted answers of `spec-question` issues #159, #366 (wording), #406, #419 (float accumulation wording), #431, #433, #434, #449, #450, #470, #471, #477, #478, #480, #486, #490, #499, #510 (see `CHANGELOG.md`). |
| 1.0-rc33 (2026-11-15) | Spec-amendment pass 13: accepted answers of `spec-question` issues #546 (tile group range → `SYN-GEN-0003`) and #547 (Android `useLegacyPackaging = true`; see `CHANGELOG.md`). |
| 1.0-rc34 (2026-11-15) | Spec-amendment pass 14: accepted answers of `spec-question` issues #550 (Qt Quick accessibility under `QtQuickView`: §13.4.4 item 9, risk R-23), #551 (`SYN-H264-0033`), #552 (`SYN-H264-0071`, WI-1.109 four-way split) and #553 (HM plugin: empty RPS lists at BLA pictures); see `CHANGELOG.md`. |
| 1.0-rc35 (2026-11-15) | Spec-amendment pass 15: accepted answers of `spec-question` issues #558 (WI-1.230 AT1 `expected.demux`), #567 (WI-1.205 fixture, `.derived` siblings, QuickTime `hdlr.name`), #568 (WI-1.111 rule 10, field pairs, AT2), #569 (WI-4.206 PES reassembly and file-based PCR/PTS checks), #570 (WI-1.114 oracle streams, `SYN-H265-0051`/`0052`, CpbCnt) and #566 (WI-3.590 AT3 map conservation); see `CHANGELOG.md`. |
| 1.0-rc36 (2026-11-16) | Spec-amendment pass 16: accepted answers of `spec-question` issue #574 (box registry generation and walk rules; point 1: FileFormatConformance snapshots committed under BSD-3-Clause-Clear on the owner's acceptance of 2026-11-16, MP4RA data held until the owner rules); see `CHANGELOG.md`. |
| 1.0-rc37 (2026-11-16) | Spec-amendment pass 17: accepted answers of `spec-question` issues #589 (WI-2.582 AT1 deferrals, AT3 manual record), #591 (WI-4.109 AV1 MD5 side-file format; bit-rate limits split to part b), #592 (`CNT-1010` owner, `tx3g` flag names), #593 (WI-4.207 timer checks on the PCR axis, 2.3 counting), #596 (recipe 26 non-WPP stream), #597 (WI-2.168 libde265 flags, threading note, record location), #603 (WI-1.572 comparison method, interim driver, four-way split), #606 (verify-mode Diagnostics and heat map in `VerifyReport`, prose; schema fields land with WI-4.106), #605 (WI-2.165 AT2 sums CTU bits exactly, V-020 remainder, a/b split) and #612 (`corpora-cache` refreshes the `fast` set, nightly saves `all`, only `main` saves), records WI-1.109 part d (PR #598) in §7.3.4 rows 22–23, §7.9 `SYN-H264-0071` and WI-4.111, and corrects §8.2.10 to the merged WI-1.203 (PR #579): upstream snapshots fetched at pinned commits, not committed; see `CHANGELOG.md`. |
| 1.0-rc38 (2026-11-16) | Spec-amendment pass 18: accepted answers of `spec-question` issues #627 (`bench_render_*` input is the fixture's coded size, synthetic picture), #632 (CVD bi colour `#0072B2`), #633 (`LegendShape`, halo tables, one legend entry per colour), #634 (interim contrast backgrounds), #629 (WI-4.210 segment numbering, selector grammar, ranged cache key, provenance JSON), #630 (no `bytes_read()` in the public header, `PagedSpan` moves to WI-1.209) and #631 (unexpanded `moof` contents, `mfro` hint), the WI-1.415 GPU-time partial (follow-up #628) and the WI-1.204 Produces line; see `CHANGELOG.md`. |
| 1.0-rc39 (2026-11-16) | Spec-amendment pass 19: accepted answers of `spec-question` issues #637 (WI-1.412 `mDCV`/`cLLi`, lodepng native fields, null view, `ViewPng8` alpha, AT2 via WI-1.575), #638 (lodepng stays vendored; §13.3.1 row), #644 (intra direction stroke geometry and mode-to-angle mapping), #645 (K = 9 arrow vertices, AT1 golden at the 1080p viewport, wall-time arrow-pass counter), #650 (WI-1.456 interim probe/selection markers, `frameWanted` hook), #624 (WI-2.432 AT1 CTU vertex count under the left/top rule), #648 (labels on leaf blocks, decimal-only sample values, atlas height), #642 (WI-1.121 part c creates `ovt/index.h`) and #641 (WI-1.121 a/b/c split, index details); see `CHANGELOG.md`. |
| 1.0-rc40 (2026-11-16) | Spec-amendment pass 20: accepted answer of `spec-question` issue #662 (H.265 `end_of_seq_rbsp` / `end_of_bitstream_rbsp` trailing bytes, new Info `SYN-H265-0033`), the WI-1.415 partial (PR #625, GPU times in #628), the WI-1.204 landing (PR #639, Produces line, `PagedSpan` owned by WI-1.209) and the WI-1.209 one-`BitReader`-per-window reading of `PagedSpan` tables (PR #681); see `CHANGELOG.md`. |
| 1.0-rc41 (2026-11-16) | WI-4.106 part a / `spec-question` #606: `VerifyFrame` gains `diagnostics`, `mismatch_map_blob`, `mismatch_cells_w`/`mismatch_cells_h` and `VerifyReport` gains `diagnostics` (chapter 6 §6.3, additive fields); the §7.6.4 / §12.2.12 prose follows in spec pass 17; see `CHANGELOG.md`. |
