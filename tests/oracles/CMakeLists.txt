# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# In-house oracle programs that link third-party parsers (chapter 14 §14.4.2, WI-0.562): gst_parse_oracle.
#
# Two ways in: the main build includes this directory (tests/CMakeLists.txt) and the oracle image builds it on its
# own (tests/oracles/Dockerfile: `cmake -S . -B build`). Either way the program is built only where the GStreamer
# codecparsers development files exist (pkg-config gstreamer-codecparsers-1.0 >= 1.24); elsewhere the directory is
# skipped, because the committed goldens (WI-0.567) make the oracle unnecessary for a normal test run.
#
# gst_parse_oracle links LGPL GStreamer and is oracle-only: it is installed into the oracle image and is never part
# of a release (the licence gate lists GStreamer as a system dependency of a test tool).

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(ovt_oracles LANGUAGES C)
  include(GNUInstallDirs)
  enable_testing()
endif()

find_package(PkgConfig QUIET)
if(NOT PkgConfig_FOUND)
  message(STATUS "pkg-config not found: oracle programs (gst_parse_oracle) not built")
  return()
endif()
pkg_check_modules(GST_CODECPARSERS QUIET IMPORTED_TARGET gstreamer-codecparsers-1.0>=1.24)
if(NOT GST_CODECPARSERS_FOUND)
  message(STATUS "gstreamer-codecparsers-1.0 >= 1.24 not found: gst_parse_oracle not built "
                 "(libgstreamer-plugins-bad1.0-dev; the oracle image has it)")
  return()
endif()

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
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME oracle_gst_parse_oracle
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_gst_parse_oracle.py"
                   "$<TARGET_FILE:gst_parse_oracle>")
  set_tests_properties(oracle_gst_parse_oracle PROPERTIES LABELS "fast;needs_oracle_gst_parse_oracle;no_sanitizer"
                                                          SKIP_RETURN_CODE 77)
endif()
