# 15 — Licensing, patents and governance

> **Status:** normative chapter of the OVT Requirements & Architecture Specification. Conforms to `00-backbone.md` ADR-002 (licence split), ADR-015 (runtime-loaded decoder plugins), ADR-016 (DCO, no CLA, trademark, distribution channels) and §0.4 (`/LICENSES/`, `THIRD_PARTY_NOTICES.md`, `tools/` licence gate). Source: Appendix E (licence facts with URLs).
>
> **This chapter is not legal advice.** It records the project's *policy* and the engineering mechanisms that enforce it. The owner SHOULD have a lawyer review §15.1, §15.5 and §15.7 before the first public release, and MUST do so before registering a trademark or accepting a corporate contribution agreement of any kind.

---

## 15.1 Licence structure per directory

| Directory (§0.4) | SPDX licence expression | Why | Allowed inbound code |
|---|---|---|---|
| `core/` (except `core/include/ovt/abi/`) | `LGPL-2.1-or-later` | Embeddable data layer for encoder vendors' CI; relicensable to GPL-2/3 as needed (ADR-002). | Own code; permissive (MIT/BSD/Apache-2.0*/zlib/ISC); LGPL-2.1+ code. **No GPL code, no YUView code, no LGPL-3-only code.** (*Apache-2.0 is incompatible with LGPL-2.1-*only* and compatible with LGPL-3.0; combining Apache-2.0 libraries with `core/` works only because `core/` is `LGPL-2.1-or-later`, so the combined binary is effectively distributed under LGPL-3.0 terms and GPL-2.0-only downstream consumers of such a binary are unsupported — see §15.3. Copied Apache-2.0 *source* in `core/` needs a NOTICE entry; prefer not to copy.) |
| `core/include/ovt/abi/` | `BSD-2-Clause` | Lets proprietary/other-licence decoder vendors implement plugins without copyleft on the header (ADR-002). | Own code only; no third-party code. |
| `core/schema/*.fbs` | `BSD-2-Clause` | Schemas are part of the plugin interface and must be reusable by any consumer (Kotlin, JS, Python). | Own code only. |
| `render/` | `LGPL-2.1-or-later` | Same reasoning as core; links libplacebo (LGPL-2.1+). | As for `core/`. |
| `plugins/ovt-plugin-aom/` | `BSD-2-Clause` (own code) — binary also subject to `LicenseRef-AOM-Patent-License-1.0` | Follows libaom; AOM patent licence must accompany binaries. | Own code; libaom. |
| `plugins/ovt-plugin-de265/` | `LGPL-3.0-or-later` | Statically links libde265 (LGPL-3.0+); patch files are LGPL-3.0-or-later too (derivative of libde265). | Own code; libde265. |
| `plugins/ovt-plugin-ffmpeg/` | `LGPL-2.1-or-later` (becomes `GPL-2.0-or-later` when built with `OVT_FFMPEG_GPL=ON`) | Private static FFmpeg (chapter 13 §13.3.3). | Own code; FFmpeg public API only. |
| `plugins/ovt-plugin-hm/` | `BSD-3-Clause` | Follows HM. | Own code; HM. |
| `cli/` | `GPL-3.0-or-later` | Application code; may use GPL libraries and YUView-derived code (ADR-002). | Anything GPL-3-compatible. |
| `app/` | `GPL-3.0-or-later` | Qt GPL-only modules (Qt Graphs), KDDockWidgets (GPL), YUView-derived code. | Anything GPL-3-compatible. |
| `android/` | `GPL-3.0-or-later` | Part of the application. | Anything GPL-3-compatible. |
| `python/` | `GPL-3.0-or-later` | Application-level bindings; wheel bundles GPL-compatible deps only. | Anything GPL-3-compatible. |
| `tools/`, `tests/`, `.github/`, `cmake/`, `third_party/*.cmake` | `LGPL-2.1-or-later` | Build and test infrastructure must be reusable by downstream LGPL consumers of core. | Own code; permissive. |
| `docs/` | `CC-BY-SA-4.0` for prose; code snippets in docs are `BSD-2-Clause` (stated in `docs/LICENSE.md`) | Documentation is not software; share-alike keeps derived docs open. | — |
| `docs/spec/` | `CC-BY-4.0` (owner decision of 2026-11-04, issue #20; a `docs/spec/**/*.md` annotation in `REUSE.toml` after the `**/*.md` one) | Other implementations may reuse the specification's text without share-alike. | — |
| `third_party/patches/`, `plugins/*/patches/` | licence of the patched project (stated in the patch header) | A patch is a derivative work of the upstream project. | — |
| `/LICENSES/` | full texts: `LGPL-2.1-or-later.txt`, `GPL-3.0-or-later.txt`, `BSD-2-Clause.txt`, `BSD-3-Clause.txt`, `LGPL-3.0-or-later.txt`, `CC-BY-4.0.txt`, `CC-BY-SA-4.0.txt`, `LicenseRef-AOM-Patent-License-1.0.txt` (REUSE layout: https://reuse.software/spec/) | | |
| `/LICENSE` | `GPL-3.0-or-later` text with a preamble paragraph: "Open Video Tools is distributed as a whole under GPL-3.0-or-later. Individual directories carry more permissive licences; see `LICENSES/`, the SPDX header of each file, and `docs/licensing.md`." | The combined work (any official binary containing `app/` or `cli/`) is GPL-3. | |

**Requirements realised by this chapter.** Requirement ids are defined only in chapter 04; this table copies chapter 04's sentences and names what realises them here. Ids in the `FR-<AREA>-1nn` range were added to chapter 04 from this chapter's review. The CI job is `license-gate` (chapter 13 §13.7.1) and the notices file is the repository-root `THIRD_PARTY_NOTICES.md` (chapter 0 §0.4).

| ID | Requirement (chapter 04 text) | Realised by | Acceptance test (chapter 04 text) |
|---|---|---|---|
| NFR-031 | CI MUST run a licence gate that scans every dependency (vcpkg manifest, `third_party/`, Gradle, Python) and fails on any component licensed AGPL, SSPL, BUSL, Commons Clause or non-commercial, and `THIRD_PARTY_NOTICES.md` (repository root, generated) MUST be regenerated on every build. | §15.4 (`tools/license_gate.py`, `tools/license_policy.toml`), §15.6 (`tools/gen_notices.py`, target `ovt_notices`), WI-0.602, WI-0.603; CI wiring chapter 13 WI-0.032 | Given the `license-gate` job with `tools/license_gate.py`, when a test dependency with `license: AGPL-3.0` is added to `vcpkg.json` on a branch, then the job fails naming the dependency; on `main`, then it passes and `THIRD_PARTY_NOTICES.md` lists every dependency with its SPDX id. |
| NFR-032 | Each source file MUST carry an SPDX licence header matching its directory's licence per ADR-002 (`core/`, `render/` LGPL-2.1-or-later; `core/include/ovt/abi/` BSD-2-Clause; `app/`, `cli/`, `android/`, `python/` GPL-3.0-or-later; plugins per their own `LICENSE`). | §15.1 table, §15.1.1 boilerplate, `tools/spdx_header.py`, `tools/check_spdx_dirs.py`, WI-0.600, WI-0.601 | Given the repository, when `reuse lint` runs, then it reports compliance with the REUSE specification. |
| NFR-033 | Official builds MUST honour ADR-015: AVC/HEVC *analysis* decoder components MUST be separate runtime-loaded libraries (the core FFmpeg flavour's playback decoders follow `OVT_PATENT_CODECS_IN_CORE`, ON by default since the owner decision of 2026-11-12), and a build with `OVT_BUILD_PLUGIN_FFMPEG=OFF` (chapter 13 §13.2.2; the GPL flavour is selected by `OVT_FFMPEG_GPL`/`OVT_FFMPEG_GPL_PLUGIN_ONLY`) MUST still produce a fully functional AV1 product with the AOM PATENTS notice in the About dialog. | §15.5.2 POL-PAT-2/3/6, §15.5.4 distribution matrix, `[distribution]` table of §15.4.2, WI-0.602 step 5, WI-0.603 | Given a build with the option off, when `ovt plugins --format json` runs, then only `aom`, `dav1d` and `vmaf` components are listed and `ovt analyze corpus/av1/gop.ivf --frames 0` succeeds while `ovt dump corpus/h264/gop.mp4` exits 4 with `DEC-PLUGIN-0001 decoder plugin not installed (h264)` (chapter 7 §7.9.4). |
| FR-GEN-010 | The CLI and GUI MUST report the same version string (`<major>.<minor>.<patch>+<git-sha>`), the list of loaded plugins with their `id`, `version` and `license`, and the versions of FFmpeg, libaom, dav1d, libde265, libvmaf, libplacebo and Qt they were built against. | §15.6 (About panel and `ovt --version --format json` point to `THIRD_PARTY_NOTICES.md`) | Given a build, when `ovt --version --format json` runs and the About dialog is opened in `ovt-studio`, then the two `plugins[]` and `third_party[]` lists are identical. |
| FR-GEN-105 | Every source file (`.c .h .cpp .hpp .fbs .py .kt .qml .js .cmake .sh .toml .yml .yaml .rb`, `CMakeLists.txt`) MUST carry an SPDX header whose licence equals its directory's licence per chapter 15 §15.1.1. | §15.1.1, `tools/spdx_header.py`, `tools/check_spdx_dirs.py`, WI-0.601 | Given the repository, when `reuse lint` runs in the `lint` CI job, then it reports compliance; when `tools/check_spdx_dirs.py` runs, then no file's SPDX id differs from its directory's policy. |
| FR-GEN-106 | No file under `core/`, `render/`, `core/include/ovt/abi/` or `core/schema/` MAY include, link or copy code whose licence is GPL (any version), LGPL-3.0-only, or any licence not in the LGPL-2.1 allow-list of `tools/license_policy.toml` (chapter 15 §15.3, §15.4.2). | §15.3 matrix, §15.4.2 algorithm step 4 (`E-LINK`), WI-0.602 | Given the `license-gate` job, when it runs on a release preset, then the `ovt_core` link graph contains only `LGPL-2.1-or-later`-compatible libraries and the job passes; given a fixture where `ovt_core` links a GPL-3.0-only library, then the job fails with `E-LINK`. |
| FR-GEN-107 | Binary files that cannot carry headers (icons, test vectors, fonts) MUST be covered by a `.license` sidecar file or a `REUSE.toml` entry. | WI-0.600 (`REUSE.toml` annotations) | Given the repository, when `reuse lint` runs, then no file is reported as missing licensing information. |
| FR-GEN-108 | A contribution that changes a file's licence or moves code between directories with different licences MUST be a separate PR titled `licence:` and reviewed by two maintainers listed in `MAINTAINERS.md` as licence reviewers (chapter 15 §15.7.3). | §15.7.3 CODEOWNERS, WI-0.605 | Given a PR that changes an `SPDX-License-Identifier` line, when the `lint` job runs, then it fails unless the title starts with `licence:`; given such a PR, when review is requested, then CODEOWNERS requires two approvals from the licence-reviewer team. |

### 15.1.1 Header boilerplate (exact text; `tools/spdx_header.py <path>` inserts it)

C/C++ (`core/`, `render/`, `tools/`-adjacent C++):

```c
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
```

Plugin ABI headers and schemas:

```c
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
```

```fbs
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: BSD-2-Clause
```

Application code (`cli/`, `app/`, `android/`, `python/`), C++/Kotlin/QML/Python respectively:

```cpp
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
```
```kotlin
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
```
```python
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
```

Files derived from another project add a second copyright line and keep the original notice in the file body, e.g. for YUView-derived code in `app/`:

```cpp
// SPDX-FileCopyrightText: 2015-2024 Institut für Nachrichtentechnik, RWTH Aachen University (YUView)
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from YUView <path in upstream>, commit <sha>; see THIRD_PARTY_NOTICES.md.
```

Plugin directories use `LGPL-3.0-or-later` (de265), `BSD-2-Clause` (aom), `BSD-3-Clause` (hm), `LGPL-2.1-or-later` (ffmpeg) accordingly. "The Open Video Tools contributors" is used instead of individual names; authorship is tracked by git history and the DCO sign-off (§15.7). The year is the year of file creation and is never updated in bulk.

---

## 15.2 Dependency licence table

Linkage column refers to the official default build (chapter 13); "static→core" means statically absorbed into `libovt_core`, which is itself LGPL-2.1-or-later and dynamically linked by the GPL application. Obligations summarise what the release process MUST do (chapter 13 §13.8 and this chapter's §15.6 automate them).

| Dependency | Licence (SPDX) | Linked into | Linkage | Obligations in OVT releases |
|---|---|---|---|---|
| FFmpeg (core flavour) | `LGPL-2.1-or-later` | `ovt_core` | dynamic (`libav*_ovt`) | Ship exact source + configure flags (Corresponding Source archive); credit "This software uses libraries from the FFmpeg project under the LGPLv2.1"; no `--enable-nonfree`; FFmpeg is a trademark of Fabrice Bellard (notice). https://www.ffmpeg.org/legal.html |
| FFmpeg (plugin flavour) | `LGPL-2.1-or-later` (or `GPL-2.0-or-later` with `--enable-gpl`) | `ovt_plugin_ffmpeg` | static, hidden | Same; plugin is distributed separately (codec pack); licence file of the plugin states the flavour. |
| libaom | `BSD-2-Clause` + `LicenseRef-AOM-Patent-License-1.0` | `ovt_plugin_aom` | static | Keep the BSD-2-Clause notice (the only legal notice duty — the AOM Patent License 1.0 is a grant with defensive termination and has no redistribution-notice clause); **ship libaom's `LICENSE` and `PATENTS` texts with every binary** as the project's own policy (POL-PAT-6) so users see the terms they rely on; note defensive termination. https://aomedia.org/license/patent-license/ |
| dav1d | `BSD-2-Clause` | FFmpeg core flavour | static into libavcodec | Notice. |
| libde265 | `LGPL-3.0-or-later` | `ovt_plugin_de265` | static | Provide source incl. our patch, with a prominent modification notice (GPLv3 §5(a), incorporated by LGPL-3.0 §0 — LGPL-3.0 has no §2(a); the §2(a) notice clause is LGPL-2.1's and applies to the FFmpeg flavours); allow relinking (plugin is a self-contained module replaced as a whole, satisfying LGPL-3.0 §4(d)(1)); patch header `Upstream-Status`. Patent warning in its README reproduced in notices. |
| HM | `BSD-3-Clause` | `ovt_plugin_hm` | static | Notice (no endorsement clause). |
| libvmaf | `BSD-2-Clause-Patent` | `ovt_core` | static→core | Notice; patent grant is inbound only. |
| libplacebo | `LGPL-2.1-or-later` | `ovt_render` | static→render | Source + relink ability: `ovt_render` is itself LGPL and shipped as a shared library; static absorption of LGPL-2.1 into an LGPL-2.1 shared lib is fine; provide object files/`.a` on request? Not needed: full source + build scripts suffice under LGPL-2.1 §6(a) for the *combined* work as our library is LGPL itself. |
| zimg | `WTFPL` (public-domain-like; SPDX `WTFPL`) | `ovt_core` | static→core | None; notice for courtesy. |
| FlatBuffers | `Apache-2.0` | all | static (headers + lib) | Keep `NOTICE`; Apache-2.0 is GPL-3-compatible but **not** GPL-2-only-compatible — fine here (we never build GPL-2-only binaries). |
| zstd | `BSD-3-Clause OR GPL-2.0-only` | `ovt_core` | static→core | Choose BSD-3; notice. |
| fmt | `MIT` (with binary-exception clause) | all | static | Notice. |
| spdlog | `MIT` | all | static | Notice. |
| nlohmann-json | `MIT` | all | header-only | Notice. |
| Highway | `Apache-2.0 OR BSD-3-Clause` | `ovt_core` | static→core | Choose BSD-3; notice. |
| CLI11 | `BSD-3-Clause` | `ovt_cli` | header-only | Notice. |
| xxHash | `BSD-2-Clause` | `ovt_core` | static→core | Notice. |
| Catch2 | `BSL-1.0` (Boost Software Licence — **not** BUSL) | tests only | static | None for binaries (tests not shipped). The gate distinguishes `BSL-1.0` (allowed) from `BUSL-1.1` (forbidden). |
| pybind11 | `BSD-3-Clause` | `ovt_py` | header-only | Notice in wheel. |
| ONNX Runtime | `MIT` | `ovt_core` (UVQ host) | static or shared | Notice; bundled third-party notices of ORT (`ThirdPartyNotices.txt`) included. |
| UVQ models (google/uvq) | `Apache-2.0` | downloaded at build/first use | data | Keep LICENSE with model files; not modified. |
| VMAF models | `BSD-2-Clause-Patent` (in libvmaf) | built-in | data | Covered by libvmaf notice. |
| Qt 6 (qtbase, qtdeclarative, qtsvg, qtshadertools, qtimageformats) | `LGPL-3.0-only` (and GPL-2/3 options; we use LGPL-3) | `ovt_studio` | dynamic frameworks | Source/relink: Qt is dynamically linked and the app is GPL-3 — LGPL-3 §4 satisfied by shipping GPL source + build scripts; keep Qt licence texts; ship `qt.conf` not modifying Qt itself. Static Qt is **not** an option: LGPL-3 §4(d) requires that the user can replace the Qt library, which the project satisfies only by dynamic linking (chapter 13 §13.5.2, §13.6.4). https://www.qt.io/licensing/open-source-lgpl-obligations |
| Qt Graphs (qtgraphs) | `GPL-3.0-only` (open-source edition) | `ovt_studio` | dynamic | Application is GPL-3-or-later → compatible (GPL-3-only ∧ GPL-3-or-later combine as GPL-3). **Must never be used by `core/`/`render/`/`cli/`.** |
| KDDockWidgets | `GPL-2.0-only OR GPL-3.0-only` | `ovt_studio` | static/dynamic | Choose GPL-3.0-only; notice. |
| QHexView | `MIT` | `ovt_studio` | vendored | Notice. |
| MoltenVK | `Apache-2.0` | `ovt_render` (macOS) | dynamic dylib | Notice; `LICENSE` in bundle. |
| Vulkan headers/loader | `Apache-2.0` / `MIT` | `ovt_render` | headers / dynamic | Notice. |
| glslang | `BSD-3-Clause AND Apache-2.0 AND MIT AND (GPL-3.0-only WITH Bison-exception-2.2)` (the Bison-generated parser only) | `ovt_render` via libplacebo | static→render | Notice; the `GPL-3.0-only WITH Bison-exception-2.2` part covers only the Bison-generated parser source, whose exception permits any licence for the generated output — allowed for LGPL render (the exception removes GPL obligations on the generated code; the shipped glslang library itself is BSD-3/Apache-2.0/MIT). The gate accepts the `WITH` expression through the `[allow.any]` id `GPL-3.0-only WITH Bison-exception-2.2` (SPDX 3.x spelling; `GPL-3.0-with-bison-exception` is deprecated). |
| Little-CMS (lcms2) | `MIT` | `ovt_render` via libplacebo | static | Notice. |
| libpng, zlib | `libpng-2.0`, `Zlib` | tests / FFmpeg | static | Notice. |
| Android: AndroidX, Kotlin stdlib, Play Feature Delivery | `Apache-2.0` | `android/` | AAR | Notice; Play library only in `play` flavour (F-Droid forbids proprietary deps; Feature Delivery is Apache-2.0 but talks to Play services — flagged as F-Droid anti-feature only if present; hence excluded from `foss`). |
| Android NDK / SDK | NDK: various permissive + Android SDK licence (tool, not redistributed) | build only | — | None in binaries except `libc++_shared.so` (`Apache-2.0 WITH LLVM-exception`): notice. |
| Qt Gradle plugin 1.2, `androiddeployqt` | `LGPL-3.0-only`/`GPL-3.0-only` tools | build only | — | None. |
| vcpkg (tool, ports) | `MIT` | build only | — | None; ports' *targets* are covered above. |
| Conformance corpora (ITU-T JCT-VC, JVT, AOM test vectors, Netflix VMAF vectors) | various (ITU test material terms; AOM test vectors BSD-like; Netflix vectors "for VMAF testing") | tests only, never redistributed | downloads | Manifests only (`tests/corpora/*.json`); CI caches are private. |
| YUView (code reuse in `app/`, phases 2–4) | `GPL-3.0-only` | `ovt_studio` | copied source | Keep copyright; mark derived files (§15.1.1); list in notices with upstream commit. Combined work GPL-3. **Never in `core/`.** |
| aomanalyzer (design/code reuse) | `MIT` | `app/` (if any code copied) | copied source | Notice. |

Explicitly **excluded** (gate deny-list examples): Bento4 (`GPL-2.0-only`, incompatible with GPL-3), GitlHEVCAnalyzer (non-commercial clause), hevcesbrowser (`GPL-2.0-only`), Ghostscript (`AGPL-3.0`), iText (`AGPL`), MinIO/Grafana (`AGPL`), MongoDB/Elastic (`SSPL`), HashiCorp-style `BUSL-1.1`, any `CC-BY-NC*`, `LicenseRef-Proprietary`, Qt commercial-only modules, FFmpeg `--enable-nonfree` components (fdk-aac), x264/x265 in the LGPL core (they would force GPL; only allowed inside the GPL-flagged plugin flavour).

---

## 15.3 Compatibility matrix (condensed from Appendix E.2)

Rows: licence of an inbound component. Columns: where it may go in OVT.

| Inbound component licence | `core/`, `render/` (LGPL-2.1+) | `abi/`, `schema/` (BSD-2) | plugins (own licence per dir) | `cli/`, `app/`, `android/`, `python/` (GPL-3+) | Official combined binary (GPL-3) |
|---|---|---|---|---|---|
| Public domain / CC0 / Unlicense / WTFPL | ✔ | ✔ (copy with note) | ✔ | ✔ | ✔ |
| MIT / BSD-2 / BSD-3 / ISC / Zlib / BSL-1.0 | ✔ | ✘ (no third-party code in ABI headers — policy, not law) | ✔ | ✔ | ✔ |
| Apache-2.0 | ✔ link (combined work distributed under LGPL-3.0 terms because `core/` is "or-later"; GPL-2.0-only downstreams unsupported — FSF: Apache-2.0 is incompatible with (L)GPL-2.x-only, compatible with (L)GPL-3.0); ✘ copy source (avoid NOTICE burden; needs 2 reviewers) | ✘ | ✔ | ✔ | ✔ (GPL-3 compatible) |
| MPL-2.0 | ✔ link (file-level copyleft stays on MPL files) | ✘ | ✔ | ✔ | ✔ |
| LGPL-2.1-or-later | ✔ | ✘ | ✔ | ✔ | ✔ |
| LGPL-2.1-only | ✔ (link); copy source only as LGPL-2.1-only files (allowed inside an "or-later" library? No — would pin the library; **✘ copy**) | ✘ | ✔ | ✔ (upgrades to GPL-3 §3? LGPL-2.1 §3 allows conversion to GPL-2+; GPL-2+ combines with GPL-3) | ✔ |
| LGPL-3.0-or-later / -only | **✘** (would make core LGPL-3) | ✘ | ✔ (de265 plugin) | ✔ | ✔ |
| GPL-2.0-or-later | ✘ | ✘ | ✔ only in a plugin declared GPL (`OVT_FFMPEG_GPL`) — then **not** in default download | ✔ (as GPL-3) | ✔ |
| GPL-2.0-only | ✘ | ✘ | ✘ | **✘** (incompatible with GPL-3) | ✘ |
| GPL-3.0-only / -or-later | ✘ | ✘ | ✘ (plugins are loaded by the LGPL core; a GPL plugin would be loaded as user's combination — allowed for *third parties*, not shipped by us except via GPL flag) | ✔ | ✔ |
| AGPL-3.0 (any) | **✘ forbidden** | ✘ | ✘ | **✘ forbidden** (owner policy, ADR-002) | ✘ |
| SSPL, BUSL-1.1, Elastic-2.0, Commons Clause, CC-BY-NC-*, "non-commercial", "modified Apache with NC clause" (GitlHEVCAnalyzer) | ✘ | ✘ | ✘ | ✘ | ✘ |
| Proprietary / no licence / "all rights reserved" | ✘ | ✘ | ✘ (a *user-installed* proprietary plugin is the user's combination; we never ship it) | ✘ | ✘ |
| CC-BY-4.0 / CC-BY-SA-4.0 | docs only | — | — | docs only | ✔ (docs, not code) |

Reading rule for implementers: when adding a dependency, find its SPDX id (from its `LICENSE`, vcpkg `copyright`, or `VERSIONS.md`), look up the row, check the column for the directory you are touching. If the cell is ✘ or the licence is not in the table, stop and open an issue labelled `licensing` — do not add the code.

---

## 15.4 AGPL/SSPL/BUSL/non-commercial exclusion policy and CI gate

### 15.4.1 Policy

1. **Forbidden anywhere in the dependency tree** (build-time, run-time, test-time, documentation tooling that produces shipped artefacts): `AGPL-1.0*`, `AGPL-3.0*`, `SSPL-1.0`, `BUSL-1.1`, `Elastic-2.0`, `Commons-Clause`, `CC-BY-NC*`, `CC-BY-NC-SA*`, `CC-BY-NC-ND*`, any `LicenseRef-*` containing `NonCommercial`/`NC`/`Proprietary`/`Evaluation`, `JSON` (the "Good, not Evil" clause), `Hippocratic-*`, unknown/unparseable licences (`Beerware` is permissive and **allowed**, listed in `[allow.any]`). Test-only tooling under AGPL (e.g. a reporting dashboard) is also forbidden because the owner's rule is "anywhere in the dependency tree" and because test tools migrate into release scripts.
2. **Forbidden in `core/`/`render/` link graph**: any GPL, LGPL-3*, or GPL-with-exception where the exception does not cover our use.
3. **Forbidden in the official default download**: AVC/HEVC decoder binaries (§15.5) — this is a *distribution* rule enforced by `tools/release/manifest.toml` (schema chapter 13 §13.8.1, owned by chapter 13 WI-0.036), not a licence rule, but the gate checks it too.
4. **Allowed with record**: everything in §15.2; new permissive or weak-copyleft licences need a one-line PR to `tools/license_policy.toml` reviewed by a licence reviewer.

### 15.4.2 Gate specification (`tools/license_gate.py`, LGPL-2.1-or-later, Python ≥ 3.11, no third-party imports except `tomllib`/`json`/`re`; run with `python -I`)

**Inputs (all optional except `--policy`; missing inputs are reported as `skipped`; in CI the job passes `--require-all`, which demands every input whose producing WI has been merged — `--release-manifest` becomes required when chapter 13 WI-0.036 lands in M0.5; before that the distribution check (step 6) is `skipped`):**

| Flag | Source | What is read |
|---|---|---|
| `--policy tools/license_policy.toml` | repo | allow/deny lists (below) |
| `--versions third_party/VERSIONS.md` | repo | the `Licence` column of each row (SPDX expression) |
| `--vcpkg build/vcpkg_installed/<triplet>` | build tree | each `share/<port>/copyright` and `vcpkg.spdx.json` (`licenseConcluded`/`licenseDeclared`) |
| `--manifest build/<preset>/license_manifest.json` | build tree (chapter 13 target `ovt_license_manifest`; JSON schema chapter 13 §13.2.3) | `targets[]` → own `spdx` → `links[]` (direct and transitive link libraries with `name`, `source`, `version`, `spdx`, `linkage`, `patched`; from `ovt_tp_declare` and `ovt_set_license`) |
| `--gradle android/app/build/reports/dependency-license/index.json` | `com.github.jk1.dependency-license-report` Gradle plugin 2.9 (verified against its `JsonReportRenderer`; configured with `renderers = [JsonReportRenderer('index.json')]` for the `fossRelease` and `playRelease` configurations) | Android AAR/JAR licences: `dependencies[].moduleName`, `moduleVersion`, `moduleLicense` (free-text licence name, mapped to SPDX through `[gradle_license_names]` in the policy), `moduleLicenseUrl`; `importedModules[]` MUST be empty |
| `--pip python/requirements.txt python/requirements-dev.txt` | `pip-licenses --format=json` output or on-the-fly `importlib.metadata` in the wheel build venv | Python dependencies |
| `--release-manifest tools/release/manifest.toml` | repo (schema chapter 13 §13.8.1; file, schema module `tools/release/manifest_schema.py` and the `[licenses]` table are all owned by chapter 13 WI-0.036, which imports this gate as a module for `collect.py --check-licenses`) | `artifacts.<id>.components`, `default_download`, `codec_pack`; the gate writes nothing here |
| `--sources` | repo walk | `SPDX-License-Identifier` of every source file vs. directory policy (`tools/check_spdx_dirs.py` logic merged here) |

**Policy file `tools/license_policy.toml` (normative skeleton):**

```toml
# tools/license_policy.toml — one key per line (valid TOML 1.0; read with tomllib)
schema_version = 1

[deny]                      # regex on SPDX ids (case-insensitive); any match anywhere fails
patterns = [
  "^AGPL", "^SSPL", "^BUSL", "^Elastic-2", "Commons-Clause", "^CC-BY-NC", "NonCommercial",
  "LicenseRef-.*(NC|Proprietary|Evaluation)", "^JSON$", "^Hippocratic",
]

[allow]
# Most-permissive-first ordering used to resolve SPDX `OR` expressions (algorithm step 1).
# An `OR` alternative whose id is not in this list cannot be chosen; if no alternative is listed → E-UNKNOWN.
permissiveness = [
  "0BSD", "Unlicense", "CC0-1.0", "WTFPL", "MIT-0", "MIT", "ISC", "BSD-2-Clause", "BSD-3-Clause", "BSD-2-Clause-Patent",
  "Zlib", "libpng-2.0", "BSL-1.0", "NCSA", "Beerware", "Apache-2.0", "Apache-2.0 WITH LLVM-exception", "MPL-2.0",
  "LGPL-2.1-or-later", "LGPL-2.1-only", "LGPL-3.0-or-later", "LGPL-3.0-only",
  "GPL-2.0-or-later", "GPL-3.0-or-later", "GPL-3.0-only", "GPL-2.0-only",
]

[allow.any]                 # permitted in any directory/target
ids = [
  "MIT", "MIT-0", "BSD-2-Clause", "BSD-3-Clause", "BSD-2-Clause-Patent", "ISC", "Zlib", "libpng-2.0", "BSL-1.0",
  "Apache-2.0", "Apache-2.0 WITH LLVM-exception", "MPL-2.0", "CC0-1.0", "Unlicense", "0BSD",
  # WTFPL (zimg): permissive, not OSI-approved; some corporate scanners flag it — allowed by owner decision, record the hit in the inventory as "non-OSI permissive".
  "WTFPL",
  "Beerware",
  "LGPL-2.1-or-later", "LGPL-2.1-only", "LicenseRef-AOM-Patent-License-1.0",
  # SPDX 3.x spellings; the deprecated `GPL-3.0-with-bison-exception` / `GPL-3.0-with-autoconf-exception` ids are rejected.
  "GPL-3.0-only WITH Bison-exception-2.2",
  "GPL-3.0-or-later WITH Autoconf-exception-3.0",
  "NCSA", "OFL-1.1", "CC-BY-4.0", "CC-BY-SA-4.0",
]

[allow.gpl_targets]         # additionally permitted for targets whose own licence is GPL-3.0-or-later (cli, app, android, python)
ids = ["GPL-3.0-only", "GPL-3.0-or-later", "GPL-2.0-or-later", "LGPL-3.0-only", "LGPL-3.0-or-later"]
# `GPL-2.0-only OR GPL-3.0-only` (KDDockWidgets) is an expression, not an id: the OR rule picks GPL-3.0-only.

[allow.plugin_targets]      # per-plugin own licence must be one of these; a plugin's link graph must be compatible with its own licence
ids = ["BSD-2-Clause", "BSD-3-Clause", "LGPL-2.1-or-later", "LGPL-3.0-or-later", "GPL-2.0-or-later", "GPL-3.0-or-later"]

[targets]                   # own licence per target (must match ovt_set_license)
ovt_core = "LGPL-2.1-or-later"
ovt_render = "LGPL-2.1-or-later"
ovt_cli = "GPL-3.0-or-later"
ovt_cli_lib = "GPL-3.0-or-later"
ovt_studio = "GPL-3.0-or-later"
ovt_py = "GPL-3.0-or-later"
ovt_plugin_aom = "BSD-2-Clause"
ovt_plugin_de265 = "LGPL-3.0-or-later"
ovt_plugin_ffmpeg = "LGPL-2.1-or-later"
ovt_plugin_hm = "BSD-3-Clause"

[directories]               # SPDX id required in file headers; longest prefix wins
"core/include/ovt/abi" = "BSD-2-Clause"
"core/schema" = "BSD-2-Clause"
"core" = "LGPL-2.1-or-later"
"render" = "LGPL-2.1-or-later"
"cli" = "GPL-3.0-or-later"
"app" = "GPL-3.0-or-later"
"android" = "GPL-3.0-or-later"
"python" = "GPL-3.0-or-later"
"plugins/ovt-plugin-aom" = "BSD-2-Clause"
"plugins/ovt-plugin-de265" = "LGPL-3.0-or-later"
"plugins/ovt-plugin-ffmpeg" = "LGPL-2.1-or-later"
"plugins/ovt-plugin-hm" = "BSD-3-Clause"
"tools" = "LGPL-2.1-or-later"
"tests" = "LGPL-2.1-or-later"
"cmake" = "LGPL-2.1-or-later"
"third_party" = "LGPL-2.1-or-later"
"docs" = "CC-BY-SA-4.0"
".github" = "LGPL-2.1-or-later"      # §15.1 lists .github/ with tools/ (issue #22)
"plugins" = "LGPL-2.1-or-later"      # plugins/CMakeLists.txt, the dispatcher — not a plugin; the per-plugin entries above win by longest prefix
"" = "LGPL-2.1-or-later"             # the repository root (CMakeLists.txt, .clang-format, REUSE.toml, …): the shortest prefix of every path

[distribution]              # artefact rules (ADR-015); artefact ids are those of tools/release/manifest.toml (chapter 13 §13.8.1)
default_download_forbidden_plugins = ["ovt_plugin_ffmpeg", "ovt_plugin_de265", "ovt_plugin_hm"]
codec_pack_plugins = ["ovt_plugin_ffmpeg", "ovt_plugin_de265"]
required_notices = ["LICENSES/LicenseRef-AOM-Patent-License-1.0.txt", "THIRD_PARTY_NOTICES.md"]

[vcpkg_overrides]           # ports whose vcpkg.spdx.json says NOASSERTION; reviewed by a licence reviewer
# zimg = "WTFPL"

[gradle_license_names]      # free-text licence names of the Gradle report → SPDX id
"The Apache Software License, Version 2.0" = "Apache-2.0"
"Apache License, Version 2.0" = "Apache-2.0"
"Apache-2.0" = "Apache-2.0"

[[exceptions]]              # explicit, reviewed, expiring; one array-of-tables entry per exception
id = "GPL-2.0-only"
where = "tests/oracles/bento4"
reason = "oracle binary invoked as subprocess, never linked"
expires = "2027-06-30"
approved_by = ["<maintainer>", "<maintainer>"]
```

**Algorithm.**

1. Parse every SPDX expression with a small recursive parser supporting `AND`, `OR`, `WITH`, parentheses (a `WITH` expression is one id for all later steps). For `OR`, the gate chooses the alternative that appears first in `[allow].permissiveness` and is allowed for the target class, and records the choice (`chosen`); an alternative absent from `permissiveness` is never chosen; if no alternative can be chosen, fail with `E-UNKNOWN`.
2. Deny check: any id (in any alternative actually chosen, and also any alternative at all when `--strict-or`, used in CI) matching `[deny]` → **FAIL** with `E-DENY <where> <id>`.
3. Unknown check: id not in any allow list and not in `[exceptions]` → **FAIL** `E-UNKNOWN`.
4. Target compatibility: for each target in `--manifest`, compute the set of licences in its transitive link graph (static and dynamic; vcpkg ports are resolved through `--vcpkg`), and verify every id is allowed for the target class (`any` for LGPL targets; `any ∪ gpl_targets` for GPL targets; per-plugin check that the plugin's own licence is ≥ the strongest copyleft in its graph: BSD plugin may not link LGPL; LGPL-2.1 plugin may not link LGPL-3/GPL; GPL plugin may link anything allowed). Violation → **FAIL** `E-LINK <target> <lib> <id>`.
5. Directory/header check (`--sources`): for each tracked source file, the `SPDX-License-Identifier` must equal the longest-prefix-matching `[directories]` entry (derived files may add extra `SPDX-FileCopyrightText` lines but not a different id) → **FAIL** `E-HEADER <file> expected <id> got <id>`; missing header → `E-NOHEADER`.
6. Distribution check: `tools/release/manifest.toml` artefacts tagged `default_download = true` must not contain `default_download_forbidden_plugins`; every artefact containing `ovt_plugin_aom` or `libaom` must contain the AOM patent licence file; every artefact must contain `THIRD_PARTY_NOTICES.md` → **FAIL** `E-DIST`.
7. Exceptions: an `[[exceptions]]` entry suppresses the findings whose `license` equals `id` and whose `where` starts with the entry's `where`; an expired exception is a **FAIL** `E-EXPIRED`.
8. Output: human text to stderr; JSON report `build/<preset>/license_gate_report.json` (the gate writes next to the `--manifest` it reads) (`{"status":"PASS|FAIL","findings":[{"code","where","license","message"}],"inventory":[{"component","version","license","chosen","targets":[...]}]}`); the inventory is the input of the notices generator (§15.6). Exit 0 on PASS, 1 on FAIL, 2 on usage error or missing required input with `--require-all`.

**Acceptance tests** (`tools/tests/test_license_gate.py`, run in `lint`; each fixture lives under `tools/tests/fixtures/license_gate/`): (1) Given a fixture manifest where `ovt_core` links `libfoo (GPL-3.0-only)`, When the gate runs, Then it fails with `E-LINK`; (2) Given a VERSIONS.md row with `AGPL-3.0-only`, When the gate runs, Then it fails with `E-DENY`; (3) Given `zstd` declared `BSD-3-Clause OR GPL-2.0-only`, When the gate runs, Then it passes with `chosen = BSD-3-Clause`; (4) Given `Catch2` declared `BSL-1.0`, When the gate runs, Then it passes (the BSL/BUSL distinction); (5) Given a file in `core/` with a `GPL-3.0-or-later` header, When the gate runs with `--sources`, Then it fails with `E-HEADER`; (6) Given a default-download artefact containing `ovt_plugin_ffmpeg`, When the gate runs with `--release-manifest`, Then it fails with `E-DIST`; (7) Given an expired exception, When the gate runs, Then it fails with `E-EXPIRED`; (8) Given the real repository, When the gate runs with every available input, Then it passes; (9) Given `tools/license_policy.toml`, When the test loads it with `tomllib.load`, Then it parses (a syntax error is a test failure, not a runtime surprise). The `E-*`/`W-*` strings are finding codes of this tool's report, not OVT library `Diagnostic` codes; the D2 family rule (chapter 0 §0.11) does not apply to them.

---

## 15.5 Patent posture and decoder plugin distribution model

### 15.5.1 Facts the policy rests on (Appendix E.4 and E.5; URLs there)

- **AVC**: Via LA pool (formerly MPEG LA); per-unit decoder royalties with a zero-royalty tier for the first 100 000 units/year; no history of action against FOSS projects.
- **HEVC**: two pools (Access Advance, Via LA) plus Avanci Video and independent licensors (Velos Media ended its HEVC pool in 2023; its licensors moved to Access Advance and Avanci). Access Advance's software policy (Nov 2016) exempts software-only HEVC decode/encode downloaded after device sale (naming VLC, HandBrake, Firefox) **but excludes** "standalone products marketed as providing advanced HEVC video functions" priced ≥ USD 45 or ≥ USD 3.75/month. A free analyzer is inside the exemption; a *paid* fork would not be, and the policy does not bind other pools or non-pool holders. libde265's README states the library carries no patent licence.
- **AV1**: AOM Patent License 1.0 grants royalty-free rights to Necessary Claims for conforming implementations with defensive termination (filing an AV1 patent suit terminates the grant) and a duty to include the licence with binaries; Sisvel runs a non-AOM pool; the EU investigation closed in 2023; AV1 ships in every browser and distro.
- **MPEG-2** (future codec): patents expired essentially worldwide (last US patent Feb 2018; the last known patents, in Malaysia and the Philippines, expired in 2020).
- FFmpeg's position ("we do not read patents; redistribution is the redistributor's problem") and VLC's practice (ships all decoders) and Fedora's (strips AVC/HEVC; RPM Fusion `freeworld` packages) bracket the possible postures.

### 15.5.2 OVT policy (binding)

| ID | Policy |
|---|---|
| POL-PAT-1 | OVT is and remains **free of charge**. Any future paid offering (support, hosted service) MUST NOT be marketed as providing HEVC functionality and MUST be re-reviewed against the Access Advance policy by counsel. |
| POL-PAT-2 | **Official default downloads contain the pool-covered FFmpeg playback decoders in the core** (ADR-015, owner decision of 2026-11-12, accepting the patent-pool exposure of a free, non-commercial GPL distribution). They contain: own parsers (syntax parsing and visualisation are not what the pools licence), the LGPL FFmpeg core flavour with the `h264`/`hevc` decoders and hwaccels and the decoders of the other pool-covered codecs (H.266/VVC, AAC/HE-AAC, MPEG-4 Part 2 Visual and its Microsoft variants MS-MPEG4 v1–v3 and WMV7/WMV8, VC-1) enabled (`OVT_PATENT_CODECS_IN_CORE=ON`), libaom (inspection build) with `PATENTS`, dav1d inside FFmpeg, libvmaf; no AVC/HEVC *analysis* decoder plugin. The **patent-pool-free split build** (`OVT_PATENT_CODECS_IN_CORE=OFF`: those decoders and hwaccels disabled, parsers and bitstream filters still enabled since a parser is not a decoder, chapter 13 §13.3.3) stays supported for distributors such as Fedora. Any app-store, paid or commercial distribution of the default build MUST be flagged to the owner for a legal check first. |
| POL-PAT-3 | AVC/HEVC decoding (playback and analysis) is provided by **runtime-loaded plugins** (`libovt_plugin_ffmpeg`, `libovt_plugin_de265`, `libovt_plugin_hm`) distributed as a **separate "codec pack"** artefact per platform (macOS cask `ovt-codec-pack`, Linux `ovt-codec-pack-<ver>-<arch>.tar.zst` and Flatpak `Codecs` extension, Android on-demand module / sibling APK, PyPI `ovt[codecs]` download-on-first-use). The codec pack is published by the project from the same source tree, with the same signing keys, under its own licence files and a `PATENT_NOTICE.md` (text in §15.5.3). The build-time option `OVT_PATENT_CODECS_IN_CORE` (ON by default, ADR-015) decides whether the core FFmpeg flavour also *plays* AVC/HEVC; the codec pack remains the only source of AVC/HEVC *analysis* (instrumented decoders) and, in the split build (`OFF`, e.g. Fedora), of AVC/HEVC playback. |
| POL-PAT-4 | **The plugin ABI (`core/include/ovt/abi/*.h`, BSD-2-Clause) and the FlatBuffers schemas are a published, documented, stable interface** (chapter 12 §12.3.2 ABI policy). A plugin loaded by the user at runtime is the user's own combination, not OVT's distribution; a proprietary vendor decoder plugin is therefore permitted *as a user action*. The project documents this position (`docs/licensing.md`, "Plugins and the GPL") and acknowledges it is accepted practice (GCC plugins pre-exception, Linux kernel modules, Kodi binary add-ons, YUView external decoders) rather than FSF-endorsed. |
| POL-PAT-5 | The project MUST NOT host, mirror or link to third-party *proprietary* decoder plugins. It MAY link to FOSS builds (distro packages, Homebrew, F-Droid). |
| POL-PAT-6 | Every artefact containing libaom MUST include libaom's `LICENSE` and `PATENTS` texts (the latter as `LICENSES/LicenseRef-AOM-Patent-License-1.0.txt`) and the AV1 sentence from §15.5.3. The BSD-2-Clause notice is the legal duty; shipping `PATENTS` is project policy for transparency (the AOM Patent License 1.0 itself imposes no notice duty). dav1d is BSD-2-Clause (VideoLAN) and carries no AOM text obligation; artefacts containing dav1d only (no libaom) need the BSD notice and the AV1 sentence. |
| POL-PAT-7 | Companies redistributing OVT internally at scale with AVC/HEVC plugins are told, in `docs/licensing.md` and the codec-pack notice, that their patent exposure is their own and that the plugin ABI lets them substitute a licensed decoder. |
| POL-PAT-8 | Contributors do not research patents; issues or PRs discussing specific patent claims are closed with a pointer to this policy (FFmpeg practice) and forwarded to the owner privately. |

### 15.5.3 Notice texts (verbatim; `tools/gen_notices.py` emits them)

**`PATENT_NOTICE.md` (codec pack):**

> This codec pack contains software decoders for H.264/AVC and H.265/HEVC (FFmpeg's `h264` and `hevc` decoders, libde265, and optionally the HM reference decoder). These video coding standards are covered by patents held by many parties and administered in part by patent pools (Via Licensing Alliance, Access Advance) and by other licensors (Avanci Video and independent patent holders). The Open Video Tools project does not hold or grant any patent licence. Installing and using this codec pack may require a patent licence in your jurisdiction, in particular for commercial use or redistribution. You are solely responsible for determining whether you need one. The Open Video Tools application works without this pack for AV1 and for bitstream, container and syntax inspection of all supported codecs.

**AV1 sentence (every artefact):**

> This software includes libaom and/or dav1d, implementations of the AV1 video codec. AV1 is published by the Alliance for Open Media under the Alliance for Open Media Patent License 1.0, a copy of which is included as `LICENSES/LicenseRef-AOM-Patent-License-1.0.txt`; that licence terminates for any party that initiates patent litigation alleging that an AV1 implementation infringes. libaom is additionally licensed under the BSD-2-Clause licence and dav1d under the BSD-2-Clause licence; their notices are reproduced below.

**FFmpeg sentence (every artefact):**

> This software uses libraries from the FFmpeg project under the LGPLv2.1. FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project. The exact source code and configuration used are included in the Corresponding Source archive published with this release.

### 15.5.4 Decoder plugin distribution matrix

| Plugin | Licence | Codec | Default download | Codec pack | Distro/Flatpak | Notes |
|---|---|---|---|---|---|---|
| `ovt_plugin_aom` | BSD-2 + AOM patent | AV1 analysis | **yes** | — | yes | Patent notice included. |
| dav1d (inside FFmpeg core) | BSD-2 | AV1 playback | **yes** | — | yes | |
| `ovt_plugin_ffmpeg` | LGPL-2.1+ (GPL-2+ with flag) | AVC playback+analysis, HEVC playback | no | **yes** | Debian/Arch: yes; Fedora: RPM Fusion; Flathub: via runtime FFmpeg (playback) + extension | hwaccel inside. |
| `ovt_plugin_de265` | LGPL-3.0+ | HEVC analysis | no | **yes** | Flathub extension; Fedora: RPM Fusion | Carries our internals patch. |
| `ovt_plugin_hm` | BSD-3 | HEVC oracle | no | optional (`--with-hm` download) | no | Slow; developers build it. |
| Third-party / vendor plugins | any | any | never | never | never | User-installed; permitted by POL-PAT-4. |

### 15.5.5 Decoder patches (owner's decoder policy, §0.0)

Each patch under `plugins/*/patches/` or `third_party/patches/` MUST have a header:

```
From: <author>
Subject: [PATCH] <summary>
Upstream-Status: Submitted [<merge request URL>] | Accepted [<version>] | Pending | Inappropriate [<reason>]
SPDX-License-Identifier: <licence of the patched project>
Rationale: <why no public API suffices>
```

The WI that introduces a patch MUST include the upstreaming attempt as a step (chapter 07 WIs for the libde265 and FFmpeg patches), and `tools/check_patches.py` (part of `lint`) fails if `Upstream-Status` is missing or `Pending` for more than 180 days without an issue link.

---

## 15.6 Third-party notices generation

`tools/gen_notices.py` (LGPL-2.1-or-later; run as `python -I`) produces `THIRD_PARTY_NOTICES.md` at build time (CMake target `ovt_notices`). The file lives at the repository root (`/THIRD_PARTY_NOTICES.md`, chapter 0 §0.4; NFR-031): it is regenerated on every build, never edited by hand, and the `license-gate` CI job (chapter 13 §13.7.1) fails when the committed copy differs from the regenerated one. It is installed into every artefact: `Contents/Resources/`, AppImage `usr/share/doc/ovt/`, Flatpak `/app/share/doc/ovt/`, APK `assets/licenses/` (shown in the About panel), wheel `.dist-info/licenses/`).

**Inputs:** `build/<preset>/license_gate_report.json` inventory (§15.4.2), `third_party/VERSIONS.md` (name, version, URL, patch list), each dependency's licence text files: vcpkg `share/<port>/copyright`, `ovt_tp_declare(... LICENSE_FILES ...)` files from the ExternalProject install prefix (`<prefix>/share/ovt/licenses/<dep>/`), Qt `LICENSES/` from the Qt kit, Gradle report (`licenses.json` with URLs → texts fetched at build time **only** from the artefact jar's `META-INF/LICENSE*`; never from the network), YUView-derived file list (`tools/derived_files.toml`: file → upstream project, commit, licence).

**Output structure (deterministic, sorted by component name):**

```markdown
# Third-party notices for Open Video Tools <version> (<artefact name>)

This document lists the third-party components included in this artefact, their licences and required notices. Open Video Tools itself is licensed as described in LICENSE and LICENSES/. Generated by tools/gen_notices.py; do not edit.

## Summary
| Component | Version | Licence | Modified | Homepage |
| FFmpeg | n7.1.1 | LGPL-2.1-or-later | no | https://ffmpeg.org |
| libaom | 3.12.1 | BSD-2-Clause AND LicenseRef-AOM-Patent-License-1.0 | no | https://aomedia.org |
| libde265 | 1.0.15 | LGPL-3.0-or-later | **yes** — patches: 0001-de265-internals-api.patch (Upstream-Status: Submitted <url>) | https://github.com/strukturag/libde265 |
…

## Required statements
<AV1 sentence> <FFmpeg sentence> <patent notice when codec pack>

## Licence texts
### FFmpeg
<full text of COPYING.LGPLv2.1 from the source tarball>
### libaom
<LICENSE> <PATENTS>
…

## Files derived from other projects
| File in OVT | Upstream | Commit | Licence |
| app/src/items/DifferenceItem.cpp | YUView | <sha> | GPL-3.0-only |
```

Rules: every component in the artefact's link graph (per `tools/release/manifest.toml` `artifacts.<id>.components` → `targets[]` of `build/<preset>/license_manifest.json`, chapter 13 §13.8.1 and §13.2.3) appears exactly once; a component without a licence text file is a **build failure** (`E-NOTICE-MISSING`); the file is byte-identical across two builds of the same tag (sorted, no timestamps); the About panel (chapter 11) and `ovt --version --format json` (`features`, chapter 12 §12.2.1) point to it; the CI `release` job attaches it to the GitHub release as well.

---

## 15.7 Governance

### 15.7.1 Developer Certificate of Origin

The project uses the Linux Foundation **DCO 1.1** verbatim (https://developercertificate.org/), stored as `/DCO`:

```
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

Mechanics: every commit carries `Signed-off-by: Real Name <email>` (`git commit -s`); the GitHub DCO app (`dcoapp`) is a required status check (chapter 13 §13.7.3); pseudonyms are accepted if used consistently (the project follows the Linux kernel's 2024 relaxation: a "known identity", not necessarily a legal name). Because "the open source license indicated in the file" is the SPDX header, the DCO automatically binds a contribution to `LGPL-2.1-or-later` in `core/` and `GPL-3.0-or-later` in `app/` — this realises the research recommendation that core contributions be explicitly LGPL. **No CLA, no copyright assignment** (ADR-016); if a future relicensing of `core/` is ever needed, it will require consent of all copyright holders, which is accepted as the price of contributor-friendliness.

### 15.7.2 `CONTRIBUTING.md` outline

1. **Welcome and scope** — what OVT is; link to the spec (`docs/spec/`), the roadmap chapter and the "good first WI" label.
2. **Before you start** — search issues; discuss large changes in a GitHub Discussion or an issue referencing a `WI-` id; one WI per PR.
3. **Setup** — `tools/bootstrap.sh`, presets, `docs/dev/getting-started.md`.
4. **Coding standards** — chapter 13 §13.9.3 summary and link; `pre-commit install`.
5. **Licensing of contributions** — the directory table of §15.1; "if you touch `core/` your code is LGPL-2.1-or-later; if `app/` it is GPL-3.0-or-later"; no code from projects with incompatible licences (link §15.3); how to credit derived code (§15.1.1); patents: no patent discussion (POL-PAT-8).
6. **DCO** — what `-s` does, why, how to fix a missing sign-off (`git rebase --signoff`).
7. **Pull request checklist** — tests added (unit + golden/fuzz where applicable), `changelog.d/` fragment, docs updated, CI green, `WI-` reference, no new dependency without `VERSIONS.md` row and `license_policy.toml` review.
8. **Review process** — CODEOWNERS auto-assigns; one approval for `app/`/`cli/`, two for `core/include/`, `abi/`, `schema/`, `LICENSES/`, `tools/license_policy.toml`, `third_party/VERSIONS.md`; 72-hour minimum for ABI/schema changes; squash merge.
9. **Reporting security issues** — `SECURITY.md`: private reporting via GitHub Security Advisories; 90-day disclosure; hostile-input crashes are security bugs (§0.7 rule 1).
10. **Code of conduct** — Contributor Covenant 2.1 (`CODE_OF_CONDUCT.md`), enforcement contact `conduct@ovt.dev` (forwards to the owner; it must deliver before the repository goes public). Security reports do not use email: they go through GitHub private vulnerability reporting (`SECURITY.md`).
11. **Communication** — GitHub Discussions, Matrix room, monthly video call notes in `docs/meetings/`.
12. **Adding plugins / metrics** — links to chapter 13 §13.9.4–§13.9.5 and the shipping decision table (§15.5.4).

### 15.7.3 Maintainers and CODEOWNERS

`MAINTAINERS.md`: table of maintainers (name, GitHub handle, GPG key fingerprint used for tag signing, areas, "licence reviewer" flag), the owner as BDFL for the first year with a stated intention to move to a 3–5 person technical steering group once there are ≥ 3 regular external contributors; decision rule: lazy consensus on PRs, maintainers' majority on ADR changes, owner veto on licensing/trademark.

`.github/CODEOWNERS` (initial):

```
*                                   @ovt-maintainers
/core/include/ovt/                  @ovt-core-api-reviewers
/core/include/ovt/abi/              @ovt-core-api-reviewers @ovt-licence-reviewers
/core/schema/                       @ovt-core-api-reviewers
/render/                            @ovt-render
/plugins/                           @ovt-decoders
/app/ /cli/                         @ovt-app
/android/                           @ovt-android
/python/                            @ovt-python
/third_party/ /vcpkg.json           @ovt-build @ovt-licence-reviewers
/LICENSES/ /LICENSE /DCO /REUSE.toml /tools/license_policy.toml /tools/license_gate.py /tools/gen_notices.py   @ovt-licence-reviewers
/.github/                           @ovt-build
/docs/spec/                         @ovt-maintainers
```

### 15.7.4 Trademark policy outline (`TRADEMARKS.md`; to be reviewed by counsel; modelled on Mozilla's and Debian's policies)

1. **Marks**: the final project name (placeholder "Open Video Tools"/"OVT") and the logo. Register as a word mark in at least one jurisdiction (EUIPO or USPTO) in Nice classes 9 (software) and 42 (SaaS/online services), owned by the maintainer entity (LLC) or the fiscal sponsor (Software Freedom Conservancy / Open Collective host) — decision for the owner, flagged in chapter 17. Check for confusability with StreamEye, VQ Analyzer, Zond, YUView, Vooya, CodecVisa before filing (checklist WI-0.606).
2. **Permitted without asking**: unmodified redistribution (distro packages built from the released tags with packaging-only changes, F-Droid builds, Homebrew, Flathub); referring to the project nominatively ("plugin for OVT", "compatible with OVT"); community sites with a clear "unofficial" label.
3. **Not permitted without written permission**: use of the name/logo on modified builds that change functionality or defaults (e.g. forks bundling AVC/HEVC decoders by default MUST rename — this is also how the project protects its patent posture), paid products or services implying endorsement, domain names containing the mark, merchandise.
4. **Codec packs**: third-party codec packs may say "codec pack for OVT" but not "OVT codec pack".
5. **Enforcement**: polite request first; the GPL already forces source release, the trademark is the tool against proprietary-looking forks.
6. **Name change clause**: because §0.0 says names are placeholders, every user-visible string, bundle id (`org.openvideotools.*`), package names and the cask/formula names MUST come from a single source (`cmake/OvtBranding.cmake` → `OVT_PRODUCT_NAME`, `OVT_BUNDLE_ID_PREFIX`, `OVT_CLI_NAME`; QML via `Branding.qml`; Gradle via `gradle.properties`) so a rename before 1.0 is one PR (WI-0.606 verifies with a dry-run rename).

### 15.7.5 Hosting and fiscal sponsorship

- **Now**: GitHub organisation `openvideotools` (placeholder), issues/discussions/actions; mirror on GitLab for CI redundancy (optional). Funds via Open Collective or Software Freedom Conservancy application once donations or grants are in sight (SFC also handles trademark ownership and GPL compliance questions).
- **Later (after traction, ≥ 1 release used by ≥ 3 organisations)**: approach **VideoLAN** (hosts dav1d, x264, VLC; strong patent posture; CI farm; GPL-friendly) or **Xiph.Org** (hosts aomanalyzer) for project hosting. Not the Linux Foundation initially (requires neutral trademark ownership, formal governance, member sponsors) and not AOM (AV1/AV2-only scope). Pre-condition for any move: trademark transferable, DCO history intact, no CLA.

---

## 15.8 Distribution channel rules (ADR-016)

| Channel | Rule | Reason |
|---|---|---|
| **Mac App Store** | **Never.** | The App Store Usage Rules add restrictions incompatible with GPL §10/§6 (FSF/VLC 2011 precedent; VLC returned only after relicensing to LGPL/MPL). A GPL-3 app with Qt GPL modules and YUView code cannot be relicensed. Also a sandbox would break plugin loading and arbitrary file access (chapter 13 §13.5.3). |
| **macOS direct download (notarised DMG) + Homebrew cask** | Primary. Per-arch DMGs; cask in project tap then `homebrew/cask`; codec pack as separate cask. | Notarisation is compatible with the GPL (no usage restrictions on the user). |
| **Google Play** | Yes (`play` flavour AAB, base module without AVC/HEVC, codec pack as on-demand module). Publish the reproducible `foss` APK too so users can sideload a rebuilt APK. | Play's agreement does not conflict with the GPL; GPL apps are common on Play. Play App Signing means users cannot replace the Play-signed app with their own build under the same signature — GPL-3 §6 Installation Information applies to User Products conveyed with the device, not to an app download, so this is acceptable; sideloading remains possible. |
| **F-Droid** | Yes (`foss` flavour; reproducible builds verified by F-Droid; codec pack as a second app `org.openvideotools.studio.codecpack`). | FOSS credibility; builds from source; shows licences. |
| **GitHub Releases** | Yes: every artefact, SHA256SUMS, minisign signatures, Corresponding Source archive. | GPL §6(d)/(e) source availability for the lifetime of the binaries (GitHub Releases are kept indefinitely; also mirrored to a project-controlled bucket with ≥ 3-year retention). |
| **Flathub** | Yes (`org.openvideotools.Studio` + `.Codecs` extension). | Reaches all distros incl. ChromeOS Crostini. |
| **Distro repositories** | Encourage; provide `linux-x64-distro` preset and packaging skeletons; Fedora via RPM Fusion for codecs. | Patent split per distro policy. |
| **PyPI** | Yes (`ovt` wheels; GPL-3 metadata; no AVC/HEVC decoders in the wheel; `ovt[codecs]` fetches from GitHub Releases with hash check and shows the patent notice). | |
| **Snap, Microsoft Store, iOS App Store, Windows** | Not targets (§0.0); nothing in the design precludes Snap later. | |
| **Any channel requiring a CLA/terms that restrict users' GPL rights** | Refuse. | GPL §10 "no further restrictions". |

Every channel's listing MUST state the licence (GPL-3.0-or-later), link to the source tag, and include the patent notice text where the artefact contains the codec pack.

---

## 15.9 Work items (WI-0.600 … WI-0.609; 0.610–0.619 reserved; phase 0, owned by this chapter)

### WI-0.600 — Licence texts, `LICENSE`, `REUSE.toml` and `docs/licensing.md`
**Goal:** Put the full licence texts and the directory policy into the repository in REUSE-compliant form.
**Depends on:** WI-0.000.
**Produces:** `LICENSES/{LGPL-2.1-or-later.txt,GPL-3.0-or-later.txt,LGPL-3.0-or-later.txt,BSD-2-Clause.txt,BSD-3-Clause.txt,CC-BY-4.0.txt,CC-BY-SA-4.0.txt,LicenseRef-AOM-Patent-License-1.0.txt}` (repository root; texts from https://spdx.org/licenses/ and https://aomedia.org/license/patent-license/), `LICENSE` (GPL-3 text + the §15.1 preamble), `REUSE.toml` (REUSE spec 3.3: `[[annotations]]` for binary assets, corpora manifests, generated files, and `docs/spec/**/*.md` as CC-BY-4.0), `docs/licensing.md` (§15.1 table, §15.3 matrix, §15.5.2 policy, "Plugins and the GPL" position, the "not legal advice" disclaimer), `docs/LICENSE.md` (CC-BY-SA-4.0 for prose, CC-BY-4.0 for `docs/spec/`, BSD-2 for snippets), and — because REUSE 3.3 rejects a text in `LICENSES/` that no file uses — placeholder `plugins/ovt-plugin-{aom,de265,hm}/CMakeLists.txt` stubs carrying their directory's §15.1 header (BSD-2-Clause, LGPL-3.0-or-later, BSD-3-Clause; WI-2.155, WI-2.163 and WI-4.105 replace the bodies, and `plugins/CMakeLists.txt` does not add the directories yet) plus `plugins/ovt-plugin-aom/PATENTS` (a byte copy of libaom's `PATENTS`, annotated `LicenseRef-AOM-Patent-License-1.0`; POL-PAT-6 ships it with every aom plugin binary) (issue #20).
**Interface:** §15.1.
**Steps:** 1. Download texts (verify against SPDX JSON `licenseText`); 2. write `LICENSE` preamble; 3. write `REUSE.toml`; 4. write `docs/licensing.md`; 5. run `reuse lint` and fix.
**Acceptance tests:** 1. Given the repo, When `reuse lint` runs, Then it prints "Congratulations! Your project is compliant with version 3.3 of the REUSE Specification". 2. Given `LICENSES/LicenseRef-AOM-Patent-License-1.0.txt`, When hashed, Then its SHA-256 equals the `PATENTS sha256=` value recorded in the Notes column of the libaom row of `third_party/VERSIONS.md` (chapter 13 §13.3.7; same text).
**Size:** S.
**Notes / pitfalls:** Use the SPDX `licenseText` field verbatim (the FSF texts contain trailing-whitespace variants that make `reuse lint` complain). `REUSE.toml` replaces the deprecated `.reuse/dep5`.
### WI-0.601 — SPDX headers on all files and `tools/spdx_header.py` / `tools/check_spdx_dirs.py`
**Goal:** Every existing file carries the correct header; a tool inserts headers and a checker enforces the directory policy.
**Depends on:** WI-0.600.
**Produces:** `tools/spdx_header.py` (inserts §15.1.1 text for the file type, honouring shebangs/`#pragma once`), `tools/check_spdx_dirs.py` (reads `[directories]` from `tools/license_policy.toml`), the **initial** `tools/license_policy.toml` containing only the `[directories]` table of §15.4.2 (WI-0.602 completes the file with the allow/deny/exception tables — D25: this WI has no dependency on WI-0.602), pre-commit hook entries, headers on all files.
**Interface:** `tools/spdx_header.py <path>... [--check]` (exit 0 = header present and correct, 1 = inserted/would insert, 2 = unknown file type); `tools/check_spdx_dirs.py [--policy tools/license_policy.toml] [paths...]` (exit 0/1; messages `E-HEADER <file> expected <id> got <id>`, `E-NOHEADER <file>`); both are pre-commit hooks (chapter 13 §13.9.2).
**Steps:** 1. File-type table (comment syntax per extension; shebang and `#pragma once` handling); 2. header inserter; 3. write the `[directories]` table into a new `tools/license_policy.toml` and the directory checker reading it with longest-prefix match; 4. run the inserter over the tree and commit the headers in one `licence:` PR; 5. pre-commit entries; 6. tests in `tools/tests/test_spdx_tools.py`.
**Acceptance tests:** 1. Given the repository, When `tools/check_spdx_dirs.py` runs, Then it exits 0. 2. Given a new `core/src/x.cpp` with a GPL header, When pre-commit runs, Then it fails with `E-HEADER core/src/x.cpp expected LGPL-2.1-or-later got GPL-3.0-or-later` and exit 1. 3. Given a `.py` file with a shebang and no header, When `tools/spdx_header.py <file>` runs, Then the header is inserted on line 2 and a second run changes nothing (`--check` exits 0).
**Size:** S.
**Notes / pitfalls:** Generated files (`*_generated.h`, `Branding.qml`) get their header from the generator, not from this tool; list them in `REUSE.toml` instead. Derived files keep extra `SPDX-FileCopyrightText` lines — the checker compares only the identifier.
### WI-0.602 — `tools/license_gate.py` and `tools/license_policy.toml`
**Goal:** Implement the gate of §15.4.2 with all inputs, the SPDX expression parser, target compatibility, distribution checks, exceptions and the JSON report; wire into CI (chapter 13 WI-0.032). The gate reads `build/<preset>/license_manifest.json` from WI-0.000 and `third_party/VERSIONS.md` from WI-0.010; it does **not** depend on the release workflow (D25) — `tools/release/manifest.toml`, its schema module and its `[licenses]` table are chapter 13 WI-0.036's, which lands later and imports this gate as a module.
**Depends on:** WI-0.601 (initial `tools/license_policy.toml` with `[directories]`, `check_spdx_dirs` module), WI-0.000 (`license_manifest.json`), WI-0.010 (`VERSIONS.md`).
**Produces:** `tools/license_gate.py` (also importable as a module: `license_gate.resolve(expr, policy) -> chosen id`, `license_gate.inventory(report) -> list` — used by chapter 13 WI-0.036 `collect.py --check-licenses` and by WI-0.603), `tools/license_policy.toml` completed (§15.4.2 skeleton for every current dependency; the `[directories]` table comes from WI-0.601), `tools/tests/test_license_gate.py` with fixtures under `tools/tests/fixtures/license_gate/`, `docs/dev/license-gate.md`.
**Interface:** `python -I tools/license_gate.py --policy tools/license_policy.toml --versions third_party/VERSIONS.md --vcpkg build/<preset>/vcpkg_installed/<triplet> --manifest build/<preset>/license_manifest.json --gradle android/app/build/reports/dependency-license/index.json --pip python/requirements.txt --release-manifest tools/release/manifest.toml --sources [--require-all] [--strict-or] [--export-table <md>]`; policy schema (§15.4.2); report `build/<preset>/license_gate_report.json`; exit codes 0/1/2 (§15.4.2 step 8).
**Steps:** 1. SPDX expression parser (+ 20 unit tests incl. `WITH`, nested parens); 2. input readers (VERSIONS.md table, vcpkg SPDX JSON and `copyright` heuristics with a `[vcpkg_overrides]` policy section for ports whose declared licence is `NOASSERTION`); 3. link-graph compatibility rules; 4. header check (reuse `check_spdx_dirs` module); 5. distribution check (runs only when `--release-manifest` is given; tested here against the fixture `tools/tests/fixtures/license_gate/manifest.toml`, not against the real release manifest); 6. exceptions with expiry; 7. report writer; 8. fixtures and the nine acceptance tests of §15.4.2; 9. documentation `docs/dev/license-gate.md` explaining each error code and how to resolve it (add to allow list vs. remove dependency vs. exception).
**Acceptance tests:** 1.–9. The nine Given/When/Then tests of §15.4.2. 10. Given `--require-all` and a missing `--gradle` input, When the gate runs, Then it exits 2 with `missing required input: gradle`. 11. Given the real repository at M0.2, When the gate runs with every input that exists (`--require-all` without `--release-manifest`), Then it finishes in ≤ 10 s and exits 0. 12. Given the fixture `tools/tests/fixtures/license_gate/manifest.toml` with a `default_download = true` artefact containing `ovt_plugin_ffmpeg`, When the gate runs with `--release-manifest` pointing at it, Then `E-DIST` is reported and exit 1.
**Size:** M.
**Notes / pitfalls:** Input schemas: `license_manifest.json` is defined in chapter 13 §13.2.3, `tools/release/manifest.toml` in chapter 13 §13.8.1 (consumed when present, never required before WI-0.036). Scheduled in M0.2 after WI-0.601 (M0.1); nothing in this WI waits for M0.5. vcpkg's `copyright` files are free text for many ports; rely on `vcpkg.spdx.json` (`packages[].licenseConcluded`) first and keep a reviewed override table rather than guessing. Do not import `license-expression` or other PyPI packages (the gate must run in a bare `python -I`). `BSL-1.0` ≠ `BUSL-1.1`.
### WI-0.603 — `tools/gen_notices.py` and `ovt_notices` target
**Goal:** Implement §15.6, producing a deterministic `THIRD_PARTY_NOTICES.md` per artefact and installing it everywhere listed.
**Depends on:** WI-0.602, WI-0.010 (`LICENSE_FILES` collection in `ovt_tp_declare`).
**Produces:** `tools/gen_notices.py`, `tools/derived_files.toml` (initially empty with schema comment), `cmake/OvtNotices.cmake` (`ovt_notices` target; install rules per platform; Android copies into `android/app/src/main/assets/licenses/`; wheel via scikit-build-core `wheel.license-files`), `tools/tests/test_gen_notices.py`.
**Interface:** `python -I tools/gen_notices.py --report build/<preset>/license_gate_report.json --versions third_party/VERSIONS.md --licenses-dir <prefix>/share/ovt/licenses --qt-licenses <QT_ROOT_DIR>/Licenses --derived tools/derived_files.toml --artifact <id> --out THIRD_PARTY_NOTICES.md [--patent-notice]` (exit 0/1; `E-NOTICE-MISSING <dep>`); CMake target `ovt_notices` (`cmake/OvtNotices.cmake`) runs it per artefact.
**Steps:** 1. Inventory reader (gate report); 2. licence-text resolver in the order §15.6 lists; 3. Markdown writer (sorted, no timestamps); 4. `derived_files.toml` section; 5. CMake target and per-platform install rules; 6. codec-pack `PATENT_NOTICE.md` emission; 7. tests with a fixture inventory.
**Acceptance tests:** 1. Given a build, When `cmake --build . --target ovt_notices` runs twice, Then the outputs are byte-identical. 2. Given a dependency without a licence text file (fixture), When the target builds, Then it fails with `E-NOTICE-MISSING <dep>` and exit 1. 3. Given the macOS bundle, When inspected, Then `Contents/Resources/THIRD_PARTY_NOTICES.md` exists and contains the AV1 and FFmpeg sentences; Given the codec pack, When inspected, Then `PATENT_NOTICE.md` exists.
**Size:** M.
**Notes / pitfalls:** Never fetch licence texts from the network at build time (Flathub and reproducibility); Gradle artefacts provide theirs in `META-INF/`. Normalise line endings to `\n` so macOS and Linux builds produce identical bytes.
### WI-0.604 — `DCO`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, DCO CI check
**Goal:** Governance documents of §15.7.1–§15.7.2 in place and the DCO app installed so it reports on every PR (a required check once branch protection is available, chapter 13 §13.7.3); commit-msg hook (chapter 13 WI-0.007) coordinated.
**Depends on:** WI-0.000.
**Produces:** `DCO` (repository root, verbatim §15.7.1), `CONTRIBUTING.md` (outline §15.7.2 filled in), `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1 with the contact `conduct@ovt.dev`, §15.7.2 item 10), `SECURITY.md`, `.github/PULL_REQUEST_TEMPLATE.md` (checklist items of §15.7.2 item 7 incl. "I certify the DCO"), `.github/ISSUE_TEMPLATE/{bug,feature,licensing}.yml`, `.github/dco.yml` (`require: { members: true }`, `allowRemediationCommits: { individual: true, thirdParty: false }` — the DCO app's camelCase keys), `docs/dev/dco.md`.
**Interface:** Files at the paths in Produces; the DCO app (`dcoapp/app`) as status check `DCO` (required once branch protection exists, chapter 13 §13.7.3); `docs/dev/dco.md` is linked from the `dco-signoff` commit-msg hook message (chapter 13 WI-0.007), the PR template's DCO checkbox and `CONTRIBUTING.md` — the app's own failure text is fixed and `.github/dco.yml` cannot change it (issue #25).
**Steps:** 1. Copy DCO 1.1 verbatim; 2. write `CONTRIBUTING.md` from the §15.7.2 outline; 3. Contributor Covenant 2.1 with the enforcement contact; 4. `SECURITY.md` (private advisories, 90-day disclosure); 5. PR and issue templates; 6. `.github/dco.yml`; 7. the owner installs the DCO app now, so it reports on PRs; making it a required check waits until the repository is public or on a paid plan (WI-0.605 scripts it; recorded as pending in `docs/acceptance/wi/WI-0.604.md`).
**Acceptance tests:** 1. Given a PR with an unsigned commit, When the DCO check runs, Then it fails, and the `dco-signoff` hook message and the PR template link `docs/dev/dco.md`. 2. Given the governance documents, When `markdownlint` runs in the `lint` job (chapter 13 §13.7.1 row), Then it exits 0. 3. Given branch protection (once the repository is public or on a paid plan; pending until then), When the required checks are listed, Then one is named exactly `DCO` (chapter 13 §13.7.3).
**Size:** S.
**Notes / pitfalls:** `require.members: true` makes maintainers sign off too — intended. Do not enable `allowRemediationCommits.thirdParty` (a third party cannot certify for a contributor).
### WI-0.605 — `MAINTAINERS.md`, `CODEOWNERS`, branch protection and review rules
**Goal:** §15.7.3 implemented: teams, CODEOWNERS file, branch protection (required checks per chapter 13 §13.7.3, 1/2 approvals by path, linear history, signed tags).
**Depends on:** WI-0.604, WI-0.030 (CI jobs that become required checks).
**Produces:** `MAINTAINERS.md`, `.github/CODEOWNERS`, `tools/github/branch_protection.json` + `tools/github/apply_protection.sh` (uses `gh api`) so settings are reproducible, `docs/dev/governance.md` (decision rules).
**Interface:** `tools/github/apply_protection.sh [--check] [--repo <owner/name>]` (reads `tools/github/branch_protection.json`, uses `gh api`; exit 0 = applied/no drift, 1 = drift or API error); `.github/CODEOWNERS` as in §15.7.3.
**Steps:** 1. `MAINTAINERS.md` table; 2. GitHub teams (`ovt-maintainers`, `ovt-core-api-reviewers`, `ovt-licence-reviewers`, …) created manually and listed in `docs/dev/governance.md`; 3. CODEOWNERS; 4. branch-protection JSON (required checks of chapter 13 §13.7.3, linear history, signed tags, approvals by path through CODEOWNERS); 5. apply/check script; 6. `docs/dev/governance.md` decision rules.
**Acceptance tests:** 1. Given a PR touching only `app/`, When review is requested, Then one approval is required; Given one touching `core/include/ovt/abi/`, Then two are required (verified by GitHub UI; recorded in `docs/dev/governance.md` with screenshots). 2. Given the live repository settings, When `tools/github/apply_protection.sh --check` runs, Then it exits 0 and reports no drift; Given a manually changed setting, Then it exits 1 naming the key.
**Size:** S.
**Implementation note (rc15):** the script manages a repository *ruleset* (`gh api repos/<r>/rulesets`), the mechanism the owner already uses on `main`, instead of classic branch protection; `branch_protection.json` is the ruleset body and starts at the live state, and the required checks, code-owner review and signed-tag rules are added to it by later owner-approved PRs (`docs/dev/governance.md`). Acceptance test 1 is therefore pending until the teams exist and code-owner review is on.
**Notes / pitfalls:** GitHub applies one approval count per branch rule; the "two approvals for `abi/`" rule is realised by CODEOWNERS team membership (each team member is one approval) plus the `require_code_owner_reviews` flag — document this limitation.
### WI-0.606 — Trademark checklist, branding single source and rename dry-run
**Goal:** `TRADEMARKS.md` draft (§15.7.4), a confusability/clearance checklist for the owner, and a single branding source so the placeholder name can change in one PR.
**Depends on:** WI-0.000, WI-0.019.
**Produces:** `TRADEMARKS.md` (draft, marked "pending legal review"), `docs/governance/trademark-checklist.md` (search EUIPO/USPTO/WIPO Global Brand DB; domains; app-store ids; GitHub org; Nice classes 9/42; owner entity decision; budget), `cmake/OvtBranding.cmake` (`OVT_PRODUCT_NAME`, `OVT_SHORT_NAME`, `OVT_BUNDLE_ID_PREFIX`, `OVT_CLI_NAME`, `OVT_ORG_DOMAIN`), `app/qml/Branding.qml` generated from it, `android/gradle.properties` keys, `tools/rename_dry_run.sh` (greps for hard-coded occurrences outside the branding source).
**Interface:** CMake cache variables `OVT_PRODUCT_NAME`, `OVT_SHORT_NAME`, `OVT_BUNDLE_ID_PREFIX`, `OVT_CLI_NAME`, `OVT_ORG_DOMAIN` (defaults in chapter 13 §13.2.2); `cmake/OvtBranding.cmake` generates `app/qml/Branding.qml` and `android/gradle.properties` keys (`ovt.productName`, `ovt.applicationId`); `tools/rename_dry_run.sh [--name <New>]` (exit 0 = no hard-coded occurrences).
**Steps:** 1. Branding module; 2. QML and Gradle generation; 3. replace hard-coded strings in `app/`, `cli/`, `android/`, `python/`; 4. `TRADEMARKS.md` draft; 5. checklist document; 6. dry-run script and its CI run in `lint`.
**Acceptance tests:** 1. Given the repository, When `tools/rename_dry_run.sh` runs, Then it exits 0 and reports zero hard-coded product-name occurrences in `app/`, `cli/`, `android/`, `python/` outside the branding files (docs excluded). 2. Given `-DOVT_PRODUCT_NAME=Foo -DOVT_ORG_DOMAIN=example.org`, When the macOS bundle is rebuilt, Then it is named `Foo.app` with bundle id `org.example.foo` and the CLI is still named per `OVT_CLI_NAME`.
**Size:** S.
**Notes / pitfalls:** Android `applicationId` changes break upgrades for installed users — the rename must happen before the first public release. The macOS bundle id and the notarisation profile must match (chapter 13 §13.5.4). Track A runs in parallel (MA.0 overlaps M0.3–M0.4), so this WI does not wait for WI-0.050: the `android/gradle.properties` keys are generated whether or not `android/` exists yet, step 3 covers `android/` only if WI-0.050 has landed, and the Android branding is verified by WI-A.456 (MA.1), which depends on this WI.
### WI-0.607 — Decoder patch header checker and plugin distribution manifest validation
**Goal:** `tools/check_patches.py` enforcing §15.5.5 headers and `Upstream-Status` ageing. (Reduced: the `tools/release/manifest.toml` schema validation named in the title is delivered by its owner, chapter 13 WI-0.036 `tools/release/manifest_schema.py`, which lands in M0.5; this WI does not depend on it.)
**Depends on:** WI-0.602.
**Produces:** `tools/check_patches.py`, `tools/tests/test_check_patches.py`, `docs/dev/patches.md`.
**Interface:** `tools/check_patches.py [--max-pending-days 180] [paths...]` (exit 0/1; messages `E-PATCH-HEADER <file> missing <field>`, `E-PATCH-STALE <file> <days>`, warning `W-PATCH-PENDING`); the manifest schema module `tools/release/manifest_schema.py` (chapter 13 §13.8.1) is chapter 13 WI-0.036's, not this WI's.
**Steps:** 1. Header parser (§15.5.5 fields); 2. ageing rule with `Date:` and `Issue:`; 3. pre-commit/`lint` hook (chapter 13 §13.7.1 `lint` row); 4. `docs/dev/patches.md`; 5. tests.
**Acceptance tests:** 1. Given a patch without `Upstream-Status`, When `tools/check_patches.py` runs in `lint`, Then it exits 1 naming the patch. 2. Given a patch `Pending` with a `Date:` older than 180 days and no `Issue:` link, When the checker runs, Then it exits 1; Given the same patch with an issue link, Then it exits 0 with a warning.
**Size:** S.
**Notes / pitfalls:** `git format-patch` emits `Date:` automatically; hand-written patches must add it. Count days from `Date:`, not from file mtime (checkouts reset mtimes).
### WI-0.608 — `docs/licensing.md` "Plugins and the GPL" position paper and channel listings text
**Goal:** Write the user-facing explanation of POL-PAT-1..8 (§15.5.2), the plugin-ABI position (POL-PAT-4), and the per-channel listing boilerplate (§15.8) used by the release automation (Play listing, F-Droid description, Flathub metainfo, cask `desc`, PyPI long description).
**Depends on:** WI-0.600.
**Produces:** `docs/licensing.md` sections, `tools/release/listing_texts.toml` (per channel: short description, long description, licence line, patent notice flag), `tools/tests/test_listing_texts.py` (schema and length checks of the TOML, standalone); consumed later by chapter 13 WI-0.036 (`collect.py --check-listings`, which depends on this WI — not the reverse, D25).
**Interface:** `tools/release/listing_texts.toml` (`[channels.<id>]` with `short`, `long`, `licence_line`, `patent_notice = true|false`; channel ids `play`, `fdroid`, `flathub`, `homebrew`, `pypi`, `github`); consumed by `tools/release/collect.py --check-listings` (chapter 13 WI-0.036, M0.5).
**Steps:** 1. Write the `docs/licensing.md` sections (POL-PAT-1..8, plugin-ABI position, FAQ); 2. per-channel texts; 3. `tools/tests/test_listing_texts.py` (every channel marked "Yes" in §15.8 present; Play 80/4000 and F-Droid 80-byte limits); 4. metainfo `project_license` generation from the branding/licence source.
**Acceptance tests:** 1. Given `docs/licensing.md`, When `markdownlint` runs (the `lint` job of chapter 13 §13.7.1), Then it exits 0 and the section "Plugins and the GPL" contains one paragraph per distribution channel of §15.8 (`play`, `fdroid`, `flathub`, `homebrew`, `pypi`, `github`). 2. Given `tools/release/listing_texts.toml`, When `pytest tools/tests/test_listing_texts.py` runs, Then every channel marked "Yes" in §15.8 has an entry within the length limits and the test exits 0 (the same check is repeated by `collect.py --check-listings` once WI-0.036 lands). 3. Given the generated metainfo, When `appstreamcli validate` runs, Then `project_license` is `GPL-3.0-or-later`.
**Size:** S.
**Notes / pitfalls:** Play Console limits the short description to 80 characters and the long description to 4000; F-Droid summaries to 80 bytes. Keep the patent sentence out of the default-download listings (no codec pack there).
### WI-0.609 — Legal review hand-off package
**Goal:** Assemble the artefacts a lawyer needs to review before the first public release.
**Depends on:** WI-0.600–WI-0.608.
**Produces:** `docs/governance/legal-review-package.md` listing: `LICENSE`, `LICENSES/`, `docs/licensing.md`, `TRADEMARKS.md`, `PATENT_NOTICE.md`, the §15.2 table exported from the gate inventory (`tools/license_gate.py --export-table docs/governance/dependency-licences.md`), the distribution matrix, open questions for counsel (owner entity for trademark; whether the Access Advance exemption analysis is correct; whether the Play App Signing/§6 reading is acceptable; the plugin-ABI position), and a sign-off checklist.
**Interface:** `docs/governance/legal-review-package.md` (checklist with owner sign-off fields); `tools/license_gate.py --export-table <md>` (WI-0.602) produces the dependency table it embeds.
**Steps:** 1. Assemble the file list and open questions; 2. export the dependency table; 3. owner review; 4. hand to counsel; 5. record outcomes in chapter 17 (open item "legal review") and in the sign-off checklist.
**Acceptance tests:** 1. Given `docs/governance/legal-review-package.md`, When `tools/check_links.py` runs, Then every referenced file exists and it exits 0. 2. Given chapter 17, When read, Then "legal review completed" is listed as a release-1.0 gate.
**Size:** S.
**Notes / pitfalls:** the package is a release-1.0 gate (chapter 17 open item); keep it free of legal conclusions — it lists facts (licences, linking mode per platform, patent-relevant components) and questions for counsel, and the sign-off fields are filled by the maintainers after counsel answers.

---

### Glossary additions (chapter 15)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **library validation** — see chapter 13; relevant here because user-installed plugins (POL-PAT-4) require it to be disabled.

**Notes / pitfalls:** Do not paraphrase licence texts in the package — link the files. Keep counsel's answers out of the public repository unless the owner decides otherwise.
