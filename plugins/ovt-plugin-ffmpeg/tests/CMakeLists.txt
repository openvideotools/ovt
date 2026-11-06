# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Symbol-leak test of the FFmpeg plugin flavour (WI-0.012, chapter 13 §13.3.3): a shared object that links
# tp::ffmpeg_plugin must export the two plugin entry points and nothing else, in particular no av*/sws*/swr*/ff_*
# symbol of the static FFmpeg copy.
#
# Until WI-2.169 builds ovt_plugin_ffmpeg, the test runs on ovt_ffmpeg_symbol_leak_fixture, a module linked exactly
# like the plugin (tp::ffmpeg_plugin + ovt_ffmpeg_hide_symbols) whose two entry points are stubs. WI-2.169 points the
# same script at $<TARGET_FILE:ovt_plugin_ffmpeg>.
# SPEC-ASSUMPTION(#129): acceptance test 2 of WI-0.012 runs on the fixture until the plugin exists.

if(NOT TARGET tp::ffmpeg_plugin OR OVT_SYSTEM_FFMPEG)
  return()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR CMAKE_CROSSCOMPILING)
  # nm of the build host cannot be assumed to read the target's objects; the Android check runs from WI-0.034.
  return()
endif()

add_library(ovt_ffmpeg_symbol_leak_fixture MODULE symbol_leak_fixture.c)
target_link_libraries(ovt_ffmpeg_symbol_leak_fixture PRIVATE tp::ffmpeg_plugin ovt::flags)
ovt_ffmpeg_hide_symbols(ovt_ffmpeg_symbol_leak_fixture)
ovt_set_license(ovt_ffmpeg_symbol_leak_fixture "LGPL-2.1-or-later")

add_test(NAME ffmpeg_plugin_symbol_leak COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/test_symbol_leak.sh"
                                                $<TARGET_FILE:ovt_ffmpeg_symbol_leak_fixture>)
set_tests_properties(ffmpeg_plugin_symbol_leak PROPERTIES LABELS "fast;unit;third_party")
