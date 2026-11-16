# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libde265, the HEVC analysis decoder, patched (chapter 13 §13.3.4 and §13.3.6, chapter 07 §7.6.2, WI-0.014).
# Produces the imported target tp::de265: static libde265.a with the public headers under include/libde265/.
# The pin (version, URL, SHA-256) is the libde265 row of third_party/VERSIONS.md.
#
# Patch step (§13.3.6): tools/apply_patches.py --check --series plugins/ovt-plugin-de265/patches/series dry-runs
# every patch against the pristine tarball and fails naming the first one that does not apply, applies them,
# writes the .ovt-patched marker into the source tree (a rebuild never applies them twice) and records the
# applied patches in <INSTALL_DIR>/share/ovt/patches.json for tools/gen_notices.py (chapter 15).
#
# Licence: libde265 is LGPL-3.0-or-later and the patched library is linked only into the
# ovt_plugin_de265 plugin (chapter 15 §15.2), never into ovt_core. HEVC is patent-encumbered: the plugin is
# kept out of the default downloads (tools/license_policy.toml [distribution]). The archive's COPYING is
# kept with the build. ENABLE_ENCODER=OFF drops the enc265 tool; ENABLE_DECODER=OFF drops the dec265 tool;
# these and ENABLE_SDL/BUILD_SHARED_LIBS are the only options of the pinned libde265 (chapter 07 §7.6.2).
# DE265_INTERNALS is added by patch 0001 (WI-2.162): ON builds and installs the read-only internals API
# libde265/de265_internals.h that the plugin reads per-block data through; OFF would build exactly upstream.

include_guard(GLOBAL)
include(OvtThirdPartyCommon)

set(OVT_DE265_PATCH_SERIES "${CMAKE_CURRENT_LIST_DIR}/../plugins/ovt-plugin-de265/patches/series"
    CACHE FILEPATH "Quilt-style series of the libde265 patches (chapter 13 §13.3.6)")
cmake_path(NORMAL_PATH OVT_DE265_PATCH_SERIES)

# libde265/md5.cc is public domain (Alexander Peslyak) with a fallback grant that COPYING does not carry:
# third_party/licenses/libde265/.
ovt_tp_declare(libde265 LICENSE_FILES COPYING
               ${CMAKE_CURRENT_LIST_DIR}/licenses/libde265/md5-LicenseRef-Peslyak-md5-public-domain.txt)

set(_ovt_de265_prefix "${OVT_THIRD_PARTY_PREFIX}/libde265")
ovt_tp_apply_patches(_ovt_de265_patch SERIES "${OVT_DE265_PATCH_SERIES}" INSTALL_DIR "${_ovt_de265_prefix}")

# A changed series or patch re-extracts the pristine tarball (the download step depends on them), so the patch
# step never meets a tree patched with another series. An unchanged series leaves every stamp as it is.
cmake_path(GET OVT_DE265_PATCH_SERIES PARENT_PATH _ovt_de265_patch_dir)
file(GLOB _ovt_de265_patch_files CONFIGURE_DEPENDS "${_ovt_de265_patch_dir}/*.patch")

# UBSan: libde265 left-shifts negative coefficients on purpose (for example transform_skip_residual_fallback() in
# fallback-dct.cc, still so in 1.0.16), which -fsanitize=shift-base reports as undefined behaviour in C++11 (C++20
# defines it as the arithmetic shift every supported compiler emits). Suppression files cannot silence it under
# -fno-sanitize-recover=all, so this one check is turned off for libde265 only; every other check stays on and OVT's
# own code keeps shift-base. The flags repeat OVT_TP_SANITIZER_C_FLAGS, because this later -D replaces the one in
# OVT_TP_CMAKE_TOOLCHAIN_ARGS. Tracked in https://github.com/openvideotools/ovt/issues/236; remove when the pin
# includes an upstream fix.
set(_ovt_de265_flag_args "")
if(OVT_TP_SANITIZER_C_FLAGS MATCHES "-fsanitize=[^ ]*undefined")
  set(_ovt_de265_cflags "${OVT_TP_SANITIZER_C_FLAGS} -fno-sanitize=shift-base")
  set(_ovt_de265_flag_args "-DCMAKE_C_FLAGS=${_ovt_de265_cflags}" "-DCMAKE_CXX_FLAGS=${_ovt_de265_cflags}")
  unset(_ovt_de265_cflags)
endif()

