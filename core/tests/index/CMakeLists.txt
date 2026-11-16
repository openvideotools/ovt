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
add_executable(ovt_tests_index_builder test_index_builder.cpp)
target_link_libraries(ovt_tests_index_builder PRIVATE ovt_core_objects ovt_test_support ovt::flags ${Catch2_TARGET})
target_include_directories(ovt_tests_index_builder PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
target_compile_definitions(ovt_tests_index_builder
                           PRIVATE OVT_INDEX_BUILDER_SEEDS="${PROJECT_SOURCE_DIR}/core/tests/fuzz/corpus/index_builder")
ovt_set_license(ovt_tests_index_builder "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_index_builder)
