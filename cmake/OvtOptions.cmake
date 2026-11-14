# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Every build option of chapter 13 §13.2.2, with its default, plus the dependency validation between
# options. Options are only declared here; the files that act on them arrive with their work items.

include_guard(GLOBAL)

# ---- Components -----------------------------------------------------------------------------------
option(OVT_BUILD_GUI "Build ovt_studio (OFF when Qt is not found and not Android)" ON)
option(OVT_BUILD_CLI "Build the ovt command-line tool" ON)
option(OVT_BUILD_PYTHON "Build the ovt_py module (needs Python dev + pybind11)" OFF)
option(OVT_BUILD_RENDER "Build ovt_render (OFF for a core-only server build)" ON)
option(OVT_BUILD_PLUGIN_AOM "Build the AV1 analysis plugin (libaom)" ON)
option(OVT_BUILD_PLUGIN_DE265 "Build the HEVC analysis plugin (libde265)" ON)
option(OVT_BUILD_PLUGIN_FFMPEG "Build the AVC playback/analysis plugin (FFmpeg)" ON)
option(OVT_BUILD_PLUGIN_HM "Build the HM HEVC oracle plugin (slow; ON in nightly CI)" OFF)
option(OVT_BUILD_TESTS "Build the Catch2 tests" ON)
option(OVT_BUILD_DOCS "Build Doxygen + Sphinx documentation" OFF)
option(OVT_BUILD_GUI_PYTHON "Embedded Python console in the GUI (chapter 12 §12.5)" OFF)
option(OVT_BUILD_AOM_INSPECT "Also build libaom's examples/inspect oracle binary" OFF)
option(OVT_BUILD_METRIC_XPSNR "Build the XPSNR metric plugin" ON)
option(OVT_ANDROID_CODEC_PACK "Build the split Android codec pack module (§13.4.6)" OFF)
option(OVT_INSTALL_DEV "Install headers, ovt-config.cmake and .pc files" ON)

# ---- Quality tooling ------------------------------------------------------------------------------
option(OVT_ENABLE_SANITIZERS "Enable the sanitizer set selected by OVT_SANITIZER" OFF)
set(OVT_SANITIZER ""
    CACHE STRING
          "Sanitizer set applied when OVT_ENABLE_SANITIZERS=ON (empty: fuzz with OVT_ENABLE_FUZZ, else asan-ubsan)")
set_property(CACHE OVT_SANITIZER PROPERTY STRINGS asan-ubsan tsan fuzz)
option(OVT_ENABLE_FUZZ "Build the libFuzzer targets (Clang only)" OFF)
set(OVT_FUZZ_ENGINE "-fsanitize=fuzzer" CACHE STRING "Link flags of the fuzz engine for ovt_fuzz_* targets")
option(OVT_COVERAGE "Clang source-based coverage on OVT targets" OFF)

