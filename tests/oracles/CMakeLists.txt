# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# In-house oracle programs that link third-party parsers (chapter 14 §14.4.2, §14.4.3a, WI-0.562, WI-0.563):
# gst_parse_oracle and ffmpeg_mvs. hm_cu_dump links HM's own static libraries and is built by build_hm.sh, not here.
#
# Two ways in: the main build includes this directory (tests/CMakeLists.txt) and the oracle image builds it on its
# own (tests/oracles/Dockerfile: `cmake -S . -B build`). Each program is built only where its development files exist
# (gst_parse_oracle: pkg-config gstreamer-codecparsers-1.0 >= 1.24; ffmpeg_mvs: libavformat, libavcodec, libavutil);
# elsewhere it is skipped, because the committed goldens (WI-0.567) make the oracles unnecessary for a normal test run.
#
# gst_parse_oracle links LGPL GStreamer and ffmpeg_mvs links FFmpeg; both are oracle-only: they are installed into the
# oracle image and are never part of a release (the licence gate lists them as system dependencies of a test tool).
# The oracle image builds ffmpeg_mvs with build_ffmpeg_oracle.sh against its own FFmpeg instead.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(ovt_oracles LANGUAGES C)
  include(GNUInstallDirs)
  enable_testing()
endif()

option(OVT_ORACLE_FFMPEG_MVS "Build the ffmpeg_mvs oracle against the system FFmpeg (the oracle image builds its own)"
       OFF)

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
  message(STATUS "pkg-config not found: oracle programs (gst_parse_oracle, ffmpeg_mvs) not built")
  return()
endif()
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)

# --- gst_parse_oracle (WI-0.562) ---------------------------------------------------------------------------------
pkg_check_modules(GST_CODECPARSERS QUIET IMPORTED_TARGET gstreamer-codecparsers-1.0>=1.24)
if(NOT GST_CODECPARSERS_FOUND)
  message(STATUS "gstreamer-codecparsers-1.0 >= 1.24 not found: gst_parse_oracle not built "
                 "(libgstreamer-plugins-bad1.0-dev; the oracle image has it)")
else()
  add_executable(gst_parse_oracle gst_parse_oracle.c)
  target_link_libraries(gst_parse_oracle PRIVATE PkgConfig::GST_CODECPARSERS)
  target_compile_features(gst_parse_oracle PRIVATE c_std_11)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(gst_parse_oracle PRIVATE -Wall -Wextra -Werror)
  endif()
  if(COMMAND ovt_set_license)
    ovt_set_license(gst_parse_oracle "LGPL-2.1-or-later")
    ovt_set_dependency_license(
      PkgConfig::GST_CODECPARSERS
      SOURCE
      system
      SPDX
      "LGPL-2.1-or-later"
      VERSION
      "${GST_CODECPARSERS_VERSION}"
      LINKAGE
      shared)
  endif()
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(TARGETS gst_parse_oracle RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  # Acceptance tests of WI-0.562: run the program on streams encoded with the recipes of §14.3.3. They are skipped
  # (exit 77) when ffmpeg or its x264/x265/libaom encoders are absent.
  if(Python3_Interpreter_FOUND)
    add_test(NAME oracle_gst_parse_oracle
             COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_gst_parse_oracle.py"
                     "$<TARGET_FILE:gst_parse_oracle>")
    set_tests_properties(oracle_gst_parse_oracle PROPERTIES LABELS "fast;needs_oracle_gst_parse_oracle;no_sanitizer"
                                                            SKIP_RETURN_CODE 77)
  endif()
endif()

# --- ffmpeg_mvs (WI-0.563, chapter 14 §14.4.3a) ------------------------------------------------------------------
# Opt-in: it links whatever FFmpeg the machine has (GPL builds are common), so the main build does not pick it up
# by accident. The oracle image compiles ffmpeg_mvs.c itself against its pinned FFmpeg (build_ffmpeg_oracle.sh).
if(OVT_ORACLE_FFMPEG_MVS)
  pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavformat libavcodec libavutil)
  if(NOT LIBAV_FOUND)
    message(FATAL_ERROR "OVT_ORACLE_FFMPEG_MVS needs the libavformat, libavcodec and libavutil development files")
  endif()
  add_executable(ffmpeg_mvs ffmpeg_mvs.c)
  target_link_libraries(ffmpeg_mvs PRIVATE PkgConfig::LIBAV)
  target_compile_features(ffmpeg_mvs PRIVATE c_std_11)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ffmpeg_mvs PRIVATE -Wall -Wextra -Werror)
  endif()
  if(COMMAND ovt_set_license)
    ovt_set_license(ffmpeg_mvs "LGPL-2.1-or-later")
    ovt_set_dependency_license(
      PkgConfig::LIBAV
      SOURCE
      system
      SPDX
      "LGPL-2.1-or-later"
      VERSION
      "${LIBAV_libavcodec_VERSION}"
      LINKAGE
      shared)
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME oracle_ffmpeg_mvs COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_ffmpeg_mvs.py"
                                            "$<TARGET_FILE:ffmpeg_mvs>")
    set_tests_properties(oracle_ffmpeg_mvs PROPERTIES LABELS "fast;needs_oracle_ffmpeg_mvs;no_sanitizer"
                                                      SKIP_RETURN_CODE 77)
  endif()
endif()

# --- hm_cu_dump (WI-0.563, chapter 14 §14.4.4) -------------------------------------------------------------------
# Built by build_hm.sh against HM's libraries; the test runs only where the binary is on PATH or named by the cache
# variable (the oracle image has it).
find_program(OVT_HM_CU_DUMP hm_cu_dump)
if(OVT_HM_CU_DUMP AND Python3_Interpreter_FOUND)
  add_test(NAME oracle_hm_cu_dump COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_hm_cu_dump.py"
                                          "${OVT_HM_CU_DUMP}")
  set_tests_properties(oracle_hm_cu_dump PROPERTIES LABELS "fast;needs_oracle_hm_cu_dump;no_sanitizer" SKIP_RETURN_CODE
                                                    77)
endif()
