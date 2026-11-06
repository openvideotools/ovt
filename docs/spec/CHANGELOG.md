# Specification changelog

Newest first. Format and versioning rules: chapter 16 §16.2.8.

## 1.0.0-rc3 — 2026-11-06

Accepted answer of `spec-question` #119 (coordinator, under the owner's level-3 autonomy). MINOR-level change (recipe commands of WI-0.553); no WI is re-opened.

- 14 §14.3.3 — reproducibility adjustments of the synthetic recipes: pinned encoder thread counts, `MP4Box -for-test`, `mkvmerge --no-date --deterministic`, seeded lavfi sources, fixed Bento4 IV, `-strict -1` for 10-bit Y4M, `-framerate 30` for raw Annex B, tool paths and arguments of recipes 2, 16, 22, 27, 28, 31 and 68; `check_fast_subset.py` greps `docs/spec/` (#119) — WI-0.553

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
