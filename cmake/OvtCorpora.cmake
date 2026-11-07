# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Corpus access from CMake (chapter 13 §13.2.2, chapter 14 §14.2.4, WI-0.040).
#
# The corpora themselves are never committed or built here: tools/fetch_corpora.py (WI-0.551) downloads and verifies
# them into <cache>/<id>/<file> plus <cache>/<id>/.ok, and tests find them by id (ovt::test::corpus_path). This file
# only answers "is the tagged subset present?" and turns a missing subset into a skip, not a failure.
#
#   OVT_CORPORA_DIR (cache variable, cmake/OvtOptions.cmake)
#       Default corpus cache of this build. The environment variable OVT_CORPORA_DIR, when set and non-empty,
#       wins at test time (same rule as ovt::test::corpora_dir()).
#
#   ovt_corpora_index(<out-var> [MANIFEST <file>])
#       Writes <build>/corpora_index.txt from the manifest (default tests/corpora/manifest.json) once per configure,
#       and sets <out-var> to its path. One line per fetchable entry: `<id>|<tag>,<tag>...`. "Fetchable" is the
#       fetcher's rule: a `url` or `vcs` and neither `unverified` nor `manual_download`. The manifest is a
#       CMAKE_CONFIGURE_DEPENDS of the project. Parsing the manifest in CMake takes several seconds, so call this only
#       from functions that need it (ovt_add_golden_test with CORPUS_TAG does).
#
#   ovt_corpus_set_present(<tag> <out-var> [MANIFEST <file>] [DIR <dir>])
#       Configure-time check: <out-var> is TRUE when every fetchable entry tagged <tag> has <dir>/<id>/.ok
#       (<dir> defaults to $ENV{OVT_CORPORA_DIR} if set, else OVT_CORPORA_DIR). A tag with no fetchable entry counts
#       as missing. Test registration should not depend on this (corpora are fetched after configure); use the gate.
#
#   ovt_corpus_gate_command(<out-var> TAG <tag> [INDEX <file>] COMMAND <program> [args...])
#       Sets <out-var> to a command line that runs cmake/OvtCorpusGate.cmake, which checks the subset when the test
#       runs, prints `corpus subset '<tag>' missing: run tools/fetch_corpora.py --tags <tag>` and then runs COMMAND
#       only when the subset is present. Pair it with ovt_corpus_skip_properties(<test>): the message makes ctest
#       report the test as skipped. With OVT_REQUIRE_CORPORA=1 the gate fails instead, with a message that does not
#       match the skip pattern.
#
#   ovt_corpus_skip_properties(<test>...)
#       Sets SKIP_REGULAR_EXPRESSION on the tests so the gate's message marks them skipped.

include_guard(GLOBAL)

set(_OVT_CORPORA_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
# The one place the message text lives; OvtCorpusGate.cmake builds the same text.
set(OVT_CORPUS_SKIP_PATTERN "corpus subset '[^']*' missing: run tools/fetch_corpora.py --tags [^ \n]*")

function(ovt_corpora_index out_var)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "MANIFEST" "")
  if(NOT arg_MANIFEST)
    set(arg_MANIFEST "${CMAKE_SOURCE_DIR}/tests/corpora/manifest.json")
  endif()
  string(MD5 key "${arg_MANIFEST}")
  set(index "${CMAKE_BINARY_DIR}/corpora_index_${key}.txt")
  get_property(done GLOBAL PROPERTY OVT_CORPORA_INDEX_DONE_${key})
  if(NOT done)
    set_property(GLOBAL APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${arg_MANIFEST}")
    file(READ "${arg_MANIFEST}" text)
    string(JSON entries ERROR_VARIABLE err GET "${text}" entries)
    if(err)
      message(FATAL_ERROR "OvtCorpora: ${arg_MANIFEST} has no \"entries\" array: ${err}")
    endif()
    string(JSON count LENGTH "${entries}")
    set(lines "")
    if(count GREATER 0)
      math(EXPR last "${count} - 1")
      foreach(i RANGE ${last})
        string(JSON entry GET "${entries}" ${i})
        string(JSON id GET "${entry}" id)
        string(JSON unverified ERROR_VARIABLE e1 GET "${entry}" unverified)
        string(JSON manual ERROR_VARIABLE e2 GET "${entry}" manual_download)
        string(JSON url ERROR_VARIABLE e3 GET "${entry}" url)
        string(JSON vcs ERROR_VARIABLE e4 TYPE "${entry}" vcs)
        if(unverified STREQUAL "ON"
           OR unverified STREQUAL "1"
           OR unverified STREQUAL "true"
           OR manual STREQUAL "ON"
           OR manual STREQUAL "1"
           OR manual STREQUAL "true")
          continue()
        endif()
        if(e3 AND e4)
          continue() # neither url nor vcs: generated or placeholder entries are not fetchable
        endif()
        set(tags "")
        string(JSON tag_count ERROR_VARIABLE e5 LENGTH "${entry}" tags)
        if(NOT e5 AND tag_count GREATER 0)
          math(EXPR tag_last "${tag_count} - 1")
          foreach(j RANGE ${tag_last})
            string(JSON tag GET "${entry}" tags ${j})
            list(APPEND tags "${tag}")
          endforeach()
        endif()
        list(JOIN tags "," tag_text)
        string(APPEND lines "${id}|${tag_text}\n")
      endforeach()
    endif()
    file(WRITE "${index}" "${lines}")
    set_property(GLOBAL PROPERTY OVT_CORPORA_INDEX_DONE_${key} TRUE)
  endif()
  set(${out_var} "${index}" PARENT_SCOPE)
endfunction()

function(ovt_corpus_set_present tag out_var)
  cmake_parse_arguments(PARSE_ARGV 2 arg "" "MANIFEST;DIR" "")
  if(NOT arg_DIR)
    if(DEFINED ENV{OVT_CORPORA_DIR} AND NOT "$ENV{OVT_CORPORA_DIR}" STREQUAL "")
      set(arg_DIR "$ENV{OVT_CORPORA_DIR}")
    else()
      set(arg_DIR "${OVT_CORPORA_DIR}")
    endif()
  endif()
  if(arg_MANIFEST)
    ovt_corpora_index(index MANIFEST "${arg_MANIFEST}")
  else()
    ovt_corpora_index(index)
  endif()
  include("${_OVT_CORPORA_CMAKE_DIR}/OvtCorpusCheck.cmake")
  ovt_corpus_check_index("${index}" "${tag}" "${arg_DIR}" present missing)
  set(${out_var} ${present} PARENT_SCOPE)
endfunction()

function(ovt_corpus_gate_command out_var)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "TAG;INDEX" "COMMAND")
  if(NOT arg_TAG OR NOT arg_COMMAND)
    message(FATAL_ERROR "ovt_corpus_gate_command: TAG and COMMAND are required")
  endif()
  if(NOT arg_INDEX)
    ovt_corpora_index(arg_INDEX)
  endif()
  set(${out_var}
      "${CMAKE_COMMAND}" "-DOVT_CORPUS_INDEX=${arg_INDEX}" "-DOVT_CORPUS_TAG=${arg_TAG}"
      "-DOVT_CORPORA_DIR_DEFAULT=${OVT_CORPORA_DIR}" -P "${_OVT_CORPORA_CMAKE_DIR}/OvtCorpusGate.cmake" --
      ${arg_COMMAND}
      PARENT_SCOPE)
endfunction()

function(ovt_corpus_skip_properties)
  set_tests_properties(${ARGV} PROPERTIES SKIP_REGULAR_EXPRESSION "${OVT_CORPUS_SKIP_PATTERN}")
endfunction()