# OVT_ENABLE_LTO: "ON for Release presets". Without a preset, follow the build type.
if(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  set(_ovt_lto_default ON)
else()
  set(_ovt_lto_default OFF)
endif()
option(OVT_ENABLE_LTO "Interprocedural optimisation (CMAKE_INTERPROCEDURAL_OPTIMIZATION)" ${_ovt_lto_default})
unset(_ovt_lto_default)
option(OVT_WERROR "Treat warnings as errors (ON in CI presets, OFF for developer presets)" OFF)

# ---- Features -------------------------------------------------------------------------------------
if(ANDROID)
  set(_ovt_desktop_default OFF)
else()
  set(_ovt_desktop_default ON)
endif()
option(OVT_ENABLE_UVQ "UVQ metric via ONNX Runtime (ON desktop, OFF Android)" ${_ovt_desktop_default})
option(OVT_CLI_WITH_RENDER_PROBE "ovt doctor GPU probe (ON desktop, OFF Android)" ${_ovt_desktop_default})
unset(_ovt_desktop_default)
option(OVT_ENABLE_HW_DECODE "VideoToolbox / MediaCodec / VA-API hwaccel in the FFmpeg core flavour" ON)
# Fuzz builds only (core/tests/fuzz/oss-fuzz/build.sh): OSS-Fuzz runs each binary from $OUT with no shared libraries
# beside it, so the core FFmpeg flavour is linked statically there. Never in a distributed build: the shipped core
# flavour stays a shared LGPL library (chapter 13 §13.3.3, chapter 15). SPEC-ASSUMPTION(#406): not in the
# chapter 13 §13.2.2 option table yet.
option(OVT_FFMPEG_CORE_STATIC "Build the core FFmpeg flavour as static archives (fuzz builds only)" OFF)
mark_as_advanced(OVT_FFMPEG_CORE_STATIC)
option(OVT_UNIVERSAL "macOS: lipo arm64+x86_64 artefacts (§13.5.1)" OFF)

# ---- Licensing-relevant options (chapter 15 §15.4) ------------------------------------------------
option(OVT_FFMPEG_GPL "Configure FFmpeg with --enable-gpl" OFF)
option(OVT_FFMPEG_GPL_PLUGIN_ONLY "With OVT_FFMPEG_GPL=ON: apply --enable-gpl to the plugin flavour only" OFF)
option(
  OVT_FFMPEG_GPL_CORE_ACK
  "Acknowledge that a GPL FFmpeg may be linked into ovt_core (whole build becomes GPL-3.0-or-later; never in official builds)"
  OFF)
# ON by default (owner decision of 2026-11-12, ADR-015 / chapter 13 §13.3.3): the core FFmpeg flavour keeps the
# h264/hevc decoders, their hwaccels and the other pool-covered decoders. OFF produces the patent-pool-free split
# build (Fedora-style), where AVC/HEVC come only from the runtime-loaded plugins.
option(
  OVT_PATENT_CODECS_IN_CORE
  "Keep the h264/hevc and other pool-covered decoders in the core FFmpeg flavour (OFF: patent-pool-free split build)"
  ON)

# ---- Dependencies ---------------------------------------------------------------------------------
foreach(_dep FFMPEG LIBAOM DAV1D LIBPLACEBO LIBVMAF ZIMG DE265)
  option(OVT_SYSTEM_${_dep} "Use the system ${_dep} instead of the third_party/ build" OFF)
endforeach()
unset(_dep)
option(OVT_USE_VCPKG "Use the vcpkg toolchain; OFF = plain find_package/pkg-config" ON)
option(OVT_ONNXRUNTIME_PREBUILT "Use the pinned prebuilt ONNX Runtime archive instead of the vcpkg port" OFF)
# The presets' binaryDir is build/<preset>, so the binary directory's name stands in for the preset name.
get_filename_component(OVT_PRESET_NAME "${CMAKE_BINARY_DIR}" NAME)
set(OVT_THIRD_PARTY_PREFIX "${CMAKE_SOURCE_DIR}/build/third_party/${OVT_PRESET_NAME}"
    CACHE PATH "Install prefix of the third_party/*.cmake builds (set per preset)")
set(OVT_TP_DOWNLOADS "${CMAKE_SOURCE_DIR}/build/third_party/downloads"
    CACHE PATH "Archive download cache for third_party/*.cmake")
set(OVT_CORPORA_DIR "${CMAKE_SOURCE_DIR}/build/corpora"
    CACHE PATH "Where tools/fetch_corpora.py put the corpora; tests skip when a set is missing")

# ---- Install and branding -------------------------------------------------------------------------
include(GNUInstallDirs)
set(OVT_PLUGIN_DIR "${CMAKE_INSTALL_LIBDIR}/ovt/plugins"
    CACHE STRING
          "Install-time plugin directory (relative to the prefix), compiled into ovt_core as a default search path")
set(OVT_PRODUCT_NAME "OVT Studio" CACHE STRING "Product name compiled into the GUI/CLI")
set(OVT_CLI_NAME "ovt" CACHE STRING "CLI name compiled into the GUI/CLI")
set(OVT_VERSION_SUFFIX "" CACHE STRING "Appended to the version (-dev, -rc1)")
set(OVT_GIT_SHA "" CACHE STRING "Git sha to embed when the source tree has no .git (release tarballs)")

# ---- Validation -----------------------------------------------------------------------------------
# An LGPL ovt_core must never link a GPL FFmpeg (chapter 13 §13.2.2, chapter 15 §15.4).
if(OVT_FFMPEG_GPL AND NOT OVT_FFMPEG_GPL_PLUGIN_ONLY AND NOT OVT_FFMPEG_GPL_CORE_ACK)
  message(
    FATAL_ERROR
      "OVT_FFMPEG_GPL=ON would link a GPL FFmpeg into the LGPL-2.1-or-later ovt_core. "
      "Set OVT_FFMPEG_GPL_PLUGIN_ONLY=ON to apply --enable-gpl to the plugin flavour only (intended use), "
      "or OVT_FFMPEG_GPL_CORE_ACK=ON to accept a GPL-3.0-or-later core (never in official builds).")
endif()
if(OVT_FFMPEG_GPL_PLUGIN_ONLY AND NOT OVT_FFMPEG_GPL)
  message(WARNING "OVT_FFMPEG_GPL_PLUGIN_ONLY=ON has no effect without OVT_FFMPEG_GPL=ON.")
endif()
if(OVT_FFMPEG_GPL_CORE_ACK AND NOT OVT_FFMPEG_GPL)
  message(WARNING "OVT_FFMPEG_GPL_CORE_ACK=ON has no effect without OVT_FFMPEG_GPL=ON.")
endif()

# Licence of ovt_core as recorded in the licence manifest.
if(OVT_FFMPEG_GPL AND OVT_FFMPEG_GPL_CORE_ACK AND NOT OVT_FFMPEG_GPL_PLUGIN_ONLY)
  set(OVT_CORE_LICENSE "GPL-3.0-or-later")
else()
  set(OVT_CORE_LICENSE "LGPL-2.1-or-later")
endif()

# Empty means "pick the set that fits": libFuzzer targets need the fuzz set (coverage instrumentation,
# -fsanitize=fuzzer-no-link), so a fuzz build that names no set gets it (WI-0.557). The preset
# linux-x64-fuzz relies on this. A normal variable, so the cache keeps the user's empty choice.
# SPEC-ASSUMPTION(#106): the spec's preset names no set.
if(OVT_SANITIZER STREQUAL "")
  if(OVT_ENABLE_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(OVT_SANITIZER "fuzz")
  else()
    set(OVT_SANITIZER "asan-ubsan")
  endif()
endif()
if(NOT OVT_SANITIZER MATCHES "^(asan-ubsan|tsan|fuzz)$")
  message(FATAL_ERROR "OVT_SANITIZER must be one of asan-ubsan, tsan, fuzz (got '${OVT_SANITIZER}').")
endif()
if(OVT_ENABLE_FUZZ AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "OVT_ENABLE_FUZZ=ON needs Clang (libFuzzer); no fuzz targets will be created with "
                  "${CMAKE_CXX_COMPILER_ID}.")
endif()
if(OVT_COVERAGE AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(WARNING "OVT_COVERAGE=ON uses Clang source-based coverage; ignored with ${CMAKE_CXX_COMPILER_ID}.")
endif()
if(OVT_UNIVERSAL AND NOT APPLE)
  message(WARNING "OVT_UNIVERSAL=ON only applies to macOS; ignored.")
endif()
if(OVT_BUILD_GUI_PYTHON AND NOT OVT_BUILD_GUI)
  message(WARNING "OVT_BUILD_GUI_PYTHON=ON has no effect without OVT_BUILD_GUI=ON.")
endif()

# OVT_BUILD_GUI defaults to ON but turns OFF (with a warning, not an error) when Qt 6 is not found on a
# desktop host. Android always builds the GUI shell.
if(OVT_BUILD_GUI AND NOT ANDROID)
  # Qt 6.8's FindWrapOpenGL.cmake links the AGL framework on Apple, and Xcode 26's SDK no longer has it
  # (`ld: framework 'AGL' not found`, found on the Mac Mini for WI-0.019). find_library() can't decide it:
  # the host's /System/Library/Frameworks still has AGL while ld resolves frameworks in the SDK. So test
  # the SDK and, without AGL, cache WrapOpenGL_AGL (which Qt takes as is) as the SDK's OpenGL.framework,
  # already linked. FORCE replaces a host path an earlier configure cached. This has to run before the
  # first find_package(Qt6). Remove it once the Qt pin carries the upstream fix.
  if(APPLE)
    set(_ovt_sdk "${CMAKE_OSX_SYSROOT}")
    if(NOT _ovt_sdk)
      execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE _ovt_sdk OUTPUT_STRIP_TRAILING_WHITESPACE
                                                                             ERROR_QUIET)
    endif()
    if(_ovt_sdk AND NOT EXISTS "${_ovt_sdk}/System/Library/Frameworks/AGL.framework")
      set(WrapOpenGL_AGL "${_ovt_sdk}/System/Library/Frameworks/OpenGL.framework"
          CACHE FILEPATH "AGL stand-in (the SDK has no AGL)" FORCE)
    endif()
    unset(_ovt_sdk)
  endif()
  find_package(Qt6 QUIET COMPONENTS Core Quick)
  if(NOT Qt6_FOUND)
    message(WARNING "Qt 6 not found (set QT_ROOT_DIR or CMAKE_PREFIX_PATH); building with OVT_BUILD_GUI=OFF.")
    # A normal variable shadows the cache entry, so a later configure with Qt available turns the GUI back on.
    set(OVT_BUILD_GUI OFF)
  endif()
endif()
