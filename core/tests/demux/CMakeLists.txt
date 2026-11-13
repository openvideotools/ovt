# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the demux module (chapter 08 §8.7, WI-1.230, WI-1.231). They link the module objects directly
# because ovt_core exports only the C API; the core FFmpeg flavour comes with ovt_core_objects.

set(_ovt_unit_split_samples "${CMAKE_CURRENT_BINARY_DIR}/unit_split_samples")
add_executable(ovt_tests_demux demuxer_test.cpp unit_split_test.cpp)
target_include_directories(ovt_tests_demux PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(ovt_tests_demux PRIVATE ovt_core_objects ovt_test_support ovt_schemas ovt::flags ${Catch2_TARGET}
                                              ${nlohmann_json_TARGET} ${flatbuffers_TARGET})
target_compile_definitions(ovt_tests_demux PRIVATE "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
                                                   "OVT_UNIT_SPLIT_SAMPLES_DIR=\"${_ovt_unit_split_samples}\"")
ovt_set_license(ovt_tests_demux "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_demux)

# Acceptance test 1: tools/extract_samples.py writes the first 30 samples of the corpus MP4, then the hidden Catch2
# case splits them. Both skip (77 / Catch2 SKIP) when the corpus is not in the cache.
find_package(Python3 3.11 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(
    NAME unit_split_extract_samples
    COMMAND
      "${Python3_EXECUTABLE}" -I "${PROJECT_SOURCE_DIR}/tools/extract_samples.py" --corpus-id
      syn_h264_multislice_cavlc_mp4 --corpora-dir "${OVT_CORPORA_DIR}" --track 1 --samples 0-29 -o
      "${_ovt_unit_split_samples}")
  set_tests_properties(unit_split_extract_samples PROPERTIES LABELS "fast;needs-corpus" SKIP_RETURN_CODE 77
                                                             FIXTURES_SETUP unit_split_samples)
  add_test(NAME unit_split_corpus COMMAND ovt_tests_demux "[.corpus]")
  set_tests_properties(unit_split_corpus PROPERTIES LABELS "fast;needs-corpus" SKIP_RETURN_CODE 4 FIXTURES_REQUIRED
                                                    unit_split_samples)
endif()
unset(_ovt_unit_split_samples)
