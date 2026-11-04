# Third-party dependency pins

Columns: Name | Version / commit | Licence (SPDX) | URL | Patches (relative paths) | SHA-256 of archive | Build path | Notes

| Name | Version | Licence | URL | Patches | SHA-256 | Build | Notes |
|---|---|---|---|---|---|---|---|
| vcpkg baseline | `9eec002df1cd109f8f104cd966a763daf9751489` | MIT (tool) | https://github.com/microsoft/vcpkg | – | – | manifest | bump via `vcpkg x-update-baseline` |
| FFmpeg | n7.1.1 | LGPL-2.1-or-later (core flavour); LGPL-2.1-or-later or GPL-2.0-or-later (plugin flavour, see OVT_FFMPEG_GPL) | https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz | – | `<sha>` | third_party/ffmpeg.cmake | two flavours §13.3.3; trademark notice required |
| libaom | 3.12.1 | BSD-2-Clause AND LicenseRef-AOM-Patent-License-1.0 | https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz | – | `9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e` | third_party/libaom.cmake | CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1; `PATENTS sha256=661fb8e504744e95587b556b94a58343448300606a41bea8c7a9b97125696e61` (the archive's `PATENTS` file; chapter 15 WI-0.600 compares `LICENSES/LicenseRef-AOM-Patent-License-1.0.txt` against it) |
| dav1d | 1.5.1 | BSD-2-Clause | https://code.videolan.org/videolan/dav1d/-/archive/1.5.1/dav1d-1.5.1.tar.bz2 | – | `<sha>` | third_party/dav1d.cmake (Meson) | playback only |
| libde265 | 1.0.15 | LGPL-3.0-or-later | https://github.com/strukturag/libde265/releases/download/v1.0.15/libde265-1.0.15.tar.gz | plugins/ovt-plugin-de265/patches/0001-de265-internals-api.patch | `00251986c29d34d3af7117ed05874950c875dd9292d016be29d3b3762666511d` | third_party/libde265.cmake | Upstream-Status: Submitted <PR url> |
| HM | HM-18.0 (`<commit>`) | BSD-3-Clause | https://vcgit.hhi.fraunhofer.de/jvet/HM | third_party/patches/hm/0001-cmake-static-decoder-only.patch | – (git) | third_party/hm.cmake | optional oracle |
| libvmaf | 3.0.0 (any 3.x; D5) | BSD-2-Clause-Patent | https://github.com/Netflix/vmaf/archive/refs/tags/v3.0.0.tar.gz | – | `<sha>` | third_party/libvmaf.cmake (Meson) | built-in models `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1` (newer bundled models when present) |
| libplacebo | 7.349.0 | LGPL-2.1-or-later | https://code.videolan.org/videolan/libplacebo/-/archive/v7.349.0/libplacebo-v7.349.0.tar.bz2 | – | `<sha>` | third_party/libplacebo.cmake (Meson) | glslang, lcms |
| MoltenVK | 1.2.11 | Apache-2.0 | https://github.com/KhronosGroup/MoltenVK/releases/download/v1.2.11-artifacts/MoltenVK-macos.tar | – | `6b984a8ea17028dcd1485f69465f7c3220ce8f56fbbd53b3dfb34c177af46ab8` | third_party/moltenvk.cmake | macOS only, prebuilt; archives sit on the `v<version>-artifacts` tag (chapter 13 §13.3.7) |
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
