# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the model module (chapter 06 §6.11). They link the module objects directly because ovt_core
# exports only the C API (#62).

# WI-0.083 acceptance test 2: the embedded .bfbs resources and the schema identity accessors.
# WI-0.084: the model helpers (grid, tree, units, labels); test_labels compares labels_golden.txt.
# WI-0.087: the PictureStore and the ovt/decode.h view types.
add_executable(ovt_tests_model test_bfbs_resources.cpp test_grid.cpp test_labels.cpp test_picture_c_api.cpp
                               test_picture_store.cpp test_tree.cpp test_units.cpp)
target_link_libraries(ovt_tests_model PRIVATE "$<BUILD_INTERFACE:ovt_core_objects>" ovt::flags ovt_test_support
                                              ${flatbuffers_TARGET} ${Catch2_TARGET})
target_compile_definitions(ovt_tests_model
                           PRIVATE "OVT_MODEL_LABELS_GOLDEN=\"${CMAKE_CURRENT_SOURCE_DIR}/labels_golden.txt\"")
# ovt_metrics.fbs arrives with WI-0.082. Until then bfbs(Metrics) is empty and its check is skipped; once the
# file exists the check is required (a schema that is present but not embedded fails).
if(EXISTS "${PROJECT_SOURCE_DIR}/core/schema/ovt_metrics.fbs")
  target_compile_definitions(ovt_tests_model PRIVATE OVT_EXPECT_METRICS_BFBS=1)
endif()
ovt_set_license(ovt_tests_model "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_model)

# WI-0.087: the public C headers it adds compile as C11 (chapter 12 §12.3.1 rule 1). A compile-only check.
add_library(ovt_tests_model_c_api OBJECT c_api_decode_compile.c)
target_link_libraries(ovt_tests_model_c_api PRIVATE ovt::flags)
target_include_directories(ovt_tests_model_c_api PRIVATE "${PROJECT_SOURCE_DIR}/core/include")
set_target_properties(ovt_tests_model_c_api PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
ovt_set_license(ovt_tests_model_c_api "LGPL-2.1-or-later")
add_dependencies(ovt_tests_model ovt_tests_model_c_api)

# WI-0.083 acceptance test 3: the namespace-layout Python package imports as the spec writes it. Needs the
# flatbuffers Python runtime (tools/requirements-test.txt); without it the test is skipped (77).
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME test_fb_python_import
    COMMAND
      "${Python3_EXECUTABLE}" -c
      "import importlib.util, sys; sys.exit(77) if importlib.util.find_spec('flatbuffers') is None else None; import ovt.fb.FrameAnalysis as m; m.FrameAnalysis"
  )
  set_tests_properties(test_fb_python_import PROPERTIES ENVIRONMENT "PYTHONPATH=${OVT_GENERATED_DIR}/python" LABELS
                                                        "fast;unit;schema" SKIP_RETURN_CODE 77)
endif()

# WI-0.093: the round-trip test over the golden fixtures of core/tests/golden/ (the test the phase-0 exit gate
# runs, chapter 16 §16.1.1 #10). Needs only the generated schema code.
add_executable(test_model_roundtrip test_model_roundtrip.cpp)
target_link_libraries(test_model_roundtrip PRIVATE ovt_golden_fixtures ovt::flags ${flatbuffers_TARGET}
                                                   ${Catch2_TARGET})
target_compile_definitions(test_model_roundtrip PRIVATE "OVT_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/core/tests/golden\"")
ovt_set_license(test_model_roundtrip "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_model_roundtrip EXTRA_LABELS golden)

# WI-0.089a: the YUView statistics CSV exporter; acceptance test 1 compares the export of
# tiny16_hevc.fa with core/tests/golden/tiny16_hevc.csv (a mismatch writes tiny16_hevc.actual.csv here).
add_executable(test_yuview_csv test_yuview_csv.cpp)
target_link_libraries(test_yuview_csv PRIVATE "$<BUILD_INTERFACE:ovt_core_objects>" ovt::flags ovt_test_support
                                              ${flatbuffers_TARGET} ${Catch2_TARGET})
target_compile_definitions(
  test_yuview_csv
  PRIVATE "OVT_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/core/tests/golden\"" "OVT_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
          "OVT_TEST_OUTPUT_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(test_yuview_csv "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_yuview_csv EXTRA_LABELS golden)

# WI-0.086: the §6.9 validator on the same fixtures (acceptance tests 1-3; mutators in mutate.hpp).
add_executable(test_validator test_validator.cpp)
target_link_libraries(test_validator PRIVATE "$<BUILD_INTERFACE:ovt_core_objects>" ovt::flags ${flatbuffers_TARGET}
                                             ${Catch2_TARGET})
target_compile_definitions(test_validator PRIVATE "OVT_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/core/tests/golden\"")
ovt_set_license(test_validator "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_validator EXTRA_LABELS golden)

# WI-0.090 acceptance test 1: the libaom inspect-compatible JSON export of tiny64_av1.fa against its golden and
# the key set of aom_inspect_keys.txt.
add_executable(test_aom_json test_aom_json.cpp)
target_link_libraries(test_aom_json PRIVATE "$<BUILD_INTERFACE:ovt_core_objects>" ovt::flags ovt_test_support
                                            ${flatbuffers_TARGET} ${Catch2_TARGET})
target_compile_definitions(test_aom_json PRIVATE "OVT_GOLDEN_DIR=\"${PROJECT_SOURCE_DIR}/core/tests/golden\""
                                                 "OVT_TEST_PRODUCT_VERSION=\"${PROJECT_VERSION}${OVT_VERSION_SUFFIX}\"")
ovt_set_license(test_aom_json "LGPL-2.1-or-later")
ovt_catch_discover_tests(test_aom_json EXTRA_LABELS golden)

# WI-0.090 acceptance test 2: tools/aomanalyzer_smoke.js on the golden export (the file test_aom_json proves the
# exporter writes). Registered only when Node.js is found (hosted Linux CI has it; the Mac Mini runner does not).
find_program(OVT_NODE_EXECUTABLE node)
if(OVT_NODE_EXECUTABLE)
  add_test(NAME test_aom_json_smoke
           COMMAND "${OVT_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/aomanalyzer_smoke.js"
                   "${PROJECT_SOURCE_DIR}/core/tests/golden/tiny64_av1.inspect.json" --expect-mi 16x16)
  set_tests_properties(test_aom_json_smoke PROPERTIES LABELS "fast;unit;golden")
  # The script must reject what aomanalyzer cannot load: the wrong MI grid, and a file that is not JSON.
  add_test(NAME test_aom_json_smoke_rejects_grid
           COMMAND "${OVT_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/aomanalyzer_smoke.js"
                   "${PROJECT_SOURCE_DIR}/core/tests/golden/tiny64_av1.inspect.json" --expect-mi 16x15)
  add_test(NAME test_aom_json_smoke_rejects_text
           COMMAND "${OVT_NODE_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/aomanalyzer_smoke.js"
                   "${PROJECT_SOURCE_DIR}/core/tests/golden/aom_inspect_keys.txt")
  set_tests_properties(test_aom_json_smoke_rejects_grid test_aom_json_smoke_rejects_text PROPERTIES LABELS "fast;unit"
                                                                                                    WILL_FAIL TRUE)
endif()
