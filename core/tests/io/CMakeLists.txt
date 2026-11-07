# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the io module (chapter 7 §7.1.5, WI-1.100) and of open_fd and ovt/io.h (WI-1.101). They link the module objects directly because
# ovt_core exports only the C API.

add_executable(ovt_tests_io test_io_random_access_file.cpp test_io_fd.cpp)
target_link_libraries(ovt_tests_io PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
target_compile_definitions(ovt_tests_io PRIVATE "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_io "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_io)
