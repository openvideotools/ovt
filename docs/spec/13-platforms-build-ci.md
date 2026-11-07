# 13 — Platforms, build system, CI and distribution

> **Status:** normative chapter of the OVT Requirements & Architecture Specification. Conforms to `00-backbone.md`: ADR-001 (C++20, C ABI), ADR-002 (licence split), ADR-003 (layers), ADR-004 (Qt 6.8 LTS), ADR-005 (libplacebo), ADR-012 (CMake ≥ 3.25 + presets + vcpkg manifest + `third_party/*.cmake`), ADR-014 (Android `SurfaceView` HDR), ADR-015 (runtime decoder plugins), ADR-016 (distribution channels), repository layout §0.4. **This chapter owns phase 0** (WI-0.000…WI-0.073; 0.074–0.079 reserved) except the FlatBuffers schema, `io`, `BitReader`, logging and `jobs` modules, which are specified in chapters 06 and 07 (WI-0.080–0.095, and WI-1.100–1.107 and WI-1.512 as phase-0 deliverables, chapter 16 §16.3.11).

Readers: implementers who must get `cmake --preset <name>` to produce a working binary on each platform, keep CI green, and ship signed artefacts. Every version number below is a **pin** recorded in `third_party/VERSIONS.md` (§13.3.7); bumping a pin is a normal pull request, not a spec change.

---

## 13.1 Toolchains per platform

| Platform | Compiler | Minimum OS / ABI | SDK / tools | Notes |
|---|---|---|---|---|
| **macOS** (arm64, x86_64) | Apple clang from **Xcode ≥ 16.0** (clang 16-based C++20; `<format>` is not used, see the deployment-target cell) | `CMAKE_OSX_DEPLOYMENT_TARGET=13.0` (Ventura; the single value used by chapters 04 and 12 — wheels set `MACOSX_DEPLOYMENT_TARGET=13.0`). Reason: Qt 6.8 and MoltenVK 1.2.x support 12+; 13.0 is the minimum macOS of chapter 04 (FR-MAC-001). Consequence: Apple libc++ marks `std::format` and floating-point `std::to_chars` as available only from 13.3, so OVT code MUST NOT use them; all formatting goes through `fmt` (vcpkg, §13.3.2), which the 13.0 target compiles without availability errors. | Xcode command-line tools, `cmake` ≥ 3.28 (Homebrew), `ninja`, `pkg-config`, `nasm` (FFmpeg x86 asm), `meson`+`ninja` (libplacebo/dav1d/libvmaf), Python 3.11+, Vulkan SDK **not** required (MoltenVK fetched prebuilt §13.3.4) | Per-arch builds; `OVT_UNIVERSAL=OFF` by default (§13.5.1). |
| **Linux** (x86_64, aarch64) | **GCC ≥ 13** or **Clang ≥ 16** (D5; libstdc++ ≥ 13 for `<format>`, `<ranges>` completeness; Clang uses libstdc++ 13 on the CI image) | glibc ≥ 2.35 (**Ubuntu 22.04 LTS** compatibility baseline; also the CI image and the AppImage build host). Supported: Ubuntu 22.04 LTS or later and equivalent distributions; the reference desktop `linux-x64-ref` (chapter 14 §14.9.2) runs **Ubuntu 24.04 LTS** | `cmake` ≥ 3.25 (Kitware apt repo on 22.04), `ninja`, `nasm`, `meson`, `pkg-config`, `libvulkan-dev`, Wayland/X11 dev headers (`libwayland-dev`, `libxkbcommon-dev`, `libxcb-*-dev`), `libva-dev`, `mesa-vulkan-drivers` (CI software Vulkan via lavapipe for render tests) | Distro builds may use system libraries via `OVT_SYSTEM_*` options (§13.2.2). |
| **Android** (arm64-v8a, x86_64) | Clang from **NDK r27c** (27.2.12479018) | `minSdk 28` (Android 9; required for `AHardwareBuffer` dataspace APIs and `ANativeWindow_setBuffersDataSpace` stability, and the Qt 6.8 minimum), `targetSdk 36`, `compileSdk 36` (Google Play requires API 36 for new apps and updates from 31 August 2026; the target level is bumped every year in the release after Play's deadline, §13.4.2) | **Qt 6.8.3 for Android** (arm64-v8a and x86_64 kits via `aqtinstall`), **Qt Gradle Plugin 1.2** (versioned independently of Qt), **Gradle 8.10**, **Android Gradle Plugin 8.6.0** (the versions Qt 6.8's Android templates are tested with; newer 8.x work but are untested by Qt), **JDK 17** (Temurin), Android SDK Platform 36, Build-Tools 36.0.0, `cmake` 3.30 from the SDK or host, host `ninja` | Qt 6.8 facts from https://doc.qt.io/qt-6.8/android.html (API 28–35 documented, 36 verified by WI-0.050; Qt's documented NDK is r26b, OVT pins r27c for the 16 KB page-size toolchain defaults; JDK 17; Gradle 8.10/AGP 8.6.0; multi-ABI with CMake). |
| **ChromeOS** | same as Android (ARCVM) | x86_64 ABI mandatory (most Chromebooks), arm64 for Arm Chromebooks | — | Linux (Crostini) users get the Flatpak. |

Host requirements for *any* build: Python ≥ 3.11 (tools), Git ≥ 2.40, `curl`, 20 GB free disk (vcpkg + third_party + Qt), 16 GB RAM recommended (FFmpeg + libaom + Qt link steps).

### 13.1.1 Requirements realised by this chapter

Requirement ids are defined only in chapter 04; this table copies chapter 04's sentences and names what realises them here. Ids in the `FR-<AREA>-1nn` range were added to chapter 04 from this chapter's review.

| ID | Requirement (chapter 04 text) | Realised by | Acceptance test (chapter 04 text) |
|---|---|---|---|
| FR-GEN-002 | All native code MUST compile as C++20 with `-Wall -Wextra -Werror` on Clang ≥ 16 (macOS Xcode ≥ 16, Android NDK r27c — chapter 13 §13.1) and GCC ≥ 13 (Linux). | §13.1 toolchain table, §13.2.5 (`ovt::flags`, `OVT_WERROR`), WI-0.000 | Given a clean checkout, when `cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release` runs, then the build exits 0 with zero warnings. |
| FR-GEN-008 | Official binaries MUST be produced by `cmake --preset <name>` for the presets `macos-arm64-release`, `macos-x86_64-release`, `linux-x64-release`, `linux-arm64-release`, `android-arm64-release` and `android-x86_64-release` with no manual steps other than installing the documented toolchain. | §13.2.4 presets, §13.7.1 build jobs, §13.8 release process, WI-0.001, WI-0.030, WI-0.036 | Given a `v*` tag, when the `release` workflow (`.github/workflows/release.yml`, §13.7.1) runs, then all six presets produce their artefacts (DMG, AppImage/Flatpak, APK) and upload them. |
| FR-GEN-010 | The CLI and GUI MUST report the same version string (`<major>.<minor>.<patch>+<git-sha>`), the list of loaded plugins with their `id`, `version` and `license`, and the versions of FFmpeg, libaom, dav1d, libde265, libvmaf, libplacebo and Qt they were built against. | §13.8 item 1 (`/VERSION` + git SHA via `cmake/OvtVersion.cmake`), §13.3.7 pins exposed through `ovt_build_info_json` (chapter 12 §12.3.2), WI-0.000 | Given a build, when `ovt --version --format json` runs and the About dialog is opened in `ovt-studio`, then the two `plugins[]` and `third_party[]` lists are identical. |
| FR-GEN-101 | A clean clone MUST build with exactly `tools/bootstrap.sh` followed by `cmake --preset <preset> && cmake --build --preset <preset>` on each platform, with no undocumented manual step (chapter 13 §13.9.1). | §13.9.1, §13.2.4, WI-0.006 | Given a fresh Ubuntu 22.04 VM, when the two commands run, then `build/linux-x64-release/cli/ovt --version` prints the version. |
| FR-GEN-102 | An incremental rebuild after touching one `.cpp` in `core/` MUST take ≤ 10 s (ninja + ccache) on the M1 reference (chapter 13 §13.2.4). | §13.2.4 (`ccache` launchers), §13.2.5 | Given a warm build, when `touch core/src/syntax/h265_sps.cpp && time cmake --build --preset macos-arm64-debug` runs, then wall time is ≤ 10 s. |
| FR-GEN-103 | A full CI build and test run per platform MUST finish in ≤ 40 min with warm caches and ≤ 120 min cold (chapter 13 §13.7). | §13.7.1, §13.7.2, WI-0.030 | Given the GitHub Actions job durations of one week, when they are read from the Actions API, then every required job's median is ≤ 40 min and no cold run exceeds 120 min. |
| FR-GEN-104 | Release binaries MUST be reproducible given the same toolchain pins: two builds of the same tag differ only in signatures (`SOURCE_DATE_EPOCH`, `-ffile-prefix-map`, deterministic archives; chapter 13 §13.8). | §13.2.5, §13.4.8, §13.8, WI-0.056 | Given a tag, when `tools/repro_check.sh <tag>` builds it twice, then `diffoscope` reports no difference outside the signature blocks. |

---

## 13.2 CMake structure

### 13.2.0 Build-system decision and the Bazel option

The owner confirmed CMake everywhere on 2026-11-03 (ADR-012). Bazel with `rules_foreign_cc` was evaluated. It can wrap FFmpeg (`configure_make`), libaom and libde265 (`cmake`), and libvmaf and libplacebo (`meson`). It cannot replace the Qt 6 build functions (`qt_add_qml_module`, moc/rcc/qmlcachegen) or the Gradle/`androiddeployqt` Android packaging, so a Bazel build would always run beside CMake and Gradle.

To keep a later move of the non-GUI layers to Bazel cheap, implementers MUST keep these properties (they are already required elsewhere in this chapter and are restated here as invariants):

1. Every third-party version and source hash is pinned only in `third_party/VERSIONS.md` (§13.3.7), never in a `CMakeLists.txt`.
2. Each `third_party/*.cmake` script drives the library's own build system with flags listed in one place, so each script maps one-to-one onto a `rules_foreign_cc` rule.
3. `ovt_core`, `ovt_cli`, the `ovt-plugin-*` targets and the tests never link or include anything under `app/` or `android/`; the GUI consumes the core only through its installed headers and CMake package (§13.2.6).
4. Generated sources (FlatBuffers, §13.2.3) are produced by explicit custom commands with declared inputs and outputs.

A migration of the core, CLI, plugins and tests to Bazel, if ever wanted, is a new work item in a new range assigned by the editor (chapter 16 §16.2.6); it is not scheduled.

### 13.2.1 Targets

| CMake target | Output | Type | Licence | Directory | Links |
|---|---|---|---|---|---|
| `ovt_core` | `libovt_core.so.1` / `libovt_core.1.dylib` (+ static variant `ovt_core_static` for Android codec packs and the Python wheel) | SHARED (default) | LGPL-2.1-or-later | `core/` | `fmt`, `spdlog`, `flatbuffers`, `zstd`, `hwy` (Highway), `avformat/avcodec/avutil/swscale` (LGPL FFmpeg "core" flavour §13.3.3), `zimg`, `vmaf`, `onnxruntime` (optional), `Threads` |
| `ovt_render` | `libovt_render.so.1` | SHARED | LGPL-2.1-or-later | `render/` | `ovt_core`, `libplacebo`, `Vulkan::Headers`, `Vulkan::Loader` (Linux/Android) or MoltenVK (macOS) |
| `ovt_cli` | `ovt` | EXECUTABLE | GPL-3.0-or-later | `cli/` | `ovt_core`, `CLI11`, optionally `ovt_render` (doctor probe) |
| `ovt_cli_lib` | static | STATIC | GPL-3.0-or-later | `cli/` | parsing + writers, reused by `ovt_studio` for "Copy as command" and by tests |
| `ovt_studio` | `ovt-studio` / `OVT Studio.app` / `libovt_studio.so` (Android, loaded by `QtQuickView`) | EXECUTABLE (desktop) / SHARED (Android) | GPL-3.0-or-later | `app/` | `ovt_core`, `ovt_render`, `Qt6::Core Gui Quick QuickControls2 Qml Svg`, `Qt6::Graphs` (GPL module, RD plots) |
| `ovt_plugin_aom` | `libovt_plugin_aom.{so,dylib}` | MODULE | BSD-2-Clause + AOM Patent | `plugins/ovt-plugin-aom/` | static libaom (inspection build) |
| `ovt_plugin_de265` | `libovt_plugin_de265.{so,dylib}` | MODULE | LGPL-3.0-or-later | `plugins/ovt-plugin-de265/` | static patched libde265 |
| `ovt_plugin_ffmpeg` | `libovt_plugin_ffmpeg.{so,dylib}` | MODULE | LGPL-2.1-or-later (GPL-2.0-or-later if `OVT_FFMPEG_GPL`) | `plugins/ovt-plugin-ffmpeg/` | **private static FFmpeg "plugin" flavour** with h264/hevc decoders, hidden symbols (§13.3.3) |
| `ovt_plugin_hm` | `libovt_plugin_hm.{so,dylib}` | MODULE | BSD-3-Clause | `plugins/ovt-plugin-hm/` | static HM `TLibDecoder` |
| `ovt_metric_*` | built into `ovt_core` (psnr, ssim, vmaf host, uvq host, sidecar) | OBJECT | LGPL-2.1-or-later | `core/src/metrics/` | — |
| `ovt_py` | `ovt/_core.cpython-3<xx>-<platform>.so` (one per supported CPython; pybind11 has no abi3 mode, chapter 12 §12.4.1) | MODULE | GPL-3.0-or-later | `python/` | `ovt_core_static`, `pybind11::module`, `-static-libstdc++` on Linux |
| `ovt_tests_<module>` | test executables | EXECUTABLE | per directory | `*/tests/` | `Catch2::Catch2WithMain` (ordinary test executables; D24 and the "Catch2 linkage rule" of chapter 14 §14.2.1). Only the golden runner of chapter 14 §14.2 / WI-0.550 links `Catch2::Catch2` with its own `tests/main.cpp` (logger hook, corpus-path option) |
| `ovt_fuzz_<name>` | libFuzzer binaries, created by `ovt_add_fuzz_target(<name> <source>)` (WI-0.557; source `core/tests/fuzz/fuzz_<name>.cpp`) with `RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz`, i.e. `build/linux-x64-fuzz/fuzz/ovt_fuzz_<name>` | EXECUTABLE | LGPL | `core/tests/fuzz/` | `${OVT_FUZZ_ENGINE}` (default `-fsanitize=fuzzer`) |
| `ovt_schemas` | generated headers `*_generated.h` and Python/Kotlin readers | INTERFACE | — | `core/schema/` | `flatc` |
| `ovt_json_schema`, `ovt_notices`, `ovt_completions`, `ovt_mp4ra_tables` | generated files | custom | — | `tools/` | — |

Naming of module libraries follows ADR-015: `libovt_plugin_<id>` where `<id>` is the plugin id reported by `caps()->id`. Module targets set `PREFIX "lib"` on all platforms (Android too) so the loader pattern `libovt_plugin_*.so` is universal.

### 13.2.2 Options (`CMakeLists.txt`, all `option()`/`set(CACHE)` with these defaults)

