# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the metrics module (chapter 9 §9.2; WI-3.300). They link the module objects directly because
# ovt_core exports only the C API (#62 rule 2); the C API test links ovt::core instead, the way a consumer does.

add_subdirectory(example_plugin)

# ---- Module tests: ABI header (C11 + C++20), NormalizationConfig resolver, JobSpec parser, registry, job ----
add_executable(
  ovt_tests_metrics
  abi_header_c.c
  test_metric_abi.cpp
  test_metric_job.cpp
  test_metrics_jobspec.cpp
  test_metrics_plugin_registry.cpp
  test_metrics_psnr.cpp
  test_metrics_ssim.cpp
  test_metrics_ssim_x264.cpp
  test_metrics_identity.cpp
  test_normalization_config.cpp)
target_include_directories(ovt_tests_metrics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ovt_tests_metrics PRIVATE ovt_core_objects ovt_test_support ovt::flags ${flatbuffers_TARGET}
                                                ${Catch2_TARGET})
target_compile_definitions(
  ovt_tests_metrics
  PRIVATE "OVT_EXAMPLE_METRIC_ROOT=\"${OVT_EXAMPLE_METRIC_ROOT}\""
          "OVT_METRICS_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\""
          "OVT_CORPORA_COMPARE_EXPECTED=\"${PROJECT_SOURCE_DIR}/tests/corpora/compare_expected.json\"")
set_source_files_properties(abi_header_c.c PROPERTIES LANGUAGE C)
set_target_properties(ovt_tests_metrics PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
get_property(_ovt_example_metrics GLOBAL PROPERTY OVT_EXAMPLE_METRIC_TARGETS)
add_dependencies(ovt_tests_metrics ${_ovt_example_metrics})
ovt_set_license(ovt_tests_metrics "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_metrics)

# ---- C API test: ovt/metrics.h through the shared library (acceptance test 3 via ovt_metrics_list_descriptors) --
add_executable(ovt_tests_metrics_c_api test_metrics_c_api.cpp)
target_link_libraries(ovt_tests_metrics_c_api PRIVATE ovt::core ovt::flags ovt_schemas ${flatbuffers_TARGET}
                                                      ${nlohmann_json_TARGET} ${Catch2_TARGET})
target_compile_definitions(ovt_tests_metrics_c_api PRIVATE "OVT_EXAMPLE_METRIC_ROOT=\"${OVT_EXAMPLE_METRIC_ROOT}\"")
add_dependencies(ovt_tests_metrics_c_api ${_ovt_example_metrics})
ovt_set_license(ovt_tests_metrics_c_api "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_metrics_c_api)
unset(_ovt_example_metrics)

# ---- The three type blocks ovt/abi/metric_v1.h repeats from the LGPL headers stay textually identical (#211) --
set(_ovt_inc "${PROJECT_SOURCE_DIR}/core/include/ovt")
set(_ovt_copies "${CMAKE_CURRENT_SOURCE_DIR}/check_header_copies.cmake")
add_test(NAME metrics_abi_status_copies_match
         COMMAND ${CMAKE_COMMAND} -Da=${_ovt_inc}/status.h -Db=${_ovt_inc}/abi/metric_v1.h
                 -Dguard=OVT_STATUS_TYPES_DEFINED -P "${_ovt_copies}")
add_test(NAME metrics_abi_frame_id_copies_match
         COMMAND ${CMAKE_COMMAND} -Da=${_ovt_inc}/decode.h -Db=${_ovt_inc}/abi/metric_v1.h -Dguard=OVT_FRAME_ID_DEFINED
                 -P "${_ovt_copies}")
add_test(NAME metrics_abi_color_copies_match
         COMMAND ${CMAKE_COMMAND} -Da=${_ovt_inc}/color.h -Db=${_ovt_inc}/abi/metric_v1.h
                 -Dguard=OVT_COLOR_DESCRIPTION_DEFINED -P "${_ovt_copies}")
set_tests_properties(metrics_abi_status_copies_match metrics_abi_frame_id_copies_match metrics_abi_color_copies_match
                     PROPERTIES LABELS "fast;unit;metrics")
unset(_ovt_inc)
unset(_ovt_copies)
