# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fuzz harness (chapter 14 §14.7, chapter 13 §13.2.1, WI-0.557).
#
#   ovt_add_fuzz_target(<name> <source>)
#       <source> defines `extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t)`. It creates:
#         ovt_fuzz_<name>          the libFuzzer executable, in ${CMAKE_BINARY_DIR}/fuzz/ (so
#                                  build/linux-x64-fuzz/fuzz/ovt_fuzz_<name>). Only with OVT_ENABLE_FUZZ=ON
#                                  on Clang; it links ${OVT_FUZZ_ENGINE} (default -fsanitize=fuzzer).
#         ovt_fuzz_<name>_regress  the same source with the replay main of test_fuzz_regressions.cpp, built on
#                                  every compiler (GCC and Apple Clang have no libFuzzer), plus the ctest entry
#                                  fuzz_<name>_regress (labels fast, fuzz-regress). It replays
#                                  regressions/<name>/* (minimised crash inputs) and corpus/<name>/* (seeds).
#
# SPEC-ASSUMPTION(#90): the replay directory is regressions/<name>/ (name without the fuzz_ prefix), as in §14.7.1.
#
# The targets link the module objects (ovt_core_objects), not ovt::core: ovt_core exports only the C API (#62).
# Splitter and parser targets (fuzz_container_*, fuzz_syntax_*) are added by the work items that own those
# parsers (WI-1.238 and the syntax WIs).

function(ovt_add_fuzz_target name source)
  set(regress_dir "${CMAKE_CURRENT_SOURCE_DIR}/regressions/${name}")
  set(corpus_dir "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}")

  add_executable(ovt_fuzz_${name}_regress "${source}" test_fuzz_regressions.cpp)
  target_link_libraries(ovt_fuzz_${name}_regress PRIVATE ovt_core_objects ovt::flags)
  target_include_directories(ovt_fuzz_${name}_regress PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
  ovt_set_license(ovt_fuzz_${name}_regress "LGPL-2.1-or-later")
  add_test(NAME fuzz_${name}_regress COMMAND ovt_fuzz_${name}_regress "${regress_dir}" "${corpus_dir}")
  set_tests_properties(fuzz_${name}_regress PROPERTIES LABELS "fast;fuzz-regress")

  if(OVT_ENABLE_FUZZ AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_executable(ovt_fuzz_${name} "${source}")
    target_link_libraries(ovt_fuzz_${name} PRIVATE ovt_core_objects ovt::flags ${OVT_FUZZ_ENGINE})
    target_include_directories(ovt_fuzz_${name} PRIVATE "${PROJECT_SOURCE_DIR}/core/src")
    set_target_properties(ovt_fuzz_${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
    ovt_set_license(ovt_fuzz_${name} "LGPL-2.1-or-later")
  endif()
endfunction()

ovt_add_fuzz_target(bitreader fuzz_bitreader.cpp)
ovt_add_fuzz_target(user_data fuzz_user_data.cpp)