| Option | Default | Meaning |
|---|---|---|
| `OVT_BUILD_GUI` | ON (OFF when Qt not found and not Android) | build `ovt_studio` |
| `OVT_BUILD_CLI` | ON | build `ovt` |
| `OVT_BUILD_PYTHON` | OFF | build `ovt_py` (needs Python dev + pybind11) |
| `OVT_BUILD_RENDER` | ON | build `ovt_render` (OFF for a core-only server build) |
| `OVT_BUILD_PLUGIN_AOM` | ON | |
| `OVT_BUILD_PLUGIN_DE265` | ON | HEVC analysis plugin (built; *shipped* in official default downloads per §13.8.3 → NO) |
| `OVT_BUILD_PLUGIN_FFMPEG` | ON | AVC playback/analysis plugin |
| `OVT_BUILD_PLUGIN_HM` | OFF | slow oracle; ON in nightly CI |
| `OVT_BUILD_TESTS` | ON | Catch2 tests |
| `OVT_BUILD_DOCS` | OFF | Doxygen + Sphinx |
| `OVT_ENABLE_SANITIZERS` | OFF | enable the sanitizer selected by `OVT_SANITIZER` |
| `OVT_SANITIZER` | `asan-ubsan` | string `asan-ubsan\|tsan\|fuzz`: which sanitizer set §13.2.5 applies when `OVT_ENABLE_SANITIZERS=ON`; sanitizer builds also add the compile definition `OVT_SANITIZED=1` (not an option; tests read it) |
| `OVT_ENABLE_FUZZ` | OFF | libFuzzer targets (clang only) |
| `OVT_FUZZ_ENGINE` | `-fsanitize=fuzzer` | string: link flags of the fuzz engine for the `ovt_fuzz_*` targets; the OSS-Fuzz `build.sh` passes `$LIB_FUZZING_ENGINE` (chapter 14 §14.7.4) |
| `OVT_COVERAGE` | OFF | Clang source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`) on OVT targets only; used by the `coverage` job (§13.7.1, chapter 04 NFR-028) |
| `OVT_ENABLE_LTO` | ON for Release presets | `CMAKE_INTERPROCEDURAL_OPTIMIZATION` |
| `OVT_WERROR` | ON in CI presets, OFF for developer presets | warnings as errors |
| `OVT_ENABLE_UVQ` | ON desktop / OFF Android | ONNX Runtime dependency |
| `OVT_ENABLE_HW_DECODE` | ON | VideoToolbox / MediaCodec / VA-API hwaccel in the FFmpeg core flavour |
| `OVT_FFMPEG_GPL` | OFF | `--enable-gpl` for FFmpeg builds (x264/x265 for future round-trip features). Makes `ovt_core` **unbuildable** (CMake `FATAL_ERROR`) unless `OVT_FFMPEG_GPL_CORE_ACK=ON`, because an LGPL core must not link a GPL FFmpeg; the intended use is plugin-only (`OVT_FFMPEG_GPL_PLUGIN_ONLY=ON`). |
| `OVT_FFMPEG_GPL_PLUGIN_ONLY` | OFF | with `OVT_FFMPEG_GPL=ON`: apply `--enable-gpl` to the plugin flavour only; the core flavour stays LGPL |
| `OVT_FFMPEG_GPL_CORE_ACK` | OFF | explicit acknowledgement that a GPL FFmpeg may be linked into `ovt_core` (makes the whole build GPL-3.0-or-later; never set in official builds; `tools/license_gate.py` records it) |
| `OVT_PATENT_CODECS_IN_CORE` | OFF | enable h264/hevc decoders in the *core* FFmpeg flavour (distro builds on Debian/Arch, user builds). OFF = official default download (ADR-015). |
| `OVT_SYSTEM_FFMPEG`, `OVT_SYSTEM_LIBAOM`, `OVT_SYSTEM_DAV1D`, `OVT_SYSTEM_LIBPLACEBO`, `OVT_SYSTEM_LIBVMAF`, `OVT_SYSTEM_ZIMG`, `OVT_SYSTEM_DE265` | OFF | use `find_package`/`pkg-config` system libraries instead of `third_party/` builds (distro packaging). `OVT_SYSTEM_LIBAOM=ON` requires that the system libaom was built with `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1` (checked at configure time by compiling a probe against `aom/aomdx.h` `AV1_SET_INSPECTION_CALLBACK`). |
| `OVT_CLI_WITH_RENDER_PROBE` | ON desktop / OFF Android | `ovt doctor` GPU probe |
| `OVT_USE_VCPKG` | ON | OFF = no vcpkg toolchain; every dependency of §13.3.1's first row is found with plain `find_package`/`pkg-config` (Flatpak and distro builds) |
| `OVT_ONNXRUNTIME_PREBUILT` | OFF | use the pinned prebuilt ONNX Runtime archive instead of the vcpkg port (§13.3.1) |
| `OVT_BUILD_AOM_INSPECT` | OFF | also build libaom's `examples/inspect` oracle binary (`-DENABLE_EXAMPLES=1` in the libaom ExternalProject; chapter 07 §7.7; nightly CI) |
| `OVT_BUILD_METRIC_XPSNR` | ON | build the XPSNR metric plugin `plugins/ovt-metric-xpsnr/` (chapter 09) |
| `OVT_THIRD_PARTY_PREFIX` | `build/third_party/<preset>` | path: install prefix of the `third_party/*.cmake` builds (set by the presets) |
| `OVT_TP_DOWNLOADS` | `build/third_party/downloads` | path: archive download cache for `third_party/*.cmake` and `tools/third_party_offline.py` |
| `OVT_CORPORA_DIR` | `build/corpora` | path: where `tools/fetch_corpora.py` put the corpora; tests skip (not fail) when a set is missing (WI-0.040, chapter 14 §14.2) |
| `OVT_PLUGIN_DIR` | `${CMAKE_INSTALL_LIBDIR}/ovt/plugins` | install-time plugin directory, compiled into `ovt_core` as a default search path (chapter 12 §12.3.11) and exported in `ovt-config.cmake` |
| `OVT_PRODUCT_NAME` / `OVT_CLI_NAME` | `OVT Studio` / `ovt` | branding strings compiled into the GUI/CLI (chapter 15 §15.7.4 name-change clause); every user-visible product-name string derives from them |
| `OVT_BUILD_GUI_PYTHON` | OFF | embedded Python console (chapter 12 §12.5) |
| `OVT_ANDROID_CODEC_PACK` | OFF | build the split "codec pack" module (§13.4.6) |
| `OVT_UNIVERSAL` | OFF | macOS: `lipo` arm64+x86_64 artefacts (§13.5.1) |
| `OVT_INSTALL_DEV` | ON | install headers, `ovt-config.cmake`, `.pc` files |
| `OVT_VERSION_SUFFIX` | "" | appended to version (`-dev`, `-rc1`) |

**Runtime environment variables (not CMake options; listed here so the names stay unique).** `OVT_CACHE_DIR`, `OVT_PLUGIN_PATH`, `OVT_THREADS`, `OVT_LOG_LEVEL`, `OVT_FORMAT` (CLI defaults, chapter 12 §12.1.2); `OVT_CORPORA_DIR` (same name as the CMake option; the test runner reads the environment first, chapter 14 §14.2.4); `OVT_UPDATE_GOLDEN` (golden tests rewrite expectations, chapter 14); `OVT_GUI_TRACE=1`, `OVT_RENDER_STUB=1`, `OVT_RENDER_GL_FINISH=1`, `OVT_CONFIG_DIR` (GUI/render test hooks, chapters 10/11); `OVT_HDR` is the Android logcat tag, not a variable. Build-time variables consumed by presets: `QT_ROOT_DIR`, `QT_HOST_KIT`, `ANDROID_NDK_ROOT`, `ANDROID_SDK_ROOT`, `JAVA_HOME` (§13.2.4); `SOURCE_DATE_EPOCH` (§13.4.8, §13.8).

### 13.2.3 Directory `CMakeLists.txt` skeleton

```cmake
# /CMakeLists.txt
cmake_minimum_required(VERSION 3.25)
file(READ VERSION OVT_VERSION_RAW) ; string(STRIP "${OVT_VERSION_RAW}" OVT_VERSION)   # e.g. 1.2.0
project(OpenVideoTools VERSION ${OVT_VERSION} LANGUAGES C CXX)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(OvtOptions)        # all options of §13.2.2
include(OvtCompilerFlags)  # §13.2.5
include(OvtThirdParty)     # dispatches to third_party/*.cmake unless OVT_SYSTEM_* (§13.3)
include(OvtLicenseTags)    # ovt_set_license(<target> <SPDX>) → property OVT_LICENSE, consumed by tools/license_gate.py
add_subdirectory(core)
if(OVT_BUILD_RENDER) add_subdirectory(render) endif()
add_subdirectory(plugins)
if(OVT_BUILD_CLI) add_subdirectory(cli) endif()
if(OVT_BUILD_GUI) add_subdirectory(app) endif()
if(OVT_BUILD_PYTHON) add_subdirectory(python) endif()
if(OVT_BUILD_TESTS) enable_testing(); add_subdirectory(tests) endif()
include(OvtInstall)        # §13.2.6 export set, CPack
```

**Generated-code layout (single rule; chapters 06 WI-0.080/WI-0.083, 12 WI-1.500 and this chapter's WI-0.003 cite it).** Every generator writes below the build tree's generated-code root `${CMAKE_BINARY_DIR}/generated/` (= `build/<preset>/generated/`; never into the source tree): `flatc --cpp` headers to `generated/<schema>_generated.h` (added to the include path as `generated/`, so sources write `#include "ovt_model_generated.h"`); `flatc --python -o generated/python` (namespace layout, **no** `--gen-onefile`: in FlatBuffers 24.x/25.x one-file mode writes `<schema>_generated.py` at the output root, which is not importable as `ovt.fb.*`) to `generated/python/ovt/fb/<Type>.py` plus the `__init__.py` files flatc creates, so that `import ovt.fb.FrameAnalysis` works exactly as chapter 06 WI-0.083 test 3 imports it; the wheel build (chapter 12 WI-4.540) copies `generated/python/ovt/fb/` into the wheel's `ovt/fb/` — nothing is generated into `python/`; `flatc --kotlin` to `generated/kotlin/`; the CLI completion scripts to `generated/completions/` (chapter 12 WI-1.500); the box registry of chapter 08 §8.2.10 is the one exception — `box_registry.gen.cpp` is committed because `tools/gen_box_registry.py --check` guards it. `build/gen/` and a bare `generated/` are not used.

Every target calls `ovt_set_license(target "<SPDX>")`; `tools/license_gate.py` reads `cmake --build . --target ovt_license_manifest` output (`build/<preset>/license_manifest.json`) and fails on forbidden combinations (chapter 15 §15.4). The manifest's schema (owned here, produced by WI-0.000 step 4, consumed by chapter 15 WI-0.602) is:

```json
{
  "schema_version": 1,
  "preset": "linux-x64-release",
  "options": { "OVT_FFMPEG_GPL": false, "OVT_FFMPEG_GPL_CORE_ACK": false, "OVT_PATENT_CODECS_IN_CORE": false },
  "targets": [
    { "name": "ovt_core", "type": "SHARED", "spdx": "LGPL-2.1-or-later", "directory": "core/",
      "links": [ { "name": "fmt", "source": "vcpkg", "version": "11.0.2", "spdx": "MIT", "linkage": "static" },
                 { "name": "ffmpeg-core", "source": "third_party", "version": "7.1.1", "spdx": "LGPL-2.1-or-later", "linkage": "shared", "patched": false } ] },
    { "name": "ovt_plugin_de265", "type": "MODULE", "spdx": "LGPL-3.0-or-later", "directory": "plugins/ovt-plugin-de265/",
      "links": [ { "name": "libde265", "source": "third_party", "version": "1.0.15", "spdx": "LGPL-3.0-or-later", "linkage": "static", "patched": true } ] }
  ]
}
```

`source` ∈ `vcpkg | third_party | system | vendored`; `linkage` ∈ `static | shared | plugin | header-only`; `spdx` is an SPDX expression (the Licence column of `third_party/VERSIONS.md` for `third_party` rows, `vcpkg_installed/<triplet>/share/<port>/copyright` plus the port's `license` field for vcpkg rows). The file is written by a CMake script walking each target's `LINK_LIBRARIES` transitively.

### 13.2.4 `CMakePresets.json` (complete, normative)

```json
{
  "version": 6,
  "cmakeMinimumRequired": { "major": 3, "minor": 25, "patch": 0 },
  "configurePresets": [
    {
      "name": "base", "hidden": true, "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "${sourceDir}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON",
        "CMAKE_C_COMPILER_LAUNCHER": "ccache", "CMAKE_CXX_COMPILER_LAUNCHER": "ccache",
        "VCPKG_MANIFEST_MODE": "ON", "VCPKG_INSTALLED_DIR": "${sourceDir}/build/vcpkg_installed",
        "OVT_THIRD_PARTY_PREFIX": "${sourceDir}/build/third_party/${presetName}"
      },
      "environment": { "VCPKG_FEATURE_FLAGS": "manifests,versions,binarycaching", "VCPKG_OVERLAY_PORTS": "${sourceDir}/third_party/vcpkg-overlays" }
    },
    { "name": "debug", "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug", "OVT_ENABLE_LTO": "OFF", "OVT_WERROR": "OFF" } },
    { "name": "release", "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo", "OVT_ENABLE_LTO": "ON", "OVT_WERROR": "ON" } },

    { "name": "macos-arm64", "hidden": true, "inherits": "base",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Darwin" },
      "cacheVariables": { "CMAKE_OSX_ARCHITECTURES": "arm64", "CMAKE_OSX_DEPLOYMENT_TARGET": "13.0", "VCPKG_TARGET_TRIPLET": "arm64-osx-ovt",
                          "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}/macos", "OVT_ENABLE_UVQ": "ON" } },
    { "name": "macos-x86_64", "hidden": true, "inherits": "base",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Darwin" },
      "cacheVariables": { "CMAKE_OSX_ARCHITECTURES": "x86_64", "CMAKE_OSX_DEPLOYMENT_TARGET": "13.0", "VCPKG_TARGET_TRIPLET": "x64-osx-ovt",
                          "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}/macos" } },
    { "name": "macos-arm64-debug",   "inherits": ["macos-arm64", "debug"],   "displayName": "macOS arm64 Debug" },
    { "name": "macos-arm64-release", "inherits": ["macos-arm64", "release"], "displayName": "macOS arm64 Release" },
    { "name": "macos-x86_64-debug",   "inherits": ["macos-x86_64", "debug"] },
    { "name": "macos-x86_64-release", "inherits": ["macos-x86_64", "release"] },
    { "name": "macos-arm64-asan",     "inherits": ["macos-arm64", "debug"], "displayName": "macOS arm64 ASan+UBSan (nightly)",
      "cacheVariables": { "OVT_ENABLE_SANITIZERS": "ON", "OVT_SANITIZER": "asan-ubsan", "OVT_BUILD_GUI": "OFF" } },

    { "name": "linux-x64", "hidden": true, "inherits": "base",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "x64-linux-ovt", "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}/gcc_64", "OVT_ENABLE_UVQ": "ON" } },
    { "name": "linux-arm64", "hidden": true, "inherits": "base",
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" },
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "arm64-linux-ovt", "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}/gcc_arm64" } },
    { "name": "linux-x64-debug",     "inherits": ["linux-x64", "debug"] },
    { "name": "linux-x64-release",   "inherits": ["linux-x64", "release"] },
    { "name": "linux-arm64-release", "inherits": ["linux-arm64", "release"] },
    { "name": "linux-x64-asan-ubsan", "inherits": ["linux-x64", "debug"],
      "cacheVariables": { "CMAKE_C_COMPILER": "clang-16", "CMAKE_CXX_COMPILER": "clang++-16", "OVT_ENABLE_SANITIZERS": "ON", "OVT_SANITIZER": "asan-ubsan", "OVT_BUILD_GUI": "OFF" } },
    { "name": "linux-x64-tsan",      "inherits": ["linux-x64", "debug"],
      "cacheVariables": { "CMAKE_C_COMPILER": "clang-16", "CMAKE_CXX_COMPILER": "clang++-16", "OVT_ENABLE_SANITIZERS": "ON", "OVT_SANITIZER": "tsan", "OVT_BUILD_GUI": "OFF" } },
    { "name": "linux-x64-fuzz",      "inherits": ["linux-x64", "debug"],
      "cacheVariables": { "CMAKE_C_COMPILER": "clang-16", "CMAKE_CXX_COMPILER": "clang++-16", "OVT_ENABLE_FUZZ": "ON", "OVT_ENABLE_SANITIZERS": "ON", "OVT_BUILD_GUI": "OFF", "OVT_BUILD_CLI": "OFF", "OVT_BUILD_RENDER": "OFF" } },
    { "name": "linux-x64-distro",    "inherits": ["linux-x64", "release"], "displayName": "Linux using system libraries (packagers)",
      "cacheVariables": { "OVT_SYSTEM_FFMPEG": "ON", "OVT_SYSTEM_DAV1D": "ON", "OVT_SYSTEM_LIBPLACEBO": "ON", "OVT_SYSTEM_LIBVMAF": "ON", "OVT_SYSTEM_ZIMG": "ON", "OVT_SYSTEM_DE265": "ON",
                          "OVT_SYSTEM_LIBAOM": "OFF", "OVT_PATENT_CODECS_IN_CORE": "ON", "CMAKE_INSTALL_PREFIX": "/usr" },
      "toolchainFile": "" },

    { "name": "android", "hidden": true, "inherits": "base",
      "toolchainFile": "${sourceDir}/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": {
        "VCPKG_CHAINLOAD_TOOLCHAIN_FILE": "$env{ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake",
        "ANDROID_PLATFORM": "android-28", "ANDROID_STL": "c++_shared",
        "CMAKE_SYSTEM_NAME": "Android", "CMAKE_FIND_ROOT_PATH_MODE_PACKAGE": "BOTH",
        "OVT_BUILD_CLI": "ON", "OVT_BUILD_PYTHON": "OFF", "OVT_ENABLE_UVQ": "OFF", "OVT_CLI_WITH_RENDER_PROBE": "OFF",
        "OVT_BUILD_PLUGIN_HM": "OFF", "QT_HOST_PATH": "$env{QT_ROOT_DIR}/$env{QT_HOST_KIT}" } },
    { "name": "android-arm64", "hidden": true, "inherits": "android",
      "cacheVariables": { "ANDROID_ABI": "arm64-v8a", "VCPKG_TARGET_TRIPLET": "arm64-android-ovt", "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}/android_arm64_v8a" } },
    { "name": "android-x86_64", "hidden": true, "inherits": "android",
      "cacheVariables": { "ANDROID_ABI": "x86_64", "VCPKG_TARGET_TRIPLET": "x64-android-ovt", "CMAKE_PREFIX_PATH": "$env{QT_ROOT_DIR}/android_x86_64" } },
    { "name": "android-arm64-debug",   "inherits": ["android-arm64", "debug"] },
    { "name": "android-arm64-release", "inherits": ["android-arm64", "release"] },
    { "name": "android-x86_64-debug",   "inherits": ["android-x86_64", "debug"] },
    { "name": "android-x86_64-release", "inherits": ["android-x86_64", "release"] }
  ],
  "buildPresets": [
    { "name": "macos-arm64-debug",   "configurePreset": "macos-arm64-debug" },
    { "name": "macos-arm64-release", "configurePreset": "macos-arm64-release" },
    { "name": "macos-x86_64-debug",   "configurePreset": "macos-x86_64-debug" },
    { "name": "macos-x86_64-release", "configurePreset": "macos-x86_64-release" },
    { "name": "macos-arm64-asan",     "configurePreset": "macos-arm64-asan" },
    { "name": "linux-x64-debug",     "configurePreset": "linux-x64-debug" },
    { "name": "linux-x64-release",   "configurePreset": "linux-x64-release" },
    { "name": "linux-arm64-release", "configurePreset": "linux-arm64-release" },
    { "name": "linux-x64-asan-ubsan", "configurePreset": "linux-x64-asan-ubsan" },
    { "name": "linux-x64-tsan",      "configurePreset": "linux-x64-tsan" },
    { "name": "linux-x64-fuzz",      "configurePreset": "linux-x64-fuzz" },
    { "name": "linux-x64-distro",    "configurePreset": "linux-x64-distro" },
    { "name": "android-arm64-debug",   "configurePreset": "android-arm64-debug" },
    { "name": "android-arm64-release", "configurePreset": "android-arm64-release" },
    { "name": "android-x86_64-debug",   "configurePreset": "android-x86_64-debug" },
    { "name": "android-x86_64-release", "configurePreset": "android-x86_64-release" }
  ],
  "testPresets": [
    { "name": "default", "hidden": true, "output": { "outputOnFailure": true, "outputJUnitFile": "${sourceDir}/build/${presetName}/junit.xml" }, "execution": { "jobs": 4, "timeout": 600 } },
    { "name": "asan-env", "hidden": true, "environment": { "ASAN_OPTIONS": "detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1:halt_on_error=1", "UBSAN_OPTIONS": "print_stacktrace=1:halt_on_error=1" } },
    { "name": "tsan-env", "hidden": true, "environment": { "TSAN_OPTIONS": "halt_on_error=1 second_deadlock_stack=1" } },

    { "name": "macos-arm64-debug",   "inherits": "default", "configurePreset": "macos-arm64-debug" },
    { "name": "macos-arm64-release", "inherits": "default", "configurePreset": "macos-arm64-release" },
    { "name": "macos-arm64-asan",    "inherits": ["default", "asan-env"], "configurePreset": "macos-arm64-asan" },
    { "name": "linux-x64-debug",     "inherits": "default", "configurePreset": "linux-x64-debug" },
    { "name": "linux-x64-release",   "inherits": "default", "configurePreset": "linux-x64-release" },
    { "name": "linux-x64-asan-ubsan", "inherits": ["default", "asan-env"], "configurePreset": "linux-x64-asan-ubsan" },
    { "name": "linux-x64-tsan",      "inherits": ["default", "tsan-env"], "configurePreset": "linux-x64-tsan" },

    { "name": "pr-filter",         "hidden": true, "filter": { "include": { "label": "fast" }, "exclude": { "label": "needs_display|needs_android|canary" } } },
    { "name": "nightly-filter",    "hidden": true, "filter": { "exclude": { "label": "canary" } } },
    { "name": "sanitizers-filter", "hidden": true, "filter": { "include": { "label": "fast|golden" }, "exclude": { "label": "needs_display|needs_android|canary|no_sanitizer" } } },
    { "name": "perf-filter",       "hidden": true, "filter": { "include": { "label": "perf" } }, "execution": { "jobs": 1 } },

    { "name": "pr",         "inherits": ["default", "pr-filter"],         "configurePreset": "linux-x64-debug" },
    { "name": "nightly",    "inherits": ["default", "nightly-filter"],    "configurePreset": "linux-x64-release" },
    { "name": "sanitizers", "inherits": ["default", "sanitizers-filter", "asan-env"], "configurePreset": "linux-x64-asan-ubsan" },
    { "name": "perf",       "inherits": ["default", "perf-filter"],       "configurePreset": "linux-x64-release" },
    { "name": "pr-macos",         "inherits": ["default", "pr-filter"],         "configurePreset": "macos-arm64-release" },
    { "name": "nightly-macos",    "inherits": ["default", "nightly-filter"],    "configurePreset": "macos-arm64-release" },
    { "name": "sanitizers-macos", "inherits": ["default", "sanitizers-filter", "asan-env"], "configurePreset": "macos-arm64-asan" },
    { "name": "perf-macos",       "inherits": ["default", "perf-filter"],       "configurePreset": "macos-arm64-release" },
    { "name": "pr-android",       "inherits": ["default", "pr-filter"],         "configurePreset": "android-x86_64-debug" }
  ],
  "workflowPresets": [
    { "name": "ci-macos-arm64", "steps": [ { "type": "configure", "name": "macos-arm64-release" }, { "type": "build", "name": "macos-arm64-release" }, { "type": "test", "name": "macos-arm64-release" } ] },
    { "name": "ci-linux-x64",   "steps": [ { "type": "configure", "name": "linux-x64-release" },   { "type": "build", "name": "linux-x64-release" },   { "type": "test", "name": "linux-x64-release" } ] },
    { "name": "ci-linux-asan-ubsan", "steps": [ { "type": "configure", "name": "linux-x64-asan-ubsan" }, { "type": "build", "name": "linux-x64-asan-ubsan" }, { "type": "test", "name": "sanitizers" } ] },
    { "name": "ci-macos-asan",  "steps": [ { "type": "configure", "name": "macos-arm64-asan" },     { "type": "build", "name": "macos-arm64-asan" },     { "type": "test", "name": "sanitizers-macos" } ] }
  ]
}
```

**Preset names (D23; this list is exhaustive — chapters 14 and 16 use these names and no others).** Configure/build: `macos-arm64-{debug,release}`, `macos-arm64-asan`, `macos-x86_64-{debug,release}`, `linux-x64-{debug,release}`, `linux-x64-asan-ubsan`, `linux-x64-tsan`, `linux-x64-fuzz`, `linux-x64-distro`, `linux-arm64-release`, `android-arm64-{debug,release}`, `android-x86_64-{debug,release}`. Test presets: one per native-host debug/release/sanitizer configure preset (`macos-arm64-{debug,release,asan}`, `linux-x64-{debug,release,asan-ubsan,tsan}`; runs everything; cross presets such as `android-*`, `macos-x86_64-*`, `linux-arm64-release` and the `fuzz`/`distro` presets have none), plus the four **lane** presets `pr` (label `fast`, excluding `needs_display|needs_android|canary`), `nightly` (all but `canary`), `sanitizers` (`fast|golden` under ASan+UBSan) and `perf` (label `perf`, single job), bound to the Linux CI configure presets, and their macOS counterparts `pr-macos`, `nightly-macos`, `sanitizers-macos`, `perf-macos` (bound to `macos-arm64-release` / `macos-arm64-asan`) and `pr-android` (emulator). A CMake test preset is bound to exactly one configure preset and `configurePreset` does not expand macros, so one host-agnostic `pr` cannot exist; wherever this specification writes `ctest --preset pr` (chapters 14 §14.1.2/§14.11, 16 §16.1/§16.2/§16.6) it means the lane preset of the host (`pr` after a `linux-*` configure, `pr-macos` after a `macos-*` configure), and `tools/ctest.sh <pr|nightly|sanitizers|perf> [configure-preset]` (WI-0.001) is the dispatcher that CI and the exit demos call. Lane presets filter by the CTest labels that `catch_discover_tests` derives from the Catch2 tags of chapter 14 §14.2.5 (`fast`, `slow`, `golden`, `perf`, `needs_corpus`, `needs_display`, `needs_android`, `no_sanitizer`, `canary`). JUnit output of every test preset goes to `build/<preset>/junit.xml` (`outputJUnitFile`, CMake ≥ 3.21). Workflow presets: `ci-macos-arm64`, `ci-linux-x64`, `ci-linux-asan-ubsan`, `ci-macos-asan` (nightly).

Custom vcpkg triplets `*-ovt` live in `third_party/vcpkg-triplets/` and set `VCPKG_LIBRARY_LINKAGE static`, `VCPKG_CRT_LINKAGE dynamic`, `VCPKG_OSX_DEPLOYMENT_TARGET 13.0`, `VCPKG_CXX_FLAGS -fvisibility=hidden`, and for Android `VCPKG_CMAKE_SYSTEM_NAME Android`, `VCPKG_CMAKE_SYSTEM_VERSION 28` (the API level; vcpkg has no `VCPKG_ANDROID_API` variable; the triplet file names keep vcpkg's `x64-` spelling — `x64-osx-ovt`, `x64-android-ovt` — while the preset names use `x86_64`) plus `VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_STL=c++_shared` (static vcpkg libs are absorbed into `libovt_core.so`, keeping the `.so` count small on Android). Every preset has its own `binaryDir` (`${presetName}`), so the `asan-ubsan`, `tsan` and `fuzz` trees never mix sanitizer flags. Clang on the Ubuntu 22.04 CI image comes from `apt.llvm.org` (`clang-16`); the developer presets leave the compiler to CMake's default. Environment variables consumed by presets: `QT_ROOT_DIR` (e.g. `~/Qt/6.8.3`), `QT_HOST_KIT` (`macos` or `gcc_64`), `ANDROID_NDK_ROOT`, `ANDROID_SDK_ROOT`, `JAVA_HOME`.

### 13.2.5 Compiler flags (`cmake/OvtCompilerFlags.cmake`)

Applied through an INTERFACE target `ovt::flags` linked by every OVT target (never to third-party builds). **This section is the single source of the warning set (D24):** chapter 14 §14.8.3 and WI-0.559 reference it and define no second list; there is no separate warning-applying CMake function (the `cmake/OvtWarnings.cmake` of chapter 14 WI-0.559 is not written) — linking `ovt::flags` is the only mechanism.

**Common.** `-std=c++20` (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_EXTENSIONS OFF`), `-fvisibility=hidden -fvisibility-inlines-hidden`, `-fno-strict-aliasing` (bitstream code), `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2` (Release), `-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/ovt` (reproducibility), `-fPIC`.

**Warnings (all compilers):** `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align -Wcast-qual -Wundef -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Wnon-virtual-dtor -Wmisleading-indentation`. GCC adds `-Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast`. Clang adds `-Wthread-safety -Wextra-semi -Wunreachable-code-aggressive -Wrange-loop-analysis`. With `OVT_WERROR=ON` (every CI preset; `OFF` only in the developer `debug` presets and for downstream packagers): `-Werror`; third-party headers are included with `-isystem` so their warnings never fail the build. Disallowed suppressions: a `#pragma` or `-Wno-*` in OVT code needs a `// NOLINT(reason)`-style comment `// OVT-WARN-SUPPRESS: <issue-url>`.

**Sanitizer presets (`OVT_SANITIZER`).** `asan-ubsan`: `-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1 -g`; `tsan`: `-fsanitize=thread -O1 -g`; `fuzz` (with `OVT_ENABLE_FUZZ`): `-fsanitize=fuzzer-no-link,address,undefined` on `ovt_core`, `${OVT_FUZZ_ENGINE}` (default `-fsanitize=fuzzer`) on the fuzz executables. Sanitizer builds add the compile definition `OVT_SANITIZED=1` (via `ovt::flags`) so tests can relax timing assertions.

**Release.** `-O2` (not `-O3`: measured no gain on parsers, worse code size), `-DNDEBUG`, LTO via `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` (thin LTO on clang: `-flto=thin`), `-g1` with separate debug symbols (`dsymutil` on macOS; `objcopy --only-keep-debug` to `*.debug` on Linux, uploaded as release artefacts for symbolisation). Linker: `-Wl,--as-needed -Wl,-z,relro,-z,now -Wl,--gc-sections` (Linux), `-Wl,-dead_strip` (macOS), `-Wl,-z,max-page-size=16384` (Android, 16 KB page requirement for Android 15+ targets).

**SIMD.** No `-march=native` anywhere. Highway (`hwy`) dynamic dispatch for own kernels; x86_64 baseline is x86-64-v2 (SSE4.2), runtime dispatch up to AVX-512; arm64 baseline is ARMv8.0 with NEON, dispatch to SVE2 where present.

### 13.2.6 Install and export (`cmake/OvtInstall.cmake`)

```cmake
install(TARGETS ovt_core ovt_render EXPORT OvtTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ovt)
install(DIRECTORY core/include/ovt/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ovt PATTERN "cxx" EXCLUDE)   # C11 headers only; ovt/cxx/*.hpp are in-tree C++ helpers (chapter 12 §12.3.1 rule 1, D22)
install(TARGETS ovt_plugin_aom ovt_plugin_ffmpeg ovt_plugin_de265 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/ovt/plugins COMPONENT plugins)
install(EXPORT OvtTargets NAMESPACE ovt:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ovt FILE ovt-targets.cmake)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/ovt-config.cmake.in ${CMAKE_BINARY_DIR}/ovt-config.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ovt)
get_target_property(_ovt_api_version ovt_core VERSION)   # the C API version (OVT_API_VERSION_*, 1.0.0), not the 0.x product version (issue #13)
write_basic_package_version_file(${CMAKE_BINARY_DIR}/ovt-config-version.cmake VERSION ${_ovt_api_version} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_BINARY_DIR}/ovt-config.cmake ${CMAKE_BINARY_DIR}/ovt-config-version.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ovt)
configure_file(cmake/ovt-core.pc.in ovt-core.pc @ONLY) ; install(FILES ${CMAKE_BINARY_DIR}/ovt-core.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
```

`ovt-config.cmake.in` exposes `ovt::core` and `ovt::render` imported targets and the variables `OVT_PLUGIN_DIR` and `OVT_SCHEMA_DIR` (installed `.fbs` files under `share/ovt/schema/` for third-party plugin authors). **Install layout (owner of these paths; chapters 09, 11 and 12 cite them):** shared libraries in `${CMAKE_INSTALL_LIBDIR}`; `ovt` in `${CMAKE_INSTALL_BINDIR}`; C headers in `${CMAKE_INSTALL_INCLUDEDIR}/ovt/` (no `cxx/`); **all** runtime plugins — analysis decoders `libovt_plugin_*` and metric plugins `libovt_metric_*` (chapter 09 §9.7) — in `OVT_PLUGIN_DIR = ${CMAKE_INSTALL_LIBDIR}/ovt/plugins/` (one directory; there is no `share/ovt/metrics/`); `.fbs` schemas in `OVT_SCHEMA_DIR = ${CMAKE_INSTALL_DATADIR}/ovt/schema/`; licence texts in `${CMAKE_INSTALL_DATADIR}/ovt/licenses/` (chapter 15 §15.6); JSON Schemas in `${CMAKE_INSTALL_DATADIR}/ovt/schemas/json/`. Downloadable metric models (UVQ, chapter 09 §9.6) are **not installed**: they are fetched on first use into `<cache_root>/models/<model-id>/` under the cache root of chapter 06 §6.7 (D32) and only their `manifest.json` ships: repository source `core/share/models/<model-id>/manifest.json`, installed to `${CMAKE_INSTALL_DATADIR}/ovt/models/<model-id>/manifest.json` (macOS `Contents/Resources/ovt/models/`, Android APK assets `models/`). The macOS bundle maps `LIBDIR/ovt/plugins` to `Contents/PlugIns/` and `DATADIR/ovt` to `Contents/Resources/ovt/` (§13.5.2); Android uses `nativeLibraryDir` (§13.4.6).

**Package version.** The CMake package version is the **C API version** (`OVT_API_VERSION_MAJOR.MINOR.PATCH` of `core/include/ovt/version.h`, the version the SONAME follows), not the product version of the `VERSION` file, which is `0.x` until 1.0 (chapter 12 §12.3.2, issue #4). `find_package(ovt 1 …)` therefore works from the first release; an ABI break that bumps `OVT_API_VERSION_MAJOR` (the `abi-check` job) makes old `find_package(ovt 1)` requests fail, which is intended. The product version stays visible through `ovt_version_string()`.

A downstream `find_package(ovt 1 CONFIG REQUIRED)` MUST work from the install tree with no vcpkg (dependencies are statically absorbed; only system libs remain: `libc++/libstdc++`, `libz`, Vulkan loader, platform frameworks).

CPack components: `core` (libs + plugins without AVC/HEVC), `codecs-patented` (ffmpeg plugin with h264/hevc, de265 plugin), `cli`, `gui`, `dev`. Generators: `DragNDrop` is **not** used (DMG built by `tools/mac/make_dmg.sh` for control over layout), `TGZ` for Linux binaries, `DEB`/`RPM` as hints for packagers only.

---

## 13.3 Dependency management

### 13.3.1 Decision summary

| Dependency | Source | Why |
|---|---|---|
| fmt, spdlog, flatbuffers (library + `flatc` host tool), zstd, catch2, nlohmann-json, highway (`hwy`), cli11, pybind11, zimg, vulkan-headers, vulkan-loader (Linux desktop only — Android uses the NDK's system `libvulkan.so`, macOS links MoltenVK directly), glslang (libplacebo shader compiler), lcms (libplacebo ICC), libxxhash (cache keys), libpng (test image I/O), xz (`tools/` only) | **vcpkg manifest** (`vcpkg.json`, baseline-pinned); with `OVT_USE_VCPKG=OFF` the same names are resolved by `find_package`/`pkg-config` (Flatpak, distros) | Ports exist, build with CMake, static absorption works on all three platforms. |
| **onnxruntime** | vcpkg port `onnxruntime` (exists since vcpkg 2024.10; CPU execution provider) for macOS and Linux x64/arm64. If the port fails on a platform in CI, `third_party/onnxruntime.cmake` downloads the official prebuilt release archive (`onnxruntime-osx-arm64-<v>.tgz`, `onnxruntime-linux-x64-<v>.tgz`) pinned by SHA-256. Not built on Android (`OVT_ENABLE_UVQ=OFF`; UVQ on Android is a phase-4+ question, chapter 17). | Port is heavy (30+ min cold) but gives a static lib; prebuilt is the escape hatch. |
| **FFmpeg** | **Own build** `third_party/ffmpeg.cmake` (ExternalProject running `configure`/`make`), two flavours (§13.3.3). vcpkg's `ffmpeg` port is **not** used: its feature flags cannot express per-decoder enable/disable, hwaccel selection per platform, the `--disable-decoder=h264,hevc` default or two flavours from one source. | Control over GPL/LGPL, patent codecs, hwaccels, reproducible Corresponding Source. |
| **libaom** | `third_party/libaom.cmake` (ExternalProject, CMake) with `-DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1 -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_DOCS=0 -DCONFIG_AV1_ENCODER=0 -DCONFIG_MULTITHREAD=1 -DBUILD_SHARED_LIBS=0` | Inspection flags are not a vcpkg feature. |
| **dav1d** | `third_party/dav1d.cmake` → Meson wrapper (§13.3.5); options `-Denable_tools=false -Denable_tests=false -Ddefault_library=static` | Used by FFmpeg (`--enable-libdav1d`) for AV1 playback. |
| **libde265** | `third_party/libde265.cmake` (CMake ExternalProject) with **patch application step** applying `plugins/ovt-plugin-de265/patches/0001-de265-internals-api.patch` (chapter 07 spec) and `-DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DBUILD_SHARED_LIBS=OFF` | Patched build must be isolated from any system libde265. |
| **HM** | `third_party/hm.cmake` (CMake ExternalProject from `vcgit.hhi.fraunhofer.de/jvet/HM`, tag pinned, `TLibCommon`+`TLibDecoder` only) | Oracle; optional. |
| **libvmaf** | `third_party/libvmaf.cmake` → Meson wrapper, `-Denable_float=true -Denable_avx512=true -Dbuilt_in_models=true -Denable_cuda=false -Ddefault_library=static` | Built-in models give bit-exact `vmaf` CLI parity without model files. |
| **libplacebo** | `third_party/libplacebo.cmake` → Meson wrapper, `-Dvulkan=enabled -Dvk-proc-addr=enabled -Dopengl=enabled(Android)/disabled -Dgl-proc-addr=enabled(Android) -Dglslang=enabled -Dshaderc=disabled -Dlcms=enabled -Ddemos=false -Dtests=false -Ddefault_library=static -Dvulkan-registry=<vcpkg vk.xml>` (option names verified against libplacebo v7.349 `meson_options.txt`; there is no `vulkan-loader` option — entry points are resolved through `vkGetInstanceProcAddr` of whatever loader the platform provides) | Not in vcpkg with the needed options on Android. |
| **MoltenVK** (macOS) | `third_party/moltenvk.cmake` downloads the KhronosGroup/MoltenVK GitHub release archive (`MoltenVK-macos.tar`) pinned by SHA-256; `libMoltenVK.dylib` is bundled in `Contents/Frameworks/` and loaded via the Vulkan loader ICD (`vulkan-loader` from vcpkg) **or** linked directly as the Vulkan implementation (`PL_VK_IMPORT`). Decision: **direct link to MoltenVK** (no loader, no layers) in release; loader + validation layers only in debug presets via `VK_ICD_FILENAMES`. | Fewer moving parts; mpv/VLC do the same. |
| **Qt 6.8.3** | **Prebuilt** via `aqtinstall` (`tools/install_qt.py`, pinned `6.8.3`; the essential modules `qtbase`, `qtdeclarative`, `qtsvg` come with every kit and MUST NOT be passed to `aqt -m` (it rejects them); add-on modules `-m qtshadertools qtgraphs qtimageformats`; Android kits `android_arm64_v8a android_x86_64` and the host kit). Official Qt installer is accepted for developers; CI uses aqtinstall. Qt is **never** built from source in CI. | 6.8 is the LTS line of ADR-004; aqt is cacheable and scriptable. |
| **Android SDK/NDK** | `sdkmanager` pins in `android/gradle/libs.versions.toml` and `tools/bootstrap_android.sh` | |

### 13.3.2 `vcpkg.json`

The listing below is the manifest's **end state**, not the content of one work item. WI-0.002 commits only the nine ports its Goal names (with `catch2` keeping its `!android` platform filter) plus the `features` block; every later work item adds its ports in the PR that first links them: `benchmark` with WI-0.550/WI-0.560, `zimg` with the metrics normalisation (chapter 09), `vulkan-headers`, `vulkan-loader`, `glslang` and `lcms` with WI-0.015/WI-1.400, `libpng` with the first image test, `pybind11` with `python/`, and `onnxruntime` with the UVQ host (chapter 09 §9.6). Ports nobody links yet would otherwise build on every first configure (`onnxruntime` alone takes 30+ min cold, §13.3.1). `docs/dev/dependencies.md` states the rule (issue #8).

```json
{
  "$schema": "https://raw.githubusercontent.com/microsoft/vcpkg-tool/main/docs/vcpkg.schema.json",
  "name": "openvideotools",
  "version-string": "1.0.0",
  "builtin-baseline": "<40-hex vcpkg commit pinned in third_party/VERSIONS.md>",
  "dependencies": [
    "fmt", "spdlog", { "name": "flatbuffers", "host": true }, "flatbuffers", "zstd", "nlohmann-json", "highway", "cli11", "xxhash", "libpng",
    "zimg",
    "vulkan-headers",
    { "name": "vulkan-loader", "platform": "linux" },
    { "name": "glslang", "features": ["tools"] },
    "lcms",
    { "name": "catch2", "platform": "!android" },
    { "name": "benchmark", "platform": "!android" },
    { "name": "pybind11", "platform": "!android" },
    { "name": "onnxruntime", "platform": "(osx | linux) & !uwp" }
  ],
  "features": {
    "tests": { "description": "unit tests and the bench_* perf harness (chapter 14 §14.9)", "dependencies": [ "catch2", "benchmark" ] },
    "benchmarks": { "description": "Google Benchmark perf harness (chapter 14 §14.9, WI-0.560)", "dependencies": [ "benchmark" ] },
    "python": { "description": "python bindings", "dependencies": [ "pybind11" ] }
  },
  "overrides": [ ]
}
```

`builtin-baseline` is the single pin for all port versions; `vcpkg x-update-baseline` bumps are reviewed PRs. Binary caching uses the `files` provider on a directory that `actions/cache` restores and saves (`VCPKG_BINARY_SOURCES="clear;files,${{ github.workspace }}/build/vcpkg-archives,readwrite"`) in CI — the former `x-gha` provider was removed from vcpkg in 2025 — and `files,<path>` locally.

### 13.3.3 FFmpeg flavours (`third_party/ffmpeg.cmake`)

One source tarball (pinned release, e.g. `n7.1.1`), two ExternalProject builds:

| Flavour | Linked by | Linkage | `configure` (common part first) |
|---|---|---|---|
| **common** | | | `--disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-avfilter --disable-encoders --disable-muxers --enable-muxer=null,framemd5,rawvideo,yuv4mpegpipe --disable-network --disable-debug --enable-pic --disable-autodetect --enable-zlib --pkg-config-flags=--static --cc=<toolchain> --prefix=<OVT_THIRD_PARTY_PREFIX>/ffmpeg-<flavour>` plus per-platform `--enable-cross-compile --arch= --target-os=android --sysroot=` (Android) / `--enable-videotoolbox --disable-audiotoolbox` (macOS) / `--enable-vaapi --disable-vulkan` (Linux; FFmpeg's `configure` has no `--enable-X=no` form) / `--enable-mediacodec --enable-jni` (Android). |
| **core** (`ffmpeg-core`) | `ovt_core` | **shared** (`--enable-shared --disable-static`), SONAMEs `libavcodec.so.61` etc. bundled; `--build-suffix=_ovt` → `libavcodec_ovt.so` to avoid clashing with a system FFmpeg | `--enable-libdav1d --disable-decoder=h264,hevc,h264_mediacodec,hevc_mediacodec,vvc,aac,aac_fixed,aac_latm,aac_at,aac_mediacodec,mpeg4,mpeg4_mediacodec,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,vc1,vc1image,wmv3,wmv3image --disable-hwaccel=h264_videotoolbox,hevc_videotoolbox,h264_vaapi,hevc_vaapi,mpeg4_videotoolbox,mpeg4_vaapi,vc1_vaapi,wmv3_vaapi` (besides AVC/HEVC, the other pool-covered codecs H.266/VVC, AAC/HE-AAC, MPEG-4 Part 2 Visual (with its Microsoft pre-standard variants MS-MPEG4 v1–v3 and WMV7/WMV8, `msmpeg4v1`–`msmpeg4v3`, `wmv1`, `wmv2`, which share FFmpeg's `msmpeg4dec` code) and VC-1 are left out too: OVT probes and demuxes them but never decodes them in the core, and no requirement asks it to; VideoToolbox and VA-API exist only as *hwaccels*, not as decoders — `h264_videotoolbox`/`h264_vaapi` in `--disable-decoder=` would match nothing; MediaCodec has real decoder entries) **unless** `OVT_PATENT_CODECS_IN_CORE=ON` (then none of those `--disable-*` are passed). GPL **never** (`--disable-gpl`). Note: the demuxers for MP4/Matroska/TS **and the `h264`/`hevc`/`av1`/`vp9` parsers and the `*_mp4toannexb` bitstream filters stay enabled in every flavour** — the raw `h264`/`hevc` demuxers and the `mpegts` demuxer `_select` their parsers in `configure` and need them to split access units (chapter 08 §8.7 relies on ffprobe packet-count parity), and a parser or bitstream filter is not a decoder. |
| **plugin** (`ffmpeg-plugin`) | `ovt_plugin_ffmpeg` | **static** (`--disable-shared --enable-static`), all symbols hidden in the plugin via `-fvisibility=hidden` on the plugin and linker `-Wl,--exclude-libs,ALL` (Linux/Android) / `-Wl,-exported_symbol,_ovt_analysis_decoder_v1 -Wl,-exported_symbol,_ovt_plugin_describe` (macOS) | `--enable-decoder=h264,hevc --enable-hwaccel=<platform h264/hevc> --disable-everything-else-via-common` (parsers and bitstream filters are already on from the common part); `--enable-gpl` only if `OVT_FFMPEG_GPL=ON` (plugin then becomes GPL-2.0-or-later and `ovt_set_license` records it). |

Rationale (binding): the core stays LGPL and free of pool-covered decoders by default (ADR-002, ADR-015); the AVC/HEVC plugin carries its own private FFmpeg so a single `libovt_plugin_ffmpeg.so` file is the whole "codec pack" for AVC playback+analysis, and no symbol of it leaks into the process (two FFmpeg copies coexist). `OVT_SYSTEM_FFMPEG=ON` (distro preset) replaces both flavours with the system FFmpeg, dynamically linked, and `ovt_plugin_ffmpeg` becomes a thin dynamic wrapper. FFmpeg's `configure` is run with `SOURCE_DATE_EPOCH` and `--extra-cflags=-ffile-prefix-map=...` for reproducibility. The trademark notice "This software uses libraries from the FFmpeg project under the LGPLv2.1" is emitted into `THIRD_PARTY_NOTICES.md` (chapter 15).

### 13.3.4 ExternalProject conventions (`third_party/*.cmake`)

Every script follows the same template (`third_party/_template.cmake`):

```cmake
# third_party/libaom.cmake — SPDX-License-Identifier: LGPL-2.1-or-later
include(OvtThirdPartyCommon)                      # ovt_tp_fetch(), ovt_tp_apply_patches(), ovt_tp_meson(), ovt_tp_import_static()
ovt_tp_declare(libaom
  URL      https://storage.googleapis.com/aom-releases/libaom-${OVT_TP_libaom_VERSION}.tar.gz
  SHA256   ${OVT_TP_libaom_SHA256}                   # from third_party/versions.cmake (generated from VERSIONS.md)
  LICENSE  "BSD-2-Clause AND LicenseRef-AOM-Patent-License-1.0"
  LICENSE_FILES LICENSE PATENTS)
ExternalProject_Add(tp_libaom
  URL ${OVT_TP_libaom_URL} URL_HASH SHA256=${OVT_TP_libaom_SHA256}
  DOWNLOAD_DIR ${OVT_TP_DOWNLOADS} PREFIX ${OVT_THIRD_PARTY_PREFIX}/libaom
  PATCH_COMMAND ${OVT_TP_PATCH} ${CMAKE_SOURCE_DIR}/third_party/patches/libaom   # applies *.patch in lexical order with `git apply --check` first; no-op when the directory is empty
  CMAKE_ARGS ${OVT_TP_CMAKE_TOOLCHAIN_ARGS} -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DBUILD_SHARED_LIBS=OFF
             -DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1 -DCONFIG_AV1_ENCODER=0 -DCONFIG_MULTITHREAD=1
             -DENABLE_EXAMPLES=0 -DENABLE_TESTS=0 -DENABLE_DOCS=0 -DENABLE_TOOLS=0 -DCONFIG_WEBM_IO=0 -DAOM_TARGET_CPU=${OVT_AOM_CPU}
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libaom.a)
ovt_tp_import_static(tp::aom tp_libaom lib/libaom.a include)
```

Rules: downloads are verified by SHA-256 (never a bare `GIT_REPOSITORY` without `GIT_TAG` = full commit SHA); all patches live in `third_party/patches/<dep>/NNNN-*.patch` (or in `plugins/ovt-plugin-*/patches/` when they are the plugin's reason to exist) and carry a header with `Upstream-Status: Submitted <url> | Pending | Inappropriate [reason]` (OpenEmbedded convention) — chapter 15 §15.5 requires an upstreaming attempt for decoder patches; the installed prefix is `build/third_party/<preset>/<dep>/` and is cached in CI keyed by the hash of `third_party/VERSIONS.md` + the script + patches; the `ovt_tp_meson()` helper (§13.3.5) wraps Meson builds; `tools/third_party_offline.py` pre-downloads every archive into `OVT_TP_DOWNLOADS` for offline/F-Droid builds.

### 13.3.5 Meson wrapper (`cmake/OvtMeson.cmake`)

```cmake
function(ovt_tp_meson name)   # args: SOURCE_DIR, OPTIONS..., OUTPUT_LIBS...
  # 1. writes <binary>/third_party/<name>-cross.ini from the active CMake toolchain:
  #    [binaries] c = '<CMAKE_C_COMPILER>' cpp = ... ar = ... strip = ... pkg-config = '<vcpkg pkgconf>'
  #    [host_machine] system = 'android'|'darwin'|'linux' cpu_family = 'aarch64'|'x86_64' cpu = ... endian = 'little'
  #    [built-in options] c_args = [...CMAKE_C_FLAGS split...] c_link_args = [...] pkg_config_path = ['<vcpkg_installed>/<triplet>/lib/pkgconfig']
  # 2. ExternalProject_Add(tp_${name} ... CONFIGURE_COMMAND meson setup <BINARY_DIR> <SOURCE_DIR> --prefix <INSTALL_DIR> --libdir lib --buildtype release
  #       --default-library static -Db_lto=false --cross-file <ini> (omitted when host == target and not Android) ${OPTIONS}
  #    BUILD_COMMAND meson compile -C <BINARY_DIR>   INSTALL_COMMAND meson install -C <BINARY_DIR>)
  # 3. imports OUTPUT_LIBS as STATIC IMPORTED targets with INTERFACE_INCLUDE_DIRECTORIES <INSTALL_DIR>/include
