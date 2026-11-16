# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the container module (chapter 08 §8.10, WI-1.200). They link the module objects directly
# because ovt_core exports only the C API.

add_executable(
  ovt_tests_container
  annexb_test.cpp
  box_walk_test.cpp
  c_api_test.cpp
  decoders_movie_test.cpp
  ebml_schema_test.cpp
  field_builder_test.cpp
  ivf_test.cpp
  m3u8_test.cpp
  mpd_test.cpp
  mxf_placeholder_test.cpp
  obu_test.cpp
  psi_test.cpp
  rawyuv_test.cpp
  registry_gen_test.cpp
  registry_lookup_test.cpp
  sniff_test.cpp
  test_container_annexb.cpp
  test_container_ivf.cpp
  test_container_obu.cpp
  test_container_y4m.cpp
  mpegts_pes_test.cpp
  tr101290_test.cpp
  ts_packets_test.cpp
  y4m_test.cpp)
target_include_directories(ovt_tests_container PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(
  ovt_tests_container PRIVATE ovt_core_objects ovt_test_support ovt_schemas ovt::flags ${Catch2_TARGET}
                              ${nlohmann_json_TARGET} ${flatbuffers_TARGET} ${pugixml_TARGET})
target_compile_definitions(ovt_tests_container PRIVATE "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
                                                       "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_container "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_container)

# WI-1.203: the committed generated registry matches a fresh generator run (chapter 08 §8.2.10 step 7). Needs the
# upstream snapshots (fetched from github.com at the pinned commits on first use), so it is not in the fast lane.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME registry_gen_check COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/gen_box_registry.py"
                                           --check)
  set_tests_properties(registry_gen_check PROPERTIES LABELS "registry;tools")
  # WI-4.200: the committed EBML table matches a fresh generator run. The inputs are committed, so it needs no network.
  add_test(NAME ebml_schema_gen_check COMMAND "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/gen_ebml_schema.py"
                                              --check)
  set_tests_properties(ebml_schema_gen_check PROPERTIES LABELS "registry;tools;fast")
endif()
