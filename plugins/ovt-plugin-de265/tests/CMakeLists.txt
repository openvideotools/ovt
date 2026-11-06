# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Tests of the HEVC analysis plugin's third-party build (WI-0.014). The plugin itself arrives with chapter 07
# (WI-2.163); until then plugins/CMakeLists.txt adds only this directory.
#
# The probe links the patched, LGPL-3.0-or-later libde265, so the test target carries the plugin's licence and
# the ovt_plugin_ name prefix: tools/license_gate.py checks it with the plugin rules (chapter 15 §15.4.2), which
# allow LGPL-3.0 links, and tools/license_policy.toml [targets] lists it.

ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)

add_executable(ovt_plugin_de265_probe_tests test_de265_probe.cpp)
target_link_libraries(ovt_plugin_de265_probe_tests PRIVATE tp::de265 ovt::flags ${Catch2_TARGET})
if(NOT OVT_SYSTEM_DE265)
  # The pinned version of third_party/VERSIONS.md; a system libde265 reports its own.
  target_compile_definitions(ovt_plugin_de265_probe_tests
                             PRIVATE "OVT_DE265_PINNED_VERSION=\"${OVT_TP_libde265_VERSION}\"")
endif()
ovt_set_license(ovt_plugin_de265_probe_tests "LGPL-3.0-or-later")
ovt_catch_discover_tests(ovt_plugin_de265_probe_tests)

if(NOT OVT_SYSTEM_DE265)
  # Acceptance test 1 of WI-0.014: the build recorded the applied patches for tools/gen_notices.py.
  externalproject_get_property(tp_libde265 INSTALL_DIR)
  add_test(NAME ovt_plugin_de265_patches_recorded
           COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/check_patches_json.py"
                   "${INSTALL_DIR}/share/ovt/patches.json" "${OVT_DE265_PATCH_SERIES}")
  set_tests_properties(ovt_plugin_de265_patches_recorded PROPERTIES LABELS "fast;third_party")
endif()