endfunction()
```

Meson ≥ 1.4 and Ninja are host tools installed by `tools/bootstrap.sh` (pip `meson==1.5.2` pinned). On macOS the cross file is written even for native builds to force `CMAKE_OSX_ARCHITECTURES`/deployment target into `c_args`.

### 13.3.6 Patch application step (libde265 as the reference case)

`third_party/libde265.cmake` uses `PATCH_COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/apply_patches.py --series ${CMAKE_SOURCE_DIR}/plugins/ovt-plugin-de265/patches/series --check` which: (1) refuses to run twice (writes `.ovt-patched` marker containing the SHA-256 of the series); (2) runs `git apply --check` (or `patch --dry-run -p1` when the source is not a git checkout) for every patch in `series` order and aborts on the first failure with the patch name; (3) applies; (4) records applied patch names into `<INSTALL_DIR>/share/ovt/patches.json`, which `tools/gen_notices.py` lists under the dependency in `THIRD_PARTY_NOTICES.md` ("modified; patches: …"), satisfying the modification-notice duty: for libde265 (LGPL-3.0-or-later) that is GPLv3 §5(a) ("prominent notices stating that you modified it") incorporated through LGPL-3.0 §0; for the LGPL-2.1 FFmpeg flavours it is LGPL-2.1 §2(a).

### 13.3.7 `third_party/VERSIONS.md` (template; the single source of truth, parsed by `tools/versions_to_cmake.py` into `build/<preset>/third_party/versions.cmake` at configure time — a generated file, never in the source tree)

```markdown
# Third-party dependency pins

Columns: Name | Version / commit | Licence (SPDX) | URL | Patches (relative paths) | SHA-256 of archive | Build path | Notes

| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |
|---|---|---|---|---|---|---|---|
| vcpkg baseline | `<commit>` | MIT (tool) | https://github.com/microsoft/vcpkg | – | – | manifest | bump via `vcpkg x-update-baseline` |
| FFmpeg | n7.1.1 | LGPL-2.1-or-later (core flavour); LGPL-2.1-or-later or GPL-2.0-or-later (plugin flavour, see OVT_FFMPEG_GPL) | https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz | – | `<sha>` | third_party/ffmpeg.cmake | two flavours §13.3.3; trademark notice required |
| libaom | 3.12.1 | BSD-2-Clause AND LicenseRef-AOM-Patent-License-1.0 | https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz | – | `<sha>` | third_party/libaom.cmake | CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1; `PATENTS sha256=<sha>` (the archive's `PATENTS` file; chapter 15 WI-0.600 compares `LICENSES/LicenseRef-AOM-Patent-License-1.0.txt` against it) |
| dav1d | 1.5.1 | BSD-2-Clause | https://code.videolan.org/videolan/dav1d/-/archive/1.5.1/dav1d-1.5.1.tar.bz2 | – | `<sha>` | third_party/dav1d.cmake (Meson) | playback only |
| libde265 | 1.0.15 | LGPL-3.0-or-later | https://github.com/strukturag/libde265/releases/download/v1.0.15/libde265-1.0.15.tar.gz | plugins/ovt-plugin-de265/patches/0001-de265-internals-api.patch | `<sha>` | third_party/libde265.cmake | Upstream-Status: Submitted <PR url> |
| HM | HM-18.0 (`<commit>`) | BSD-3-Clause | https://vcgit.hhi.fraunhofer.de/jvet/HM | third_party/patches/hm/0001-cmake-static-decoder-only.patch | – (git) | third_party/hm.cmake | optional oracle |
| libvmaf | 3.0.0 (any 3.x; D5) | BSD-2-Clause-Patent | https://github.com/Netflix/vmaf/archive/refs/tags/v3.0.0.tar.gz | – | `<sha>` | third_party/libvmaf.cmake (Meson) | built-in models `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1` (newer bundled models when present) |
| libplacebo | 7.349.0 | LGPL-2.1-or-later | https://code.videolan.org/videolan/libplacebo/-/archive/v7.349.0/libplacebo-v7.349.0.tar.bz2 | – | `<sha>` | third_party/libplacebo.cmake (Meson) | glslang, lcms |
| MoltenVK | 1.2.11 | Apache-2.0 | https://github.com/KhronosGroup/MoltenVK/releases/download/v1.2.11-artifacts/MoltenVK-macos.tar | – | `<sha>` | third_party/moltenvk.cmake | macOS only, prebuilt; since 1.2.10 KhronosGroup attaches the archives to the separate `v<version>-artifacts` tag (the plain `v1.2.11` URL is 404); later bumps use the same scheme |
| ONNX Runtime | 1.20.1 | MIT | vcpkg port / https://github.com/microsoft/onnxruntime/releases | – | `<sha>` (prebuilt fallback) | vcpkg or third_party/onnxruntime.cmake | CPU EP only |
| Qt | 6.8.3 | LGPL-3.0-only (most modules) / GPL-3.0-only (qtgraphs) | https://download.qt.io/official_releases/qt/6.8/ via aqtinstall 3.1.x | – | – (aqt verifies) | tools/install_qt.py | GUI only; GPL module use is allowed by ADR-002 |
| Android NDK | r27c (27.2.12479018) | NDK licence | sdkmanager | – | – | tools/bootstrap_android.sh | |
| Gradle / AGP | 8.10 / 8.6.0 | Apache-2.0 | gradle-wrapper (distributionSha256Sum set) | – | `<sha>` | android/gradle/wrapper | versions Qt 6.8 tests against |
| Qt Gradle Plugin | 1.2 | LGPL-3.0-only | Maven Central `org.qtproject.qt.gradleplugin` | – | – (Gradle verifies) | android/build.gradle.kts | versioned independently of Qt |
| KDDockWidgets | 2.2.x | GPL-2.0-only OR GPL-3.0-only (open-source) | https://github.com/KDAB/KDDockWidgets | – | `<sha>` | third_party/kddockwidgets.cmake | GUI docking (chapter 11) — GPL-3 chosen |
| QHexView | `<commit>` | MIT | https://github.com/Dax89/QHexView | – | – | app/third_party (vendored) | |
| UVQ models | google/uvq `<commit>` | Apache-2.0 | https://github.com/google/uvq | – | `<sha>` of exported .onnx | tools/export_uvq_onnx.py | models downloaded at build, not committed |
| VMAF models | in libvmaf | BSD-2-Clause-Patent | – | – | – | built-in | |
| Conformance corpora | manifests | various (ITU/AOM test data, free for testing) | tests/corpora/manifest.json (chapter 14 §14.2.4) | – | per file | tools/fetch_corpora.py | never redistributed in releases |
```

Each row's SHA-256 MUST be verified by the build; a row with `–` under SHA-256 is allowed only for `git` full-commit pins and aqtinstall (which verifies its own hashes). The `<sha>`/`<commit>` placeholders above are filled by `tools/versions_to_cmake.py --update-hashes` (WI-0.010: downloads each archive once into `OVT_TP_DOWNLOADS`, writes the hash into the row and, for libaom, the `PATENTS sha256=` note) — never by hand. The parser emits one CMake variable triple per row, `OVT_TP_<dep>_{VERSION,URL,SHA256}` with `<dep>` the row Name lower-cased and every run of characters other than `a–z`/`0–9` replaced by one `_` (`OVT_TP_libaom_VERSION`, `OVT_TP_ffmpeg_URL`, `vcpkg baseline` → `vcpkg_baseline`, `Gradle / AGP` → `gradle_agp`, …); no other spelling (`OVT_<DEP>_VERSION`) exists. `tools/license_gate.py` reads the Licence column (chapter 15).

### 13.3.8 Dependency-update cadence

Pins move by pull request on a fixed rhythm, never ad hoc: (1) **weekly** — Renovate (`renovate.json` at the root, WI-0.007) opens grouped PRs for GitHub Actions SHAs, Gradle/AGP/Kotlin/AndroidX, Python `requirements*.txt` and the vcpkg baseline (`vcpkg x-update-baseline`); each PR must be green on the required checks of §13.7.3 and is merged by a maintainer; (2) **per upstream release** — `third_party/VERSIONS.md` rows for decoders and FFmpeg follow upstream tags (libaom, dav1d, libde265, HM, FFmpeg point releases) in a PR that re-runs the golden suite (`ctest --preset nightly`) and the oracle image rebuild (chapter 14 §14.2.6); a minor FFmpeg or Qt bump (D5 pins `7.1.x`, `6.8.x`) is a normal PR, a major bump is an ADR; (3) **security** — an advisory touching a pinned component is fixed within 7 days by pin bump or backport in `third_party/patches/`, released as a PATCH version (§13.8 item 6); (4) **never automatically** — Qt (installed by `tools/install_qt.py`), the NDK, `compileSdk`/`targetSdk` (yearly rule of §13.4.2) and the macOS deployment target change only by maintainer PR with a CHANGELOG entry under `deps`. `tools/check_versions_md.py` (WI-0.007) fails `lint` when a `VERSIONS.md` row and the script that consumes it disagree.

---

## 13.4 Android project

### 13.4.1 Directory layout (`android/`)

```
android/
  settings.gradle.kts            includes ":app", ":codecpack" (when OVT_ANDROID_CODEC_PACK)
  build.gradle.kts               plugin versions from gradle/libs.versions.toml
  gradle.properties              org.gradle.jvmargs=-Xmx4g, android.useAndroidX=true, qt.cmake.dir, ovt.abis=arm64-v8a;x86_64
  gradle/libs.versions.toml      agp = "8.6.0", kotlin = "2.0.21", qt = "6.8.3", qtGradlePlugin = "1.2", ndk = "27.2.12479018", cmake = "3.30.5", compileSdk = "36"
  gradle/wrapper/                gradle-wrapper.properties (distributionUrl gradle-8.10-bin.zip + distributionSha256Sum)
  app/
    build.gradle.kts             application module (§13.4.2)
    src/main/AndroidManifest.xml
    src/main/kotlin/org/openvideotools/studio/
      MainActivity.kt            §13.4.4
      HdrSurfaceView.kt          SurfaceView sibling + dataspace (ADR-014)
      FilePicker.kt              SAF → ParcelFileDescriptor → ovt_file_open_fd
      OvtNative.kt               JNI declarations (ovt_jni_*)
      PluginStore.kt             app-private plugin dir management, codec pack discovery (§13.4.6)
      DisplayCaps.kt             Display.HdrCapabilities, isHdrSdrRatioAvailable, headroom
    src/main/cpp/
      CMakeLists.txt             add_subdirectory(../../../../.. ovt) with Android options; JNI glue target ovt_jni
      ovt_jni.cpp                JNI → C API (file open by fd, surface attach, headroom)
    src/main/res/                themes (Material3, dynamic colour), layouts (FrameLayout host), xml/file_paths.xml
    src/main/assets/             default settings json, colour maps
    src/main/jniLibs/            EMPTY in git; populated by CMake/androiddeployqt at build time
  codecpack/                     split feature module (Play Feature Delivery, on-demand) or separate APK for F-Droid
    build.gradle.kts             com.android.dynamic-feature; ships libovt_plugin_ffmpeg.so + libovt_plugin_de265.so only
    src/main/AndroidManifest.xml <dist:module dist:onDemand="true" dist:title="@string/codec_pack">
  qt/                            Qt Gradle plugin configuration (org.qtproject.qt.gradleplugin) or androiddeployqt input json (§13.4.3)
  fdroid/                        metadata template for fdroiddata (org.openvideotools.studio.yml)
  keystore/                      .gitignored; CI gets signing material from secrets
```

### 13.4.2 `app/build.gradle.kts` (normative skeleton)

```kotlin
plugins { id("com.android.application"); id("org.jetbrains.kotlin.android"); id("org.qtproject.qt.gradleplugin") version libs.versions.qtGradlePlugin.get() }   // the Qt Gradle plugin has its own version line (1.x), not Qt's
android {
  namespace = "org.openvideotools.studio"; compileSdk = libs.versions.compileSdk.get().toInt(); ndkVersion = libs.versions.ndk.get()
  defaultConfig {
    applicationId = "org.openvideotools.studio"; minSdk = 28; targetSdk = libs.versions.compileSdk.get().toInt()   // 36 (D5)
    versionCode = ovtVersionCode()      // (major*1_000_000 + minor*10_000 + patch*100 + abiIndex) from /VERSION, see §13.8
    versionName = ovtVersionName()
    ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
    externalNativeBuild { cmake { arguments += listOf("-DANDROID_STL=c++_shared", "-DANDROID_PLATFORM=android-28", "-DOVT_BUILD_GUI=ON", "-DOVT_BUILD_CLI=OFF",
        "-DQT_ROOT_DIR=${qtRootDir()}", "-DQT_HOST_PATH=${qtHostPath()}", "-DOVT_THIRD_PARTY_PREFIX=${rootDir}/../build/third_party/android-${'$'}{ANDROID_ABI}",
        "-DVCPKG_TARGET_TRIPLET=${'$'}{ANDROID_ABI}-android-ovt", "-DCMAKE_TOOLCHAIN_FILE=${rootDir}/../third_party/vcpkg/scripts/buildsystems/vcpkg.cmake",
        "-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE=${android.ndkDirectory}/build/cmake/android.toolchain.cmake") ; targets += listOf("ovt_studio", "ovt_jni", "ovt_plugin_aom") } }
  }
  externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = libs.versions.cmake.get() } }
  splits { abi { isEnable = true; reset(); include("arm64-v8a", "x86_64"); isUniversalApk = false } }   // per-ABI APKs for F-Droid/GitHub; AAB for Play
  buildTypes { release { isMinifyEnabled = false; isShrinkResources = false; signingConfig = signingConfigs.findByName("release") }
               debug   { applicationIdSuffix = ".debug" } }
  packaging { jniLibs { useLegacyPackaging = false; keepDebugSymbols += "**/libovt_plugin_*.so" } }   // uncompressed .so → dlopen directly from APK (nativeLibraryDir)
  compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
  kotlin { compilerOptions { jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17) } }   // `kotlinOptions {}` is deprecated in KGP 2.x
  dynamicFeatures += setOf(":codecpack")
}
dependencies { implementation("androidx.core:core-ktx:1.15.0"); implementation("androidx.appcompat:appcompat:1.7.0"); implementation("androidx.activity:activity-ktx:1.9.3")
               implementation("com.google.android.play:feature-delivery-ktx:2.1.0") /* Play builds only: F-Droid flavour excludes it */ }
```

**Yearly target-level rule.** Google Play requires new apps and updates to target the previous year's API level by 31 August; `compileSdk`/`targetSdk` (one value, `libs.versions.compileSdk`) are bumped in the first release after each deadline (36 for the 2026 deadline, 37 for 2027, …) together with the emulator image of the smoke job (§13.7.1) and `minSdk` stays 28.

Product flavours: `play` (feature-delivery dependency, AAB) and `foss` (no Google libraries; the codec pack is a second APK `org.openvideotools.studio.codecpack` signed by the same key, discovered by `PluginStore` through `PackageManager.getApplicationInfo().nativeLibraryDir` of that package — allowed because both are same-signature; F-Droid builds the `foss` flavour).

### 13.4.3 Qt integration

Decision: use the **Qt Gradle plugin** (`org.qtproject.qt.gradleplugin`, shipped with Qt 6.8, https://doc.qt.io/qt-6.8/android-build-environment-variables.html and the "Qt Quick for Android" embedding docs) which runs `androiddeployqt` at `mergeJniLibFolders` time to copy Qt's `.so` files, QML modules and the `libplugins_*` into the APK and generates `libs.xml` / `qtbundle`. `MainActivity` embeds Qt via **`QtQuickView`** (class `org.qtproject.qt.android.QtQuickView`, Qt 6.7+), **not** `QtActivity`, so the Activity remains a plain Android Activity owning the `SurfaceView` sibling (ADR-004, ADR-014). The Qt application entry is `libovt_studio.so` exporting `main()` built as a shared library with `qt_add_executable(ovt_studio ...)` under Android (Qt turns it into a library automatically). Known Qt 6.8 limitation (research 05 §1.1): Fragments cannot host Qt directly — we only use the Activity.

### 13.4.4 `MainActivity.kt` responsibilities (normative list)

1. **Layout.** A `FrameLayout` with two children: `HdrSurfaceView` (bottom, z-order `setZOrderMediaOverlay(false)`) and `QtQuickView` (top, transparent background `setBackgroundColor(0)`; Qt window set to translucent via `QT_ANDROID_TRANSLUCENT_WINDOW`-equivalent `QSurfaceFormat` alpha 8 and `Qt::WA_TranslucentBackground`). The picture area in QML is a transparent "hole" (`PicturePanel.qml` emits its geometry to Kotlin via `QtQuickView.setProperty`/signal), and `MainActivity` positions `HdrSurfaceView` to that rectangle (`layoutParams` updated on every geometry signal; clipped to window). On ChromeOS/desktop windowing, re-layout on `onConfigurationChanged` (free-form resize) — no Activity restart (`android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|density|keyboard|keyboardHidden|navigation|uiMode"`).
2. **HDR output.** `HdrSurfaceView` implements `SurfaceHolder.Callback2`; on `surfaceCreated` passes the `Surface` to native (`ovt_jni_surface_attach(ANativeWindow_fromSurface)`); native `libovt-render` sets `ANativeWindow_setBuffersDataSpace(window, STANDARD_BT2020 | TRANSFER_ST2084 | RANGE_LIMITED)` (= `ADATASPACE_BT2020_PQ`) or `… | TRANSFER_HLG | …` (= `ADATASPACE_BT2020_HLG`) per content (chapter 10) — OR only *field* constants (`STANDARD_*`, `TRANSFER_*`, `RANGE_*`), never two pre-combined `ADATASPACE_*` values via EGL `EGL_GL_COLORSPACE_BT2020_PQ_EXT` or `AHardwareBuffer` queue (ADR-014). **Never `TextureView`.** `window.colorMode = ActivityInfo.COLOR_MODE_HDR` on API ≥ 26 when `display.isHdr`; API ≥ 34: `window.setDesiredHdrHeadroom(ratio)` using `display.hdrSdrRatio` and `Display.registerHdrSdrRatioChangedListener` to forward the live headroom to `DisplayTarget.headroom`; API ≥ 35: `window.isColorModeHdr` check. `DisplayCaps` reports `Display.getHdrCapabilities().supportedHdrTypes` (HDR10, HLG, HDR10+, DV) and max/avg/min luminance into `ovt doctor`-equivalent diagnostics.
3. **File opening.** Storage Access Framework only (no `READ_EXTERNAL_STORAGE`): `ActivityResultContracts.OpenDocument()` with MIME `video/*`, `application/octet-stream`, `*/*`; persist `takePersistableUriPermission` for recents; `contentResolver.openFileDescriptor(uri, "r")` → `ParcelFileDescriptor.detachFd()` → `ovt_file_open_fd(fd, size /*from fstat, 0 = let the library fstat*/, /*take_ownership=*/1, display_name, &file)` (chapter 12 §12.3.6 signature); `ovt::io` reads with `pread` (§0.3). Handles `ACTION_VIEW`/`ACTION_SEND` intents and ChromeOS drag-and-drop (`View.OnDragListener`, `DragEvent.clipData` URIs with `requestDragAndDropPermissions`). Files larger than 2 GB: ensure 64-bit `off_t` (`-D_FILE_OFFSET_BITS=64` is default on arm64/x86_64).
4. **Input.** Enables hover, right-click (`MotionEvent.BUTTON_SECONDARY` → Qt context menu), mouse wheel with Ctrl for zoom, keyboard shortcuts (chapter 11 keymap) — all routed to Qt by `QtQuickView`; the Activity only handles hardware keys Qt does not receive (`KEYCODE_BACK` → Qt `back` signal).
5. **Lifecycle.** `onPause` → `ovt_jni_pause()` cancels analysis jobs and releases the decoder surface; `onResume` restores; `onTrimMemory(TRIM_MEMORY_RUNNING_LOW)` → `ovt_cache_set_max_bytes` shrink and LRU flush (§0.7 rule 3: ≤ 1 GiB RSS on Android, D5).
6. **Plugins.** On start, `PluginStore.scan()` collects `applicationInfo.nativeLibraryDir`, `filesDir/plugins`, and the codec pack's `nativeLibraryDir` (if installed) and calls `ovt_plugins_add_search_path` for each (§13.4.6).
7. **Cache dir.** `ovt_cache_set_dir(cacheDir/ovt)`; Android may purge it.
8. **CLI for tests.** The CLI is not packaged; CI pushes `build/android-x86_64-release/cli/ovt` to the emulator with `adb push … /data/local/tmp/ovt` for smoke tests.

