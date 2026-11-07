# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Template for a third_party/<dep>.cmake script (chapter 13 §13.3.4, WI-0.010). Copy it to
# third_party/<dep>.cmake, replace `mydep`/`MyDep`, and include the copy from cmake/OvtThirdParty.cmake.
# Nothing includes this file itself.
#
# Rules (§13.3.4, §13.3.7):
#   * The version, URL and SHA-256 come only from third_party/VERSIONS.md, through OVT_TP_<dep>_* (see
#     build/<preset>/third_party/versions.cmake). Never write a version number in a script;
#     tools/versions_to_cmake.py --check fails when a script spells its row's version.
#   * Every download is verified by SHA-256. A git source needs GIT_TAG = full commit SHA.
#   * Patches live in third_party/patches/<dep>/NNNN-*.patch (or plugins/ovt-plugin-*/patches/ with a
#     series file) and carry an `Upstream-Status:` header.
#   * Install into OVT_THIRD_PARTY_PREFIX/<dep> (= build/third_party/<preset>/<dep>/); CI caches it keyed by
#     VERSIONS.md, the script and the patches.
#   * Meson-based dependencies use ovt_tp_meson() (cmake/OvtMeson.cmake, §13.3.5) instead of the
#     ExternalProject_Add() call below.

include_guard(GLOBAL)
include(OvtThirdPartyCommon)

# 1. Declare the VERSIONS.md row: fails when the row is missing or has no hash, downloads the archive into
#    OVT_TP_DOWNLOADS and verifies it, so ExternalProject finds the file there and does not fetch it again.
#    LICENSE defaults to the row's Licence column.
ovt_tp_declare(mydep LICENSE_FILES ./LICENSE) # "./": a bare LICENSE would be read as the LICENSE keyword

# 2. Patch step: every third_party/patches/mydep/*.patch, `git apply --check` first, applied once.
#    For a series file and a patches.json record use instead:
#      ovt_tp_apply_patches(mydep_patch SERIES <series> INSTALL_DIR <INSTALL_DIR>) ...  PATCH_COMMAND ${mydep_patch}
externalproject_add(
  tp_mydep
  URL ${OVT_TP_mydep_URL}
  URL_HASH SHA256=${OVT_TP_mydep_SHA256}
  DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
  PREFIX ${OVT_THIRD_PARTY_PREFIX}/mydep
  INSTALL_DIR ${OVT_THIRD_PARTY_PREFIX}/mydep
  PATCH_COMMAND ${OVT_TP_PATCH} ${CMAKE_SOURCE_DIR}/third_party/patches/mydep
  CMAKE_ARGS ${OVT_TP_CMAKE_TOOLCHAIN_ARGS} -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR> -DBUILD_SHARED_LIBS=OFF
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libmydep.a)

# 3. Imported target for OVT code, described for the licence manifest with the row's licence and version.
ovt_tp_import_static(tp::mydep tp_mydep lib/libmydep.a include)
