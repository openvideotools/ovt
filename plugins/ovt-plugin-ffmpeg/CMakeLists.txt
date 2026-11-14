# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# ovt_plugin_ffmpeg, the AVC/HEVC codec pack (chapter 7 §7.6.3): WI-2.169 builds its H.264 analysis decoder, WI-2.171
# the playback decoders, WI-4.107 the optional H.265 motion vector / QP plugin ovt_plugin_ffmpeg_hevc. They link no
# part of libovt-core (chapter 0 §0.6.2), only the static FFmpeg plugin flavour (tp::ffmpeg_plugin; the system FFmpeg
# with OVT_SYSTEM_FFMPEG=ON), whose symbols stay inside (§13.3.3).

set(OVT_FFMPEG_PLUGIN_SOURCES src/plugin.cpp src/h264_analysis.cpp src/map_h264_blockparams.cpp
                              src/map_h264_sidedata.cpp src/map_hevc_sidedata.cpp)
# Patch 0002 (WI-4.107) adds AV_VIDEO_ENC_PARAMS_HEVC to the plugin flavour; a system FFmpeg has no patches.
if(OVT_SYSTEM_FFMPEG)
  set(OVT_FFMPEG_HEVC_ENC_PARAMS 0)
else()
  set(OVT_FFMPEG_HEVC_ENC_PARAMS 1)
endif()

# ovt_ffmpeg_plugin_module(<target> <output name> [HEVC]): one plugin library of the shared sources.
function(ovt_ffmpeg_plugin_module target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "HEVC" "" "")
  add_library(${target} MODULE ${OVT_FFMPEG_PLUGIN_SOURCES})
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
  target_link_libraries(${target} PRIVATE tp::ffmpeg_plugin ovt_schemas ovt::flags)
  target_compile_definitions(${target} PRIVATE "OVT_FFMPEG_HEVC_ENC_PARAMS=${OVT_FFMPEG_HEVC_ENC_PARAMS}"
                                               "OVT_FFMPEG_PLUGIN_HEVC=$<BOOL:${arg_HEVC}>")
  set_target_properties(
    ${target} PROPERTIES PREFIX "lib" OUTPUT_NAME "${target}" SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
                         LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/plugins/lib")
  ovt_ffmpeg_hide_symbols(${target})
  # The plugin flavour turns GPL-2.0-or-later with OVT_FFMPEG_GPL (chapter 13 §13.3.3); the plugin follows it.
  if(OVT_FFMPEG_GPL)
    ovt_set_license(${target} "GPL-2.0-or-later")
  else()
    ovt_set_license(${target} "LGPL-2.1-or-later")
  endif()
endfunction()

ovt_ffmpeg_plugin_module(ovt_plugin_ffmpeg)
# WI-4.107: the optional H.265 motion vector / QP plugin, id ffmpeg_hevc. Only the patched plugin flavour exports the
# side data it maps, so it is not built against a system FFmpeg. SPEC-ASSUMPTION(#370): a second library of the same
# sources, because the plugin ABI carries one codec per plugin.
if(NOT OVT_SYSTEM_FFMPEG)
  ovt_ffmpeg_plugin_module(ovt_plugin_ffmpeg_hevc HEVC)
endif()

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
