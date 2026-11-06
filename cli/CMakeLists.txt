# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ovt command-line tool (chapter 12 §12.1, WI-1.500).
#   ovt_cli_lib  the front-end: CLI11 tree, global options, exit codes, logger, output writers
#   ovt_cli      the executable (output name ${OVT_CLI_NAME})

ovt_find_package(CLI11 SPDX "BSD-3-Clause" TARGETS CLI11::CLI11 HEADER_ONLY)
# Imported targets are directory-scoped, so the core's dependencies are looked up again here.
ovt_find_package(fmt SPDX "MIT" TARGETS fmt::fmt)
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)

add_library(ovt_cli_lib STATIC src/app.cpp src/output/json_writer.cpp)
target_include_directories(ovt_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src" "${OVT_GENERATED_DIR}")
target_link_libraries(ovt_cli_lib PUBLIC ovt::core ${CLI11_TARGET} ${fmt_TARGET} PRIVATE ovt::flags
                                                                                         ${nlohmann_json_TARGET})
target_compile_definitions(ovt_cli_lib PUBLIC "OVT_CLI_NAME_VALUE=\"${OVT_CLI_NAME}\""
                           PRIVATE "OVT_CLI_VERSION_VALUE=\"${PROJECT_VERSION}${OVT_VERSION_SUFFIX}\"")
add_dependencies(ovt_cli_lib ovt_version_header)
ovt_set_license(ovt_cli_lib "GPL-3.0-or-later")

add_executable(ovt_cli src/main.cpp)
target_link_libraries(ovt_cli PRIVATE ovt_cli_lib ovt::flags)
set_target_properties(ovt_cli PROPERTIES OUTPUT_NAME "${OVT_CLI_NAME}")
ovt_set_license(ovt_cli "GPL-3.0-or-later")

if(OVT_BUILD_TESTS)
  ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
  add_executable(ovt_tests_cli tests/test_globals.cpp)
  target_link_libraries(ovt_tests_cli PRIVATE ovt_cli_lib ovt::flags ${nlohmann_json_TARGET} ${Catch2_TARGET})
  ovt_set_license(ovt_tests_cli "GPL-3.0-or-later")
  ovt_catch_discover_tests(ovt_tests_cli)

  # The real binary, for acceptance test 1 (exit status and the end of stderr).
  add_test(NAME cli_no_args COMMAND "${CMAKE_COMMAND}" -DOVT_EXE=$<TARGET_FILE:ovt_cli> -P
                                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_no_args.cmake")
  set_tests_properties(cli_no_args PROPERTIES LABELS "fast;unit;cli")
endif()
