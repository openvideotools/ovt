# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the demux module (chapter 08 §8.7, WI-1.230). They link the module objects directly because
# ovt_core exports only the C API; the core FFmpeg flavour comes with ovt_core_objects.

add_executable(ovt_tests_demux demuxer_test.cpp)
target_link_libraries(ovt_tests_demux PRIVATE ovt_core_objects ovt_test_support ovt_schemas ovt::flags ${Catch2_TARGET}
                                              ${nlohmann_json_TARGET} ${flatbuffers_TARGET})
target_compile_definitions(ovt_tests_demux PRIVATE "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
ovt_set_license(ovt_tests_demux "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_demux)
