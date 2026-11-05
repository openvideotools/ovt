# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# dav1d, the AV1 playback decoder (chapter 13 §13.3.1, §13.3.5, WI-0.011). First consumer of ovt_tp_meson().
# Produces the imported target tp::dav1d (static libdav1d.a with the public headers under include/dav1d/).
# The pin (version, URL, SHA-256) is the dav1d row of third_party/VERSIONS.md.
#
# Options: static, no CLI tools, no tests (§13.3.1). Assembly stays on; NASM is a host tool on x86_64
# (tools/bootstrap.sh). If the NDK clang integrated assembler fails on Android x86_64, add
# `-Denable_asm=false` for that target only and record it in the VERSIONS.md notes (WI-0.011 pitfall).

include_guard(GLOBAL)
include(OvtThirdPartyCommon)
include(OvtMeson)

ovt_tp_declare(dav1d LICENSE_FILES COPYING)

ovt_tp_meson(
  dav1d
  URL
  ${OVT_TP_dav1d_URL}
  URL_HASH
  SHA256=${OVT_TP_dav1d_SHA256}
  OPTIONS
  -Denable_tools=false
  -Denable_tests=false
  -Denable_examples=false
  OUTPUT_LIBS
  tp::dav1d=lib/libdav1d.a)

# libdav1d.a needs the platform thread, math and dlopen libraries (Android's libc contains them).
find_package(Threads REQUIRED)
set(_ovt_dav1d_libs Threads::Threads)
if(NOT ANDROID_ABI AND NOT APPLE)
  list(APPEND _ovt_dav1d_libs m ${CMAKE_DL_LIBS})
endif()
set_property(TARGET tp::dav1d PROPERTY INTERFACE_LINK_LIBRARIES "${_ovt_dav1d_libs}")
unset(_ovt_dav1d_libs)
