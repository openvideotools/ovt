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
add_executable(ovt_plugin_aom_no_inspection_tests test_aom_no_inspection.cpp ../src/decoder.cpp ../src/planes.cpp
                                                  aom_no_inspection_stub.c)
target_include_directories(
  ovt_plugin_aom_no_inspection_tests SYSTEM
  PRIVATE "$<TARGET_PROPERTY:tp::aom_inspection,INTERFACE_INCLUDE_DIRECTORIES>"
          "$<TARGET_PROPERTY:tp::aom,INTERFACE_INCLUDE_DIRECTORIES>")
target_include_directories(ovt_plugin_aom_no_inspection_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/include"
                                                                      "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(ovt_plugin_aom_no_inspection_tests PRIVATE ovt_schemas ovt::flags ${Catch2_TARGET})
add_dependencies(ovt_plugin_aom_no_inspection_tests tp_libaom) # the headers come from the ExternalProject
ovt_set_license(ovt_plugin_aom_no_inspection_tests "BSD-2-Clause")
ovt_catch_discover_tests(ovt_plugin_aom_no_inspection_tests)

# ---- WI-2.156: insp_mi_data mapping (map_mi.cpp, partition_reconstruct.cpp) --------------------------------------
# The mapping sources are compiled in directly (they need no libaom) for the hand-made frames; the stream tests load
# the plugin through the registry and compare with libaom inspect layers in data/inspect_reference.json.
ovt_find_package(nlohmann_json SPDX "MIT" TARGETS nlohmann_json::nlohmann_json HEADER_ONLY)
add_executable(ovt_plugin_aom_map_mi_tests test_aom_map_mi.cpp ../src/map_mi.cpp ../src/map_mi_ext.cpp
                                           ../src/partition_reconstruct.cpp ../src/fb_builder.cpp ../src/planes.cpp)
target_include_directories(ovt_plugin_aom_map_mi_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
# planes.cpp reads aom_image_t fields: libaom's public headers only, no libaom.a.
target_include_directories(ovt_plugin_aom_map_mi_tests SYSTEM
                           PRIVATE "$<TARGET_PROPERTY:tp::aom,INTERFACE_INCLUDE_DIRECTORIES>")
add_dependencies(ovt_plugin_aom_map_mi_tests tp_libaom)
target_link_libraries(ovt_plugin_aom_map_mi_tests PRIVATE ovt_core_objects ovt_schemas ovt_test_support ovt::flags
                                                          nlohmann_json::nlohmann_json ${Catch2_TARGET})
target_compile_definitions(
  ovt_plugin_aom_map_mi_tests PRIVATE "OVT_AOM_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_aom>\""
                                      "OVT_AOM_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
add_dependencies(ovt_plugin_aom_map_mi_tests ovt_plugin_aom)
ovt_set_license(ovt_plugin_aom_map_mi_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_aom_map_mi_tests)

# ---- WI-2.158: Reconstruction planes (planes.cpp) ------------------------------------------------------------------
# The frame-buffer pool and PlaneRefs are compiled in directly (libaom's public headers only, no libaom.a); the stream
# tests load the plugin through the registry. MD5 comes from the core FFmpeg flavour that ovt_core_objects links.
add_executable(ovt_plugin_aom_planes_tests test_aom_planes.cpp ../src/planes.cpp)
target_include_directories(ovt_plugin_aom_planes_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_include_directories(ovt_plugin_aom_planes_tests SYSTEM
                           PRIVATE "$<TARGET_PROPERTY:tp::aom,INTERFACE_INCLUDE_DIRECTORIES>")
target_link_libraries(ovt_plugin_aom_planes_tests PRIVATE ovt_core_objects ovt_schemas ovt_test_support ovt::flags
                                                          ${Catch2_TARGET})
target_compile_definitions(ovt_plugin_aom_planes_tests
                           PRIVATE "OVT_AOM_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_aom>\"")
add_dependencies(ovt_plugin_aom_planes_tests ovt_plugin_aom tp_libaom)
ovt_set_license(ovt_plugin_aom_planes_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_aom_planes_tests)

# ---- WI-2.159: extension patches 0001/0002 (map_mi_ext.cpp, snapshot planes) ------------------------------------
# Like the map_mi tests: hand-made frames through the mapping sources, and the plugin through the registry against
# `inspect` built with patch 0001 (data/inspect_ext_reference.json).
add_executable(ovt_plugin_aom_map_mi_ext_tests test_aom_map_mi_ext.cpp ../src/map_mi.cpp ../src/map_mi_ext.cpp
                                               ../src/partition_reconstruct.cpp)
target_include_directories(ovt_plugin_aom_map_mi_ext_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(ovt_plugin_aom_map_mi_ext_tests PRIVATE ovt_core_objects ovt_schemas ovt_test_support ovt::flags
                                                              nlohmann_json::nlohmann_json ${Catch2_TARGET})
target_compile_definitions(
  ovt_plugin_aom_map_mi_ext_tests PRIVATE "OVT_AOM_PLUGIN_DIR=\"$<TARGET_FILE_DIR:ovt_plugin_aom>\""
                                          "OVT_AOM_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
add_dependencies(ovt_plugin_aom_map_mi_ext_tests ovt_plugin_aom)
ovt_set_license(ovt_plugin_aom_map_mi_ext_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_plugin_aom_map_mi_ext_tests)
