# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# ovt_plugin_de265, the HEVC analysis plugin (chapter 07 §7.6.2, WI-2.163): libovt_plugin_de265.{so,dylib}, a
# separately loaded module that links the patched libde265 (tp::de265, third_party/libde265.cmake) statically. Like
# every analysis plugin it links no part of libovt-core (chapter 0 §0.6.2): it sees only the ABI header
# ovt/abi/analysis_decoder_v1.h and the generated FlatBuffers headers. The plugin and libde265 are
# LGPL-3.0-or-later (chapter 15 §15.2); HEVC is patent-encumbered, so tools/license_policy.toml keeps the plugin out
# of the default downloads.

add_library(ovt_plugin_de265 MODULE src/plugin.cpp src/decoder.cpp src/map_grid.cpp src/planes.cpp)
target_include_directories(ovt_plugin_de265 PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
target_link_libraries(ovt_plugin_de265 PRIVATE tp::de265 ovt_schemas ovt::flags)
if(NOT OVT_SYSTEM_DE265)
  # Patch 0001 (WI-2.162): POC, slice types and slice QP come from the read-only internals API. A system libde265
  # (OVT_SYSTEM_DE265=ON, #114) has no de265_internals.h; the plugin then leaves those FrameInfo fields at their
  # defaults and the host fills them from its index (chapter 07 §7.5.3 A6).
  target_compile_definitions(ovt_plugin_de265 PRIVATE OVT_DE265_INTERNALS=1)
endif()
set_target_properties(
  ovt_plugin_de265
  PROPERTIES PREFIX "lib"
             OUTPUT_NAME "ovt_plugin_de265"
             SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
             LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/plugins/lib"
             C_VISIBILITY_PRESET hidden
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON)
ovt_set_license(ovt_plugin_de265 "LGPL-3.0-or-later")

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
