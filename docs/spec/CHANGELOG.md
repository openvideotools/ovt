# Specification changelog

Newest first. Format and versioning rules: chapter 16 §16.2.8.

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