# Optimisation (chapter 07 §7.6.2 mitigation 4, WI-2.168). the pinned libde265 has no arm64 kernels (its NEON code is
# 32-bit ARM assembly that only the autotools build uses), so on Apple silicon and Android it runs its scalar C++
# fallbacks and the compiler flags are the only lever. The release presets are RelWithDebInfo, which CMake builds at
# -O2, and debug presets build third-party code as RelWithDebInfo too (OvtThirdPartyCommon); libde265 is always built
# with OVT_DE265_OPT_FLAGS instead (-O3, plus -mcpu=apple-m1 on macOS arm64 and -march=armv8-a on Android arm64;
# measurements in the "Performance" section of plugins/ovt-plugin-de265/README.md). The flags go into
# CMAKE_<LANG>_FLAGS_<CONFIG>, never CMAKE_<LANG>_FLAGS, so the sanitizer flags below are untouched. -DNDEBUG stays:
# libde265's assert()s are debug checks. Override the cache value to sweep flags.
# SPEC-ASSUMPTION(#597): §7.6.2 mitigation 4 reads "-O3 -ffast-math=off -march=armv8-a"; -ffast-math=off is not a
# compiler flag (no fast-math is the default), so it is left out.
set(_ovt_de265_opt_default "-O3")
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR (NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR
                                                                                           STREQUAL "arm64")))
  string(APPEND _ovt_de265_opt_default " -mcpu=apple-m1") # every Apple silicon Mac; about 3 % less CPU on an M4
elseif(ANDROID AND ANDROID_ABI STREQUAL "arm64-v8a")
  string(APPEND _ovt_de265_opt_default " -march=armv8-a") # the Android arm64 baseline; no FP16/dot-product
endif()
set(OVT_DE265_OPT_FLAGS "${_ovt_de265_opt_default}"
    CACHE STRING "Optimisation flags of the libde265 build (chapter 07 §7.6.2 mitigation 4, WI-2.168)")
unset(_ovt_de265_opt_default)
foreach(_lang C CXX)
  list(APPEND _ovt_de265_flag_args "-DCMAKE_${_lang}_FLAGS_RELEASE=${OVT_DE265_OPT_FLAGS} -DNDEBUG"
       "-DCMAKE_${_lang}_FLAGS_RELWITHDEBINFO=${OVT_DE265_OPT_FLAGS} -g -DNDEBUG")
endforeach()
unset(_lang)

externalproject_add(
  tp_libde265
  URL ${OVT_TP_libde265_URL}
  URL_HASH SHA256=${OVT_TP_libde265_SHA256}
  DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
  DOWNLOAD_EXTRACT_TIMESTAMP OFF
  PREFIX ${_ovt_de265_prefix}
  INSTALL_DIR ${_ovt_de265_prefix}
  PATCH_COMMAND ${_ovt_de265_patch}
  CMAKE_ARGS ${OVT_TP_CMAKE_TOOLCHAIN_ARGS}
             ${_ovt_de265_flag_args}
             -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
             # The pinned libde265 asks for CMake 3.3.2; CMake 4 refuses anything below 3.5 without this.
             -DCMAKE_POLICY_VERSION_MINIMUM=3.5
             -DCMAKE_INSTALL_LIBDIR=lib
             -DCMAKE_INSTALL_INCLUDEDIR=include
             -DBUILD_SHARED_LIBS=OFF
             -DENABLE_SDL=OFF
             -DENABLE_DECODER=OFF
             -DENABLE_ENCODER=OFF
             -DDE265_INTERNALS=ON
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libde265.a)
externalproject_add_stepdependencies(tp_libde265 download "${OVT_DE265_PATCH_SERIES}" ${_ovt_de265_patch_files})

ovt_tp_import_static(tp::de265 tp_libde265 lib/libde265.a include)
# libde265.a is C++ and uses threads; LIBDE265_STATIC_BUILD selects the static declarations of de265.h.
find_package(Threads REQUIRED)
set_property(TARGET tp::de265 PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
set_property(TARGET tp::de265 PROPERTY INTERFACE_COMPILE_DEFINITIONS LIBDE265_STATIC_BUILD)
set_property(TARGET tp::de265 PROPERTY IMPORTED_LINK_INTERFACE_LANGUAGES CXX)

unset(_ovt_de265_prefix)
unset(_ovt_de265_patch)
unset(_ovt_de265_patch_dir)
unset(_ovt_de265_patch_files)
unset(_ovt_de265_flag_args)
