# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the model module (chapter 06 §6.11). They link the module objects directly because ovt_core
# exports only the C API (#62).

# WI-0.083 acceptance test 2: the embedded .bfbs resources and the schema identity accessors.
# WI-0.084: the model helpers (grid, tree, units, labels); test_labels compares labels_golden.txt.
add_executable(ovt_tests_model test_bfbs_resources.cpp test_grid.cpp test_labels.cpp test_tree.cpp test_units.cpp)
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
