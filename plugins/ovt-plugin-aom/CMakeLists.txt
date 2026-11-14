# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-2-Clause
#
# ovt_plugin_aom, the AV1 analysis plugin over libaom's inspection API (chapter 07 §7.6.1, WI-2.155). It links no
# part of libovt-core (chapter 0 §0.6.2), only the static libaom of third_party/libaom.cmake built with
# CONFIG_INSPECTION, whose symbols stay inside: the one exported symbol is ovt_analysis_decoder_v1.
#
# The plugin needs libaom's semi-public av1/decoder/inspection.h from the pinned source tree (tp::aom_inspection),
# which exists only for the bundled libaom. With OVT_SYSTEM_LIBAOM=ON the plugin is not built.

if(NOT TARGET tp::aom_inspection)
  message(STATUS "OVT: ovt_plugin_aom needs the bundled libaom (OVT_SYSTEM_LIBAOM=OFF); not building it")
  return()
endif()

set(OVT_AOM_PLUGIN_SOURCES src/plugin.cpp src/decoder.cpp src/fb_builder.cpp src/map_mi.cpp src/map_mi_ext.cpp
                           src/partition_reconstruct.cpp src/planes.cpp)
add_library(ovt_plugin_aom MODULE ${OVT_AOM_PLUGIN_SOURCES})
target_include_directories(ovt_plugin_aom PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
target_link_libraries(ovt_plugin_aom PRIVATE tp::aom_inspection ovt_schemas ovt::flags)
set_target_properties(
  ovt_plugin_aom
  PROPERTIES PREFIX "lib"
             OUTPUT_NAME "ovt_plugin_aom"
             SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
             LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/plugins/lib"
             C_VISIBILITY_PRESET hidden
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON)
# Hidden visibility does not cover libaom.a's public API or the libstdc++ template instantiations, so the linker
# keeps only the entry point (as cmake/ovt_plugin_exports.map does for the FFmpeg plugin).
if(APPLE)
  target_link_options(ovt_plugin_aom PRIVATE "LINKER:-exported_symbol,_ovt_analysis_decoder_v1")
elseif(NOT WIN32)
  target_link_options(ovt_plugin_aom PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:-Bsymbolic"
                      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
  set_property(TARGET ovt_plugin_aom APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
endif()
ovt_set_license(ovt_plugin_aom "BSD-2-Clause")

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
