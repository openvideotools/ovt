# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Sanitizer flag plumbing and CTest label registration (chapter 13 §13.2.5, WI-0.004).
#
#   ovt_enable_sanitizers(<target>)
#       Applies the sanitizer set selected by OVT_SANITIZER (when OVT_ENABLE_SANITIZERS=ON) to <target>,
#       plus the compile definition OVT_SANITIZED=1. ovt::flags calls it on itself, so every OVT target
#       gets the flags; call it directly only for a target that cannot link ovt::flags. No-op when
#       sanitizers are off.
#
#   OVT_SANITIZER_COMPILE_FLAGS, OVT_SANITIZER_LINK_FLAGS
#       The active flag lists (empty when sanitizers are off), for scripts that pass them on to
#       third-party builds (WI-0.558).
#
#   OVT_INSTALL_SANITIZER_LINK_FLAGS
#       What a project linking the installed, sanitized libovt_core needs on its own link line: the link flags,
#       plus on Clang (not Apple) -fsanitize-link-c++-runtime and the C++ standard library. Clang links the C++
#       parts of the runtime (the UBSan vptr checks) only when its C++ driver links, and a C consumer uses the C
#       driver. Exported as a usage requirement of ovt::core (core/CMakeLists.txt) and in ovt-core.pc.
#
#   ovt_catch_discover_tests(<target> [EXTRA_LABELS <label>...] [ENVIRONMENT_MODIFICATION <op>...])
#       Registers every Catch2 test case of <target> as a CTest test, at test time (PRE_TEST discovery,
#       which works on macOS when the binary links dylibs from the build tree). Each test gets the CTest
#       labels derived from its Catch2 tags (chapter 14 §14.2.5): a tag `[needs-corpus]` becomes the
#       label `needs_corpus`, `[needs-oracle:ffmpeg]` becomes `needs_oracle_ffmpeg`. Every test also gets
#       the label `unit` and the EXTRA_LABELS. Used by WI-0.550's test framework. It does not depend on
#       the Catch2 version (Catch2 < 3.5 has no ADD_TAGS_AS_LABELS). ENVIRONMENT_MODIFICATION entries
#       (`VAR=op:value`, CTest's property of that name) are set on every test of <target>.
#
#   ovt_add_sanitizer_canary()
#       Adds test_asan_canary, a deliberate heap overflow, in ASan builds only. It carries the label
#       `canary`, which every lane preset excludes, so it runs only when asked for: `ctest -L canary`.
#       It must fail; a pass means ASan is not active.

include_guard(GLOBAL)

set(OVT_SANITIZER_COMPILE_FLAGS "")
set(OVT_SANITIZER_LINK_FLAGS "")
if(OVT_ENABLE_SANITIZERS)
  if(OVT_SANITIZER STREQUAL "asan-ubsan")
    set(OVT_SANITIZER_COMPILE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1
                                    -g)
    set(OVT_SANITIZER_LINK_FLAGS -fsanitize=address,undefined)
  elseif(OVT_SANITIZER STREQUAL "tsan")
    set(OVT_SANITIZER_COMPILE_FLAGS -fsanitize=thread -O1 -g)
    set(OVT_SANITIZER_LINK_FLAGS -fsanitize=thread)
  else() # fuzz: instrumentation only; the fuzz executables add ${OVT_FUZZ_ENGINE} (WI-0.557).
    set(OVT_SANITIZER_COMPILE_FLAGS -fsanitize=fuzzer-no-link,address,undefined)
    set(OVT_SANITIZER_LINK_FLAGS -fsanitize=address,undefined)
  endif()
  message(STATUS "OVT sanitizers: ${OVT_SANITIZER} (${OVT_SANITIZER_COMPILE_FLAGS})")
endif()

set(OVT_INSTALL_SANITIZER_LINK_FLAGS ${OVT_SANITIZER_LINK_FLAGS})
if(OVT_SANITIZER_LINK_FLAGS AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT APPLE)
  list(APPEND OVT_INSTALL_SANITIZER_LINK_FLAGS -fsanitize-link-c++-runtime)
  foreach(_lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
    if(_lib MATCHES "^(stdc\\+\\+|c\\+\\+|c\\+\\+abi)$")
      list(APPEND OVT_INSTALL_SANITIZER_LINK_FLAGS "-l${_lib}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES OVT_INSTALL_SANITIZER_LINK_FLAGS)
  unset(_lib)
endif()

function(ovt_enable_sanitizers target)
  if(NOT OVT_ENABLE_SANITIZERS)
    return()
  endif()
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "INTERFACE_LIBRARY")
    set(scope INTERFACE)
  else()
    set(scope PRIVATE)
  endif()
  target_compile_options(${target} ${scope} ${OVT_SANITIZER_COMPILE_FLAGS})
  target_link_options(${target} ${scope} ${OVT_SANITIZER_LINK_FLAGS})
  target_compile_definitions(${target} ${scope} OVT_SANITIZED=1)
endfunction()

set(_OVT_TESTING_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(ovt_catch_discover_tests target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "EXTRA_LABELS;ENVIRONMENT_MODIFICATION")
  set(ctest_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_ovt_tests.cmake")
  set(include_file "${CMAKE_CURRENT_BINARY_DIR}/${target}_ovt_include.cmake")
  string(JOIN ";" extra ${arg_EXTRA_LABELS})
  string(JOIN ";" env_mods ${arg_ENVIRONMENT_MODIFICATION})
  # Runs inside ctest: lists the test cases and writes add_test()/set_tests_properties() lines, then
  # includes them. Re-running on every ctest invocation keeps the list in step with the binary.
  file(
    GENERATE
    OUTPUT "${include_file}"
    CONTENT
      "if(EXISTS \"$<TARGET_FILE:${target}>\")
  execute_process(COMMAND \"${CMAKE_COMMAND}\"
    -D \"TEST_EXECUTABLE=$<TARGET_FILE:${target}>\"
    -D \"TEST_WORKING_DIR=${CMAKE_CURRENT_BINARY_DIR}\"
    -D \"TEST_EXTRA_LABELS=${extra}\"
    -D \"TEST_ENVIRONMENT_MODIFICATION=${env_mods}\"
    -D \"CTEST_FILE=${ctest_file}\"
    -P \"${_OVT_TESTING_DIR}/OvtCatchAddTests.cmake\"
    RESULT_VARIABLE _ovt_rc)
  if(NOT _ovt_rc EQUAL 0)
    add_test(${target}_NOT_DISCOVERED \"${target}_NOT_DISCOVERED\")
  else()
    include(\"${ctest_file}\")
  endif()
else()
  add_test(${target}_NOT_BUILT \"${target}_NOT_BUILT\")
endif()
")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${include_file}")
endfunction()

function(ovt_add_sanitizer_canary)
  if(NOT (OVT_ENABLE_SANITIZERS AND OVT_SANITIZER STREQUAL "asan-ubsan"))
    return()
  endif()
  add_executable(ovt_asan_canary "${_OVT_TESTING_DIR}/../tests/canary/asan_canary.cpp")
  target_link_libraries(ovt_asan_canary PRIVATE ovt::flags)
  ovt_set_license(ovt_asan_canary "LGPL-2.1-or-later")
  add_test(NAME test_asan_canary COMMAND ovt_asan_canary)
  set_tests_properties(test_asan_canary PROPERTIES LABELS "canary")
endfunction()
