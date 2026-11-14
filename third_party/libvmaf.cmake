# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libvmaf, Netflix's VMAF implementation (chapter 13 §13.3.1, §13.3.5, WI-0.015). Meson wrapper.
# Produces the imported target tp::vmaf (static libvmaf.a with the public headers under include/libvmaf/).
# The pin (version, URL, SHA-256) is the libvmaf row of third_party/VERSIONS.md.
#
# Options (§13.3.1, chapter 9 §9.5.1): float feature extractors, AVX-512 (x86 only, needs nasm >= 2.14; Meson turns
# it off by itself with an older nasm), built-in models, no CUDA, no NVTX. No tests and no documentation. The repository
# archive holds the Meson project in its `libvmaf/` directory and the model files in `model/`.
#
# Host tools: nasm on x86_64 (tools/bootstrap.sh) and `xxd`, which Meson uses to compile the built-in models
# into the library. Without xxd Meson still succeeds and the library silently has no built-in models, so the
# configure step of this script fails when xxd is missing (WI-0.015 acceptance test 2).

include_guard(GLOBAL)
include(OvtThirdPartyCommon)
include(OvtMeson)

find_program(OVT_XXD NAMES xxd)
if(NOT OVT_XXD)
  message(FATAL_ERROR "libvmaf: `xxd` not found. Meson needs it to compile the built-in VMAF models into the "
                      "library. Install it (macOS ships it; Debian/Ubuntu: apt install xxd; tools/bootstrap.sh).")
endif()

# "./LICENSE", not LICENSE: a bare LICENSE is read as ovt_tp_declare()'s LICENSE keyword and records no file.
# The archive compiles in third-party code whose licence text sits only in source-file headers (libsvm, Xiph
# psnr_hvs, Tom Distler's IQA, ciede2000, mkdirp, pdjson, dav1d-derived files, x86inc); third_party/licenses/libvmaf/
# holds those texts, one file per component (tools/tests/test_bundled_licences.py checks the list).
set(_ovt_vmaf_licdir "${CMAKE_CURRENT_LIST_DIR}/licenses/libvmaf")
ovt_tp_declare(
  libvmaf
  LICENSE_FILES
  ./LICENSE
  ${_ovt_vmaf_licdir}/libsvm-BSD-3-Clause.txt
  ${_ovt_vmaf_licdir}/xiph-psnr_hvs-BSD-2-Clause.txt
  ${_ovt_vmaf_licdir}/iqa-tdistler-BSD-3-Clause.txt
  ${_ovt_vmaf_licdir}/ciede2000-MIT.txt
  ${_ovt_vmaf_licdir}/dav1d-derived-BSD-2-Clause.txt
  ${_ovt_vmaf_licdir}/x86inc-ISC.txt
  ${_ovt_vmaf_licdir}/mkdirp-MIT.txt
  ${_ovt_vmaf_licdir}/pdjson-Unlicense.txt)
unset(_ovt_vmaf_licdir)

# The libvmaf options of chapter 9 §9.5.1 (also checked after configure, below).
set(OVT_LIBVMAF_OPTIONS enable_float=true enable_avx512=true built_in_models=true enable_cuda=false enable_nvtx=false
                        enable_docs=false)
list(TRANSFORM OVT_LIBVMAF_OPTIONS PREPEND "-D" OUTPUT_VARIABLE _ovt_vmaf_meson_options)
ovt_tp_meson(
  libvmaf
  URL
  ${OVT_TP_libvmaf_URL}
  URL_HASH
  SHA256=${OVT_TP_libvmaf_SHA256}
  SOURCE_SUBDIR
  libvmaf
  OPTIONS
  -Db_ndebug=true
  -Denable_tests=false
  ${_ovt_vmaf_meson_options}
  OUTPUT_LIBS
  tp::vmaf=lib/libvmaf.a)
unset(_ovt_vmaf_meson_options)

# WI-3.309 step 1 and acceptance test 3: `meson introspect` must show the option set of §9.5.1 (plus the release
# static build ovt_tp_meson asks for) and, on arm64, the NEON sources; the build stops here otherwise.
# core/tests/metrics runs the same check as the test metrics_vmaf_meson_introspection.
_ovt_meson_machine(_ovt_vmaf)
if(_ovt_vmaf_CPU_FAMILY STREQUAL "aarch64")
  set(OVT_LIBVMAF_REQUIRE_NEON ON)
else()
  set(OVT_LIBVMAF_REQUIRE_NEON OFF)
endif()
list(JOIN OVT_LIBVMAF_OPTIONS "|" OVT_LIBVMAF_EXPECT)
string(PREPEND OVT_LIBVMAF_EXPECT "buildtype=release|default_library=static|")
set(OVT_LIBVMAF_INTROSPECT_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../cmake/OvtLibvmafIntrospect.cmake")
cmake_path(NORMAL_PATH OVT_LIBVMAF_INTROSPECT_SCRIPT)
externalproject_get_property(tp_libvmaf BINARY_DIR)
set(OVT_LIBVMAF_BUILD_DIR "${BINARY_DIR}")
unset(BINARY_DIR)
externalproject_add_step(
  tp_libvmaf introspect
  COMMAND ${CMAKE_COMMAND} "-DOVT_MESON=${OVT_MESON}" "-DBUILD_DIR=<BINARY_DIR>" "-DEXPECT=${OVT_LIBVMAF_EXPECT}"
          "-DREQUIRE_NEON=${OVT_LIBVMAF_REQUIRE_NEON}" -P "${OVT_LIBVMAF_INTROSPECT_SCRIPT}"
  COMMENT "Checking the libvmaf Meson options and NEON sources (chapter 9 §9.5.1)"
  DEPENDEES configure
  DEPENDERS build
  DEPENDS "${OVT_LIBVMAF_INTROSPECT_SCRIPT}")
unset(_ovt_vmaf_SYSTEM)
unset(_ovt_vmaf_CPU_FAMILY)
unset(_ovt_vmaf_CPU)

# libvmaf.a needs threads and libm; svm.cpp is C++, so a C consumer also needs the C++ runtime (CMake adds it
# for the C++ targets of OVT).
find_package(Threads REQUIRED)
set(_ovt_vmaf_libs Threads::Threads)
if(NOT ANDROID_ABI AND NOT APPLE)
  list(APPEND _ovt_vmaf_libs m)
endif()
set_property(TARGET tp::vmaf PROPERTY INTERFACE_LINK_LIBRARIES "${_ovt_vmaf_libs}")
set_property(TARGET tp::vmaf PROPERTY IMPORTED_LINK_INTERFACE_LANGUAGES CXX)
unset(_ovt_vmaf_libs)