**JNI surface (`android/app/src/main/cpp/ovt_jni.cpp`, registered with `RegisterNatives` on `org.openvideotools.studio.OvtNative`; these are the only native entry points the Kotlin side calls — everything else goes through Qt):**

```c
/* All functions are noexcept C++ bodies behind extern "C"; errors are returned, never thrown into the JVM. */
jstring  ovt_jni_version(JNIEnv*, jclass);                                   /* ovt_version_string() + build info json */
jlong    ovt_jni_open_fd(JNIEnv*, jclass, jint fd, jlong size, jstring display_name);   /* fd from ParcelFileDescriptor.detachFd() → ovt_file_open_fd(fd, size, take_ownership = 1, name) → ovt_container_open; returns a session handle, 0 on error (ovt_last_error_json logged) */
void     ovt_jni_close(JNIEnv*, jclass, jlong session);
jboolean ovt_jni_surface_attach(JNIEnv*, jclass, jlong session, jobject surface);   /* ANativeWindow_fromSurface → libovt-render DisplayTarget (chapter 10 §10.4.3) */
void     ovt_jni_surface_detach(JNIEnv*, jclass, jlong session);
void     ovt_jni_set_headroom(JNIEnv*, jclass, jlong session, jfloat hdr_sdr_ratio);  /* Display.hdrSdrRatio → DisplayTarget.headroom */
void     ovt_jni_pause(JNIEnv*, jclass, jlong session);                      /* cancels jobs, releases decoder surfaces */
void     ovt_jni_resume(JNIEnv*, jclass, jlong session);
void     ovt_jni_add_plugin_path(JNIEnv*, jclass, jstring dir);             /* → ovt_plugins_add_search_path */
```

The `ovt_jni_*` functions are **internal JNI glue** of the Android app (`android/app/src/main/cpp/ovt_jni.cpp`, declared on the Kotlin side in `OvtNative.kt`): they are not part of the C API of chapter 12 §12.3, live in no public header and are exported only through `JNI_OnLoad`/`RegisterNatives`. The additional line `OVT_HDR dataspace=<name>` (logcat tag `OVT_HDR`; `<name>` ∈ `BT2020_PQ|BT2020_HLG|SRGB`) is logged by `ovt_jni_surface_attach` every time the dataspace is set — the phase-0 exit demo (chapter 16 §16.1.1 #8) and chapter 14 §14.11 grep for it, so the format is normative from WI-0.052 on (WI-1.577 reuses it).

### 13.4.5 `AndroidManifest.xml` essentials

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android" xmlns:dist="http://schemas.android.com/apk/distribution">
  <uses-feature android:name="android.hardware.vulkan.version" android:version="0x401000" android:required="false"/>
  <uses-feature android:name="android.hardware.touchscreen" android:required="false"/>          <!-- Chromebooks without touch -->
  <uses-feature android:name="android.software.leanback" android:required="false"/>
  <application android:label="@string/app_name" android:theme="@style/Theme.OVT" android:extractNativeLibs="false"
               android:largeHeap="true" android:supportsRtl="true" android:resizeableActivity="true" android:enableOnBackInvokedCallback="true">
    <meta-data android:name="android.allow_multiple_resumed_activities" android:value="true"/>     <!-- ChromeOS multi-window -->
    <activity android:name=".MainActivity" android:exported="true" android:launchMode="singleTask" android:colorMode="hdr"
              android:configChanges="orientation|screenSize|smallestScreenSize|screenLayout|density|keyboard|keyboardHidden|navigation|uiMode"
              android:windowSoftInputMode="adjustResize">
      <layout android:defaultWidth="1280dp" android:defaultHeight="800dp" android:minWidth="800dp" android:minHeight="600dp"/>
      <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
      <intent-filter><action android:name="android.intent.action.VIEW"/><category android:name="android.intent.category.DEFAULT"/>
        <data android:scheme="content"/><data android:mimeType="video/*"/><data android:mimeType="application/octet-stream"/></intent-filter>
    </activity>
  </application>
</manifest>
```

No `INTERNET` permission in the `foss` flavour (F-Droid anti-feature avoidance); the `play` flavour needs it only for Play Feature Delivery.

### 13.4.6 Plugin `.so` packaging and loading rules on Android

1. Android ≥ 10 (W^X: the `untrusted_app` SELinux policy forbids executing a file the app can write, and Play policy requires native code to ship inside the APK with `extractNativeLibs=false`) — `dlopen` of a library in the app's private storage remains permitted as long as the file is not writable at load time. Allowed plugin locations, in search order: (a) `applicationInfo.nativeLibraryDir` of the app (bundled `libovt_plugin_aom.so`); (b) `nativeLibraryDir` of the installed codec pack (split or sibling package with the same signing certificate — checked with `PackageManager.checkSignatures == SIGNATURE_MATCH`, else ignored with diagnostic `ANA-PLG-0003` (plugin-host family, chapter 07 §7.9)); (c) `filesDir/plugins/` for user-copied `.so` files (developer feature behind a settings toggle "Allow user plugins"; files MUST be owned by the app uid and `chmod 0500`; W^X: the file is copied to `codeCacheDir`, made read-only (`chmod 0500`) and `dlopen`ed from there, so the SELinux `execute` permission for `app_data_file` applies and the file is never writable while mapped). `/sdcard` or SAF-picked `.so` files are copied into (c), never loaded in place.
2. All plugin `.so` files MUST be built with `-Wl,-z,max-page-size=16384`, `c++_shared` STL (shared with the app) and `ANDROID_PLATFORM=android-28`; `ovt plugins --check` equivalent in the Settings panel reports ABI version mismatches.
3. Default download (Play `play` flavour base module, F-Droid `foss` APK): **no AVC/HEVC decoders** (ADR-015). The "codec pack" (`:codecpack` on-demand module for Play; `org.openvideotools.studio.codecpack` APK for F-Droid/GitHub) contains `libovt_plugin_ffmpeg.so` (private FFmpeg with h264/hevc + `mediacodec` hwaccel) and `libovt_plugin_de265.so`. The GUI offers "Install codec pack" which on Play calls `SplitInstallManager.startInstall`, on `foss` opens the F-Droid/GitHub page. Patent notice text (chapter 15) is shown before install.
4. Hardware playback of AVC/HEVC without the codec pack: **not available** in the default download (the LGPL core FFmpeg flavour has `h264_mediacodec`/`hevc_mediacodec` disabled, §13.3.3) — this is deliberate; MediaCodec via the plugin is the supported path. AV1 hardware playback via `av1_mediacodec` stays in core (patent licence is AOM royalty-free).

### 13.4.7 ChromeOS specifics

- **ABI:** `x86_64` is mandatory in every release (ARCVM on Intel/AMD Chromebooks has no arm64 emulation for native code); `arm64-v8a` for Arm Chromebooks. 32-bit ABIs are not built.
- **Windowing:** `resizeableActivity=true`, `<layout defaultWidth/Height minWidth/minHeight>`, handle `onConfigurationChanged` without recreation, `onMultiWindowModeChanged`; the Qt window follows the Activity surface automatically; the `HdrSurfaceView` rectangle is re-synced on every QML geometry change.
- **Input:** keyboard shortcuts with `Ctrl` (ChromeOS convention) — chapter 11 defines the keymap with `Ctrl` as the primary modifier on Android/ChromeOS and `Cmd` on macOS; mouse hover tooltips, right-click, scroll wheel (`MotionEvent.ACTION_SCROLL` with `AXIS_VSCROLL/HSCROLL`), touchpad pinch (`ScaleGestureDetector` → zoom about cursor), stylus. Qt Quick handles these natively through `QtQuickView`.
- **External displays:** `DisplayManager.DisplayListener`; when the window moves to a display with different `hdrCapabilities`, `DisplayCaps` re-evaluates and `DisplayTarget.hdr_capable/headroom` are updated. Multi-display move is via the ChromeOS window manager; the Activity receives `onConfigurationChanged(density)`.
- **HDR pass-through is unverified** (ADR-014): WI-0.055 spike measures whether a `SurfaceView` with `BT2020_PQ` dataspace reaches the panel on an HDR-capable Chromebook (e.g. a 2024 Intel model with an HDR panel) and documents the result in `docs/platforms/chromeos-hdr.md`. Fallback: tone-map to SDR in libplacebo when `Display.isHdr == false` or the spike fails.
- **Files:** ChromeOS Files app and Linux (Crostini) shares appear as SAF providers; Google Drive via SAF works but seeks are slow — the `io` layer MUST use large reads (≥ 1 MiB) with the read-ahead policy of chapter 07 when the file descriptor is not seekable-fast (`fstat` `st_dev` of a FUSE mount).

### 13.4.8 Signing, Play and F-Droid

- **Signing key:** one upload key for Play (Play App Signing holds the app signing key) and a separate **release key** used for F-Droid-reproducible APKs and GitHub releases. Both are 4096-bit RSA, `apksigner` v2+v3 (v4 optional) signatures; `minSdk 28` makes v1 unnecessary. Keys live in CI secrets (`ANDROID_KEYSTORE_B64`, `ANDROID_KEYSTORE_PASSWORD`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`); never in the repo.
- **Play:** upload an AAB built with `./gradlew :app:bundlePlayRelease`; `versionCode` scheme in §13.8; data safety form: no data collection; the base module has no AVC/HEVC decoders; the codec-pack on-demand module is declared with `dist:onDemand="true"`.
- **Reproducible `foss` builds and F-Droid:** the `foss` flavour MUST build reproducibly from the tag: `./gradlew :app:assembleFossRelease` with `SOURCE_DATE_EPOCH` from the tag's commit date, Gradle `org.gradle.caching=false` in the verifier config, Qt installed by `tools/install_qt.py` with the pinned version; `tools/third_party_offline.py` pre-fetches SHA-verified sources so the build runs offline. `tools/repro_check.sh android` builds twice in two directories and compares with `diffoscope`; the only allowed difference is the signature block. **Decision on F-Droid:** the official `fdroiddata` repository requires every component to be built from source on F-Droid's own servers and does not accept the prebuilt Qt for Android kits that `aqtinstall` downloads; building Qt 6.8 from source inside an `fdroiddata` recipe is out of scope for phases 0–4 (chapter 17 open item). OVT therefore publishes the `foss` APKs (a) on GitHub releases and (b) through a **self-hosted F-Droid repository** (`fdroid` server tool, metadata in `android/fdroid/`, signed with the release key) that users add to their F-Droid client; the `fdroiddata` recipe template `android/fdroid/org.openvideotools.studio.yml` (`Binaries:` pointing to GitHub release APKs for reproducible-build verification) is kept ready for the day a from-source Qt build becomes feasible.
- **GitHub releases:** per-ABI APKs (`ovt-studio-<ver>-arm64-v8a.apk`, `-x86_64.apk`) and codec-pack APKs, with `SHA256SUMS` and a detached `minisign` signature.

---

## 13.5 macOS

### 13.5.1 Architectures

Decision: **per-architecture builds and per-architecture DMGs** (`OVT Studio-<ver>-arm64.dmg`, `-x86_64.dmg`); `OVT_UNIVERSAL=ON` (MAY, later) runs `tools/mac/make_universal.py` which `lipo`s every Mach-O in two complete `.app` bundles. Reasons: (1) Meson/autotools dependencies (FFmpeg, libplacebo, dav1d, libvmaf with assembly) cannot build universal objects in one pass, so a universal build is two builds plus `lipo` anyway; (2) GitHub-hosted runners are arm64 (`macos-15`) and x86_64 (`macos-15-intel`; the `macos-13` Intel image was retired in 2025), so per-arch jobs map 1:1; (3) the Homebrew cask supports per-arch `url`/`sha256` via `on_arm`/`on_intel`. Qt from aqtinstall is universal; CMake with `CMAKE_OSX_ARCHITECTURES` single-arch links only that slice.

### 13.5.2 Bundle layout and `macdeployqt`

```
OVT Studio.app/Contents/
  Info.plist              CFBundleIdentifier org.openvideotools.studio, LSMinimumSystemVersion 13.0, CFBundleDocumentTypes (mp4, m4v, mov, 264/h264, 265/hevc, ivf, obu, webm, mkv, y4m, yuv), NSHighResolutionCapable, NSSupportsAutomaticGraphicsSwitching
  MacOS/OVT Studio        ovt_studio
  MacOS/ovt               CLI (symlinked into PATH by the cask: /opt/homebrew/bin/ovt → …/MacOS/ovt)
  Frameworks/             Qt*.framework, libovt_core.1.dylib, libovt_render.1.dylib, libavcodec_ovt.61.dylib …, libMoltenVK.dylib
  PlugIns/                Qt plugins (platforms, imageformats, …) and ovt/libovt_plugin_aom.dylib (+ libovt_plugin_ffmpeg.dylib, libovt_plugin_de265.dylib only in the "full" DMG)
  Resources/              qml/ (compiled .qmlc via qt_add_qml_module), colour maps, icons, THIRD_PARTY_NOTICES.md, PATENTS (AOM), LICENSES/
```

Build steps (`tools/mac/bundle.sh <preset>`): `cmake --install` into a staging prefix with `CMAKE_INSTALL_PREFIX=<stage>/OVT Studio.app/Contents`, then `macdeployqt "<stage>/OVT Studio.app" -qmldir=<src>/app/qml -executable=<app>/Contents/MacOS/ovt -libpath=<third_party prefix>/ffmpeg-core/lib -always-overwrite -verbose=1`, then `tools/mac/fix_rpaths.py` (verifies with `otool -L` that every Mach-O references only `@rpath`/`@executable_path` or `/usr/lib`/`/System`; fails otherwise), then `tools/mac/strip.sh` (dSYMs saved to `build/dsym/`).

### 13.5.3 Hardened runtime and entitlements (decision)

Official builds are signed with the hardened runtime (required for notarisation) and **the following entitlements** (`tools/mac/entitlements.plist`):

```xml
<dict>
  <key>com.apple.security.cs.disable-library-validation</key><true/>
  <key>com.apple.security.cs.allow-unsigned-executable-memory</key><false/>
  <key>com.apple.security.cs.allow-jit</key><false/>
  <key>com.apple.security.cs.allow-dyld-environment-variables</key><false/>
</dict>
```

