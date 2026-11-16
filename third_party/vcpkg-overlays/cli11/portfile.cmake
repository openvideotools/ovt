# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The upstream cli11 port of the pinned baseline (2.7.2) with one change: CLI11 is installed header-only instead of as
# the precompiled static library (CLI11_PRECOMPILED=ON plus a CLI11_COMPILE define patched into CLI.hpp). See
# README.md for why.
set(VCPKG_BUILD_TYPE release) # header-only

vcpkg_from_github(
  OUT_SOURCE_PATH
  SOURCE_PATH
  REPO
  CLIUtils/CLI11
  REF
  "v${VERSION}"
  SHA512
  437cd9d704c1b0de0516bdc8cb115befb2e7170417375a64845a2bc2385fa07948c4af724ec615d27fbc102dbe13a7117f33873d6776fe9b17f293d42fe64dd9
  HEAD_REF
  main)

vcpkg_cmake_configure(
  SOURCE_PATH
  "${SOURCE_PATH}"
  OPTIONS
  -DCLI11_BUILD_EXAMPLES=OFF
  -DCLI11_BUILD_DOCS=OFF
  -DCLI11_BUILD_TESTS=OFF
  -DCLI11_PRECOMPILED=OFF
  -DCLI11_SINGLE_FILE=OFF
  -DCMAKE_CXX_STANDARD=17)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH share/cmake/CLI11)
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib" "${CURRENT_PACKAGES_DIR}/debug")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
