# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of the index module (chapter 7 §7.4). They link the module objects directly because ovt_core
# exports only the C API (#62); the module-internal headers are included as "index/<name>.h".

# WI-1.119: AV1 reference slot model, set_frame_refs, frame / temporal-unit grouping.
add_executable(ovt_tests_index_av1_slots test_index_av1_slots.cpp)
target_link_libraries(ovt_tests_index_av1_slots PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
target_include_directories(ovt_tests_index_av1_slots PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
ovt_set_license(ovt_tests_index_av1_slots "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_index_av1_slots)

# WI-1.115: H.265 ActiveSets, frame boundary, POC (and, in part b, RPS/DPB).
add_executable(ovt_tests_index_h265 test_index_h265_rps.cpp)
target_link_libraries(ovt_tests_index_h265 PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_index_h265 "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_index_h265)

# WI-1.111: H.264 ActiveSets (part a), frame boundary and POC (part b).
add_executable(ovt_tests_index_h264 test_index_h264_active_sets.cpp test_index_h264_poc.cpp)
target_link_libraries(ovt_tests_index_h264 PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
target_include_directories(ovt_tests_index_h264 PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
# The hand-made Annex B streams of the h264_index fuzz seeds double as a corpus-free end-to-end test.
target_compile_definitions(ovt_tests_index_h264
                           PRIVATE OVT_H264_INDEX_SEEDS="${PROJECT_SOURCE_DIR}/core/tests/fuzz/corpus/h264_index")
ovt_set_license(ovt_tests_index_h264 "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_index_h264)

# WI-1.121: StreamIndex builder (§7.4.2). The libaom-encoded streams of the index_builder fuzz seeds double as a
# corpus-free end-to-end test.
# test_index_vs_ffprobe.cpp (H.264 / H.265, acceptance test 1) runs ffprobe through the helpers of
# core/tests/container/units_oracle.hpp. test_index_lazy.cpp is WI-1.125 (lazy trees, lazy EPB stripping, the job
# wiring and the partial index of ovt_stream_build_index()).
# test_index_av1_index.cpp is WI-1.124 (AV1 declared_refs / dpb_after, operating point, Annex B).
add_executable(ovt_tests_index_builder test_index_builder.cpp test_index_vs_ffprobe.cpp test_stream_api.cpp
                                       test_index_lazy.cpp test_index_av1_index.cpp)
target_link_libraries(ovt_tests_index_builder PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
target_include_directories(ovt_tests_index_builder PRIVATE "${PROJECT_SOURCE_DIR}/core/src"
                                                           "${PROJECT_SOURCE_DIR}/core/tests")
target_compile_definitions(
  ovt_tests_index_builder
  PRIVATE OVT_INDEX_BUILDER_SEEDS="${PROJECT_SOURCE_DIR}/core/tests/fuzz/corpus/index_builder"
          "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"" "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_tests_index_builder "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_index_builder)
