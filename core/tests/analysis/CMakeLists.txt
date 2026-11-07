# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the analysis module (chapter 7 §7.5; WI-2.150 plugin registry). They link the module objects
# directly because ovt_core exports only the C API, and never ovt::core as well (#62 rule 2).

add_subdirectory(mock_plugin)

add_executable(ovt_tests_analysis test_analysis_plugin_registry.cpp abi_header_c_first.c abi_header_c_second.c)
target_link_libraries(ovt_tests_analysis PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
target_compile_definitions(ovt_tests_analysis PRIVATE "OVT_MOCK_PLUGIN_ROOT=\"${OVT_MOCK_PLUGIN_ROOT}\"")
get_property(_ovt_mock_plugins GLOBAL PROPERTY OVT_MOCK_PLUGIN_TARGETS)
add_dependencies(ovt_tests_analysis ${_ovt_mock_plugins})
ovt_set_license(ovt_tests_analysis "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_analysis)
# #211: the two copies of ovt_status/ovt_Buffer (ovt/status.h, ovt/abi/analysis_decoder_v1.h) stay identical.
set(_ovt_inc "${PROJECT_SOURCE_DIR}/core/include/ovt")
add_test(NAME analysis_status_copies_match
         COMMAND ${CMAKE_COMMAND} -Dstatus=${_ovt_inc}/status.h -Dabi=${_ovt_inc}/abi/analysis_decoder_v1.h -P
                 "${CMAKE_CURRENT_SOURCE_DIR}/check_status_copies.cmake")
set_tests_properties(analysis_status_copies_match PROPERTIES LABELS "fast;unit;analysis")
