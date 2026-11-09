# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-2-Clause

# ---- WI-2.155: the plugin, loaded through the host's plugin registry ----------------------------------------------
ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)

add_executable(ovt_plugin_aom_smoke_tests test_aom_plugin_smoke.cpp)
target_link_libraries(ovt_plugin_aom_smoke_tests PRIVATE ovt_core_objects ovt_schemas ovt_test_support ovt::flags
                                                         ${Catch2_TARGET})
target_compile_definitions(ovt_plugin_aom_smoke_tests
                           PRIVATE "OVT_AOM_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_aom>\"")
add_dependencies(ovt_plugin_aom_smoke_tests ovt_plugin_aom)
ovt_set_license(ovt_plugin_aom_smoke_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_aom_smoke_tests)

# ---- acceptance test 2: decoder.cpp against a libaom stub without CONFIG_INSPECTION --------------------------------
# Only libaom's headers are used (the include directories of tp::aom_inspection and tp::aom), not libaom.a.
add_executable(ovt_plugin_aom_no_inspection_tests test_aom_no_inspection.cpp ../src/decoder.cpp
                                                  aom_no_inspection_stub.c)
target_include_directories(
  ovt_plugin_aom_no_inspection_tests SYSTEM
  PRIVATE "$<TARGET_PROPERTY:tp::aom_inspection,INTERFACE_INCLUDE_DIRECTORIES>"
          "$<TARGET_PROPERTY:tp::aom,INTERFACE_INCLUDE_DIRECTORIES>")
target_include_directories(ovt_plugin_aom_no_inspection_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/include"
                                                                      "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(ovt_plugin_aom_no_inspection_tests PRIVATE ovt::flags ${Catch2_TARGET})
add_dependencies(ovt_plugin_aom_no_inspection_tests tp_libaom) # the headers come from the ExternalProject
ovt_set_license(ovt_plugin_aom_no_inspection_tests "BSD-2-Clause")
ovt_catch_discover_tests(ovt_plugin_aom_no_inspection_tests)
