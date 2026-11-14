# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# HM, the HEVC reference decoder, as static libraries for the optional HM oracle plugin (chapter 07 §7.6.4 build row,
# chapter 13 §13.3.4, WI-4.104). cmake/OvtThirdParty.cmake includes this file when OVT_BUILD_PLUGIN_HM=ON.
# The pin (tag, commit, URL) is the HM row of third_party/VERSIONS.md.
#
# Imported targets:
#   tp::hm            lib/libTLibDecoder.a, with tp::hm_common; the plugin includes "TLibDecoder/TDecTop.h" etc.
#   tp::hm_common     lib/libTLibCommon.a (HM's libmd5 is compiled into it, as HM does)
#   tp::hm_utilities  lib/libUtilities.a (TVideoIOYuv), for consumers that write YUV files
# Every target carries RExt__HIGH_BIT_DEPTH_SUPPORT=1, which changes HM's class layouts, and HM's include
# directories (include/hm and include/hm/<library>, as HM's own lists export them).
#
# SPEC-ASSUMPTION(#345): pinned by commit, no patch, OVT's own CMake list, libraries TLibCommon/TLibDecoder/Utilities.
#
# Steps:
#   download   third_party/hm/fetch.cmake fetches the pinned tag one commit deep and fails unless it is the pinned
#              commit (HM publishes no release tarball with a stable hash; §13.3.7 allows a git source pinned by
#              its full commit id, and git verifies every object against that id).
#   patch      none: §7.6.4 builds HM unmodified, so the licence manifest reports it unmodified.
#   configure  third_party/hm/CMakeLists.txt, OVT's build of HM's library sources (HM's own list adds x86-only
#              -msse4.1 and -Werror; see that file). Warnings are off for HM's code only.
#
# Licence: HM's COPYING is the 3-clause BSD licence (ITU/ISO/IEC), preceded by a note that no patent rights are
# granted; HEVC is patent-encumbered (chapter 15). Its libmd5 is Colin Plumb's public-domain MD5. COPYING is
# installed to share/doc/hm/. Offline builds: tools/third_party_offline.py pre-fetches archives only, so an offline
# HM build needs a local mirror URL in VERSIONS.md (file:// works; the commit check still applies).

include_guard(GLOBAL)
include(OvtThirdPartyCommon)
find_package(Git REQUIRED)
find_package(Threads REQUIRED)

if(NOT OVT_TP_hm_COMMIT)
  message(FATAL_ERROR "third_party/hm.cmake: ${OVT_TP_hm_ROW} has no full commit id in its Version column.")
endif()

ovt_tp_declare(hm NO_DOWNLOAD LICENSE_FILES COPYING)

set(_ovt_hm_prefix "${OVT_THIRD_PARTY_PREFIX}/hm")
# The download step depends on this file, which changes only when the pin does, so a new pin fetches again.
file(CONFIGURE OUTPUT "${_ovt_hm_prefix}/pin.txt" CONTENT "${OVT_TP_hm_URL} ${OVT_TP_hm_VERSION} ${OVT_TP_hm_COMMIT}\n")

externalproject_add(
  tp_hm
  PREFIX ${_ovt_hm_prefix}
  INSTALL_DIR ${_ovt_hm_prefix}
  DOWNLOAD_COMMAND "${CMAKE_COMMAND}" "-DGIT=${GIT_EXECUTABLE}" "-DURL=${OVT_TP_hm_URL}" "-DTAG=${OVT_TP_hm_VERSION}"
                   "-DCOMMIT=${OVT_TP_hm_COMMIT}" -DDEST=<SOURCE_DIR> -P "${CMAKE_CURRENT_LIST_DIR}/hm/fetch.cmake"
  UPDATE_COMMAND ""
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/hm" -B <BINARY_DIR> -G "${CMAKE_GENERATOR}"
                    ${OVT_TP_CMAKE_TOOLCHAIN_ARGS} -DHM_SOURCE_DIR=<SOURCE_DIR> -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
  BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
  INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
  BUILD_BYPRODUCTS <INSTALL_DIR>/lib/libTLibCommon.a <INSTALL_DIR>/lib/libTLibDecoder.a
                   <INSTALL_DIR>/lib/libUtilities.a)
externalproject_add_stepdependencies(tp_hm download "${_ovt_hm_prefix}/pin.txt"
                                     "${CMAKE_CURRENT_LIST_DIR}/hm/fetch.cmake")
externalproject_add_stepdependencies(tp_hm configure "${CMAKE_CURRENT_LIST_DIR}/hm/CMakeLists.txt")

ovt_tp_import_static(tp::hm_common tp_hm lib/libTLibCommon.a include/hm)
ovt_tp_import_static(tp::hm tp_hm lib/libTLibDecoder.a include/hm)
ovt_tp_import_static(tp::hm_utilities tp_hm lib/libUtilities.a include/hm)

foreach(_ovt_hm_dir TLibCommon TLibDecoder Utilities libmd5)
  file(MAKE_DIRECTORY "${_ovt_hm_prefix}/include/hm/${_ovt_hm_dir}")
endforeach()
foreach(_ovt_hm_target tp::hm_common tp::hm tp::hm_utilities)
  set_property(TARGET ${_ovt_hm_target} PROPERTY INTERFACE_COMPILE_DEFINITIONS RExt__HIGH_BIT_DEPTH_SUPPORT=1)
  set_property(TARGET ${_ovt_hm_target} PROPERTY IMPORTED_LINK_INTERFACE_LANGUAGES CXX)
endforeach()
set_property(
  TARGET tp::hm_common APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_ovt_hm_prefix}/include/hm/TLibCommon"
                                       "${_ovt_hm_prefix}/include/hm/libmd5")
set_property(TARGET tp::hm_common PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
set_property(TARGET tp::hm APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_ovt_hm_prefix}/include/hm/TLibDecoder")
set_property(TARGET tp::hm PROPERTY INTERFACE_LINK_LIBRARIES tp::hm_common)
set_property(TARGET tp::hm_utilities APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                                                     "${_ovt_hm_prefix}/include/hm/Utilities")
set_property(TARGET tp::hm_utilities PROPERTY INTERFACE_LINK_LIBRARIES tp::hm_common)

unset(_ovt_hm_prefix)
unset(_ovt_hm_dir)
unset(_ovt_hm_target)
