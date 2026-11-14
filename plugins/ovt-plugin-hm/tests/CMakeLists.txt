# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Tests of the HEVC oracle plugin (chapter 07 §7.6.4): the smoke program of the HM build (WI-4.104) and the plugin
# itself (WI-4.105).
#
# The smoke program links HM's BSD-3-Clause libraries and nothing of OVT, so it carries the plugin's licence and the
# ovt_plugin_ name prefix (tools/license_policy.toml [targets]). HM's headers are included as system headers: HM's
# warnings are not OVT's (WI-4.104 pitfall), while the program's own code builds with ovt::flags.

add_executable(ovt_plugin_hm_smoke hm_smoke.cpp)
target_link_libraries(ovt_plugin_hm_smoke PRIVATE tp::hm ovt::flags)
get_target_property(_ovt_hm_includes tp::hm_common INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(_ovt_hm_decoder_includes tp::hm INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(ovt_plugin_hm_smoke SYSTEM PRIVATE ${_ovt_hm_includes} ${_ovt_hm_decoder_includes})
unset(_ovt_hm_includes)
unset(_ovt_hm_decoder_includes)
ovt_set_license(ovt_plugin_hm_smoke "BSD-3-Clause")

# Acceptance test 1 of WI-4.104 on the conformance stream WPP_A_ericsson_MAIN_2 (tools/fetch_corpora.py). Without the
# corpus file the test is skipped, or fails when OVT_REQUIRE_CORPORA=1, like the [needs-corpus] Catch2 tests.
add_test(
  NAME ovt_plugin_hm_smoke_wpp_a
  COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:ovt_plugin_hm_smoke>" -DCORPUS_ID=jctvc_WPP_A_ericsson_MAIN_2
          "-DDEFAULT_CORPORA_DIR=${OVT_CORPORA_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/run_hm_smoke.cmake")
set_tests_properties(ovt_plugin_hm_smoke_wpp_a PROPERTIES LABELS "third_party;needs-corpus" SKIP_REGULAR_EXPRESSION
                                                          "corpus file missing: ")

# ---- WI-4.105: the plugin, loaded through the host's plugin registry ----------------------------------------------
# Loads libovt_plugin_hm through the host's PluginRegistry (LGPL-2.1-or-later, so the test is too, as for
# ovt_plugin_aom_smoke_tests); MD5 comes from HM's public-domain libmd5 in tp::hm_common.
ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)
add_executable(ovt_plugin_hm_tests test_hm_plugin.cpp)
target_link_libraries(ovt_plugin_hm_tests PRIVATE ovt_core_objects ovt_schemas ovt_test_support tp::hm_common
                                                  ovt::flags nlohmann_json::nlohmann_json ${Catch2_TARGET})
get_target_property(_ovt_hm_includes tp::hm_common INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(ovt_plugin_hm_tests SYSTEM PRIVATE ${_ovt_hm_includes})
unset(_ovt_hm_includes)
target_compile_definitions(ovt_plugin_hm_tests PRIVATE "OVT_HM_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_hm>\""
                                                   "OVT_HM_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/tests/golden/h265\"")
add_dependencies(ovt_plugin_hm_tests ovt_plugin_hm)
ovt_set_license(ovt_plugin_hm_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_hm_tests)
