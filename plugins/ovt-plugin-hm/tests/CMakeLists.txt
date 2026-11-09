# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Smoke test of the HM build (chapter 07 §7.6.4, WI-4.104). The plugin itself arrives with WI-4.105; until then
# plugins/CMakeLists.txt adds only this directory, and only with OVT_BUILD_PLUGIN_HM=ON.
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
