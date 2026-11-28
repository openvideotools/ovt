# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The upstream fmt port of the pinned baseline (12.2.0, port-version 1) with one change: the upstream backport patch
# is read from this directory instead of being fetched with vcpkg_download_distfile from
# github.com/fmtlib/fmt/commit/<sha>.patch, which answers HTTP 503 intermittently. See README.md.

vcpkg_from_github(
  OUT_SOURCE_PATH
  SOURCE_PATH
  REPO
  fmtlib/fmt
  REF
  "${VERSION}"
  SHA512
  5ac2ba0f54a484999ed5407d82b77aad170cea49a267decd2c0eedadf3b14413e2a83fcc8e9ca9c16640595e019b8636e160f72314d8be50653324e82ac745eb
  HEAD_REF
  master
  PATCHES
  patches/backport-4813-uint128-bitwise-not.patch)

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}" OPTIONS -DFMT_CMAKE_DIR=share/fmt -DFMT_TEST=OFF -DFMT_DOC=OFF)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
