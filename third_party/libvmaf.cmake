# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libvmaf, Netflix's VMAF implementation (chapter 13 §13.3.1, §13.3.5, WI-0.015). Meson wrapper.
# Produces the imported target tp::vmaf (static libvmaf.a with the public headers under include/libvmaf/).
# The pin (version, URL, SHA-256) is the libvmaf row of third_party/VERSIONS.md.
#
# Options (§13.3.1): float feature extractors, AVX-512 (x86 only, needs nasm >= 2.14; Meson turns it off by
# itself with an older nasm), built-in models, no CUDA. No tests and no documentation. The repository
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

ovt_tp_declare(libvmaf LICENSE_FILES LICENSE)

ovt_tp_meson(
  libvmaf
  URL
  ${OVT_TP_libvmaf_URL}
  URL_HASH
  SHA256=${OVT_TP_libvmaf_SHA256}
  SOURCE_SUBDIR
  libvmaf
  OPTIONS
  -Denable_float=true
  -Denable_avx512=true
  -Dbuilt_in_models=true
  -Denable_cuda=false
  -Denable_tests=false
  -Denable_docs=false
  OUTPUT_LIBS
  tp::vmaf=lib/libvmaf.a)

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
