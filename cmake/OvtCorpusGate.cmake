# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Test-time corpus gate (WI-0.040): cmake -DOVT_CORPUS_INDEX=<file> -DOVT_CORPUS_TAG=<tag>
#   [-DOVT_CORPORA_DIR_DEFAULT=<dir>] -P OvtCorpusGate.cmake [-- <program> [args...]]
#
# Checks that every fetchable entry of the corpus subset <tag> is in the cache (the directory $OVT_CORPORA_DIR when
# set and non-empty, else OVT_CORPORA_DIR_DEFAULT). Present: runs the program (if any) and passes its exit code on
# as pass/fail. Missing: prints `corpus subset '<tag>' missing: run tools/fetch_corpora.py --tags <tag>`, which ctest
# turns into a skip (SKIP_REGULAR_EXPRESSION, ovt_corpus_skip_properties); with OVT_REQUIRE_CORPORA=1 it exits 1 with
# a message that does not match that pattern.

cmake_minimum_required(VERSION 3.25) # script mode starts with the OLD policies (IN_LIST)

include("${CMAKE_CURRENT_LIST_DIR}/OvtCorpusCheck.cmake")

foreach(var OVT_CORPUS_INDEX OVT_CORPUS_TAG)
  if(NOT ${var})
    message(FATAL_ERROR "OvtCorpusGate: -D${var}=... is required")
  endif()
endforeach()

if(DEFINED ENV{OVT_CORPORA_DIR} AND NOT "$ENV{OVT_CORPORA_DIR}" STREQUAL "")
  set(dir "$ENV{OVT_CORPORA_DIR}")
else()
  set(dir "${OVT_CORPORA_DIR_DEFAULT}")
endif()

ovt_corpus_check_index("${OVT_CORPUS_INDEX}" "${OVT_CORPUS_TAG}" "${dir}" present missing)
if(NOT present)
  list(LENGTH missing n_missing)
  if("$ENV{OVT_REQUIRE_CORPORA}" STREQUAL "1")
    message(
      FATAL_ERROR "OVT_REQUIRE_CORPORA=1 and the corpus set '${OVT_CORPUS_TAG}' is incomplete in '${dir}' "
                  "(${n_missing} entries missing); fetch it with: tools/fetch_corpora.py --tags ${OVT_CORPUS_TAG}")
  endif()
  message("corpus subset '${OVT_CORPUS_TAG}' missing: run tools/fetch_corpora.py --tags ${OVT_CORPUS_TAG}")
  return()
endif()

# Everything after "--" is the program to run.
set(gated_command "")
set(seen_separator FALSE)
math(EXPR last_arg "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last_arg})
  if(seen_separator)
    list(APPEND gated_command "${CMAKE_ARGV${i}}")
  elseif("${CMAKE_ARGV${i}}" STREQUAL "--")
    set(seen_separator TRUE)
  endif()
endforeach()
if(gated_command)
  execute_process(COMMAND ${gated_command} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "OvtCorpusGate: command failed (${rc})")
  endif()
endif()
