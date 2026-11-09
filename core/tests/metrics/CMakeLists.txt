# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the metrics module (chapter 9 §9.2; WI-3.300). They link the module objects directly because
# ovt_core exports only the C API (#62 rule 2). WI-3.300b adds the example plugin, the registry tests and the C
# API test that links ovt::core the way a consumer does; WI-3.300c the job and JobSpec tests.

# ---- Module tests: ABI header (C11 + C++20), NormalizationConfig resolver ----
add_executable(ovt_tests_metrics abi_header_c.c test_metric_abi.cpp test_normalization_config.cpp)
target_include_directories(ovt_tests_metrics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ovt_tests_metrics PRIVATE ovt_core_objects ovt_test_support ovt::flags ${flatbuffers_TARGET}
                                                ${Catch2_TARGET})
target_compile_definitions(ovt_tests_metrics PRIVATE "OVT_METRICS_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\"")
set_source_files_properties(abi_header_c.c PROPERTIES LANGUAGE C)
set_target_properties(ovt_tests_metrics PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
ovt_set_license(ovt_tests_metrics "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_metrics)

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
