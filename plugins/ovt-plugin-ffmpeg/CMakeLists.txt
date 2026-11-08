# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# ovt_plugin_ffmpeg, the AVC/HEVC codec pack (chapter 7 §7.6.3): WI-2.169 builds its H.264 analysis decoder, WI-2.171
# the playback decoders. It links no part of libovt-core (chapter 0 §0.6.2), only the static FFmpeg plugin flavour
# (tp::ffmpeg_plugin; the system FFmpeg with OVT_SYSTEM_FFMPEG=ON), whose symbols stay inside (§13.3.3).

add_library(ovt_plugin_ffmpeg MODULE src/plugin.cpp src/h264_analysis.cpp)
target_include_directories(ovt_plugin_ffmpeg PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
target_link_libraries(ovt_plugin_ffmpeg PRIVATE tp::ffmpeg_plugin ovt_schemas ovt::flags)
set_target_properties(
  ovt_plugin_ffmpeg PROPERTIES PREFIX "lib" OUTPUT_NAME "ovt_plugin_ffmpeg" SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
                               LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/plugins/lib")
ovt_ffmpeg_hide_symbols(ovt_plugin_ffmpeg)
# The plugin flavour turns GPL-2.0-or-later with OVT_FFMPEG_GPL (chapter 13 §13.3.3); the plugin follows it.
if(OVT_FFMPEG_GPL)
  ovt_set_license(ovt_plugin_ffmpeg "GPL-2.0-or-later")
else()
  ovt_set_license(ovt_plugin_ffmpeg "LGPL-2.1-or-later")
endif()

if(OVT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
