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

add_executable(ovt_plugin_ffmpeg_sidedata ffmpeg_sidedata_csv.cpp)
target_link_libraries(ovt_plugin_ffmpeg_sidedata PRIVATE tp::ffmpeg_plugin ovt::flags)
ovt_set_license(ovt_plugin_ffmpeg_sidedata "LGPL-2.1-or-later")

# The internals of the plugin (WI-2.170): compiled with the static plugin flavour, as the plugin itself is.
# test_ffmpeg_hevc_internals.cpp (WI-4.107) compiles to nothing without patch 0002 (OVT_SYSTEM_FFMPEG).
# test_ffmpeg_blockparams_internals.cpp (WI-4.102) checks the block-params mapping; most of it needs patch 0001.
add_executable(
  ovt_plugin_ffmpeg_internal_tests
  test_ffmpeg_h264_internals.cpp test_ffmpeg_hevc_internals.cpp test_ffmpeg_blockparams_internals.cpp
  ../src/h264_analysis.cpp ../src/map_h264_blockparams.cpp ../src/map_h264_sidedata.cpp ../src/map_hevc_sidedata.cpp)
target_include_directories(ovt_plugin_ffmpeg_internal_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/include" ../src)
target_compile_definitions(ovt_plugin_ffmpeg_internal_tests
                           PRIVATE "OVT_FFMPEG_HEVC_ENC_PARAMS=${OVT_FFMPEG_HEVC_ENC_PARAMS}")
target_link_libraries(ovt_plugin_ffmpeg_internal_tests PRIVATE tp::ffmpeg_plugin ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_plugin_ffmpeg_internal_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_ffmpeg_internal_tests)

add_executable(ovt_plugin_ffmpeg_smoke_tests test_ffmpeg_plugin_smoke.cpp test_ffmpeg_map_sidedata.cpp
                                             test_ffmpeg_map_blockparams.cpp)
target_link_libraries(ovt_plugin_ffmpeg_smoke_tests PRIVATE ovt_core_objects tp::ffmpeg_core ovt_schemas
                                                            ovt_test_support ovt::flags ${Catch2_TARGET})
target_compile_definitions(ovt_plugin_ffmpeg_smoke_tests PRIVATE "OVT_FFMPEG_SYSTEM=$<BOOL:${OVT_SYSTEM_FFMPEG}>"
                                                                 "OVT_FFMPEG_HW_DECODE=$<BOOL:${OVT_ENABLE_HW_DECODE}>")
target_compile_definitions(
  ovt_plugin_ffmpeg_smoke_tests
  PRIVATE "OVT_FFMPEG_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_ffmpeg>\""
          "OVT_FFMPEG_FRAMEMD5=\"$<TARGET_FILE:ovt_plugin_ffmpeg_framemd5>\""
          "OVT_FFMPEG_SIDEDATA=\"$<TARGET_FILE:ovt_plugin_ffmpeg_sidedata>\""
          "OVT_JM_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/tests/golden/h264\"")
add_dependencies(ovt_plugin_ffmpeg_smoke_tests ovt_plugin_ffmpeg ovt_plugin_ffmpeg_framemd5 ovt_plugin_ffmpeg_sidedata)
ovt_set_license(ovt_plugin_ffmpeg_smoke_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_ffmpeg_smoke_tests)

# ---- WI-4.101: the block-params export of the patched plugin flavour against the JM goldens ------------------------
# ovt_plugin_ffmpeg_blockparams prints AV_FRAME_DATA_VIDEO_BLOCK_PARAMS; ffmpeg_mbparams_vs_jm compares it with
# tests/golden/h264/<id>.jm-trace.json.gz and checks frame threads against one thread. Only third_party/ffmpeg.cmake
# applies the patch series (to the plugin flavour), so a system FFmpeg has no block params to test.
if(NOT OVT_SYSTEM_FFMPEG)
  add_executable(ovt_plugin_ffmpeg_blockparams ffmpeg_blockparams_dump.cpp)
  target_link_libraries(ovt_plugin_ffmpeg_blockparams PRIVATE tp::ffmpeg_plugin ovt::flags)
  ovt_set_license(ovt_plugin_ffmpeg_blockparams "LGPL-2.1-or-later")

  ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)
  add_executable(ovt_plugin_ffmpeg_blockparams_tests test_ffmpeg_blockparams.cpp)
  # WI-4.102: it also loads the plugin (host registry, core FFmpeg flavour) to compare its AvcMb tables.
  target_link_libraries(
    ovt_plugin_ffmpeg_blockparams_tests PRIVATE ovt_core_objects tp::ffmpeg_core ovt_schemas ovt_test_support
                                                ovt::flags ${nlohmann_json_TARGET} ${Catch2_TARGET})
  target_compile_definitions(
    ovt_plugin_ffmpeg_blockparams_tests
    PRIVATE "OVT_FFMPEG_BLOCKPARAMS=\"$<TARGET_FILE:ovt_plugin_ffmpeg_blockparams>\""
            "OVT_FFMPEG_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_ffmpeg>\""
            "OVT_JM_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/tests/golden/h264\"")
  add_dependencies(ovt_plugin_ffmpeg_blockparams_tests ovt_plugin_ffmpeg_blockparams ovt_plugin_ffmpeg)
  ovt_set_license(ovt_plugin_ffmpeg_blockparams_tests "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_plugin_ffmpeg_blockparams_tests)
endif()

# ---- WI-4.107: the optional ffmpeg_hevc plugin against libde265 ----------------------------------------------------
# Acceptance test 1 compares the plugin's MV/QP grid with ovt_plugin_de265_grid_dump (plugins/ovt-plugin-de265/tests,
# patched libde265). That directory is added after this one, so the conditions are its own, not if(TARGET).
if(NOT OVT_SYSTEM_FFMPEG)
  add_executable(ovt_plugin_ffmpeg_hevc_tests test_ffmpeg_hevc_sidedata.cpp)
  target_link_libraries(ovt_plugin_ffmpeg_hevc_tests PRIVATE ovt_core_objects tp::ffmpeg_core ovt_schemas
                                                             ovt_test_support ovt::flags ${Catch2_TARGET})
  target_compile_definitions(ovt_plugin_ffmpeg_hevc_tests
                             PRIVATE "OVT_FFMPEG_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_ffmpeg_hevc>\"")
  add_dependencies(ovt_plugin_ffmpeg_hevc_tests ovt_plugin_ffmpeg_hevc ovt_plugin_ffmpeg)
  if(OVT_BUILD_PLUGIN_DE265 AND TARGET tp::de265 AND NOT OVT_SYSTEM_DE265)
    target_compile_definitions(ovt_plugin_ffmpeg_hevc_tests
                               PRIVATE "OVT_DE265_GRID_DUMP=\"$<TARGET_FILE:ovt_plugin_de265_grid_dump>\"")
    add_dependencies(ovt_plugin_ffmpeg_hevc_tests ovt_plugin_de265_grid_dump)
  endif()
  ovt_set_license(ovt_plugin_ffmpeg_hevc_tests "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_plugin_ffmpeg_hevc_tests)
endif()

# ---- WI-4.100: the FFmpeg patch series applies to the pinned tarball (chapter 07 §7.6.3, chapter 13 §13.3.6) ---
# Only the files the patches touch are extracted, so this stays in the fast lane. With OVT_SYSTEM_FFMPEG there is no
# pinned tarball. SPEC-ASSUMPTION(#331): third_party/ffmpeg.cmake applies the series to the plugin flavour (WI-4.101).
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
add_test(NAME ffmpeg_plugin_symbol_leak_plugin_hevc COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/test_symbol_leak.sh"
                                                            $<TARGET_FILE:ovt_plugin_ffmpeg_hevc>)
set_tests_properties(ffmpeg_plugin_symbol_leak_plugin_hevc PROPERTIES LABELS "fast;unit;third_party")
