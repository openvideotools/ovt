# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ovt command-line tool (chapter 13 §13.2.1). WI-0.000 implements only --version and --help;
# WI-1.500 replaces main.cpp with the CLI11 front-end and adds ovt_cli_lib.

add_executable(ovt_cli src/main.cpp)
target_link_libraries(ovt_cli PRIVATE ovt::core ovt::flags)
target_include_directories(ovt_cli PRIVATE "${OVT_GENERATED_DIR}")
add_dependencies(ovt_cli ovt_version_header)
target_compile_definitions(ovt_cli PRIVATE "OVT_CLI_NAME_VALUE=\"${OVT_CLI_NAME}\"")
set_target_properties(ovt_cli PROPERTIES OUTPUT_NAME "${OVT_CLI_NAME}")
ovt_set_license(ovt_cli "GPL-3.0-or-later")
