# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Install and export (chapter 13 §13.2.6). Targets that do not exist yet in this build (ovt_render, the
# plugins) are skipped, so the export works with only an empty ovt_core.

include_guard(GLOBAL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(OVT_SCHEMA_DIR "${CMAKE_INSTALL_DATADIR}/ovt/schema")

set(_ovt_export_targets "")
foreach(_t ovt_core ovt_render)
  if(TARGET ${_t})
    list(APPEND _ovt_export_targets ${_t})
  endif()
endforeach()

install(
  TARGETS ${_ovt_export_targets}
  EXPORT OvtTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ovt)

# libovt_core links the shared core FFmpeg flavour (libav*_ovt, chapter 13 §13.3.3) since the demuxer of WI-1.230.
# Install those libraries next to it (symlinks preserved, so the SONAME names exist) and give libovt_core a
# relative run path, so an installed prefix is self-contained: the linker of a downstream consumer and the loader
# both find libav*_ovt through it. The FFmpeg libraries carry RUNPATH $ORIGIN themselves (third_party/ffmpeg.cmake).
# Packaging and relocation proper (macOS install names, bundles) belong to WI-0.036. The system-FFmpeg build
# installs nothing: the distribution provides the libraries.
if(TARGET ovt_core)
  if(APPLE)
    set_target_properties(ovt_core PROPERTIES INSTALL_RPATH "@loader_path")
  else()
    set_target_properties(ovt_core PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
endif()
if(TARGET tp::ffmpeg_core_avformat AND NOT OVT_SYSTEM_FFMPEG)
  get_target_property(_ovt_ff_lib tp::ffmpeg_core_avformat IMPORTED_LOCATION)
  get_filename_component(_ovt_ff_libdir "${_ovt_ff_lib}" DIRECTORY)
  install(
    DIRECTORY "${_ovt_ff_libdir}/"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT core
    FILES_MATCHING
    PATTERN "lib*_ovt.so*"
    PATTERN "lib*_ovt.*dylib"
    PATTERN "pkgconfig" EXCLUDE)
  unset(_ovt_ff_lib)
  unset(_ovt_ff_libdir)
endif()

if(TARGET ovt_cli)
  install(TARGETS ovt_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cli)
endif()

# UVQ model manifest and YouTube-8M label names (chapter 9 §9.6.3 and §9.6.5; WI-3.314). The .onnx files are
# not installed: the UVQ host downloads them into the cache on first use and checks them against the manifest.
install(FILES "${CMAKE_SOURCE_DIR}/core/share/models/uvq/manifest.json"
              "${CMAKE_SOURCE_DIR}/core/share/models/uvq/labels_yt8m.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/ovt/models/uvq" COMPONENT core)

set(_ovt_plugin_targets "")
foreach(_t ovt_plugin_aom ovt_plugin_ffmpeg ovt_plugin_de265)
  if(TARGET ${_t})
    list(APPEND _ovt_plugin_targets ${_t})
  endif()
endforeach()
if(_ovt_plugin_targets)
  install(TARGETS ${_ovt_plugin_targets} LIBRARY DESTINATION ${OVT_PLUGIN_DIR} COMPONENT plugins)
endif()

if(OVT_INSTALL_DEV)
  # C11 headers only; ovt/cxx/*.hpp are in-tree C++ helpers (chapter 12 §12.3.1 rule 1, D22).
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/core/include/ovt/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ovt COMPONENT dev
          PATTERN "cxx" EXCLUDE)
  install(
    EXPORT OvtTargets
    NAMESPACE ovt::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ovt
    FILE ovt-targets.cmake
    COMPONENT dev)
  configure_package_config_file(
    "${CMAKE_SOURCE_DIR}/cmake/ovt-config.cmake.in" "${CMAKE_BINARY_DIR}/ovt-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ovt PATH_VARS OVT_PLUGIN_DIR OVT_SCHEMA_DIR)
  # The package version is the C API version (OVT_API_VERSION, which also sets the SONAME), not the
  # 0.x product version: find_package(ovt 1 CONFIG REQUIRED) asks for API major 1 (chapter 13 §13.2.6).
  get_target_property(_ovt_api_version ovt_core VERSION)
  write_basic_package_version_file("${CMAKE_BINARY_DIR}/ovt-config-version.cmake" VERSION ${_ovt_api_version}
                                   COMPATIBILITY SameMajorVersion)
  unset(_ovt_api_version)
  install(FILES "${CMAKE_BINARY_DIR}/ovt-config.cmake" "${CMAKE_BINARY_DIR}/ovt-config-version.cmake"
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ovt COMPONENT dev)
  # Same usage requirement as the exported target's INTERFACE_LINK_OPTIONS (core/CMakeLists.txt).
  string(JOIN " " OVT_PC_SANITIZER_LIBS ${OVT_INSTALL_SANITIZER_LINK_FLAGS})
  configure_file("${CMAKE_SOURCE_DIR}/cmake/ovt-core.pc.in" "${CMAKE_BINARY_DIR}/ovt-core.pc" @ONLY)
  install(FILES "${CMAKE_BINARY_DIR}/ovt-core.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig COMPONENT dev)
endif()

unset(_t)
unset(_ovt_export_targets)
unset(_ovt_plugin_targets)
unset(OVT_PC_SANITIZER_LIBS)
