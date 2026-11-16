# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Regression test for cmake/OvtCatchAddTests.cmake (test names with ";" or an unbalanced "[" or "]" were
# silently dropped). Runs the discovery script on TEST_EXECUTABLE (ovt_tests_catch_discovery) and checks
#  * every test case that `--list-tests` reports is registered exactly once, under its exact name;
#  * the generated test spec selects exactly that test case;
#  * a tag is turned into a label even when it holds ";".
#
# cmake -DTEST_EXECUTABLE=<exe> -DSCRIPT=<cmake/OvtCatchAddTests.cmake> -DWORK=<dir> -P check_catch_discovery.cmake

cmake_minimum_required(VERSION 3.25)
foreach(var TEST_EXECUTABLE SCRIPT WORK)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "check_catch_discovery.cmake: ${var} is not set")
  endif()
endforeach()

set(ctest_file "${WORK}/catch_discovery_tests.cmake")
file(REMOVE "${ctest_file}")
execute_process(COMMAND "${CMAKE_COMMAND}" -D "TEST_EXECUTABLE=${TEST_EXECUTABLE}" -D "TEST_WORKING_DIR=${WORK}" -D
                        "TEST_EXTRA_LABELS=extra" -D "CTEST_FILE=${ctest_file}" -P "${SCRIPT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "the discovery script failed: ${rc}")
endif()

# The names Catch2 itself reports, one per line.
execute_process(COMMAND "${TEST_EXECUTABLE}" --list-tests --verbosity quiet OUTPUT_VARIABLE listed)
string(REPLACE "\r" "" listed "${listed}")

# Replay the generated file with add_test/set_tests_properties replaced by recorders. Names, specs and
# labels are kept in numbered variables, never in a list, because they contain ";" and "[".
set(n 0)
function(add_test name exe spec)
  math(EXPR i "${n} + 1")
  set(n ${i} PARENT_SCOPE)
  set(name_${i} "${name}" PARENT_SCOPE)
  set(spec_${i} "${spec}" PARENT_SCOPE)
endfunction()
function(set_tests_properties name)
  cmake_parse_arguments(PARSE_ARGV 1 p "" "LABELS;WORKING_DIRECTORY;SKIP_RETURN_CODE;ENVIRONMENT_MODIFICATION" "")
  if(DEFINED p_LABELS)
    set(labels_${n} "${p_LABELS}" PARENT_SCOPE)
  endif()
endfunction()
include("${ctest_file}")

set(expected 0)
set(rest "${listed}")
while(rest MATCHES "^([^\n]*)\n(.*)$")
  set(line "${CMAKE_MATCH_1}")
  set(rest "${CMAKE_MATCH_2}")
  if(line STREQUAL "")
    continue()
  endif()
  math(EXPR expected "${expected} + 1")
  set(found 0)
  foreach(i RANGE 1 ${n})
    if(n GREATER 0 AND "${name_${i}}" STREQUAL "${line}")
      math(EXPR found "${found} + 1")
      set(hit ${i})
    endif()
  endforeach()
  if(NOT found EQUAL 1)
    message(SEND_ERROR "'${line}' registered ${found} times, expected once")
    continue()
  endif()
  # The spec must select exactly this test case and nothing else.
  execute_process(COMMAND "${TEST_EXECUTABLE}" "${spec_${hit}}" --list-tests --verbosity quiet OUTPUT_VARIABLE selected)
  string(REPLACE "\r" "" selected "${selected}")
  string(STRIP "${selected}" selected)
  if(NOT "${selected}" STREQUAL "${line}")
    message(SEND_ERROR "the spec of '${line}' selects '${selected}'")
  endif()
  if(line STREQUAL "discovery: a tag with a semicolon" AND NOT "${labels_${hit}}" MATCHES "(^|;)semi_colon(;|$)")
    message(SEND_ERROR "the tag [semi;colon] did not become the label semi_colon: '${labels_${hit}}'")
  endif()
endwhile()
if(expected LESS 7)
  message(SEND_ERROR "'${TEST_EXECUTABLE} --list-tests' reported ${expected} test cases, expected at least 7")
endif()
if(NOT n EQUAL expected)
  message(SEND_ERROR "${n} tests registered for ${expected} test cases")
endif()
