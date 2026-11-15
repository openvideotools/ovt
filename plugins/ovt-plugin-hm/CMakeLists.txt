# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# ovt_plugin_hm, the HEVC oracle plugin over HM's unpatched static libraries (tp::hm, WI-4.104; chapter 07 §7.6.4,
# WI-4.105). It links no part of libovt-core (chapter 0 §0.6.2). The plugin and HM are BSD-3-Clause; HM's COPYING
# (installed to share/doc/hm/) grants no patent rights and HEVC is patent-encumbered, so the plugin is optional and
# tools/license_policy.toml keeps it out of the default downloads. HM's headers are system headers (WI-4.104 pitfall).

add_library(ovt_plugin_hm MODULE src/plugin.cpp src/decoder.cpp src/analysis.cpp src/map_cu.cpp)
target_include_directories(ovt_plugin_hm PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
get_target_property(_ovt_hm_common_includes tp::hm_common INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_ovt_hm_decoder_includes tp::hm INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(ovt_plugin_hm SYSTEM PRIVATE ${_ovt_hm_common_includes} ${_ovt_hm_decoder_includes})
unset(_ovt_hm_common_includes)
unset(_ovt_hm_decoder_includes)
target_link_libraries(ovt_plugin_hm PRIVATE tp::hm ovt_schemas ovt::flags)
set_target_properties(
  ovt_plugin_hm
  PROPERTIES PREFIX "lib"
             OUTPUT_NAME "ovt_plugin_hm"
             SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
             LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/plugins/lib"
             C_VISIBILITY_PRESET hidden
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON)
# HM's classes and globals (g_scanOrder, the z-scan tables) have default visibility in the static libraries; the
# linker keeps only the entry point, so they never clash with another module's copy (as for ovt_plugin_aom).
if(APPLE)
  target_link_options(ovt_plugin_hm PRIVATE "LINKER:-exported_symbol,_ovt_analysis_decoder_v1")
elseif(NOT WIN32)
  target_link_options(ovt_plugin_hm PRIVATE "LINKER:--exclude-libs,ALL" "LINKER:-Bsymbolic"
                      "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
  set_property(TARGET ovt_plugin_hm APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
endif()
ovt_set_license(ovt_plugin_hm "BSD-3-Clause")

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
