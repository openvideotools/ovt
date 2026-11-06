# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Unit tests of libovt-core (chapter 14 §14.2, WI-0.550).
#
# Catch2 linkage rule (D24, chapter 14 §14.2.1): every test executable links Catch2::Catch2WithMain and
# has no main of its own, except the golden runner ovt_core_tests, which links Catch2::Catch2 and brings
# main.cpp (logger hook, OVT_UPDATE_GOLDEN / OVT_REQUIRE_CORPORA / OVT_CORPORA_DIR).
#
# Tests are registered with ovt_catch_discover_tests() (cmake/OvtTesting.cmake): Catch2 tags become CTest
# labels, so the lane presets of CMakePresets.json (pr = label fast, …) select them. Every test case
# carries exactly one speed tag, [fast] or [slow] (chapter 14 §14.2.5).
#
# A module's tests go in core/tests/<module>/ with their own executable ovt_tests_<module> (linking
# ovt_test_support and Catch2WithMain), or into ovt_core_tests when they compare golden files.

ovt_find_package(Catch2 SPDX "BSL-1.0" TARGETS Catch2::Catch2WithMain)
if(NOT TARGET Catch2::Catch2)
  message(FATAL_ERROR "Catch2::Catch2 (without main) is required for the golden runner ovt_core_tests.")
endif()

# ---- Test support library: corpus lookup, golden comparison, normalisation -----------------------
add_library(ovt_test_support STATIC test_support/corpus.cpp test_support/env.cpp test_support/fb_normalize.cpp
                                    test_support/golden.cpp test_support/golden_compare.cpp)
target_include_directories(ovt_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(ovt_test_support PRIVATE "OVT_TEST_DEFAULT_CORPORA_DIR=\"${OVT_CORPORA_DIR}\"")
# Catch2::Catch2 (not WithMain): the support code uses SKIP/FAIL/WARN; the executable decides about main.
# nlohmann_json is PUBLIC: golden_compare.h and fb_normalize.h expose nlohmann::json (WI-0.556). ovt_schemas (the
# generated FlatBuffers headers) is private: the family writers read the tables, callers see forward declarations.
target_link_libraries(ovt_test_support PUBLIC Catch2::Catch2 ${nlohmann_json_TARGET}
                      PRIVATE ovt::flags ${fmt_TARGET} ${flatbuffers_TARGET} ovt_schemas)
ovt_set_license(ovt_test_support "LGPL-2.1-or-later")

# ---- Golden runner: the one test executable with its own main -------------------------------------
add_executable(ovt_core_tests main.cpp test_support_corpus.cpp test_support_flatbuffer.cpp test_support_golden.cpp
                              test_golden_compare.cpp test_fb_normalize_families.cpp)
target_link_libraries(ovt_core_tests PRIVATE ovt::core ovt::flags ovt_test_support ovt_schemas Catch2::Catch2
                                             ${flatbuffers_TARGET} ${nlohmann_json_TARGET})
target_compile_definitions(ovt_core_tests PRIVATE "OVT_TEST_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
                                                  "OVT_TEST_BINARY_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
ovt_set_license(ovt_core_tests "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_core_tests)

# ---- Ordinary test executables (Catch2WithMain) ---------------------------------------------------
add_executable(ovt_tests_version test_version.cpp)
target_link_libraries(ovt_tests_version PRIVATE ovt::core ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_version "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_version)

# ovt::Result<T> (WI-1.512): header only; linked against the module objects for their include paths.
add_executable(ovt_tests_result test_result.cpp)
target_link_libraries(ovt_tests_result PRIVATE ovt_core_objects ovt::flags ${Catch2_TARGET})
ovt_set_license(ovt_tests_result "LGPL-2.1-or-later")
ovt_catch_discover_tests(ovt_tests_result)

# ---- Module tests ---------------------------------------------------------------------------------
add_subdirectory(io)
add_subdirectory(bitreader)
add_subdirectory(fuzz) # WI-0.557: libFuzzer targets and their replay tests
add_subdirectory(jobs)
add_subdirectory(log)
add_subdirectory(model)
add_subdirectory(syntax)

# ---- Schema code generation (WI-0.003) ---------------------------------------------------------------
add_subdirectory(schema)

# ---- Third-party probes (build scripts of chapter 13 §13.3) ---------------------------------------
# WI-0.011: tp::dav1d (Meson wrapper) links into a test that calls dav1d_version().
if(TARGET tp::dav1d)
  add_executable(ovt_tests_dav1d_probe test_dav1d_probe.cpp)
  target_link_libraries(ovt_tests_dav1d_probe PRIVATE tp::dav1d ovt::flags ${Catch2_TARGET})
  ovt_set_license(ovt_tests_dav1d_probe "LGPL-2.1-or-later")
  ovt_catch_discover_tests(ovt_tests_dav1d_probe)
endif()