**Justification for `disable-library-validation = true`:** ADR-015 requires loading user-installed decoder plugins (vendor decoders, distro/Homebrew-built `libovt_plugin_de265.dylib`, a user's own HM build) from `~/Library/Application Support/ovt/plugins`. Library validation would reject any dylib not signed by the same Team ID or Apple, so plugins from any third party would fail to `dlopen` with `code signature in ... not valid for use in process`. Apple permits this entitlement under notarisation (it is the documented plugin-host case: https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.disable-library-validation). Mitigations: (1) OVT refuses to load a plugin whose file is world-writable or not owned by the current user (diagnostic `ANA-PLG-0004`, chapter 07 §7.9); (2) the Settings panel shows the plugin's SHA-256 and signing identity (`codesign -dv` via `SecStaticCodeCheckValidity`) and a first-load confirmation dialog for plugins outside the bundle; (3) the bundled plugins are themselves signed with the hardened runtime and `--timestamp`. `allow-dyld-environment-variables` stays **false** (we do not want `DYLD_INSERT_LIBRARIES` into a notarised app); plugin search uses explicit paths, not `DYLD_LIBRARY_PATH`. **No sandbox** (`com.apple.security.app-sandbox` absent): a sandboxed app cannot read arbitrary files given on the command line or `dlopen` user plugins without bookmarks; the Mac App Store, the only channel that would require a sandbox, is excluded by ADR-016. Consequently no `com.apple.security.files.*` entitlement is listed — those are App Sandbox entitlements and are meaningless without `app-sandbox`. The `ovt` CLI inside the bundle is signed with the **same** entitlements file: it `dlopen`s the same third-party plugins (chapter 12 §12.2.14) and would fail library validation without `disable-library-validation`.

### 13.5.4 codesign and notarisation steps (`tools/mac/sign_and_notarize.sh`)

```bash
# Inputs: $APP (path to .app), $IDENTITY ("Developer ID Application: <Org> (TEAMID)"), $KEYCHAIN_PROFILE (notarytool credentials stored via `xcrun notarytool store-credentials`)
ENT=tools/mac/entitlements.plist
# 1. Enumerate every Mach-O inside-out (frameworks' Versions/A binaries, dylibs, .so plugins, helper executables) — never rely on `codesign --deep` for signing (deprecated).
tools/mac/list_macho.py "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" --exclude-bundle-roots | while IFS= read -r f; do
  codesign --force --options runtime --timestamp --sign "$IDENTITY" "$f"
done
find "$APP/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0 | xargs -0 -n1 codesign --force --options runtime --timestamp --sign "$IDENTITY"
# 2. The CLI needs the same entitlements as the app (it dlopens third-party plugins).
codesign --force --options runtime --timestamp --entitlements "$ENT" --sign "$IDENTITY" "$APP/Contents/MacOS/ovt"
codesign --force --options runtime --timestamp --entitlements "$ENT" --sign "$IDENTITY" "$APP"   # main bundle last
codesign --verify --deep --strict --verbose=2 "$APP"                                           # --deep is fine for *verification*
# 3. Notarize and staple the APP first (the ticket must be inside the bundle before it is packaged).
ditto -c -k --keepParent "$APP" "$APP.zip"
xcrun notarytool submit "$APP.zip" --keychain-profile "$KEYCHAIN_PROFILE" --wait --timeout 30m
xcrun stapler staple "$APP"
# 4. Then build, sign, notarize and staple the DMG (Apple TN3147 order).
tools/mac/make_dmg.sh "$APP" "$DMG"            # hdiutil create -format UDZO -fs HFS+ with /Applications symlink and background; deterministic ordering, SOURCE_DATE_EPOCH mtime
codesign --force --timestamp --sign "$IDENTITY" "$DMG"
xcrun notarytool submit "$DMG" --keychain-profile "$KEYCHAIN_PROFILE" --wait --timeout 30m
xcrun stapler staple "$DMG"
spctl --assess --type open --context context:primary-signature -v "$DMG"                        # Gatekeeper acceptance check
xcrun stapler validate "$APP" && xcrun stapler validate "$DMG"
```

`tools/mac/list_macho.py` walks the directories, identifies Mach-O files by magic (`0xfeedfacf`, `0xcafebabe`), and prints them deepest-first so nested code is signed before its container.

CI (tags only) imports the Developer ID certificate from `MAC_CERT_P12_B64`/`MAC_CERT_PASSWORD` into a temporary keychain and stores notarytool credentials from `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD`. Pull-request builds are **ad-hoc signed** (`--sign -`) with the same entitlements so plugin loading tests run under the hardened runtime.

### 13.5.5 Homebrew cask (`Casks/ovt-studio.rb` in a project tap `openvideotools/homebrew-tap`; later submitted to `homebrew/cask`)

```ruby
cask "ovt-studio" do
  arch arm: "arm64", intel: "x86_64"
  version "1.2.0"
  sha256 arm: "<sha>", intel: "<sha>"
  url "https://github.com/openvideotools/ovt/releases/download/v#{version}/OVT-Studio-#{version}-#{arch}.dmg", verified: "github.com/openvideotools/ovt/"
  name "OVT Studio"
  desc "Open Video Tools: bitstream, container and quality analyzer for AVC, HEVC and AV1"
  homepage "https://openvideotools.org/"
  livecheck { url :url; strategy :github_latest }
  depends_on macos: ">= :ventura"
  app "OVT Studio.app"
  binary "#{appdir}/OVT Studio.app/Contents/MacOS/ovt"
  zap trash: ["~/Library/Application Support/ovt", "~/Library/Caches/org.openvideotools.ovt", "~/Library/Preferences/org.openvideotools.studio.plist"]
end
```

A second cask `ovt-codec-pack` installs `libovt_plugin_ffmpeg.dylib` and `libovt_plugin_de265.dylib` into `~/Library/Application Support/ovt/plugins` (cask stanza `artifact ... target:`), signed and notarised as a `.pkg`-free zip; it is the macOS "codec pack" of ADR-015. A Homebrew *formula* `ovt` (CLI + libs from source, `depends_on "ffmpeg"` system build with `OVT_SYSTEM_FFMPEG=ON`) is the developer-oriented alternative.

### 13.5.6 Metal / MoltenVK packaging

`libMoltenVK.dylib` (from the pinned release, arm64 or x86_64 slice extracted with `lipo -thin`) is placed in `Contents/Frameworks/` and linked directly by `ovt_render` (`-framework Metal -framework IOSurface -framework QuartzCore` are pulled transitively). libplacebo is built with `-Dvulkan=enabled` against the vcpkg Vulkan headers and loads Vulkan entry points through `vkGetInstanceProcAddr` obtained from MoltenVK (`PL_VK_IMPORT` with `get_proc_addr`). The `CAMetalLayer` is created by `libovt-render` for the picture area (`DisplayTarget.native = CAMetalLayer*`, `wantsExtendedDynamicRangeContent = YES`, `colorspace = kCGColorSpaceITUR_2100_PQ` / `...HLG` / `extendedLinearSRGB` as chapter 10 specifies) and layered under Qt's own Metal layer via `QQuickWindow` + `NSView` sibling (chapter 10/11). Debug presets set `VK_ICD_FILENAMES` to the MoltenVK ICD JSON so validation layers from the Vulkan SDK can be enabled with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`.

---

## 13.6 Linux

### 13.6.1 Flatpak (`flatpak/org.openvideotools.Studio.yml`, published on Flathub)

```yaml
app-id: org.openvideotools.Studio
runtime: org.kde.Platform
runtime-version: '6.8'
sdk: org.kde.Sdk
command: ovt-studio
finish-args:
  - --share=ipc
  - --socket=wayland
  - --socket=fallback-x11
  - --device=dri                    # Vulkan / VA-API
  - --filesystem=host:ro            # analyzers open arbitrary files; portals are offered first (QFileDialog uses the portal), host:ro keeps CLI usage (flatpak run --command=ovt) workable
  - --filesystem=xdg-data/ovt/plugins:create   # user plugins (ADR-015)
  - --env=OVT_PLUGIN_PATH=/app/lib/ovt/plugins:/app/extensions/codecs/lib/ovt/plugins
add-extensions:
  org.openvideotools.Studio.Codecs:  # separate extension = Fedora-style codec split; built from the same tree with OVT_BUILD_PLUGIN_* for AVC/HEVC
    directory: extensions/codecs
    version: '1'
    autodelete: true
    no-autodownload: true           # user opts in from the GUI "Install codec pack" (xdg-open appstream://org.openvideotools.Studio.Codecs)
modules:
  # Dependencies that the KDE 6.8 runtime/SDK does not provide are built here from release tarballs (hashes generated from third_party/VERSIONS.md
  # and vcpkg's port versions by tools/gen_flatpak_sources.py). Flathub builds have no network: vcpkg is NOT used (OVT_USE_VCPKG=OFF).
  - flatpak/modules/fmt.json
  - flatpak/modules/spdlog.json
  - flatpak/modules/flatbuffers.json
  - flatpak/modules/highway.json
  - flatpak/modules/cli11.json
  - flatpak/modules/xxhash.json
  - flatpak/modules/nlohmann-json.json
  - flatpak/modules/glslang.json
  - flatpak/modules/libplacebo.json      # Meson module with the options of §13.3.1 (the SDK's libplacebo lacks lcms/glslang settings OVT needs)
  - flatpak/modules/libaom.json          # CMake module with CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1
  - flatpak/modules/libvmaf.json
  - flatpak/modules/zimg.json
  - name: ovt
    buildsystem: cmake-ninja
    builddir: true
    config-opts:
      - -DCMAKE_BUILD_TYPE=RelWithDebInfo
      - -DOVT_USE_VCPKG=OFF
      - -DOVT_SYSTEM_FFMPEG=ON             # the KDE runtime ships FFmpeg (LGPL, with h264/hevc): the runtime decides, not OVT
      - -DOVT_PATENT_CODECS_IN_CORE=ON
      - -DOVT_SYSTEM_LIBPLACEBO=ON         # built by the module above
      - -DOVT_SYSTEM_LIBAOM=ON
      - -DOVT_SYSTEM_LIBVMAF=ON
      - -DOVT_SYSTEM_ZIMG=ON
      - -DOVT_SYSTEM_DAV1D=ON              # from the runtime's FFmpeg
      - -DOVT_BUILD_PLUGIN_DE265=OFF
      - -DOVT_BUILD_PLUGIN_FFMPEG=OFF
      - -DOVT_BUILD_PYTHON=OFF
      - -DOVT_ENABLE_UVQ=OFF               # ONNX Runtime is not built in the Flatpak (chapter 17 open item)
      - -DOVT_ENABLE_LTO=ON
    sources:
      - type: git
        url: https://github.com/openvideotools/ovt.git
        tag: v1.2.0
        commit: <sha>
```

Notes: the KDE 6.8 runtime provides Qt 6.8 and an FFmpeg with AVC/HEVC enabled; since the runtime, not OVT, ships the decoders, the Flathub build uses system FFmpeg and the patent-codec split is realised through the `Codecs` extension only for `libde265`-based deep analysis. Flathub forbids network during build and requires every source to be declared with a hash, hence the generated per-dependency module files under `flatpak/modules/` (one `sources:` entry each, `type: archive` with `sha256`) and `OVT_USE_VCPKG=OFF`; `tools/gen_flatpak_sources.py` regenerates them from `third_party/VERSIONS.md` and the vcpkg baseline so the Flatpak uses the same pins as every other build. The `Codecs` extension is built from `flatpak/org.openvideotools.Studio.Codecs.yml` with a `libde265` module (patched, §13.3.6) and `-DOVT_BUILD_PLUGIN_DE265=ON` only. `--filesystem=host:ro` is justified in the Flathub submission by the analyzer use-case (same as MediaInfo, Kdenlive).

### 13.6.2 AppImage (`tools/linux/appimage.sh`)

Built on the Ubuntu 22.04 CI image for glibc 2.35 compatibility: `cmake --install` into `AppDir/usr`, then `linuxdeploy --appdir AppDir --plugin qt --output appimage` with `QMAKE=$QT_ROOT_DIR/gcc_64/bin/qmake`, `EXTRA_QT_MODULES=svg;quickcontrols2;graphs`, `QML_SOURCES_PATHS=app/qml`, `LDAI_UPDATE_INFORMATION="gh-releases-zsync|openvideotools|ovt|latest|OVT_Studio-*-x86_64.AppImage.zsync"`, and `DEPLOY_PLATFORM_THEMES=1`. The bundled FFmpeg is the LGPL **core flavour without AVC/HEVC** (default download); `ovt_plugin_ffmpeg`/`de265` are shipped as a separate `ovt-codec-pack-<ver>-x86_64.tar.zst` to extract into `$XDG_DATA_HOME/ovt/plugins`. The AppImage sets `OVT_PLUGIN_PATH=$APPDIR/usr/lib/ovt/plugins` in `AppRun`. Vulkan: the loader is linked from the system (`libvulkan.so.1` is in `linuxdeploy`'s exclude list, as it must be, to pick up the host ICDs); the AppImage requires a host Vulkan 1.2 driver (Mesa ≥ 22 or vendor) — `ovt doctor` reports otherwise.

### 13.6.3 Distro packaging notes

| Distro | Approach |
|---|---|
| **Fedora / RPM Fusion** | `ovt` (core, cli, gui, aom plugin; built with `OVT_SYSTEM_FFMPEG=ON` against Fedora's patent-free FFmpeg → AVC/HEVC unavailable), `ovt-codecs-freeworld` in RPM Fusion (`ovt_plugin_ffmpeg` against RPM Fusion's full FFmpeg plus `ovt_plugin_de265`), analogous to `ffmpeg`/`ffmpeg-free`. `ovt_set_license` metadata and the per-target split (§13.2.1) make the two spec files trivial. Fedora's Mesa VA-API situation (research 07 §2.3) affects only the plugin's hwaccel. |
| **Debian / Ubuntu** | single source package `ovt`, binaries `libovt-core1`, `libovt-render1`, `libovt-dev`, `ovt-cli`, `ovt-studio`, `ovt-plugin-aom`, `ovt-plugin-ffmpeg` (Debian ships full FFmpeg, so the plugin is in main), `ovt-plugin-de265`, `python3-ovt`. Uses `linux-x64-distro` preset. libaom in Debian lacks inspection → `OVT_SYSTEM_LIBAOM=OFF` (bundled static build in the aom plugin; Debian policy tolerates a static convenience copy when the system library lacks a feature; documented in `debian/README.source`). |
| **Arch (AUR)** | `ovt`, `ovt-git`; full FFmpeg from `extra`. |
| **Homebrew (Linux)** | formula `ovt` shares the macOS formula. |

### 13.6.4 Wayland / X11 runtime requirements

- Wayland (preferred): Qt `wayland` platform plugin; HDR output requires a compositor supporting the final `wp_color_manager_v1` protocol (chapter 10 §10.4.2 is the owner of these versions: **KWin ≥ 6.4** / Plasma 6.4 — Plasma 6.2/6.3 shipped only the non-interoperable `xx-color-management-v4` draft —, **Mutter ≥ 48** / GNOME 48, wlroots ≥ 0.19 / Sway 1.12 with the Vulkan renderer; Mesa ≥ 25.1 for the Vulkan WSI side) and a later Qt 6.x release for the client side (Qt's own Wayland HDR support arrived after 6.8; 6.10 is not an LTS, so OVT does not move to it — research 05 §1.1, D5); with Qt 6.8 the HDR surface is driven directly by libplacebo via a `wl_surface` subsurface with the protocol handled in `libovt-render` (chapter 10) — the Qt version pin does not block HDR. SDR fallback everywhere else.
- X11: `xcb` platform plugin, no HDR (tone-map to SDR), Vulkan via `VK_KHR_xcb_surface`.
- Runtime libraries not bundled: `libvulkan.so.1`, Mesa/vendor ICDs, `libva.so.2` + driver (`intel-media-driver`/`mesa-va-drivers`) for VA-API, `libwayland-client`, `libxkbcommon`, `libxcb*`, `libGL`/`libEGL` (Qt), fontconfig/freetype, `libglib`, `libdbus`. `tools/linux/check_runtime_deps.sh` lists them and `ovt doctor` probes them.

---

## 13.7 CI (GitHub Actions)

### 13.7.1 Jobs

| Job id | Runner | Trigger | What it does | Gate? |
|---|---|---|---|---|
| `lint` | `ubuntu-22.04` | PR, push | `clang-format --dry-run -Werror` (LLVM 16, `.clang-format` in repo), `cmake-format`, `ruff` + `mypy` on `tools/` and `python/`, `reuse lint` (SPDX headers, chapter 15), `tools/check_spdx_dirs.py` (WI-0.601), `markdownlint` on the Markdown files the change touches, excluding `docs/spec/` (WI-0.604, WI-0.608), `tools/check_patches.py` (WI-0.607), `tools/rename_dry_run.sh` (WI-0.606), `tools/check_versions_md.py` and `tools/check_wi_refs.py --table` (WI-0.007), `shellcheck`, `ktlint` on `android/`. A step whose tool or configuration file is not on the branch prints a notice and is skipped; once M0.1 is merged every step runs on `main` (issue #27) | required |
| `license-gate` | `ubuntu-22.04` | PR, push | `python -I tools/license_gate.py --policy tools/license_policy.toml --versions third_party/VERSIONS.md --vcpkg build/vcpkg_installed/x64-linux-ovt --manifest build/linux-x64-release/license_manifest.json --gradle android/app/build/reports/dependency-license --pip python/requirements*.txt`; fails on AGPL-*, SSPL-*, BUSL-*, CC-BY-NC-*, LicenseRef-*-NonCommercial, unknown licence, or an LGPL `ovt_core` linking a GPL library (chapter 15 §15.4) | required |
| `build-linux-x64` | `ubuntu-22.04` | PR, push | `cmake --workflow --preset ci-linux-x64` (GCC 13), whose test step runs the `linux-x64-release` test preset — a superset of the `pr` lane, which §13.2.4 binds to `linux-x64-debug`, a tree this job does not build; this is the step chapter 14 calls the `tests` job — there is no separate job; JUnit at `build/linux-x64-release/junit.xml` uploaded as artefact (a `pr-release` lane preset, should the full preset become too slow, is a D23 change); CLI golden tests; uploads `ovt-linux-x64.tar.zst` while the repository is private (a public artefact would be a binary distribution without notices, chapter 15 §15.6) | required |
| `build-linux-x64-clang` | `ubuntu-22.04` | push to main, nightly | same with Clang 16 (`CC=clang-16`, apt.llvm.org) | no |
| `build-linux-arm64` | `ubuntu-22.04-arm` (GitHub arm64 runner) | push to main, tags | `linux-arm64-release` | tags |
| `build-macos-arm64` | `[self-hosted, macOS, ARM64]` (the owner's Mac Mini, the macOS reference build host; `macos-15` is the fallback for a public repository) | PR, push | `cmake --workflow --preset ci-macos-arm64`, then `tools/ctest.sh pr macos-arm64-release` (= `ctest --preset pr-macos`; JUnit at `build/macos-arm64-release/junit.xml`); ad-hoc sign; bundle; `tools/mac/smoke.sh` (launches `OVT Studio.app --self-test` which opens a corpus file offscreen and exits); on pushes to `main` uploads the `ovt` CLI and `OVT Studio.app` (once it exists) as the zipped artefact `ovt-macos-arm64`, while the repository is private (as for `build-linux-x64`) | required |
| `build-macos-x86_64` | `macos-15-intel` | push to main, tags | `macos-x86_64-release` (the `macos-13` image was retired in 2025) | tags |
| `asan-ubsan` | `ubuntu-22.04` | PR, push | `ci-linux-asan-ubsan` workflow (Clang 16; `OVT_BUILD_GUI=OFF`): `linux-x64-asan-ubsan` configure/build, `ctest --preset sanitizers` (the job chapter 14 §14.8.1 calls `sanitizers`) | required |
| `macos-asan` | `macos-15` | nightly | `ci-macos-asan` workflow (`macos-arm64-asan`, `ctest --preset sanitizers-macos`) | no |
| `tsan` | `ubuntu-22.04` | nightly | `linux-x64-tsan`; `ctest --preset linux-x64-tsan -L "jobs|cache|analysis"` | no |
| `fuzz-smoke` (gate over the matrix job `fuzz-smoke-shard`, 4 shards) | `ubuntu-22.04` | PR, push | build `linux-x64-fuzz`; run **each** `ovt_fuzz_*` target for 60 s with its seed corpus (`core/tests/fuzz/corpus/<target>/`), `-max_len=1048576 -timeout=10 -rss_limit_mb=2048`; any crash is uploaded as artefact and fails the job | required |
| `fuzz-nightly` | `ubuntu-22.04` | nightly | 20 min per target in 4 shards, corpus minimisation (`-merge=1`), PR auto-opened with new corpus entries by the job `fuzz-corpus-pr` (`tools/fuzz_corpus_pr.py`; the only job with write permission) (OSS-Fuzz onboarding is chapter 14 WI-0.569; there is no OSS-Fuzz CMake preset — its `build.sh` configures with `-DOVT_FUZZ_ENGINE=$LIB_FUZZING_ENGINE`) | no |
| `clang-tidy` | `ubuntu-22.04` | PR | `run-clang-tidy -p build/linux-x64-debug` on changed files (`.clang-tidy`: `bugprone-*, cert-*, clang-analyzer-*, cppcoreguidelines-{pro-bounds,narrowing}*, modernize-*, performance-*, readability-{braces,container,redundant}*`, `-modernize-use-trailing-return-type`); warnings as errors | required |
| `android-build` | `ubuntu-22.04` | PR, push | `tools/bootstrap_android.sh`; `./gradlew :app:assembleFossDebug :app:assembleFossRelease --no-daemon`; `./gradlew :app:bundlePlayRelease` on tags; builds both ABIs; uploads APKs | required |
| `android-emulator-smoke` | `ubuntu-22.04` (GitHub Linux runners expose KVM) | PR (x86_64 only), nightly | `reactivecircus/android-emulator-runner` API 36 `google_apis` x86_64 image (= `targetSdk`; bumped with it, §13.4.2); `adb install` APK; `adb shell am start` + `uiautomator dump` asserts the main window; `adb push ovt /data/local/tmp && adb shell /data/local/tmp/ovt info /data/local/tmp/mini.mp4` must exit 0; `adb logcat -d | grep -c 'OVT-FATAL' == 0`; screenshot artefact | required |
| `python-wheels` | `macos-15`, `macos-15-intel`, `ubuntu-22.04` (+ `ubuntu-22.04-arm`) | push to main, tags | `cibuildwheel` with `CIBW_BUILD="cp311-* cp312-* cp313-*"` (one wheel per interpreter; pybind11 has no abi3 mode — chapter 12 §12.4.1), `CIBW_MANYLINUX_X86_64_IMAGE=manylinux_2_28`, `CIBW_MANYLINUX_AARCH64_IMAGE=manylinux_2_28`, `CIBW_ENVIRONMENT_MACOS="MACOSX_DEPLOYMENT_TARGET=13.0"`, `CIBW_ENVIRONMENT_LINUX="LDFLAGS=-static-libstdc++ -static-libgcc"`, `CIBW_TEST_COMMAND="pytest {project}/python/tests"`; `auditwheel`/`delocate` repair is the gate; uploads 12 wheels; on tags publishes to PyPI via trusted publishing | tags |
| `python-test` | `ubuntu-22.04` | PR | `pytest python/tests` and every `python/examples/*.py` against the `fast` corpus subset (chapter 12 WI-4.545) | required |
| `python-typecheck` | `ubuntu-22.04` | PR | `mypy --strict python/` (chapter 04 FR-API acceptance names this job) | required |
| `python-docs` | `ubuntu-22.04` | PR | `sphinx-build -W python/docs` | required |
| `bootstrap-check` | `ubuntu-22.04`, `macos-15` | monthly (`schedule: cron '0 4 1 * *'`), manual | fresh container/runner with only `git` + `sudo`: `tools/bootstrap.sh --no-qt && cmake --preset linux-x64-debug && cmake --build --preset linux-x64-debug` (macOS: `macos-arm64-debug`) — proves FR-GEN-101 (WI-0.006) | no |
| `DCO` | GitHub app (`dcoapp/app`), not a workflow job | every PR | every commit carries `Signed-off-by` (chapter 15 §15.7.1, WI-0.604); the status-check name is exactly `DCO` | required |
| `docs` | `ubuntu-22.04` | push to main | Doxygen (C API) + mkdocs for `docs/spec`, JSON schemas, deployed to GitHub Pages | no |
| `abi-check` | `ubuntu-22.04` | PR touching `core/include/` | `abidiff --harmless --no-added-syms` of `libovt_core.so` (both sides `RelWithDebInfo`: abidiff needs DWARF) against the last release tag's build (downloaded artefact); fails on removed/changed symbols unless PR has label `abi-break` and bumps `OVT_API_VERSION_MAJOR` | required (path-filtered) |
| `perf` | self-hosted `m1-air-8gb` (`[self-hosted, macOS, ARM64]`) + `linux-x64-ref` (chapter 14 §14.9.2) | nightly | `ctest --preset perf-macos` / `ctest --preset perf` through `tools/perf_run.py` (chapter 14 §14.9, WI-0.560 — budgets live in `tests/perf/budgets.json`, chapter 14 §14.9.3), results uploaded as artefacts and compared against `tests/perf/baselines/` by `tools/perf_compare.py` (WI-0.560); regression > 20 % or a budget miss opens an issue | no |
| `release` | `ubuntu-22.04` orchestrator | tag `v*` | waits for all build jobs; `tools/release/collect.py` gathers DMGs (notarised in `build-macos-*` tag runs), AppImage, Linux tarballs, APKs/AAB, wheels, `SHA256SUMS`, `minisign` signatures, `THIRD_PARTY_NOTICES.md`, Corresponding Source tarball (`ovt-<ver>-src-with-deps.tar.zst` including every third_party download — GPL §6); creates the GitHub Release from `CHANGELOG.md` section; triggers cask bump PR and Flathub PR | tags |
| `gui` | `ubuntu-22.04` (offscreen), `macos-15` | PR, push | chapter 11 §11.9 GUI tests: models, QML units, `--test-ipc` interaction scripts and screenshot diffs (`OVT_RENDER_STUB=1` on Linux); job definition contributed to `ci.yml` by chapter 11 WI-1.465, scripts by chapter 14 WI-0.568/WI-1.576 | required |
| `coverage` | `ubuntu-22.04` | PR, push | Clang 16 build with `-fprofile-instr-generate -fcoverage-mapping` (preset `linux-x64-debug` + `OVT_COVERAGE=ON`), `ctest --preset pr`, `llvm-cov report` over `core/src/`; fails below 80 % overall or 90 % for `bitreader/`, `syntax/`, `container/` (chapter 04 NFR-028); owned by WI-0.031 | required |
| `docs-generated` | `ubuntu-22.04` | PR | builds `ovt_json_schema_check` (chapter 12 WI-1.501): regenerates `docs/spec/schemas/*.schema.json` and fails on any diff against the committed files (chapter 04 NFR-027) | required |
| `corpora-health` | `ubuntu-22.04` | nightly; PR touching `tests/corpora/` or `docs/spec/14-*.md` | `tools/fetch_corpora.py --verify-only` on the full set (opens an issue when a URL has gone dead, chapter 14 §14.2.4) plus the name check of chapter 14 §14.3.2 (every fixture named in §14.3 has a manifest entry or synthetic recipe) | required (path-filtered) |
| `oracle-image` | `ubuntu-24.04` | push to `tests/oracles/**`, monthly | builds and pushes `ghcr.io/openvideotools/ovt-oracles:<yyyymmdd>` from `tests/oracles/Dockerfile` (chapter 14 §14.2.6, WI-0.554) | no |
| `corpora-cache` | `ubuntu-22.04` | weekly | `tools/fetch_corpora.py` without a filter (= every manifest entry; grammar chapter 14 §14.2.4) to refresh the actions cache of conformance streams listed in `tests/corpora/manifest.json` | no |

Workflow files (the complete set; chapters 04, 11 and 14 name jobs, not files — chapter 14's `tests` is the test step inside each `build-*` job (`pr-macos` on macOS, the `linux-x64-release` test preset on Linux), its `sanitizers` is the `asan-ubsan` job, and `licence-gate` is `license-gate`; chapter 04's `android` is `android-build`, `linux-x64` is `build-linux-x64`, `sanitize-asan-ubsan` is `asan-ubsan` and `sanitize-tsan` is `tsan`): `.github/workflows/ci.yml` (skeleton committed by WI-0.000 so that M0.1 WIs can add jobs; jobs: lint, license-gate, builds with their ctest step, asan-ubsan, clang-tidy, android build + emulator smoke, `python-test`/`python-typecheck`/`python-docs`, `gui` job of chapter 11 WI-1.465, `coverage`, `docs-generated`, `corpora-health`), `oracle-image.yml` (see below), `nightly.yml` (tsan, macos-asan, long fuzz, distro packaging), `bootstrap-check.yml` (WI-0.006), `android-nightly.yml` (chapter 14 WI-1.577; device-farm/emulator matrix), `perf-nightly.yml` (WI-0.560), `fuzz-smoke.yml` (WI-0.557), `static-analysis.yml` (WI-0.559), `oracle-image.yml` (chapter 14 oracle container image), `release.yml`, `wheels.yml`, `abi.yml`, `docs.yml`. All jobs use `concurrency: { group: ${{ github.workflow }}-${{ github.ref }}, cancel-in-progress: true }` for PRs. Third-party actions are pinned by commit SHA (`actions/checkout@<sha>`), not tags.

### 13.7.2 Caching strategy

| Cache | Key | Restore keys | Tool |
|---|---|---|---|
| vcpkg binary cache | `vcpkg-${{ runner.os }}-${{ triplet }}-${{ hashFiles('vcpkg.json','third_party/vcpkg-triplets/**','third_party/vcpkg-overlays/**') }}` | `vcpkg-${{ runner.os }}-${{ triplet }}-` | `actions/cache` on `build/vcpkg-archives` + `VCPKG_BINARY_SOURCES="clear;files,<that dir>,readwrite"` (vcpkg's own `x-gha` provider was removed in 2025) |
| `third_party` install prefix | `tp-${{ runner.os }}-${{ preset }}-${{ hashFiles('third_party/VERSIONS.md','third_party/*.cmake','third_party/patches/**','plugins/*/patches/**') }}` | none (exact only, to keep reproducibility) | `actions/cache` on `build/third_party/<preset>` |
| third_party downloads | `tpdl-${{ hashFiles('third_party/VERSIONS.md') }}` | `tpdl-` | `actions/cache` on `build/third_party/downloads` |
| ccache | `ccache-${{ runner.os }}-${{ preset }}-${{ github.sha }}` | `ccache-${{ runner.os }}-${{ preset }}-` | `hendrikmuhs/ccache-action` (max 2 GB, `compiler_check=content`) |
| Qt | `qt-${{ runner.os }}-6.8.3-${{ hashFiles('tools/install_qt.py') }}` | none | `actions/cache` on `$QT_ROOT_DIR` (aqtinstall) |
| Gradle | default of `gradle/actions/setup-gradle` | | |
| Android SDK/NDK | `android-sdk-${{ hashFiles('android/gradle/libs.versions.toml') }}` | | `actions/cache` on `$ANDROID_SDK_ROOT` |
| Corpora | `corpora-${{ hashFiles('tests/corpora/manifest.json') }}-${{ runner.os }}` (key of chapter 14 §14.2.4) | | `actions/cache` on the fetcher cache directory (≤ 8 GB total; PR jobs fetch only `--tags fast`, ≤ 300 MiB, chapter 14 §14.2.4) |

Cold-start protection: `third_party` cache misses are the dominant cost (FFmpeg ×2 + libaom + Qt-less deps ≈ 25 min); the nightly job rebuilds caches so PRs stay warm.

### 13.7.3 Required status checks (branch protection on `main`)

`lint`, `license-gate`, `build-linux-x64`, `build-macos-arm64`, `asan-ubsan`, `fuzz-smoke`, `clang-tidy`, `android-build`, `android-emulator-smoke`, `gui`, `coverage`, `docs-generated`, `python-test`, `python-typecheck`, `python-docs`, `abi-check` and `corpora-health` (when triggered), plus the **`DCO`** status check (GitHub app `dcoapp/app`, configured by chapter 15 WI-0.604; the check is named `DCO`, not `dco-check`) and at least one maintainer review (CODEOWNERS). Merge method: squash or rebase (no merge commits); linear history enforced. While the repository is private on GitHub Free, branch protection and required checks are not available: the rules are enforced by the agents' instructions (`CLAUDE.md` §2), the DCO app only reports on PRs, and WI-0.605 scripts the protection once the repository is public or on a paid plan (issue #25).

---

## 13.8 Versioning and release process

1. **Semantic versioning** `MAJOR.MINOR.PATCH` in the file `/VERSION` (single line). Product version = `ovt --version`; C API version `OVT_API_VERSION` (chapter 12 §12.3.2) is kept equal while 1.x; FlatBuffers `schema_version` fields are independent integers bumped only on breaking schema changes (chapter 06). Android `versionCode = MAJOR*1_000_000 + MINOR*10_000 + PATCH*100 + abiIndex` (`arm64-v8a=1`, `x86_64=2`; AAB uses 0).
2. **Branches:** `main` (always releasable), `release/1.x` for patch backports, feature branches via PRs. Tags `v1.2.0`, signed (`git tag -s`) by a maintainer whose key is in `MAINTAINERS.md`.
3. **Changelog:** `CHANGELOG.md` in Keep-a-Changelog format; every PR adds a fragment under `changelog.d/<pr>.<type>.md` (`added|changed|fixed|removed|security|deps`) assembled by `tools/release/changelog.py` (towncrier-style) at release time.
4. **Release steps (`docs/dev/release.md`, executed by a maintainer):** (a) `tools/release/bump.py 1.2.0` updates `/VERSION`, `CMakeLists.txt` nothing else (reads VERSION), `python/pyproject.toml` (dynamic from VERSION via scikit-build-core), `android` (reads VERSION), `CHANGELOG.md`; (b) PR, merge; (c) `git tag -s v1.2.0 && git push --tags`; (d) the `release` workflow builds, signs, notarises, generates `THIRD_PARTY_NOTICES.md` (`tools/gen_notices.py`, chapter 15 §15.6) and the Corresponding Source archive, publishes the GitHub Release as a draft; (e) maintainer verifies `tools/repro_check.sh` on the F-Droid APK and the SHA256SUMS, publishes; (f) automation opens the Homebrew tap PR, Flathub PR, `fdroiddata` MR (manual), PyPI upload (trusted publisher), Play Console upload (`r0adkll/upload-google-play` to the *internal* track; promotion to production is manual).
5. **Pre-releases:** `v1.2.0-rc.1` tags produce artefacts marked pre-release, published to the Play internal track and the Homebrew tap only as `ovt-studio@rc`.
6. **Support:** the latest MINOR receives PATCH releases; the previous MINOR receives security fixes for 6 months.

### 13.8.1 `tools/release/manifest.toml` (owned by WI-0.036, which also writes its `[licenses]` table and the `--check-licenses` validation using the gate module of chapter 15 WI-0.602; WI-0.602 itself does not depend on this file — D25)

The release manifest says which built components go into which artefact; `tools/release/collect.py` and the licence gate read it.

```toml
# tools/release/manifest.toml — one key per line (valid TOML 1.0; read with tomllib)
schema_version = 1

[artifacts.macos-arm64-dmg]          # one table per artefact id; id = <platform>-<arch>-<kind>
platform = "macos"                   # macos | linux | android | python | source
arch = "arm64"                       # arm64 | x86_64 | universal | any
kind = "dmg"                         # dmg | zip | appimage | tgz | apk | aab | wheel | flatpak | tar.zst
preset = "macos-arm64-release"       # CMakePresets.json build preset that produced the components
file = "OVT-Studio-{version}-arm64.dmg"
components = ["ovt_core", "ovt_render", "ovt_cli", "ovt_studio", "ovt_plugin_aom", "ffmpeg-core", "moltenvk"]   # target names of §13.2.1 / third_party names of §13.3.7
default_download = true              # linked from the download page; MUST NOT contain default_download_forbidden_plugins (chapter 15 §15.4.2)
codec_pack = false                   # true only for codec-pack artefacts (ovt_plugin_ffmpeg, ovt_plugin_de265)
notices = true                       # THIRD_PARTY_NOTICES.md and LICENSES/ are embedded
sign = "developer-id"                # developer-id | minisign | apksigner | none

[artifacts.macos-arm64-codec-pack]
platform = "macos"
arch = "arm64"
kind = "zip"
preset = "macos-arm64-release"
file = "ovt-codec-pack-{version}-macos-arm64.zip"
components = ["ovt_plugin_ffmpeg", "ovt_plugin_de265"]
default_download = false
codec_pack = true
notices = true
sign = "developer-id"

# … one table per artefact of §13.7.1 `release` row (linux-x64-appimage, linux-x64-tgz, linux-arm64-tgz, android-arm64-apk, android-x86_64-apk, android-aab, android-codec-pack-*, python-wheels, src-with-deps) …

[licenses]                           # appended by chapter 15 WI-0.602: per artefact, the SPDX expression of the whole artefact and the required notice files
"macos-arm64-dmg" = { spdx = "GPL-3.0-or-later AND LGPL-2.1-or-later AND BSD-2-Clause AND Apache-2.0", notices = ["THIRD_PARTY_NOTICES.md", "LICENSES/LicenseRef-AOM-Patent-License-1.0.txt"] }
```

Rules: every `components` entry MUST be a target of §13.2.1 or a Name of §13.3.7 (checked by `tools/release/collect.py --check`); every artefact table MUST carry all keys shown (no defaults); a codec-pack artefact MUST have `codec_pack = true` and `default_download = false`, and its plugins MUST NOT appear in a `default_download = true` artefact's `components`; the licence gate fails when an artefact's resolved licences (from `build/<preset>/license_manifest.json`, §13.2.3) contradict its `[licenses]` row.

---

## 13.9 Developer workflow

### 13.9.1 Bootstrap (`tools/bootstrap.sh`, `tools/bootstrap_android.sh`)

`tools/bootstrap.sh` (bash, idempotent): (1) checks host tools and versions (cmake ≥ 3.25, ninja, python ≥ 3.11, git, curl, nasm, pkg-config, ccache; on macOS: `xcode-select -p`, Xcode ≥ 16; on Linux: gcc ≥ 13 or clang ≥ 16, dev headers list) and prints the exact install command for what is missing (`brew install …` / `sudo apt install …` from `tools/apt-packages.txt`, `tools/brew-packages.txt`); (2) `git submodule update --init third_party/vcpkg` (vcpkg is a submodule pinned at the baseline) and `third_party/vcpkg/bootstrap-vcpkg.sh -disableMetrics`; (3) `python -m venv .venv && pip install -r tools/requirements.txt` (meson, ninja, aqtinstall, ruff, mypy, reuse, jsonschema, check-jsonschema, pybind11-stubgen, cmake-format, markdownlint-cli2 via npm is installed separately in `lint`; `tools/requirements.txt` is the developer-tooling list — `python/requirements*.txt` is the `ovt` package's own dependency list and `tests/oracles/requirements.txt` the oracle image's; three files, three purposes); (4) `tools/install_qt.py --target desktop` into `$QT_ROOT_DIR` (default `~/Qt/ovt`), unless `--no-qt`; (5) `tools/fetch_corpora.py --tags fast` (grammar of chapter 14 §14.2.4; there is no `--set`); (6) `pre-commit install`; (7) prints the preset to use. `tools/bootstrap_android.sh` additionally installs the SDK/NDK via `sdkmanager` (JDK 17 required), Qt Android kits, and writes `android/local.properties`.

### 13.9.2 Pre-commit hooks (`.pre-commit-config.yaml`)

`clang-format` (C/C++ changed files), `cmake-format`, `ruff` (+ `ruff format`), `mypy` (tools/, python/), `reuse lint` (only staged files via `reuse annotate` suggestion), `check-added-large-files` (max 2 MB; corpora never committed), `end-of-file-fixer`, `trailing-whitespace`, `ktlint`, `shellcheck`, `tools/check_versions_md.py` (every third_party script references a VERSIONS.md row and vice versa), `tools/check_wi_refs.py` (commit message mentions `WI-x.yyy` or `#issue`), and a commit-msg hook adding `Signed-off-by` if missing (DCO, chapter 15) — the hook asks for confirmation rather than silently adding.

### 13.9.3 Coding standards (summary; full text in `docs/dev/coding-standards.md`)

- C++20, no exceptions across the C ABI (caught at module boundary → `ovt_status` + `Diagnostic`, §0.7 rule 1); exceptions allowed internally only as `ovt::ParseError` from `BitReader` and `std::bad_alloc`.
- No raw `new/delete`; `std::unique_ptr`, `std::span`, `std::string_view`; no raw pointer arithmetic on input (§0.7 rule 1).
- Every public C++ class in `core/` has a Catch2 test file `core/tests/<module>/test_<class>.cpp`; every parser has `core/tests/fuzz/fuzz_<parser>.cpp` and a seed corpus.
- Naming per §0.4. `.clang-format`: based on `Google` with `ColumnLimit: 120`, `IndentWidth: 2`, `DerivePointerAlignment: false`, `PointerAlignment: Left`, `AllowShortFunctionsOnASingleLine: Inline`.
- Thread-safety annotations (`GUARDED_BY`) on shared state; Clang `-Wthread-safety` is on.
- Logging via `spdlog` through `ovt::log::module("syntax")` only; never `printf`/`std::cout` in libraries.
- Files start with the SPDX header of their directory (chapter 15 §15.1); `reuse lint` enforces.
- Commits: imperative subject ≤ 72 chars, body explains *why*, references `WI-` id, `Signed-off-by` line.

### 13.9.4 How to add an analysis decoder plugin (checklist, `docs/dev/add-plugin.md`)

1. Create `plugins/ovt-plugin-<id>/` with `CMakeLists.txt` (`add_library(ovt_plugin_<id> MODULE …)`, `ovt_set_license`), `src/plugin.cpp` exporting only `ovt_analysis_decoder_v1`, `patches/` if the upstream decoder needs them (header with `Upstream-Status`), `README.md` (licence, data provenance, limitations), `tests/` (golden `FrameAnalysis` on 3 corpus streams, fuzz target feeding `push()`).
2. Add the dependency to `third_party/VERSIONS.md` and `third_party/<dep>.cmake`; add `OVT_BUILD_PLUGIN_<ID>` option; add to `plugins/CMakeLists.txt`.
3. Fill `ovt_AnalysisCaps` honestly (`grid_fields` mask must match what you fill; `bit_exact`).
4. Map every syntax element the decoder reads for a block into the codec extension table (§0.5.4 rule); add the table to `ovt_model.fbs` if new (chapter 06 process).
5. Run `ovt plugins --check`, `ovt analyze --plugin <id>` on the corpus, `ovt verify --plugin <id> --against <oracle>`.
6. Add to CI matrix (`fuzz-smoke` picks up new `ovt_fuzz_*` automatically), `tools/license_policy.toml` if a new licence appears, `THIRD_PARTY_NOTICES` is generated.
7. Decide shipping: default download / codec pack / not shipped (chapter 15 §15.5 table) and update `tools/release/manifest.toml`.

### 13.9.5 How to add a metric (checklist, `docs/dev/add-metric.md`)

1. Built-in: `core/src/metrics/<id>/` implementing `ovt_MetricApi` (§0.6.3) and registered in `core/src/metrics/registry.cpp`; or external: `plugins/ovt-metric-<id>/` MODULE exporting `ovt_metric_v1`.
2. Write the `MetricDescriptor` (id, domain, per_plane, outputs, license) and a JSON options schema under `docs/spec/schemas/metrics/<id>.schema.json`.
3. Implement `score()`; the normalisation stage already delivers frames in the declared `WorkingDomain`; produce `map16` if `outputs` says so.
4. Tests: parity against the reference implementation (chapter 14 names it per metric) to a stated tolerance; `core/tests/metrics/test_<id>.cpp`.
5. Add to `ovt metrics --metric all-fr` list automatically through the descriptor's `kind`; document in `docs/metrics/<id>.md`; licence gate row if a new library is involved.

---

## 13.10 Work items (phase 0 — owned by this chapter)

Numbering: WI-0.000–0.009 repository and build skeleton; 0.010–0.029 third_party scripts; 0.030–0.039 CI; 0.040–0.049 licence gate hooks and corpora; 0.050–0.059 Android skeleton and HDR spike; 0.060–0.069 macOS packaging; 0.070–0.079 Linux packaging. (Chapter 06 owns WI-0.08x for schemas; `io`, `BitReader`, log and jobs are WI-1.100–1.107 and WI-1.512 (phase-0 deliverables, see chapter 16 §16.3.11); chapter 15 owns WI-0.6xx for LICENSES, SPDX, gate policy, notices generator, DCO.)

### WI-0.000 — Repository skeleton and top-level CMake
**Goal:** Create the §0.4 directory layout with a building (empty) `ovt_core`, `ovt_cli` printing the version, and the `cmake/` module set.
**Depends on:** none.
**Produces:** `CMakeLists.txt` and `VERSION` (`0.1.0`) at the repository root, `cmake/{OvtOptions,OvtCompilerFlags,OvtThirdParty,OvtLicenseTags,OvtInstall,OvtVersion}.cmake` (`OvtVersion.cmake` derives `OVT_VERSION_FULL = <VERSION>+<git sha>` for FR-GEN-010) plus **empty stubs** `cmake/OvtMeson.cmake` and `cmake/OvtThirdPartyCommon.cmake` (filled by WI-0.011 / WI-0.010, which own their content), `cmake/ovt-config.cmake.in` and `cmake/ovt-core.pc.in` (the templates §13.2.6 consumes; D27 — verified by WI-0.008), `core/CMakeLists.txt`, `core/include/ovt/{ovt,version,status}.h` (stubs from chapter 12 §12.3), `core/src/version.cpp`, `cli/CMakeLists.txt`, `cli/src/main.cpp` (prints version), the §0.4 top-level directories `render/`, `plugins/`, `app/`, `android/`, `python/`, `third_party/`, `tests/`, `tools/`, `docs/`, `.github/` each with a placeholder `CMakeLists.txt` or `README.md`, `.gitignore`, `.gitattributes` (`* text=auto eol=lf`), `.editorconfig`, stub `.clang-format` / `.clang-tidy` / `.cmake-format.yaml` (content owned by WI-0.007 / WI-0.559), `.github/workflows/ci.yml` skeleton (`on: [pull_request, push]`, the `concurrency` block of §13.7.1 and one placeholder job `skeleton` — `ubuntu-22.04`, a single `echo` step, no third-party actions, `permissions: contents: read` — because GitHub Actions rejects a workflow whose `jobs:` map is empty; M0.1 WIs can add jobs beside it before WI-0.030 lands, and WI-0.030 deletes it; issue #2), `docs/spec/00-front-matter.md … 90-appendices.md` (verbatim copy of this specification, the §16.2.1 entry criterion) with `docs/spec/CHANGELOG.md`, and `docs/adr/ADR-001-….md … ADR-0nn-….md` (one file per ADR of chapter 0 §0.2, text copied verbatim).
**Interface:** §13.2.1 target names, §13.2.2 options.
**Steps:** 1. Create directories and placeholder files; 2. write `OvtOptions.cmake` with every option of §13.2.2 and dependency validation (`OVT_FFMPEG_GPL` guard); 3. write `OvtCompilerFlags.cmake` with the `ovt::flags` interface target and the flag lists of §13.2.5 (GCC/Clang/AppleClang branches); 4. `OvtLicenseTags.cmake` with `ovt_set_license()` storing a target property and a `ovt_license_manifest` custom target writing `build/<preset>/license_manifest.json` (schema §13.2.3) via a CMake script walking `LINK_LIBRARIES`; 5. minimal `ovt_core` with `ovt_version*` implemented; 6. `ovt_cli` printing `ovt 0.1.0+<git sha>` (FR-GEN-010 format, from `OVT_VERSION_FULL`); 7. `OvtInstall.cmake` per §13.2.6 with `ovt-config.cmake.in`/`ovt-core.pc.in` (export works with an empty library); 8. commit `docs/spec/`, `docs/adr/` and the `ci.yml` skeleton.
**Acceptance tests:** 1. Given a clone, When `cmake -S . -B build -G Ninja && cmake --build build` (presets arrive with WI-0.001), Then `build/cli/ovt --version` prints a line matching `^ovt 0\.1\.0\+[0-9a-f]{7,}` (FR-GEN-010 format). 2. Given `cmake --preset linux-x64-debug` (or the host's debug preset), When `cmake --build --preset linux-x64-debug --target ovt_license_manifest` runs, Then `build/linux-x64-debug/license_manifest.json` validates against the §13.2.3 schema and lists `ovt_core` with `LGPL-2.1-or-later` and `ovt_cli` with `GPL-3.0-or-later`. 3. Given `-DOVT_FFMPEG_GPL=ON` alone, When `cmake --preset linux-x64-debug` configures, Then configure fails with the message naming `OVT_FFMPEG_GPL_PLUGIN_ONLY`. Tests 2 and 3 name a preset that WI-0.001 creates: on the WI-0.000 branch they run against the plain configure into the directory the preset would use, `cmake -S . -B build/linux-x64-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` (plus `-DOVT_FFMPEG_GPL=ON` for test 3), and WI-0.001 re-runs both with `--preset` as a regression check (issue #3).
**Size:** M.
**Notes / pitfalls:** Do not add `vcpkg` yet (WI-0.002). Keep `cmake_minimum_required(3.25)` (not higher) because Ubuntu 22.04's Kitware repo is the baseline; but presets `version 6` need CMake 3.25 exactly — consistent.
### WI-0.001 — `CMakePresets.json` and custom vcpkg triplets
**Goal:** Commit the complete presets file of §13.2.4 and the `*-ovt` triplets.
**Depends on:** WI-0.000.
**Produces:** `/CMakePresets.json` (verbatim §13.2.4, including the lane test presets `pr`, `nightly`, `sanitizers`, `perf` and their `-macos`/`-android` counterparts), `tools/ctest.sh` (dispatcher: `tools/ctest.sh <pr|nightly|sanitizers|perf> [configure-preset]` picks the host's lane preset, §13.2.4), `third_party/vcpkg-triplets/{arm64-osx-ovt,x64-osx-ovt,x64-linux-ovt,arm64-linux-ovt,arm64-android-ovt,x64-android-ovt}.cmake`, `docs/dev/presets.md`.
**Interface:** §13.2.4.
**Steps:** 1. Write presets; 2. write triplets (static linkage, hidden visibility, deployment target, Android `VCPKG_CMAKE_SYSTEM_VERSION 28`, `VCPKG_CMAKE_CONFIGURE_OPTIONS` passing `-DCMAKE_POSITION_INDEPENDENT_CODE=ON` and `-DANDROID_STL=c++_shared`); 3. verify `cmake --list-presets` on each OS shows only applicable presets (conditions); 4. `CMakeUserPresets.json` documented as the place for local overrides and gitignored.
**Acceptance tests:** 1. Given macOS, When `cmake --list-presets=configure`, Then `macos-arm64-debug` listed and `linux-x64-debug` not listed. 2. Given Ubuntu 22.04 with `QT_ROOT_DIR` unset, When `cmake --preset linux-x64-debug` runs, Then configure succeeds with `OVT_BUILD_GUI=OFF` warning (Qt not found is not fatal); the `base` preset's vcpkg toolchain file arrives with WI-0.002, so WI-0.001 runs this test with `-DCMAKE_TOOLCHAIN_FILE= -DOVT_USE_VCPKG=OFF` appended (command-line `-D` values win over preset values) and records it in `docs/acceptance/wi/WI-0.001.md`; WI-0.002 re-runs it exactly as written (issue #6). 3. Given `cmake --list-presets=test`, When run on Linux, Then `pr`, `nightly`, `sanitizers`, `perf` and `linux-x64-asan-ubsan` are listed and no name containing `x64-asan` without `-ubsan` exists (`grep -c '"linux-x64-asan"' CMakePresets.json == 0`). 4. Given a configured `macos-arm64-release` tree on macOS, When `tools/ctest.sh pr` runs, Then it executes `ctest --preset pr-macos`.
**Size:** S.
**Notes / pitfalls:** Presets `version 6` require CMake ≥ 3.25; keep `cmakeMinimumRequired` in sync with the top-level `cmake_minimum_required`. Each preset has its own `binaryDir` (`${presetName}`), which is what keeps sanitizer trees apart.
### WI-0.002 — vcpkg manifest, submodule and binary caching
**Goal:** Add vcpkg as a submodule with the pinned baseline, the `vcpkg.json` of §13.3.2 and make `fmt`, `spdlog`, `flatbuffers`, `zstd`, `nlohmann-json`, `highway`, `cli11`, `xxhash`, `catch2` link into `ovt_core`/`ovt_cli`/tests.
**Depends on:** WI-0.001.
**Produces:** `.gitmodules` (`third_party/vcpkg`), `vcpkg.json` (the nine Goal ports and the `features` block only; §13.3.2 is the end state), `vcpkg-configuration.json` (overlay ports/triplets paths), `third_party/vcpkg-overlays/README.md`, updated `core/CMakeLists.txt` with `find_package` calls, `docs/dev/dependencies.md`.
**Interface:** `vcpkg.json` (§13.3.2); imported targets `fmt::fmt`, `spdlog::spdlog`, `flatbuffers::flatbuffers`, `zstd::libzstd_static`, `nlohmann_json::nlohmann_json`, `hwy::hwy`, `CLI11::CLI11`, `xxHash::xxhash`, `Catch2::Catch2WithMain`; `OVT_USE_VCPKG` (§13.2.2) switches to plain `find_package`.
**Steps:** 1. Add submodule at the baseline commit; 2. write manifest; 3. `find_package(fmt CONFIG REQUIRED)` etc.; 4. configure binary caching documentation (`files` provider locally and in CI, §13.3.2); 5. first Catch2 test `core/tests/test_version.cpp`.
**Acceptance tests:** 1. Given a clean checkout, When `cmake --preset linux-x64-debug && cmake --build --preset linux-x64-debug && ctest --preset linux-x64-debug` runs, Then 1 test passes. 2. Given a warm binary cache, When a second configure runs, Then the vcpkg install step takes < 60 s.
**Size:** M.
**Notes / pitfalls:** `flatbuffers` must be listed twice (host tool `flatc` + target library) for Android cross builds. Highway's port name is `highway`, target `hwy::hwy`.
### WI-0.003 — FlatBuffers code generation target and schema install
**Goal:** `ovt_schemas` INTERFACE target generating `*_generated.h` from `core/schema/*.fbs` with `flatc --cpp --gen-object-api --gen-compare --reflect-names --scoped-enums`, plus Python (`--python`, namespace layout) and Kotlin (`--kotlin`) outputs consumed by `python/` and `android/`, all under the generated-code root of §13.2.3 (`build/<preset>/generated/`).
**Overlap:** this WI delivers the first version of `cmake/OvtFlatBuffers.cmake` with the function `ovt_add_flatbuffers_schema` (code generation only) and `tools/check_schema_compat.py`; WI-0.083 extends the same file (bfbs embedding, `schema_version.h`, Python/Kotlin layout) — one file, one function name. See chapter 16 §16.3.
**Depends on:** WI-0.002 (stub schemas with the §0.5 tables suffice until WI-0.080 lands; WI-0.083 then extends this WI's CMake function).
**Produces:** `cmake/OvtFlatBuffers.cmake` (`ovt_add_flatbuffers_schema(<target> <fbs>...)`; WI-0.083 extends this file), `core/schema/CMakeLists.txt`, `tools/check_schema_compat.py` (runs `flatc --conform old.fbs` against the last tag), test `core/tests/schema/test_schema_smoke.cpp`; build output (not committed): `build/<preset>/generated/<schema>_generated.h`, Python `build/<preset>/generated/python/ovt/fb/<Type>.py` (+ `__init__.py`) and Kotlin readers under `build/<preset>/generated/kotlin/` (§13.2.3 generated-code rule; chapter 06 WI-0.080/WI-0.083 use the same paths).
**Interface:** `ovt_add_flatbuffers_schema(<target> <fbs>...)` (chapter 06 WI-0.083 extends it); generated header names `<schema>_generated.h`; target `ovt_schemas`.
**Steps:** 1. `find_program(flatc)` from the *host* vcpkg tree; 2. CMake function with one `add_custom_command` per schema (`DEPFILE`, `--cpp --gen-object-api --gen-compare --reflect-names --scoped-enums`); 3. Python/Kotlin generation under `OVT_BUILD_PYTHON` / Android; 4. `tools/check_schema_compat.py` with `flatc --conform` against the last tag; 5. round-trip test.
**Acceptance tests:** 1. Given `cmake --build --preset linux-x64-debug --target ovt_schemas`, When it finishes, Then `build/linux-x64-debug/generated/ovt_model_generated.h` and `build/linux-x64-debug/generated/python/ovt/fb/FrameAnalysis.py` exist, `PYTHONPATH=build/linux-x64-debug/generated/python python -c 'import ovt.fb.FrameAnalysis'` succeeds, and `core/tests/schema/test_schema_smoke.cpp` (WI-0.003's own schema-level build→verify→JSON→build on a stub record) passes. (The model-level `test_model_roundtrip` of the phase-0 gate, chapter 16 §16.1.1 #10, is produced by chapter 06 WI-0.093 — D25.) 2. Given a field removed from a schema, When `tools/check_schema_compat.py` runs against the last tag, Then it exits 1 naming the field.
**Size:** S.
**Notes / pitfalls:** Android cross builds must run the host `flatc` (`flatbuffers` is listed with `host: true` in `vcpkg.json`); never pick the target-triplet binary.
### WI-0.004 — Test infrastructure, CTest labels and sanitizer presets
**Goal:** Sanitizer flag plumbing so that `linux-x64-asan-ubsan`, `linux-x64-tsan` and `macos-arm64-asan` build and the `OVT_SANITIZED` define reaches the tests (reduced per chapter 16 §16.3: Catch2 integration, labels and `testPresets` are WI-0.550's).
**Overlap:** this WI delivers only `cmake/OvtTesting.cmake` sanitizer flag plumbing (`ovt_enable_sanitizers()`, `OVT_SANITIZED`, the `canary` test); WI-0.550 owns the Catch2 framework, CTest labels and `testPresets`. See chapter 16 §16.3.
**Depends on:** WI-0.002.
**Produces:** `cmake/OvtTesting.cmake` (sanitizer flag plumbing only: `ovt_enable_sanitizers()`, `OVT_SANITIZED`, the `canary` test); `tests/CMakeLists.txt`, `core/tests/CMakeLists.txt` and `tests/perf/README.md` are owned by WI-0.550 / WI-0.560 (reduced, §16.3).
**Interface:** `ovt_enable_sanitizers(<target>)`; compile definition `OVT_SANITIZED=1`; CTest labels `unit|golden|cli|perf|fuzz|canary`.
**Steps:** 1. `OvtTesting.cmake` mapping `OVT_SANITIZER` onto the flag sets of §13.2.5; 2. the `canary` heap-overflow test (label-excluded by default); 3. label registration helper used by WI-0.550; 4. verify under `ci-linux-asan-ubsan`.
**Acceptance tests:** 1. Given `cmake --workflow --preset ci-linux-asan-ubsan`, When it runs, Then it passes and a deliberate heap overflow test (`test_asan_canary`, excluded by default via label `canary`) fails under ASan when run explicitly. 2. Given a configured `linux-x64-asan-ubsan` tree, When `ctest -L canary` is run explicitly, Then only `test_asan_canary` runs and `ctest --preset sanitizers` never selects it.
**Size:** S.
**Notes / pitfalls:** ASan and TSan are never combined in one binary; the `asan-ubsan`/`tsan` presets build in separate `binaryDir`s. `detect_leaks=1` needs `ptrace` — set `ASAN_OPTIONS` in the test preset, not in the toolchain.
### WI-0.005 — libFuzzer harness scaffold
**Goal:** `ovt_add_fuzz_target(name source)` producing `ovt_fuzz_<name>` under `OVT_ENABLE_FUZZ`, with seed corpus directories and a `tools/fuzz_smoke.sh` running each for N seconds.
**Overlap:** superseded by WI-0.557 (`ovt_add_fuzz_target`, fuzz CMake, seeds, smoke workflow); its acceptance test 1 moves to WI-0.557; `tools/fuzz_smoke.sh` is not written. See chapter 16 §16.3.
**Depends on:** WI-0.004.
**Produces:** `cmake/OvtFuzz.cmake`, `core/tests/fuzz/CMakeLists.txt`, `core/tests/fuzz/fuzz_template.cpp`, `core/tests/fuzz/corpus/.gitkeep`, `tools/fuzz_smoke.sh`, `.github/workflows` snippet (used by WI-0.033).
**Interface:** `ovt_add_fuzz_target(<name> <source>)` (final form in WI-0.557); targets `ovt_fuzz_<name>` under `OVT_ENABLE_FUZZ`.
**Steps:** 1. `OvtFuzz.cmake` adding `-fsanitize=fuzzer` executables; 2. template target; 3. corpus directory layout `core/tests/fuzz/corpus/<target>/`; 4. hand the smoke workflow to WI-0.557 (no `tools/fuzz_smoke.sh`).
**Acceptance tests:** 1. Given `linux-x64-fuzz` preset and a trivial target, When `tools/fuzz_smoke.sh 10`, Then exit 0 and `fuzz-*.log` artefacts exist.
**Size:** S.
**Notes / pitfalls:** libFuzzer needs Clang; on GCC the function emits a warning and creates nothing.
### WI-0.006 — Bootstrap scripts and developer docs
**Goal:** `tools/bootstrap.sh` per §13.9.1, `tools/requirements.txt`, `tools/apt-packages.txt`, `tools/brew-packages.txt`, `docs/dev/getting-started.md`.
**Depends on:** WI-0.002.
**Produces:** `tools/bootstrap.sh`, `tools/bootstrap_android.sh` (shared helpers), `tools/requirements.txt`, `tools/apt-packages.txt`, `tools/brew-packages.txt`, `docs/dev/getting-started.md`, `.github/workflows/bootstrap-check.yml`.
**Interface:** §13.9.1 steps 1–7; flags `--no-qt`, `--fdroid` (Android variant), `--preset <name>`.
**Steps:** 1. Tool/version checks printing exact install commands; 2. vcpkg submodule + bootstrap; 3. `.venv` with pinned `tools/requirements.txt`; 4. Qt via `tools/install_qt.py` (WI-0.018) unless `--no-qt`; 5. corpus fetch (`tools/fetch_corpora.py --tags fast`); 6. `pre-commit install`; 7. the monthly `bootstrap-check` job (`.github/workflows/bootstrap-check.yml`, row in §13.7.1; `schedule` + `workflow_dispatch`).
**Acceptance tests:** 1. Given a fresh Ubuntu 22.04 container with only `git` and `sudo`, When `tools/bootstrap.sh --no-qt && cmake --preset linux-x64-debug && cmake --build --preset linux-x64-debug`, Then success (run in CI job `bootstrap-check` monthly). 2. Given a clean `macos-15` runner with only Xcode command-line tools, When `tools/bootstrap.sh --no-qt && cmake --preset macos-arm64-debug && cmake --build --preset macos-arm64-debug` runs, Then it succeeds.
**Size:** M.
**Notes / pitfalls:** Never run `sudo` from the script; print the command. Keep it POSIX-bash (macOS ships bash 3.2 — no associative arrays).
### WI-0.007 — Pre-commit hooks, clang-format/clang-tidy configs, lint CI inputs
**Goal:** §13.9.2 hooks and §13.9.3 configs in place; `tools/check_versions_md.py`, `tools/check_wi_refs.py` (modes `--table`, `--make-issues` and `--spec-version` of chapter 16 §16.2.1, §16.2.5 and §16.2.8), `renovate.json` (§13.3.8).
**Overlap:** this WI delivers only the pre-commit hooks, `clang-format` config and the `check_*.py` scripts; the `.clang-tidy` content is WI-0.559; the warning set is the `ovt::flags` target of §13.2.5 (WI-0.000), there is no `cmake/OvtWarnings.cmake` (D24). See chapter 16 §16.3.
**Depends on:** WI-0.000.
**Produces:** `.pre-commit-config.yaml`, `.clang-format` (content; the stub file is WI-0.000's), `tools/check_versions_md.py`, `tools/check_wi_refs.py` (`--table`: every `WI-x.nnn` cited in `docs/spec/` exists in §16.4; milestone(dep) ≤ milestone(WI); the §16.4 `FRs` column equals the inverse of chapter 04 §4.17 and no row cites a superseded WI; reports requirements no WI claims; prints the SPEC-ASSUMPTION count per WI; `--make-issues`: one tracker issue per §16.4 row, idempotent; `--spec-version`: fails when `docs/spec/**` changed without a version bump and a `docs/spec/CHANGELOG.md` entry), `renovate.json`, `tools/tests/test_check_scripts.py`.
**Interface:** §13.9.2 hook list; §13.9.3 `.clang-format` settings; the `lint` job of WI-0.030 consumes the same configs.
**Steps:** 1. Hook list with pinned revisions; 2. the two check scripts with unit tests; 3. commit-msg DCO prompt hook; 4. documentation in `docs/dev/getting-started.md`.
**Acceptance tests:** 1. Given a mis-formatted file, When `pre-commit run --all-files`, Then it fails and `clang-format` fixes it. 2. Given a commit message without `Signed-off-by`, When the `commit-msg` hook of `.pre-commit-config.yaml` runs, Then it exits 1 with the hint `git commit -s`. 3. Given a WI block whose Depends-on names a WI of a later milestone, When `tools/check_wi_refs.py --table` runs, Then it exits 1 naming both WIs. 4. Given a PR that edits `docs/spec/04-requirements.md` without changing the spec version or `docs/spec/CHANGELOG.md`, When `tools/check_wi_refs.py --spec-version` runs, Then it exits 1. 5. Given `tools/check_wi_refs.py --make-issues --dry-run` run twice, When the second run finishes, Then it reports zero issues to create.
**Size:** S.
**Notes / pitfalls:** `reuse lint` on staged files only; the full-tree run is the CI job. `.clang-tidy` content is WI-0.559's.
### WI-0.008 — Install/export verification and downstream consumer test
**Goal:** Prove `find_package(ovt CONFIG)` works from an install tree without vcpkg.
**Depends on:** WI-0.002.
**Produces:** `tests/downstream/CMakeLists.txt` + `main.c` (uses `ovt::core`), CTest `downstream_consumer` that configures/builds it against `cmake --install` output in a temp dir.
**Interface:** `find_package(ovt 1 CONFIG REQUIRED)` (the package version is the C API version, §13.2.6, so `1` matches while the product is 0.x); imported targets `ovt::core`, `ovt::render`; variables `OVT_PLUGIN_DIR`, `OVT_SCHEMA_DIR` (§13.2.6).
**Steps:** 1. Downstream project `tests/downstream/`; 2. CTest that runs `cmake --install` into a temporary prefix, then configures/builds/runs the consumer with `CMAKE_PREFIX_PATH` set only to that prefix and no vcpkg toolchain; 3. assert the binary links and prints `ovt_version_string()`.
**Acceptance tests:** 1. Given an installed prefix produced by `cmake --install`, When `ctest -R downstream_consumer` runs on macOS and Linux, Then the consumer configures with `find_package(ovt 1 CONFIG REQUIRED)`, links `ovt::core`, and prints `ovt_version_string()`.
**Size:** S.
**Notes / pitfalls:** Static absorption means `ovt-targets.cmake` must not leak vcpkg imported targets in `INTERFACE_LINK_LIBRARIES`; use `$<BUILD_INTERFACE:...>` for private deps.
### WI-0.010 — `third_party` common infrastructure and `VERSIONS.md` parser
**Goal:** `OvtThirdPartyCommon.cmake` (`ovt_tp_declare`, `ovt_tp_import_static`, patch runner), `tools/versions_to_cmake.py`, `tools/apply_patches.py`, `third_party/VERSIONS.md` initial table (§13.3.7), `tools/third_party_offline.py`.
**Depends on:** WI-0.002.
**Produces:** `cmake/OvtThirdPartyCommon.cmake`, `third_party/_template.cmake`, `tools/versions_to_cmake.py`, `tools/apply_patches.py`, `tools/third_party_offline.py`, `third_party/VERSIONS.md` (initial table of §13.3.7).
**Interface:** `ovt_tp_declare`, `ovt_tp_import_static`, `ovt_tp_apply_patches` (§13.3.4); VERSIONS.md columns (§13.3.7); generated `OVT_TP_<dep>_{VERSION,URL,SHA256}` variables (§13.3.7 naming; `tools/versions_to_cmake.py --update-hashes` fills the `<sha>` placeholders).
**Steps:** 1. Parser producing `versions.cmake`; 2. declare/import helpers; 3. patch runner with `--check` and marker file; 4. offline pre-fetcher; 5. VERSIONS.md ↔ script consistency check (hooked by WI-0.007).
**Acceptance tests:** 1. Given VERSIONS.md, When configure runs, Then `build/<preset>/third_party/versions.cmake` has `OVT_TP_libaom_VERSION`, `OVT_TP_libaom_URL`, `OVT_TP_libaom_SHA256` etc. and no `OVT_<DEP>_*` spelling. 2. Given a VERSIONS.md row whose URL/SHA mismatch, When configure downloads it, Then configure fails naming the row. 3. Given network access, When `tools/third_party_offline.py --out dl/` runs, Then every archive downloads and verifies.
**Size:** M.
**Notes / pitfalls:** A version string appears in VERSIONS.md only; scripts read `versions.cmake`, which is written to the build tree (`build/<preset>/third_party/versions.cmake`, test 1), never to the source tree. Reject rows whose URL embeds a version different from the Version column (a tag suffix such as MoltenVK's `v1.2.11-artifacts` is not a different version). `<dep>` ids follow the §13.3.7 mapping (`Gradle / AGP` → `gradle_agp`).
### WI-0.011 — Meson wrapper (`ovt_tp_meson`)
**Goal:** §13.3.5 implemented and proven with dav1d on macOS, Linux and Android (cross).
**Depends on:** WI-0.010.
**Produces:** `cmake/OvtMeson.cmake`, `third_party/dav1d.cmake`, `third_party/meson/cross_template.ini.in`.
**Interface:** `ovt_tp_meson(name SOURCE_DIR … OPTIONS … OUTPUT_LIBS …)` (§13.3.5); imported `tp::dav1d`.
**Steps:** 1. Cross-file template from the active toolchain; 2. the function (configure/compile/install commands); 3. `third_party/dav1d.cmake` as the first consumer; 4. probe tests on the four platform presets.
**Acceptance tests:** 1. Given each of the four platform presets, When built, Then `libdav1d.a` exists in the prefix and `tp::dav1d` links into a probe test calling `dav1d_version()`. 2. Given the `android-arm64-release` and `android-x86_64-debug` presets, When `readelf -h libdav1d.a` runs on each prefix, Then the members report `AArch64` / `X86-64` respectively.
**Size:** M.
**Notes / pitfalls:** Meson needs `pkg-config` that is *not* the system one on Android; use vcpkg's `pkgconf` host tool. Set `b_staticpic=true`. NASM for dav1d x86 asm; disable asm on x86_64 Android only if the NDK Clang integrated assembler fails (record in VERSIONS.md notes).
### WI-0.012 — FFmpeg two-flavour build script
**Goal:** `third_party/ffmpeg.cmake` building the `core` (shared, LGPL, no AVC/HEVC by default) and `plugin` (static, hidden, AVC/HEVC) flavours per §13.3.3 on all platforms with the platform hwaccels.
**Depends on:** WI-0.011 (dav1d), WI-0.010.
**Produces:** `third_party/ffmpeg.cmake`, `third_party/ffmpeg/configure_flags.cmake` (lists per flavour/platform), `third_party/ffmpeg/android_toolchain.sh`, `core/tests/test_ffmpeg_probe.cpp` (asserts `avcodec_find_decoder(AV_CODEC_ID_HEVC) == nullptr` in core flavour unless `OVT_PATENT_CODECS_IN_CORE`), `plugins/ovt-plugin-ffmpeg/tests/test_symbol_leak.sh` (`nm -D` shows no `av*` symbols).
**Interface:** §13.3.3 flavour table; imported targets `tp::ffmpeg_core` (shared, `_ovt` suffix) and `tp::ffmpeg_plugin` (static); options `OVT_PATENT_CODECS_IN_CORE`, `OVT_FFMPEG_GPL*`, `OVT_ENABLE_HW_DECODE`.
**Steps:** 1. Flag lists per flavour and platform in `configure_flags.cmake` (parsers and bitstream filters always on); 2. two ExternalProjects from two extracted source trees; 3. Android toolchain wrapper; 4. probe and symbol-leak tests; 5. option wiring and `ovt_set_license` recording.
**Acceptance tests:** 1. Given default options on Linux, When built, Then `nm -D libavcodec_ovt.so | grep -c ff_h264` is 0 and `avformat` opens an MP4 from the corpus. 2. Given the plugin flavour, When `nm -D --defined-only` runs on `libovt_plugin_ffmpeg.so`, Then it exports exactly `ovt_analysis_decoder_v1` (+ `ovt_plugin_describe`). 3. Given a macOS build, When the plugin flavour's `avcodec_configuration()` string is printed by `test_ffmpeg_probe`, Then it contains `--enable-videotoolbox`. 4. Given an Android arm64 build, When `test_ffmpeg_probe` runs on the emulator, Then `avcodec_configuration()` of `libavcodec_ovt.so` contains `--enable-mediacodec` and `avcodec_find_decoder_by_name("av1_mediacodec") != nullptr`.
**Size:** L.
**Notes / pitfalls:** FFmpeg `configure` for Android needs `--cc=<ndk>/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android28-clang --nm=llvm-nm --ar=llvm-ar --ranlib=llvm-ranlib --strip=llvm-strip`; `--enable-jni --enable-mediacodec` require `--target-os=android`. Two flavours built from one extracted source tree conflict → extract twice (`SOURCE_DIR` per flavour). `--build-suffix=_ovt` changes pkg-config names too (`libavcodec_ovt.pc`).
### WI-0.013 — libaom inspection build script
**Goal:** `third_party/libaom.cmake` per §13.3.4 template, static, inspection + accounting, all platforms.
**Depends on:** WI-0.010.
**Produces:** `third_party/libaom.cmake`, `core/tests/test_aom_probe.cpp` (compiles against the upstream header `<aom/aomdx.h>` and checks `AV1_SET_INSPECTION_CALLBACK` is defined and `aom_codec_version_str()` matches the pin).
**Interface:** imported `tp::aom`; variable `OVT_AOM_CPU`; option `OVT_BUILD_AOM_INSPECT` (adds `-DENABLE_EXAMPLES=1` and installs `inspect`).
**Steps:** 1. Script from `_template.cmake`; 2. per-platform `AOM_TARGET_CPU`; 3. probe test; 4. configure-time inspection check for `OVT_SYSTEM_LIBAOM=ON`.
**Acceptance tests:** 1. Given the four platform presets, When each is built, Then the probe test passes. 2. Given `OVT_SYSTEM_LIBAOM=ON` with a system libaom lacking inspection, When configure runs, Then it fails with `libaom was not built with CONFIG_INSPECTION`.
**Size:** S.
**Notes / pitfalls:** `AOM_TARGET_CPU` must be `arm64` for Android arm64 and `x86_64` for x86_64; libaom's own Android toolchain file is not used — pass the NDK toolchain through `CMAKE_TOOLCHAIN_FILE`. Enable `CONFIG_INSPECTION` requires `CONFIG_ACCOUNTING` for symbol costs.
### WI-0.014 — libde265 patched build script
**Goal:** `third_party/libde265.cmake` with the patch step of §13.3.6 (patch content from chapter 07; a placeholder patch touching only a comment is used until it lands).
**Depends on:** WI-0.010.
**Produces:** `third_party/libde265.cmake`, `plugins/ovt-plugin-de265/patches/series`, `tools/apply_patches.py` tests (`tools/tests/test_apply_patches.py`), `<prefix>/share/ovt/patches.json`.
**Interface:** imported `tp::de265`; `tools/apply_patches.py --series <file> --check`; `<prefix>/share/ovt/patches.json` consumed by `tools/gen_notices.py` (chapter 15).
**Steps:** 1. Script with the patch step of §13.3.6; 2. `series` file; 3. marker and `patches.json`; 4. `tools/tests/test_apply_patches.py`.
**Acceptance tests:** 1. Given a clean build, When it runs twice, Then patches apply once, `patches.json` lists them and the rebuild does not re-apply. 2. Given a patch that fails `--check`, When configure/build runs, Then it fails naming it.
**Size:** S.
**Notes / pitfalls:** Patches must pass `git apply --check` against the pristine 1.0.15 tarball; the marker file written by `tools/apply_patches.py` prevents a second application after a rebuild. The patched library is GPLv3 — it is linked only into the `ovt_plugin_libde265` plugin (chapter 15 §15.2).
### WI-0.015 — libvmaf and libplacebo Meson scripts
**Goal:** `third_party/libvmaf.cmake` and `third_party/libplacebo.cmake` using `ovt_tp_meson` with the options of §13.3.1; glslang and lcms from vcpkg; Vulkan registry path from vcpkg's `vulkan-headers`.
**Depends on:** WI-0.011.
**Produces:** the two scripts; probe tests `core/tests/test_vmaf_probe.cpp` (`vmaf_version()`), `render/tests/test_placebo_probe.cpp` (`pl_log_create` + `pl_vulkan_create` with `PL_VK_IMPORT`/lavapipe on Linux CI; skip when no Vulkan device).
**Interface:** imported `tp::vmaf`, `tp::placebo`; Meson options of §13.3.1; `OVT_SYSTEM_LIBVMAF`/`OVT_SYSTEM_LIBPLACEBO` bypass.
**Steps:** 1. Both scripts through `ovt_tp_meson`; 2. glslang/lcms from vcpkg via `pkg_config_path`; 3. Vulkan registry path from `vulkan-headers`; 4. probe tests incl. lavapipe on Linux CI.
**Acceptance tests:** 1. Given every preset of §13.2.4, When the tree is built, Then both `tp::vmaf` and `tp::placebo` are produced. 2. Given the built libvmaf, When `vmaf_model_load(..., "vmaf_v0.6.1")` is called, Then the built-in model loads. 3. Given Linux CI with lavapipe (`mesa-vulkan-drivers`), When `render/tests/test_placebo_probe` runs, Then libplacebo creates a Vulkan device.
**Size:** M.
**Notes / pitfalls:** libplacebo needs `python3-mako` and `python3-jinja2`; add to `tools/requirements.txt`. Android: `-Dopengl=enabled -Dgl-proc-addr=enabled` for GLES fallback; Vulkan on Android uses `-Dvulkan=enabled -Dvk-proc-addr=enabled` and the NDK's system `libvulkan.so` (no vcpkg `vulkan-loader` on Android; libplacebo has no `vulkan-loader` option — verified against v7.349 `meson_options.txt`).
### WI-0.016 — HM oracle build script (optional)
**Goal:** `third_party/hm.cmake` building `TLibCommon`/`TLibDecoder`/`TLibVideoIO` as static libs from the pinned HM tag, decoder only, with the `-fPIC` patch.
**Overlap:** superseded by WI-4.104 (`third_party/hm.cmake`, scheduled in phase 4); the `-fPIC` note moves to WI-4.104. See chapter 16 §16.3.
**Depends on:** WI-0.010.
**Produces:** none here — `third_party/hm.cmake` is produced by WI-4.104.
**Interface:** none (superseded).
**Steps:** none here; see WI-4.104.
**Acceptance tests:** 1. Given `OVT_BUILD_PLUGIN_HM=ON` on Linux/macOS, When the tree is built, Then `libTLibDecoder.a` exists and a probe decodes the first frame of an HEVC conformance stream.
**Size:** S.
**Notes / pitfalls:** Kept for numbering stability only.
### WI-0.017 — MoltenVK and ONNX Runtime acquisition scripts
**Goal:** `third_party/moltenvk.cmake` (macOS prebuilt, per-arch slice) and `third_party/onnxruntime.cmake` (fallback prebuilt when the vcpkg port is unavailable) per §13.3.1.
**Depends on:** WI-0.010.
**Produces:** `third_party/moltenvk.cmake`, `third_party/onnxruntime.cmake`, probe `core/tests/test_ort_probe.c`, `render/tests/test_moltenvk_probe.cpp`.
**Interface:** imported `tp::moltenvk` (macOS), `tp::onnxruntime`; option `OVT_ONNXRUNTIME_PREBUILT` (§13.2.2).
**Steps:** 1. SHA-verified downloads; 2. `lipo -thin` slice for MoltenVK; 3. prebuilt ONNX Runtime layout import (headers + `libonnxruntime`); 4. probes.
**Acceptance tests:** 1. Given a macOS build, When `lipo -archs` runs on `libMoltenVK.dylib` in the prefix, Then it reports a single architecture. 2. Given Linux x64 configured with `-DOVT_ONNXRUNTIME_PREBUILT=ON` and no vcpkg port, When the build runs, Then `onnxruntime_c_api.h` is found and the `OrtGetApiBase()` probe exits 0.
**Size:** S.
**Notes / pitfalls:** The ONNX Runtime prebuilt for Linux links glibc ≥ 2.28 — fine for the Ubuntu 22.04 baseline and manylinux_2_28 wheels; not available for Android (UVQ off there).
### WI-0.018 — Qt installer script (`tools/install_qt.py`)
**Goal:** aqtinstall-based pinned Qt 6.8.3 install for desktop (`macos`, `gcc_64`, `gcc_arm64`) and Android kits (`android_arm64_v8a`, `android_x86_64`) with the module list of §13.3.1; idempotent; prints `QT_ROOT_DIR`.
**Depends on:** WI-0.006.
**Produces:** `tools/install_qt.py`.
**Interface:** `--target desktop|android`, `--dest <dir>`; prints `QT_ROOT_DIR`; module list of §13.3.1 (`-m` only for add-on modules).
**Steps:** 1. One `aqt install-qt` per kit (host, `gcc_64`/`gcc_arm64`/`macos`, `android_arm64_v8a`, `android_x86_64`); 2. idempotency through `qmake -query QT_VERSION`; 3. the CI cache key hashes this script.
**Acceptance tests:** 1. Given a machine without Qt, When `tools/install_qt.py --target desktop --target android --dest ~/Qt/ovt` runs, Then `qmake -query QT_VERSION` prints `6.8.3` for every kit. 2. Given the installed kits, When the script re-runs, Then it finishes in < 5 s (skips).
**Size:** S.
**Notes / pitfalls:** Passing `qtbase`/`qtdeclarative` to `-m` makes aqt fail ("module not found"); they are implicit.
### WI-0.019 — Qt GUI skeleton target (`ovt_studio`) building on all presets
**Goal:** An `ovt_studio` executable (desktop) / library (Android) showing an empty Qt Quick window titled "OVT Studio", built with `qt_add_executable`, `qt_add_qml_module`, `qt_finalize_executable`; `--self-test` flag exits after first frame (for CI smoke).
**Overlap:** this WI delivers only the minimal buildable `ovt_studio` target with `--self-test`; WI-1.450 extends the same `app/CMakeLists.txt`, `main.cpp` and `Main.qml` (no second target). See chapter 16 §16.3.
**Depends on:** WI-0.018, WI-0.002.
**Produces:** `app/CMakeLists.txt`, `app/src/main.cpp`, `app/qml/Main.qml`, `app/resources/icons/`.
**Interface:** `ovt-studio --self-test`; targets of §13.2.1 (`ovt_studio`).
**Steps:** 1. `qt_add_executable` + `qt_add_qml_module` + `qt_finalize_executable`; 2. `--self-test` exits after the first rendered frame; 3. Android: `libovt_studio.so` through the same target; 4. CI smoke on the three platforms.
**Acceptance tests:** 1. Given Linux CI with `QT_QPA_PLATFORM=offscreen`, When `ovt-studio --self-test` runs, Then it exits 0 in < 5 s. 2. Given a macOS build, When the ad-hoc-signed `OVT Studio.app` is launched, Then the main window appears. 3. Given the Android presets, When the build runs, Then `libovt_studio.so` is produced for both ABIs.
**Size:** M.
**Notes / pitfalls:** `qt_add_qml_module` needs the host `qmlcachegen`; on Android cross builds set `QT_HOST_PATH`. Do not link `ovt_core` statically into the Qt target and into the CLI with different `CMAKE_INTERPROCEDURAL_OPTIMIZATION` values — ODR warnings follow.
### WI-0.030 — CI: lint and build jobs for Linux and macOS
**Goal:** Fill the `ci.yml` skeleton of WI-0.000 with `lint`, `build-linux-x64`, `build-macos-arm64` per §13.7.1 (each build job ends with its test step — the `linux-x64-release` test preset on Linux, `pr-macos` on macOS — and uploads `junit.xml`; `build-macos-arm64` runs on the owner's self-hosted Mac Mini and uploads `ovt-macos-arm64` on pushes to `main`) and the cache strategy of §13.7.2; delete the `skeleton` placeholder job.
**Depends on:** WI-0.007, WI-0.002, WI-0.010–0.015 (as they land, caches cover them).
**Produces:** `.github/workflows/ci.yml` (jobs `lint`, `build-linux-x64`, `build-macos-arm64`), `.github/actions/setup-ovt/action.yml` (composite action implementing the caches of §13.7.2).
**Interface:** §13.7.1 job rows; §13.7.2 cache keys; required checks §13.7.3.
**Steps:** 1. Composite setup action (caches, Qt, vcpkg `files` cache dir; installs `tools/requirements-test.txt` so the `tools_pytest` CTest test of chapter 14 WI-0.550 runs in the test step); 2. the three jobs, each `lint` step skipping with a notice while its tool or configuration is not on the branch (§13.7.1); 3. `concurrency` and SHA-pinned actions; 4. artefact upload.
**Acceptance tests:** 1. Given a PR, When CI runs, Then the three jobs run and pass; 2. Given a warm cache, When `build-linux-x64` runs, Then it finishes in ≤ 40 min.
**Size:** M.
**Notes / pitfalls:** Pin every third-party action by commit SHA; Dependabot keeps them fresh.
### WI-0.031 — CI: sanitizer, clang-tidy and ABI jobs
**Goal:** `asan-ubsan`, `tsan` (nightly), `coverage`, `clang-tidy`, `abi-check` jobs.
**Overlap:** this WI delivers only the `asan-ubsan`, `tsan`, `macos-asan`, `coverage` and `abi-check` jobs; the `clang-tidy` job and `static-analysis.yml` are WI-0.559. See chapter 16 §16.3.
**Depends on:** WI-0.030, WI-0.004.
**Produces:** jobs `asan-ubsan` and `coverage` (`ci.yml`; `OVT_COVERAGE` option and `tools/coverage_gate.py` reading `llvm-cov export -summary-only` JSON, thresholds of chapter 04 NFR-028), `tsan` and `macos-asan` (`nightly.yml`), `abi-check` (`abi.yml`).
**Interface:** §13.7.1 rows; `abidiff --harmless --no-added-syms` on `RelWithDebInfo` builds.
**Steps:** 1. Sanitizer workflows using the `ci-linux-asan-ubsan` / `ci-macos-asan` workflow presets and `ctest --preset sanitizers`; 2. abidiff job downloading the last tag's artefact; 3. `abi-break` label escape hatch; 4. required-check wiring.
**Acceptance tests:** 1. Given a PR introducing UB (`1 << 33` in a test), When `asan-ubsan` runs, Then the job fails naming the test. 2. Given a PR removing an `ovt_*` symbol and lacking the `abi-break` label, When `abi-check` runs, Then it fails with the abidiff report attached. 3. Given a PR that deletes the tests of `core/src/bitreader/`, When `coverage` runs, Then it fails naming `bitreader/` and its percentage (< 90 %).
**Size:** S.
**Notes / pitfalls:** abidiff without DWARF reports nothing useful: both the baseline artefact and the PR build must be `RelWithDebInfo` with unstripped symbols.
### WI-0.032 — CI: licence gate job wiring
**Goal:** Run `tools/license_gate.py` (implemented in chapter 15 WI-0.602) in CI with all inputs (`license_manifest.json`, vcpkg SPDX files, VERSIONS.md, Gradle license report, pip).
**Depends on:** WI-0.030, WI-0.602.
**Produces:** job `license-gate` in `ci.yml`; fixture `tools/tests/fixtures/VERSIONS_bad.md`.
**Interface:** `tools/license_gate.py` command line (chapter 15 §15.4.2); `build/<preset>/license_manifest.json` schema (§13.2.3).
**Steps:** 1. Build the `ovt_license_manifest` target; 2. Gradle licence report step; 3. run the gate with every input; 4. negative test with the bad fixture.
**Acceptance tests:** 1. Given a fake `third_party/VERSIONS.md` row with `AGPL-3.0-only`, When the licence gate job runs, Then it fails and names the row. 2. Given a vcpkg dependency whose licence is absent from `license_policy.toml`, When the gate runs on `build/<preset>/license_manifest.json`, Then it fails with `unknown SPDX id`.
**Size:** S.
**Notes / pitfalls:** The gate must run on the *built* tree (`build/<preset>/license_manifest.json`, §13.2.3), not on `vcpkg.json` alone — a transitive vcpkg dependency can pull in a licence the manifest does not list. Fail the job on an unknown SPDX id; never downgrade to a warning.
### WI-0.033 — CI: fuzz smoke (60 s per target) and nightly fuzz
**Goal:** `fuzz-smoke` on PRs and `fuzz-nightly` per §13.7.1 with crash artefact upload.
**Overlap:** this WI delivers only the nightly fuzz lane and the required-check wiring; the smoke workflow `fuzz-smoke.yml` is produced by WI-0.557. See chapter 16 §16.3.
**Depends on:** WI-0.557 (fuzz harness and smoke workflow), WI-0.030.
**Produces:** job `fuzz-nightly` in `nightly.yml`; `tools/fuzz_corpus_pr.py`.
**Interface:** §13.7.1 fuzz rows; WI-0.557's `fuzz-smoke.yml`.
**Steps:** 1. Nightly lane (20 min per target); 2. `-merge=1` corpus minimisation; 3. auto-PR with new corpus entries; 4. required-check wiring of the smoke job.
**Acceptance tests:** 1. Given a deliberately crashing fuzz target (label `canary`, enabled by env), When the fuzz smoke job runs, Then the job fails and uploads `crash-*` artefacts. 2. Given healthy targets, When the smoke job runs, Then each target runs 60 s and the job finishes in ≤ 15 min.
**Size:** S.
**Notes / pitfalls:** Give each target `-max_total_time=60 -timeout=10` and upload crashers as artefacts; a crash reproducer must be minimised (`-minimize_crash=1`) before it is committed to `core/tests/fuzz/corpus/`. libFuzzer requires the Clang preset.
### WI-0.034 — CI: Android build job
**Goal:** `android-build` job: SDK/NDK/Qt caches, Gradle `assembleFossDebug/Release` both ABIs, artefacts.
**Depends on:** WI-0.050, WI-0.018.
**Produces:** job `android-build` in `ci.yml`.
**Interface:** §13.7.1 row; caches of §13.7.2 (SDK/NDK/Qt/Gradle).
**Steps:** 1. Caches; 2. `tools/bootstrap_android.sh`; 3. `assembleFossDebug`/`assembleFossRelease` for both ABIs, `bundlePlayRelease` on tags; 4. APK/AAB artefacts.
**Acceptance tests:** 1. Given a push to `main`, When the Android job runs, Then it passes and uploads APK artefacts for `arm64-v8a` and `x86_64` of ≤ 120 MB each.
**Size:** M.
**Notes / pitfalls:** Install the NDK through `android-actions/setup-android` pinned to r27c; the Qt Android kit must match the host Qt version exactly or `androiddeployqt` fails late. Emulator tests need the `x86_64` kit and KVM (`ubuntu-latest` has it).
### WI-0.035 — CI: Android emulator smoke
**Goal:** `android-emulator-smoke` per §13.7.1 (API 36 x86_64 image, install, launch, uiautomator assertion, CLI `ovt info`, logcat check, screenshot).
**Depends on:** WI-0.034.
**Produces:** job `android-emulator-smoke` in `ci.yml`; `tools/android/smoke.sh`.
**Interface:** §13.7.1 row (API 36 image).
**Steps:** 1. emulator-runner setup with KVM; 2. install + launch; 3. uiautomator assertion; 4. CLI push and `ovt info`; 5. logcat check and screenshot artefact.
**Acceptance tests:** 1. Given a PR, When the emulator job runs, Then it passes in ≤ 20 min. 2. Given a build whose Activity crashes on start, When the job runs, Then it fails with the logcat excerpt attached.
**Size:** M.
**Notes / pitfalls:** Enable KVM (`sudo chown $USER /dev/kvm`) on GitHub Linux runners; use `-no-window -gpu swiftshader_indirect`; Vulkan is unavailable in the emulator → the app must fall back to GLES (libplacebo `opengl`) without crashing; the HDR path is skipped (emulator reports no HDR).
### WI-0.036 — CI: release workflow and artefact collection
**Goal:** `release.yml` per §13.7.1/§13.8: on tag, collect all artefacts, SHA256SUMS, minisign, notices, Corresponding Source archive, draft release; cask/Flathub automation PRs. This WI owns `tools/release/manifest.toml`, its schema module `tools/release/manifest_schema.py` (§13.8.1; D27) and the manifest's `[licenses]` table, which it writes using the gate module of chapter 15 WI-0.602 (`import license_gate` for the SPDX resolution) — so this WI depends on WI-0.602, not the reverse (D25).
**Depends on:** WI-0.030, WI-0.034, WI-0.060, WI-0.070, WI-0.602 (gate module for `[licenses]` and `--check-licenses`), WI-0.603 (notices generator), WI-0.608 (`tools/release/listing_texts.toml`).
**Produces:** `.github/workflows/release.yml`, `tools/release/{collect,changelog,bump}.py`, `tools/release/manifest_schema.py` (schema of §13.8.1 as a Python module; shared with chapter 15 WI-0.607's `check_patches.py` and the gate's distribution check), `tools/release/manifest.toml` (schema §13.8.1: which components go into which artefact, plus the per-artefact `[licenses]` rows `spdx`/`notices`; `collect.py --check` validates the structure, `collect.py --check-licenses` validates the `[licenses]` rows against `build/<preset>/license_gate_report.json`, `collect.py --check-listings` validates `tools/release/listing_texts.toml` of WI-0.608), `docs/dev/release.md`.
**Interface:** §13.8 release steps; `tools/release/manifest.toml` schema (§13.8.1) and `tools/release/manifest_schema.py` (`validate(path) -> list[str]` of error strings; empty = valid); `collect.py --check | --check-licenses | --check-listings`.
**Steps:** 1. Orchestrator workflow waiting on every build job; 2. `manifest_schema.py` and `collect.py` gathering artefacts per manifest, with `--check`, `--check-licenses` (gate module) and `--check-listings`; 3. `SHA256SUMS` + minisign; 4. notices (`tools/gen_notices.py`, chapter 15) and Corresponding Source archive; 5. draft release, cask/Flathub automation PRs.
**Acceptance tests:** 1. Given tag `v0.1.0-rc.1` on a fork, When the `release` workflow runs, Then a draft pre-release with every artefact type exists and `SHA256SUMS` verifies. 2. Given the Corresponding Source archive, When `tools/third_party_offline.py --from-archive` rebuilds it offline, Then the build succeeds. 3. Given `tools/release/manifest.toml` with a `default_download = true` artefact listing `ovt_plugin_ffmpeg`, When `collect.py --check-licenses` runs, Then it exits 1 naming the artefact (chapter 15 §15.4.2 distribution rule). 4. Given `tools/release/listing_texts.toml` lacking the `flathub` channel, When `collect.py --check-listings` runs, Then it exits 1 naming `flathub`.
**Size:** M.
**Notes / pitfalls:** The Corresponding Source archive must contain every `third_party` download (GPL §6); `collect.py` fails when an archive in `OVT_TP_DOWNLOADS` is missing.
### WI-0.037 — CI: nightly perf job on self-hosted M1
**Goal:** `perf` job per §13.7.1 recording §0.7 budgets; regression detection.
**Overlap:** superseded by WI-0.560 (`perf_run.py`, `perf_compare.py`, `perf-nightly.yml`, runner registration); acceptance tests 1–2 move to WI-0.560 (results are uploaded as artefacts and compared against `tests/perf/baselines/`, not appended to a `perf-results` branch). See chapter 16 §16.3.
**Depends on:** WI-0.030, WI-0.560 (perf scripts; this WI is superseded by WI-0.560).
**Produces:** none (superseded by WI-0.560).
**Interface:** none (superseded).
**Steps:** none here; see WI-0.560.
**Acceptance tests:** none here (superseded): the former tests 1–2 are WI-0.560's (results uploaded as artefacts and compared against `tests/perf/baselines/`; no `perf-results` branch).
**Size:** S.
**Notes / pitfalls:** Kept for numbering stability only.
### WI-0.040 — Corpus access from CMake (`OVT_CORPORA_DIR`, skip helper)
**Goal:** The CMake side of corpus access (reduced, chapter 16 §16.3): the cache variable `OVT_CORPORA_DIR` consumed by tests and the skip-not-fail helper (tests skip with a clear message when a tagged corpus subset is missing). The manifest (`tests/corpora/manifest.json`, shape of chapter 14 §14.2.4) and `tools/fetch_corpora.py` (`--tags`/`--ids`/`--sources` grammar) are chapter 14 WI-0.551's.
**Depends on:** WI-0.006, WI-0.551.
**Produces:** `cmake/OvtCorpora.cmake` (`OVT_CORPORA_DIR`, a CMake helper that reports whether a tagged corpus set is present, skip-not-fail message), `tests/corpora/README.md` (sources and licence notes; corpora are never redistributed in releases). The fetcher `tools/fetch_corpora.py` and the single manifest `tests/corpora/manifest.json` (the `fast`-tagged subset replaces the former `mini` set, ≤ 200 MB) are WI-0.551's (chapter 14 §14.2.4); this WI only consumes them.
**Interface:** CMake cache variable `OVT_CORPORA_DIR` (§13.2.2); the `OvtCorpora.cmake` helper that reports whether the corpus set `<tag>` is present; the skip message text of acceptance test 2.
**Steps:** 1. `OvtCorpora.cmake`; 2. skip-not-fail helper used by `ovt_add_golden_test` (WI-0.556); 3. README.
**Acceptance tests:** 1. Given `-DOVT_CORPORA_DIR=/tmp/c` at configure time, When a test calls `ovt::test::corpus_path("jctvc_AMP_A_Samsung_7")`, Then the returned path is below `/tmp/c` (chapter 14 §14.2.4 cache layout `<cache>/<id>/<file>`). 2. Given an empty `OVT_CORPORA_DIR`, When `ctest -L golden` runs, Then tests skip (not fail) with the message `corpus subset 'fast' missing: run tools/fetch_corpora.py --tags fast`; and with `OVT_REQUIRE_CORPORA=1` the same run fails.
**Size:** M.
**Notes / pitfalls:** ITU-T conformance streams are hosted on `https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/` (zips); the fetcher must support `zip:` member extraction (`"url": "...zip", "member": "..."`). AOM Argon vectors are large (GBs): a separate set, nightly only. **Overlap:** this WI delivers only the CMake side (`cmake/OvtCorpora.cmake`, `OVT_CORPORA_DIR`, skip-not-fail message) and the `tests/corpora/README.md`; WI-0.551 owns `tools/fetch_corpora.py`, the manifest schema and the single `tests/corpora/manifest.json`; `--set mini` becomes `--tags fast`. See chapter 16 §16.3.
### WI-0.041 — Golden test runner and corpus-driven test registration
**Goal:** `ovt_add_golden_test(<name> <command> <golden-file>)` CMake function and the Python runner used by chapters 07/08/12; `--update-goldens` mode.
**Overlap:** superseded by WI-0.556 (`ovt_add_golden_test` and the golden runner); the `--update-goldens` mode is `OVT_UPDATE_GOLDEN=1` of WI-0.550; acceptance tests 1–2 move to WI-0.556. See chapter 16 §16.3.
**Depends on:** WI-0.040.
**Produces:** none (superseded by WI-0.556).
**Interface:** none (superseded).
**Steps:** none here; see WI-0.556.
**Acceptance tests:** none here (superseded): the former tests 1–2 (unified diff on mismatch; no temp-file collision under `ctest -L golden -j8`) are WI-0.556's.
**Size:** S.
**Notes / pitfalls:** Kept for numbering stability only.
### WI-0.050 — Android Gradle project skeleton with Qt embedding
**Goal:** The `android/` project of §13.4.1–§13.4.5 (compileSdk/targetSdk 36, Qt Gradle Plugin 1.2) building an APK that shows the empty Qt Quick window of WI-0.019 inside `QtQuickView` in `MainActivity`, for both ABIs, `foss` and `play` flavours.
**Depends on:** WI-0.019, WI-0.018, WI-0.001 (android presets).
**Produces:** all files of §13.4.1 except `HdrSurfaceView.kt`/`FilePicker.kt` logic (stubs), `android/app/src/main/cpp/CMakeLists.txt`, `android/app/src/main/cpp/ovt_jni.cpp` with `ovt_jni_version()` (JNI block of §13.4.4), `tools/bootstrap_android.sh`.
**Interface:** §13.4.1–§13.4.5; JNI block of §13.4.4; `libs.versions.toml` keys (`compileSdk = "36"`, `qtGradlePlugin = "1.2"`).
**Steps:** 1. Gradle project with version catalog and wrapper; 2. Qt Gradle plugin wiring; 3. `MainActivity` with `QtQuickView`; 4. `play`/`foss` flavours; 5. `src/main/cpp` CMake glue; 6. inputs for WI-0.034.
**Acceptance tests:** 1. Given the Gradle project, When `./gradlew :app:assembleFossDebug` runs, Then two APKs are produced. 2. Given the API 36 emulator, When the APK is installed and launched, Then uiautomator finds a node with text `OVT Studio`. 3. Given the `play` flavour, When `./gradlew :app:bundlePlayRelease` runs, Then an AAB is produced (unsigned ok).
**Size:** L.
**Notes / pitfalls:** The Qt Gradle plugin expects `qtPath`/`qtKitDir` properties and the host Qt (`QT_HOST_PATH`) for `androiddeployqt`; multi-ABI needs one CMake configure per ABI (Gradle does this). `extractNativeLibs=false` requires uncompressed `.so` (default with AGP 8). Set `android.bundle.enableUncompressedNativeLibs` not needed on AGP 8.
### WI-0.051 — SAF file picking → `ovt_file_open_fd`
**Goal:** `FilePicker.kt` + `ovt_jni_open_fd` per §13.4.4 item 3, including `ACTION_VIEW` intents and ChromeOS drag-and-drop; a debug overlay shows `ovt info`-equivalent JSON (via `ovt_container_track_info`) for the picked file.
**Depends on:** WI-0.050, WI-1.101 (`ovt_file_open_fd`).
**Produces:** `android/app/src/main/kotlin/org/openvideotools/studio/FilePicker.kt`, `ovt_jni_open_fd` in `ovt_jni.cpp`, debug overlay `DebugInfoOverlay.kt`, `tools/android/uiautomator/pick_file.py` (drives the SAF picker on the emulator; used by acceptance test 1 and by WI-0.035).
**Interface:** `ovt_file_open_fd` (chapter 12 §12.3.6); JNI block of §13.4.4.
**Steps:** 1. `OpenDocument` contract and MIME list; 2. persistable URI permissions; 3. `detachFd()` → native; 4. `ACTION_VIEW`/`ACTION_SEND` and drag-and-drop; 5. overlay using `ovt_container_track_info`.
**Acceptance tests:** 1. Given `mini.mp4` pushed to `/sdcard/Download` on the emulator, When it is picked through the SAF UI (`tools/android/uiautomator/pick_file.py`), Then logcat shows `OVT: opened fd size=<bytes>` with the correct size. 2. Given a 3 GB sparse file, When it is opened, Then `ovt_file_size` returns the 64-bit size.
**Size:** M.
**Notes / pitfalls:** `ParcelFileDescriptor.detachFd()` transfers ownership: close it only on the native side.
### WI-0.052 — `HdrSurfaceView` sibling and dataspace plumbing
**Goal:** `HdrSurfaceView.kt`, `ovt_jni_surface_attach/detach`, native code setting `ANativeWindow_setBuffersDataSpace` and rendering a **PQ test pattern** (grey ramp with 100/203/1000/4000/10000-nit patches labelled; colour bars in BT.2020) through EGL with `EGL_GL_COLORSPACE_BT2020_PQ_EXT`, under the transparent Qt window; window `colorMode=hdr`; API 34+ `setDesiredHdrHeadroom`.
**Depends on:** WI-0.050 (the test pattern is drawn with plain GLES; the libplacebo path arrives with WI-A.401).
**Produces:** `HdrSurfaceView.kt`, `DisplayCaps.kt`, `android/app/src/main/cpp/hdr_surface.cpp`, `docs/platforms/android-hdr.md`.
**Interface:** `ovt_jni_surface_attach/detach`, `ovt_jni_set_headroom` (§13.4.4); `DisplayTarget` of chapter 10 §10.2.2; logcat line `OVT_HDR dataspace=<BT2020_PQ|BT2020_HLG|SRGB>` (tag `OVT_HDR`) emitted by `ovt_jni_surface_attach` whenever the dataspace is set (§13.4.4; the phase-0 exit demo chapter 16 §16.1.1 #8 greps for it; WI-1.577 keeps the format).
**Steps:** 1. `HdrSurfaceView` sibling under the transparent Qt window; 2. dataspace (field constants) and EGL colourspace; 3. PQ test pattern in plain GLES; 4. headroom API plumbing; 5. `docs/platforms/android-hdr.md`.
**Acceptance tests:** 1. Given the `android-hdr` reference device (chapter 14 §14.9.2, D29; `Display.isHdr == true` — the `pixel-tablet` panel is SDR, D12), When the pattern is shown, Then the 1000-nit patch is visibly brighter than the 203-nit patch and a photometer/eyeball check is recorded in `docs/platforms/android-hdr.md` with device, OS version, and screenshots of `dumpsys SurfaceFlinger` showing the layer dataspace `BT2020_PQ`. 2. Given the emulator (no HDR), When the view is created, Then it falls back to the SRGB dataspace with no crash and logcat shows `OVT_HDR dataspace=SRGB`. 3. Given the `android-hdr` device or the `chromebook-hdr` device, When the pattern is shown, Then `adb logcat -d | grep OVT_HDR` prints `dataspace=BT2020_PQ`.
**Size:** M.
**Notes / pitfalls:** `TextureView` is forbidden (ADR-014). The Qt window must be translucent and `HdrSurfaceView` must not use `setZOrderOnTop(true)` (it would cover the Qt UI). Check `dumpsys SurfaceFlinger --latency` only on debug builds.
### WI-0.053 — Android plugin store and codec pack skeleton
**Goal:** `PluginStore.kt` implementing the search order and W^X copy rule of §13.4.6, `:codecpack` dynamic feature module containing a stub plugin (`libovt_plugin_stub.so`, not shipped), signature check for sibling package.
**Overlap:** this WI is scheduled in phase 2 (milestone MA.2) because it needs the plugin registry (WI-2.150) and `ovt_plugins_*` (WI-2.520); numbering is kept. See chapter 16 §16.3.
**Depends on:** WI-0.050, WI-2.150, WI-2.520 (plugin registry and `ovt_plugins_*` API; scheduled in phase 2, milestone MA.2).
**Produces:** `android/app/src/main/kotlin/org/openvideotools/studio/PluginStore.kt`, `android/codecpack/` module, test plugin `libovt_plugin_stub.so` (test-only).
**Interface:** §13.4.6 rules; `ovt_plugins_add_search_path` via `ovt_jni_add_plugin_path`.
**Steps:** 1. Search order; 2. signature check (`ANA-PLG-0003`); 3. W^X copy rule into `codeCacheDir`; 4. dynamic feature module; 5. emulator tests.
**Acceptance tests:** 1. Given the codec pack APK signed with the same key installed on the emulator, When plugins are enumerated, Then `ovt_plugins_list` contains the stub; Given one signed with a different key, Then diagnostic `ANA-PLG-0003` is emitted and nothing loads. 2. Given a `.so` copied into `filesDir/plugins`, When the "Allow user plugins" toggle is off, Then it does not load; When the toggle is on, Then it loads.
**Size:** M.
**Notes / pitfalls:** `SplitInstallManager` exists only in the `play` flavour; the `foss` flavour discovers the sibling package through `PackageManager`.
### WI-0.054 — ChromeOS adaptations and large-screen manifest settings
**Goal:** §13.4.7 window attributes, `onConfigurationChanged` handling, keyboard/mouse pass-through verification, external display listener.
**Depends on:** WI-0.050, WI-0.052.
**Produces:** manifest attributes of §13.4.5, `MainActivity` configuration-change handling, `DisplayCaps` display listener.
**Interface:** §13.4.7 bullet list.
**Steps:** 1. Manifest `<layout>` and `configChanges`; 2. `onConfigurationChanged`/`onMultiWindowModeChanged` without recreation; 3. input pass-through checks; 4. `DisplayManager.DisplayListener`.
**Acceptance tests:** 1. Given a Chromebook (x86_64), When the window is resized in free-form mode, Then no Activity recreation occurs (`onCreate` count stays 1 in logcat) and the Qt window and `HdrSurfaceView` re-layout. 2. Given a mouse, When the user right-clicks, Then a Qt context menu event is logged. 3. Given the emulator started with `-feature FreeformWindowManagement`, When the window is resized, Then no recreation occurs.
**Size:** S.
**Notes / pitfalls:** Do not call `recreate()`; the Qt window must survive the resize.
### WI-0.055 — ChromeOS/Android HDR hardware spike (week one)
**Goal:** Determine on real hardware whether `SurfaceView` + `BT2020_PQ` dataspace reaches the panel on (a) the `android-hdr` reference device (chapter 14 §14.9.2; the `pixel-tablet` SDR panel is the control device), (b) an HDR-capable Chromebook under ARCVM, (c) an Android TV box over HDMI (optional), and record `Display.getHdrCapabilities`, `hdrSdrRatio`, `dumpsys SurfaceFlinger` layer dataspace, photos/photometer readings, and whether `setDesiredHdrHeadroom` changes brightness.
**Depends on:** WI-0.052.
**Produces:** `docs/platforms/chromeos-hdr.md`, `docs/platforms/android-hdr.md` updated, an entry in chapter 17 (risks) with the decision: HDR on ChromeOS *supported / unsupported-tone-map / deferred*.
**Interface:** chapter 10 §10.4.4 pass/fail criteria; WI-0.052 test pattern.
**Steps:** 1. Device list and OS versions; 2. run the pattern, record `dumpsys SurfaceFlinger`, photos, lux readings; 3. write both documents; 4. record the decision in chapter 17.
**Acceptance tests:** 1. Given the verification document, When reviewed, Then it contains results for ≥ 2 devices and the ADR-014 "unverified" note is replaced by a verified statement with owner sign-off.
**Size:** M.
**Notes / pitfalls:** ARCVM exposes `Display.isHdr` depending on ChromeOS version; test on the current stable channel and note the version. Some panels report HDR but the compositor tone-maps; compare a 203-nit and 1000-nit patch brightness with a lux meter. **Overlap:** primary for the HDR hardware spike (absorbs WI-A.402); the §10.4.4 pass/fail criteria are its Interface. See chapter 16 §16.3.
### WI-0.056 — Android signing configuration, F-Droid metadata and reproducibility check
**Goal:** §13.4.8: Gradle signing from env/secrets, the self-hosted F-Droid repository metadata and the `fdroiddata` recipe template `android/fdroid/org.openvideotools.studio.yml`, `tools/repro_check.sh android` with diffoscope, `SOURCE_DATE_EPOCH` propagation into CMake and Gradle.
**Depends on:** WI-0.050, WI-0.034.
**Produces:** `android/app/signing.gradle.kts`, `android/fdroid/` (self-hosted repo metadata and the `fdroiddata` recipe template `org.openvideotools.studio.yml`), `tools/repro_check.sh`.
**Interface:** §13.4.8; secrets `ANDROID_KEYSTORE_*`.
**Steps:** 1. Signing config from env/secrets; 2. `SOURCE_DATE_EPOCH` into CMake and Gradle; 3. repo metadata and recipe template; 4. diffoscope comparison script.
**Acceptance tests:** 1. Given two consecutive `assembleFossRelease` builds in different directories, When compared with `diffoscope`, Then the APKs are identical except the signature block. 2. Given two unsigned (`--no-sign`) builds, When compared, Then they are byte-identical.
**Size:** M.
**Notes / pitfalls:** Reproducibility killers: build paths in `.so` (`-ffile-prefix-map`), Gradle build timestamps in `META-INF`, `BuildConfig` timestamps (disable), ordering of `androiddeployqt` output (sort), zip entry mtimes (`SOURCE_DATE_EPOCH`), QML cache files (`qmlcachegen` is deterministic given the same Qt build).
### WI-0.060 — macOS bundle script and `macdeployqt` integration
**Goal:** `tools/mac/bundle.sh`, `fix_rpaths.py`, `strip.sh`, `Info.plist.in` with document types, producing a runnable `OVT Studio.app` with `ovt` CLI inside, for `macos-arm64-release` and `macos-x86_64-release`.
**Depends on:** WI-0.019, WI-0.012 (ffmpeg core flavour dylibs), WI-0.017 (MoltenVK).
**Produces:** `tools/mac/bundle.sh`, `tools/mac/fix_rpaths.py`, `tools/mac/strip.sh`, `app/resources/mac/Info.plist.in`.
**Interface:** §13.5.2 bundle layout.
**Steps:** 1. Staging `cmake --install`; 2. `macdeployqt` invocation; 3. rpath verification; 4. strip with dSYM archive.
**Acceptance tests:** 1. Given the staged bundle, When `otool -L` runs on every Mach-O, Then only `@rpath`, `@executable_path`, `/usr/lib`, `/System` references appear (`fix_rpaths.py` exit 0). 2. Given a clean macOS 13 VM, When the bundle is copied there and launched, Then it starts (manual, recorded).
**Size:** M.
**Notes / pitfalls:** `macdeployqt` misses dylibs loaded only via `dlopen` (plugins): copy them explicitly and fix their rpaths.
### WI-0.061 — macOS signing, entitlements, notarisation and DMG
**Goal:** `tools/mac/sign_and_notarize.sh` (Mach-O enumeration, CLI signed with the app's entitlements, app notarised and stapled before the DMG is built — §13.5.4), `entitlements.plist` (§13.5.3), `make_dmg.sh`, `list_macho.py`; CI tag jobs sign+notarise; PR jobs ad-hoc sign with the same entitlements.
**Depends on:** WI-0.060, WI-0.030.
**Produces:** `tools/mac/sign_and_notarize.sh`, `tools/mac/list_macho.py`, `tools/mac/entitlements.plist`, `tools/mac/make_dmg.sh`.
**Interface:** §13.5.3–§13.5.4.
**Steps:** 1. Mach-O enumeration and inside-out signing; 2. CLI and app signed with the entitlements; 3. notarize + staple the app; 4. DMG build/sign/notarize/staple; 5. CI secrets import; 6. negative entitlement test.
**Acceptance tests:** 1. Given a tag build, When `spctl --assess` and `stapler validate` run on the DMG, Then both pass. 2. Given an ad-hoc signed PR build with hardened runtime, When `ovt plugins --check` loads a test plugin built out-of-tree and unsigned, Then it loads (exit 0) — proving the `disable-library-validation` decision works. 3. Given the entitlement removed (negative test in CI), When the same plugin is loaded, Then the load fails with the expected `dlopen` error — proving the test is meaningful.
**Size:** M.
**Notes / pitfalls:** Sign inside-out; `--deep` is deprecated for signing (only for verify). Frameworks need `--options runtime` too or notarisation flags them. Staple the `.app` *before* packaging it (Apple TN3147), then sign/notarise/staple the DMG; stapling requires network.
### WI-0.062 — Homebrew tap: cask, codec-pack cask and formula
**Goal:** `openvideotools/homebrew-tap` repository with `Casks/ovt-studio.rb` (§13.5.5), `Casks/ovt-codec-pack.rb`, `Formula/ovt.rb`; automation `tools/release/bump_cask.py` creating the PR on release.
**Depends on:** WI-0.061, WI-0.036.
**Produces:** `tools/release/bump_cask.py`; the cask and formula files (`ovt-studio.rb`, `ovt-codec-pack.rb` under the tap's cask directory, `ovt.rb` under its formula directory) live in the separate `openvideotools/homebrew-tap` repository, not in this one.
**Interface:** §13.5.5.
**Steps:** 1. Cask; 2. codec-pack cask; 3. source formula with `OVT_SYSTEM_FFMPEG=ON`; 4. bump automation PR.
**Acceptance tests:** 1. Given the tap, When `brew install --cask openvideotools/tap/ovt-studio` runs, Then `ovt --version` works. 2. Given the cask, When `brew audit --cask --online ovt-studio` runs, Then it passes. 3. Given the formula, When `brew install openvideotools/tap/ovt` runs, Then it builds from source with `OVT_SYSTEM_FFMPEG=ON`.
**Size:** S.
**Notes / pitfalls:** `brew audit --online` requires the GitHub release to exist before the cask PR.
### WI-0.063 — macOS universal2 merge script (MAY)
**Goal:** `tools/mac/make_universal.py` lipo-merging two complete bundles; `OVT_UNIVERSAL=ON` CI job on tags producing `-universal.dmg`.
**Depends on:** WI-0.060.
**Produces:** `tools/mac/make_universal.py`.
**Interface:** `OVT_UNIVERSAL` (§13.2.2).
**Steps:** 1. `lipo -create` every Mach-O pair; 2. re-sign the merged bundle (WI-0.061 script); 3. optional tag job.
**Acceptance tests:** 1. Given two per-architecture bundles from the same commit, When `make_universal.py` runs, Then `lipo -archs` on every Mach-O reports `x86_64 arm64` and the universal app launches on both architectures.
**Size:** S.
**Notes / pitfalls:** Both input bundles must come from the same commit and Qt version, or resources differ.
### WI-0.070 — Linux AppImage script
**Goal:** `tools/linux/appimage.sh` per §13.6.2 on the Ubuntu 22.04 CI image; zsync update info; `AppRun` env.
**Depends on:** WI-0.019, WI-0.012.
**Produces:** `tools/linux/appimage.sh`, `packaging/appimage/AppRun`.
**Interface:** §13.6.2.
**Steps:** 1. `cmake --install` into `AppDir`; 2. `linuxdeploy` with the Qt plugin; 3. `AppRun` environment; 4. zsync update info; 5. container tests on three distros.
**Acceptance tests:** 1. Given Ubuntu 22.04, Fedora (latest) and Arch containers with X11 (`xvfb-run`), When `./OVT_Studio.AppImage --self-test` runs, Then it exits 0 on each. 2. Given the running AppImage, When `ovt plugins` runs inside it, Then `OVT_PLUGIN_PATH` includes `$APPDIR/usr/lib/ovt/plugins` and `aom` is listed.
**Size:** M.
**Notes / pitfalls:** `libvulkan.so.1` must stay excluded so host ICDs load.
### WI-0.071 — Flatpak manifest and Flathub preparation
**Goal:** `flatpak/org.openvideotools.Studio.yml` per §13.6.1 (no vcpkg: `OVT_USE_VCPKG=OFF`, per-dependency modules), `tools/gen_flatpak_sources.py` (module files from VERSIONS.md and the vcpkg baseline), the `Codecs` extension manifest, AppStream metainfo (`app/resources/org.openvideotools.Studio.metainfo.xml`) and desktop file.
**Depends on:** WI-0.070 (shares install logic), WI-0.010.
**Produces:** `flatpak/org.openvideotools.Studio.yml`, `flatpak/org.openvideotools.Studio.Codecs.yml`, `flatpak/modules/*.json`, `tools/gen_flatpak_sources.py`, `app/resources/org.openvideotools.Studio.metainfo.xml`, `app/resources/org.openvideotools.Studio.desktop`.
**Interface:** §13.6.1; `OVT_USE_VCPKG=OFF`.
**Steps:** 1. Per-dependency module files generated from the pins; 2. main module; 3. `Codecs` extension; 4. metainfo/desktop; 5. offline `flatpak-builder` test.
**Acceptance tests:** 1. Given sources fetched with `--download-only`, When `flatpak-builder --user --install build-dir flatpak/org.openvideotools.Studio.yml` runs offline, Then it succeeds. 2. Given the installed Flatpak, When `flatpak run org.openvideotools.Studio --self-test` runs, Then it exits 0 and `appstreamcli validate` passes on the metainfo.
**Size:** M.
**Notes / pitfalls:** Flathub rejects `extra-data` for libraries and any network use; every tarball needs a `sha256`.
### WI-0.072 — Distro packaging skeletons (Debian, Fedora/RPM Fusion, Arch)
**Goal:** `packaging/debian/` (control, rules using `linux-x64-distro` preset, binary package split §13.6.3), `packaging/rpm/ovt.spec` + `ovt-codecs-freeworld.spec`, `packaging/arch/PKGBUILD`; documentation `docs/dev/packaging.md`.
**Depends on:** WI-0.008 (install tree), WI-0.012.
**Produces:** `packaging/debian/*`, `packaging/rpm/ovt.spec`, `packaging/rpm/ovt-codecs-freeworld.spec`, `packaging/arch/PKGBUILD`, `docs/dev/packaging.md`.
**Interface:** §13.6.3; `linux-x64-distro` preset.
**Steps:** 1. Debian binary split; 2. RPM specs; 3. PKGBUILD; 4. container builds in CI (`nightly.yml`).
**Acceptance tests:** 1. Given an Ubuntu 22.04 container, When `dpkg-buildpackage -us -uc` runs, Then the listed `.deb`s are produced and `lintian` has no errors. 2. Given a Fedora container, When `rpmbuild -ba` runs, Then `ovt` is produced without AVC/HEVC (`ovt plugins` lacks `ffmpeg`); the freeworld spec builds the plugin against RPM Fusion FFmpeg (mock, documented, not in CI).
**Size:** M.
**Notes / pitfalls:** Debian's static libaom convenience copy must be declared in `debian/README.source`.
### WI-0.073 — Linux runtime dependency checker and Vulkan CI setup
**Goal:** `tools/linux/check_runtime_deps.sh`, CI installation of `mesa-vulkan-drivers` (lavapipe) so render probe tests run headless; `ovt doctor` integration point.
**Depends on:** WI-0.015.
**Produces:** `tools/linux/check_runtime_deps.sh`; CI step installing `mesa-vulkan-drivers` in the composite setup action.
**Interface:** §13.6.4 runtime list; `ovt doctor` (chapter 12 §12.2.16) reports the same probes.
**Steps:** 1. Script listing/probing the libraries; 2. CI step; 3. wire `render/tests/test_placebo_probe` to require lavapipe on CI.
**Acceptance tests:** 1. Given the CI image after the composite setup action installed `mesa-vulkan-drivers`, When `vulkaninfo --summary` runs, Then it lists `llvmpipe` and `render/tests/test_placebo_probe` passes. 2. Given a release binary, When the dependency checker runs, Then every `NEEDED` entry is on the baseline whitelist (§13.6.2) or the check fails naming it.
**Size:** S.
**Notes / pitfalls:** lavapipe (`llvmpipe`) is a CPU Vulkan device: render probe tests pass on it but GPU timers and overlay budgets are meaningless there (perf runs on the §14.9.2 hardware); keep the checker a POSIX `sh` script with no `ldd` dependency on the AppImage path (`ldd` is missing in some minimal images — use `objdump -p` fallback).

---

### Glossary additions (chapter 13)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **Meson cross file** — an INI file telling Meson which compilers and flags to use for a target platform.

**Notes / pitfalls:** Mesa `lavapipe` is a CPU Vulkan ICD and is enough for render golden tests on CI; set `VK_ICD_FILENAMES` explicitly so a stray host ICD is never used. The dependency checker whitelists glibc/libstdc++ only (static libstdc++ is the baseline, §13.6.2).
