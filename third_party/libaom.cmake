# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libaom, the AV1 analysis decoder, built with inspection and symbol accounting (chapter 13 §13.3.4,
# chapter 07 §7.6.1, ADR-008, WI-0.013). Produces the imported target tp::aom: static libaom.a with the
# public headers under include/aom/. The pin (version, URL, SHA-256) is the libaom row of third_party/VERSIONS.md.
#
# Flags: decoder only, CONFIG_INSPECTION=1 and CONFIG_ACCOUNTING=1 (inspection needs accounting for the per-symbol
# bit costs), multithreaded, PIC, no tests/docs/tools/WebM, and CONFIG_SVT_AV1=0 so that no BSD-3-Clause-Clear code
# (libaom's third_party/SVT-AV1) is built in. OVT_BUILD_AOM_INSPECT=ON also builds libaom's
# examples (-DENABLE_EXAMPLES=1) and installs the `inspect` oracle into <prefix>/bin (chapter 07 §7.7).
#
# OVT_AOM_CPU is libaom's AOM_TARGET_CPU: arm64 or x86_64 from the Android ABI, the single macOS architecture or
# the target processor; `generic` (plain C) for anything else. Set it on the command line to override.
# Android uses the NDK toolchain file forwarded by OVT_TP_CMAKE_TOOLCHAIN_ARGS, never libaom's own
# build/cmake/toolchains/*-android.cmake.
#
# Patches: every third_party/patches/libaom/NNNN-*.patch, applied once with `git apply --check` first and recorded
# in <prefix>/share/ovt/patches.json. There are none yet; the inspection extension and snapshot patches of
# chapter 07 §7.6.1 arrive with WI-2.154 and later, each with its `Upstream-Status:` header (chapter 15 §15.5).
#
# Licence: BSD-2-Clause plus the AOM Patent License 1.0. The archive's LICENSE and PATENTS files are kept with
# the build (chapter 15 WI-0.600 compares PATENTS with LICENSES/LicenseRef-AOM-Patent-License-1.0.txt).

include_guard(GLOBAL)
include(OvtThirdPartyCommon)

