# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# MoltenVK, the Vulkan-on-Metal implementation OVT links directly on macOS (chapter 13 §13.3.1, §13.5.6, WI-0.017).
# Produces the imported target tp::moltenvk: a shared libMoltenVK.dylib under <prefix>/lib and the Vulkan and
# MoltenVK headers under <prefix>/include. The pin (version, URL, SHA-256) is the MoltenVK row of
# third_party/VERSIONS.md; the archive is the official prebuilt MoltenVK-macos.tar of the `v<version>-artifacts`
# tag, which holds a universal (arm64 + x86_64) dylib.
#
# Per-arch slice (§13.5.6): `lipo -thin` extracts the slice of the architecture being built (arm64 or x86_64, from
# CMAKE_OSX_ARCHITECTURES, else the host processor), so each per-arch artefact bundles a single-architecture
# dylib. A universal build (several architectures in CMAKE_OSX_ARCHITECTURES, OVT_UNIVERSAL=ON) keeps the
# universal dylib.

include_guard(GLOBAL)
include(OvtThirdPartyCommon)

if(NOT APPLE)
  message(FATAL_ERROR "third_party/moltenvk.cmake: MoltenVK is macOS only.")
endif()

ovt_tp_declare(moltenvk LICENSE_FILES LICENSE)

if(CMAKE_OSX_ARCHITECTURES)
  set(_ovt_mvk_archs "${CMAKE_OSX_ARCHITECTURES}")
else()
  set(_ovt_mvk_archs "${CMAKE_SYSTEM_PROCESSOR}")
endif()
list(LENGTH _ovt_mvk_archs _ovt_mvk_arch_count)

set(_ovt_mvk_prefix "${OVT_THIRD_PARTY_PREFIX}/moltenvk")
set(_ovt_mvk_dylib "${_ovt_mvk_prefix}/lib/libMoltenVK.dylib")
set(_ovt_mvk_src "<SOURCE_DIR>/MoltenVK")
set(_ovt_mvk_dylib_src "${_ovt_mvk_src}/dynamic/dylib/macOS/libMoltenVK.dylib")
if(_ovt_mvk_arch_count GREATER 1)
  set(_ovt_mvk_slice COMMAND "${CMAKE_COMMAND}" -E copy "${_ovt_mvk_dylib_src}" "${_ovt_mvk_dylib}")
else()
  if(NOT _ovt_mvk_archs MATCHES "^(arm64|x86_64)$")
    message(FATAL_ERROR "third_party/moltenvk.cmake: unsupported macOS architecture '${_ovt_mvk_archs}'.")
  endif()
  set(_ovt_mvk_slice COMMAND xcrun lipo "${_ovt_mvk_dylib_src}" -thin ${_ovt_mvk_archs} -output "${_ovt_mvk_dylib}")
endif()

externalproject_add(
  tp_moltenvk
  URL ${OVT_TP_moltenvk_URL}
  URL_HASH SHA256=${OVT_TP_moltenvk_SHA256}
  DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
  DOWNLOAD_EXTRACT_TIMESTAMP OFF
  PREFIX ${_ovt_mvk_prefix}
  INSTALL_DIR ${_ovt_mvk_prefix}
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ""
  INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "<INSTALL_DIR>/lib" "<INSTALL_DIR>/share/licenses/moltenvk"
                  ${_ovt_mvk_slice}
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_ovt_mvk_src}/include" "<INSTALL_DIR>/include"
  COMMAND "${CMAKE_COMMAND}" -E copy "<SOURCE_DIR>/LICENSE" "<INSTALL_DIR>/share/licenses/moltenvk/LICENSE"
  BUILD_BYPRODUCTS "${_ovt_mvk_dylib}")

# INTERFACE_INCLUDE_DIRECTORIES must exist at generate time, before the build installs anything.
file(MAKE_DIRECTORY "${_ovt_mvk_prefix}/include")
add_library(tp::moltenvk SHARED IMPORTED GLOBAL)
set_target_properties(tp::moltenvk PROPERTIES IMPORTED_LOCATION "${_ovt_mvk_dylib}" INTERFACE_INCLUDE_DIRECTORIES
                                                                                    "${_ovt_mvk_prefix}/include")
add_dependencies(tp::moltenvk tp_moltenvk)
ovt_set_dependency_license(
  tp::moltenvk
  SOURCE
  third_party
  SPDX
  "${OVT_TP_moltenvk_LICENSE}"
  VERSION
  "${OVT_TP_moltenvk_VERSION}"
  LINKAGE
  shared)

unset(_ovt_mvk_archs)
unset(_ovt_mvk_arch_count)
unset(_ovt_mvk_prefix)
unset(_ovt_mvk_dylib)
unset(_ovt_mvk_src)
unset(_ovt_mvk_dylib_src)
unset(_ovt_mvk_slice)
