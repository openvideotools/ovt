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
