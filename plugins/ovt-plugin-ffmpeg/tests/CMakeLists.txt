# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later

# ---- WI-2.169: the plugin, loaded through the host's plugin registry ----------------------------------------------
# ovt_plugin_ffmpeg_framemd5 is the in-tree `ffmpeg -f framemd5` of acceptance test 1. It links the static plugin
# flavour, so it lives in its own executable: the smoke test links the host (ovt_core_objects), whose core flavour
# defines the same av* symbols (chapter 13 §13.3.3).
ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)

add_executable(ovt_plugin_ffmpeg_framemd5 ffmpeg_framemd5.cpp)
target_link_libraries(ovt_plugin_ffmpeg_framemd5 PRIVATE tp::ffmpeg_plugin ovt::flags)
ovt_set_license(ovt_plugin_ffmpeg_framemd5 "LGPL-2.1-or-later")

add_executable(ovt_plugin_ffmpeg_smoke_tests test_ffmpeg_plugin_smoke.cpp)
target_link_libraries(ovt_plugin_ffmpeg_smoke_tests PRIVATE ovt_core_objects tp::ffmpeg_core ovt_schemas
                                                            ovt_test_support ovt::flags ${Catch2_TARGET})
target_compile_definitions(ovt_plugin_ffmpeg_smoke_tests PRIVATE "OVT_FFMPEG_SYSTEM=$<BOOL:${OVT_SYSTEM_FFMPEG}>"
                                                                 "OVT_FFMPEG_HW_DECODE=$<BOOL:${OVT_ENABLE_HW_DECODE}>")
target_compile_definitions(
  ovt_plugin_ffmpeg_smoke_tests PRIVATE "OVT_FFMPEG_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_ffmpeg>\""
                                        "OVT_FFMPEG_FRAMEMD5=\"$<TARGET_FILE:ovt_plugin_ffmpeg_framemd5>\"")
add_dependencies(ovt_plugin_ffmpeg_smoke_tests ovt_plugin_ffmpeg ovt_plugin_ffmpeg_framemd5)
ovt_set_license(ovt_plugin_ffmpeg_smoke_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_ffmpeg_smoke_tests)

# ---- WI-4.100: the FFmpeg patch series applies to the pinned tarball (chapter 07 §7.6.3, chapter 13 §13.3.6) ---
# Only the files the patches touch are extracted, so this stays in the fast lane. With OVT_SYSTEM_FFMPEG there is no
# pinned tarball. SPEC-ASSUMPTION(#331): the FFmpeg build itself applies the series from a later WI.
if(NOT OVT_SYSTEM_FFMPEG AND DEFINED OVT_TP_ffmpeg_ARCHIVE)
  add_test(
    NAME ffmpeg_patches_apply
    COMMAND
      "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check_patches_apply.py" --archive
      "${OVT_TP_ffmpeg_ARCHIVE}" --series "${CMAKE_CURRENT_SOURCE_DIR}/../patches/series" --apply-patches
      "${CMAKE_SOURCE_DIR}/tools/apply_patches.py")
  set_tests_properties(ffmpeg_patches_apply PROPERTIES LABELS "fast;unit;third_party")
endif()

# ---- WI-0.012: symbol-leak test of the plugin flavour (chapter 13 §13.3.3) ------------------------------------------
# The plugin, and a fixture module linked like it (SPEC-ASSUMPTION(#129)), export the two entry points and nothing else.

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
add_test(NAME ffmpeg_plugin_symbol_leak_plugin COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/test_symbol_leak.sh"
                                                       $<TARGET_FILE:ovt_plugin_ffmpeg>)
set_tests_properties(ffmpeg_plugin_symbol_leak_plugin PROPERTIES LABELS "fast;unit;third_party")
