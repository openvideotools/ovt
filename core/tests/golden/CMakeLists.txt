# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-0.093: the hand-built FrameAnalysis golden fixtures (chapter 06 §6.10, §6.11). The fixtures in this
# directory are written by gen_fixtures from builder code and are never edited by hand; test_model_roundtrip
# (core/tests/model/) reads them, golden_fixtures_regen regenerates them and diffs.

add_library(ovt_golden_fixtures STATIC fixture_hevc.cpp sha256.cpp)
target_include_directories(ovt_golden_fixtures PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# ovt_schemas orders the build after flatc and puts the generated headers on the (system) include path.
target_link_libraries(ovt_golden_fixtures PUBLIC ovt_schemas ${flatbuffers_TARGET} PRIVATE ovt::flags)
ovt_set_license(ovt_golden_fixtures "LGPL-2.1-or-later")

add_executable(gen_fixtures gen_fixtures.cpp)
target_link_libraries(gen_fixtures PRIVATE ovt_golden_fixtures ovt::flags)
ovt_set_license(gen_fixtures "LGPL-2.1-or-later")

# Acceptance tests 1 and 3: determinism, and no drift between builder code and the checked-in files.
add_test(NAME golden_fixtures_regen
         COMMAND "${CMAKE_COMMAND}" -D "GEN=$<TARGET_FILE:gen_fixtures>" -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/regen" -D
                 "GOLDEN=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check_fixtures.cmake")
set_tests_properties(golden_fixtures_regen PROPERTIES LABELS "fast;unit;golden")