# ---- AOM_TARGET_CPU ---------------------------------------------------------------------------------
if(NOT DEFINED OVT_AOM_CPU OR OVT_AOM_CPU STREQUAL "")
  if(ANDROID_ABI)
    set(_ovt_aom_arch "${ANDROID_ABI}")
  elseif(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _ovt_aom_narch)
    if(_ovt_aom_narch GREATER 1)
      message(
        FATAL_ERROR
          "third_party/libaom.cmake: libaom builds one architecture at a time, but "
          "CMAKE_OSX_ARCHITECTURES is '${CMAKE_OSX_ARCHITECTURES}'. Build each slice with its "
          "own preset and lipo the results (§13.5.1).")
    endif()
    unset(_ovt_aom_narch)
    set(_ovt_aom_arch "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(_ovt_aom_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  string(TOLOWER "${_ovt_aom_arch}" _ovt_aom_arch)
  if(_ovt_aom_arch MATCHES "^(arm64|arm64-v8a|aarch64)$")
    set(_ovt_aom_cpu arm64)
  elseif(_ovt_aom_arch MATCHES "^(x86_64|amd64|x64)$")
    set(_ovt_aom_cpu x86_64)
  else()
    set(_ovt_aom_cpu generic)
  endif()
  set(OVT_AOM_CPU "${_ovt_aom_cpu}" CACHE STRING "libaom AOM_TARGET_CPU (arm64, x86_64 or generic)" FORCE)
  unset(_ovt_aom_arch)
  unset(_ovt_aom_cpu)
endif()
message(STATUS "OVT: libaom AOM_TARGET_CPU=${OVT_AOM_CPU}")

# ---- build ------------------------------------------------------------------------------------------
ovt_tp_declare(libaom LICENSE_FILES LICENSE PATENTS)

set(_ovt_aom_prefix "${OVT_THIRD_PARTY_PREFIX}/libaom")
set(_ovt_aom_patch_dir "${CMAKE_CURRENT_LIST_DIR}/patches/libaom")
cmake_path(NORMAL_PATH _ovt_aom_patch_dir)
ovt_tp_apply_patches(_ovt_aom_patch DIR "${_ovt_aom_patch_dir}" INSTALL_DIR "${_ovt_aom_prefix}")
# A new or changed patch re-extracts the pristine tarball, so the patch step never meets a patched tree.
file(GLOB _ovt_aom_patch_files CONFIGURE_DEPENDS "${_ovt_aom_patch_dir}/*.patch")

if(OVT_BUILD_AOM_INSPECT)
  set(_ovt_aom_examples 1)
else()
  set(_ovt_aom_examples 0)
endif()

externalproject_add(
  tp_libaom
  URL ${OVT_TP_libaom_URL}
  URL_HASH SHA256=${OVT_TP_libaom_SHA256}
  DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
  DOWNLOAD_EXTRACT_TIMESTAMP OFF
  PREFIX ${_ovt_aom_prefix}
  INSTALL_DIR ${_ovt_aom_prefix}
  PATCH_COMMAND ${_ovt_aom_patch}
  CMAKE_ARGS ${OVT_TP_CMAKE_TOOLCHAIN_ARGS}
             -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
             -DCMAKE_INSTALL_LIBDIR=lib
             -DCMAKE_INSTALL_INCLUDEDIR=include
             -DBUILD_SHARED_LIBS=OFF
             -DCONFIG_INSPECTION=1
             -DCONFIG_ACCOUNTING=1
             -DCONFIG_AV1_DECODER=1
             -DCONFIG_AV1_ENCODER=0
             -DCONFIG_MULTITHREAD=1
             -DCONFIG_PIC=1
             -DCONFIG_WEBM_IO=0
             # Drops the SVT-AV1 convolution code (third_party/SVT-AV1, BSD-3-Clause-Clear), which is not in
             # tools/license_policy.toml; libaom's own aom_config_defaults.cmake names this switch for that purpose.
             -DCONFIG_SVT_AV1=0
             -DENABLE_EXAMPLES=${_ovt_aom_examples}
             -DENABLE_TESTS=0
             -DENABLE_DOCS=0
             -DENABLE_TOOLS=0
             -DAOM_TARGET_CPU=${OVT_AOM_CPU}
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libaom.a)
if(_ovt_aom_patch_files)
  externalproject_add_stepdependencies(tp_libaom download ${_ovt_aom_patch_files})
endif()

if(OVT_BUILD_AOM_INSPECT)
  # libaom installs aomdec with its examples but not inspect, which it builds in the top of its binary tree.
  set(_ovt_aom_inspect "inspect${CMAKE_EXECUTABLE_SUFFIX}")
  externalproject_add_step(
    tp_libaom install_inspect
    COMMAND ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/bin
    COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/${_ovt_aom_inspect} <INSTALL_DIR>/bin/${_ovt_aom_inspect}
    DEPENDEES install
    BYPRODUCTS <INSTALL_DIR>/bin/${_ovt_aom_inspect}
    COMMENT "Installing libaom's inspect oracle")
  unset(_ovt_aom_inspect)
endif()

ovt_tp_import_static(tp::aom tp_libaom lib/libaom.a include)
# libaom.a is C, multithreaded, and calls libm (part of libSystem on Apple platforms).
find_package(Threads REQUIRED)
set(_ovt_aom_libs Threads::Threads)
if(NOT APPLE)
  list(APPEND _ovt_aom_libs m)
endif()
set_property(TARGET tp::aom PROPERTY INTERFACE_LINK_LIBRARIES "${_ovt_aom_libs}")

unset(_ovt_aom_libs)
unset(_ovt_aom_examples)
unset(_ovt_aom_prefix)
unset(_ovt_aom_patch)
unset(_ovt_aom_patch_dir)
unset(_ovt_aom_patch_files)
