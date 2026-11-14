# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# Runs ovt_plugin_hm_smoke on a corpus file (WI-4.104 acceptance test 1):
#
#   cmake -DPROGRAM=<ovt_plugin_hm_smoke> -DCORPUS_ID=<id> -DDEFAULT_CORPORA_DIR=<dir> -P run_hm_smoke.cmake
#
# The corpus lookup is the one of core/tests/test_support/corpus.cpp: $OVT_CORPORA_DIR (else DEFAULT_CORPORA_DIR)
# /<id>/.ok, a JSON record whose "primary" names the file, or a bare marker next to exactly one file. A missing file
# prints "corpus file missing: <id>" (the test's SKIP_REGULAR_EXPRESSION), or fails with another message when
# OVT_REQUIRE_CORPORA=1.

set(dir "${DEFAULT_CORPORA_DIR}")
if(DEFINED ENV{OVT_CORPORA_DIR} AND NOT "$ENV{OVT_CORPORA_DIR}" STREQUAL "")
  set(dir "$ENV{OVT_CORPORA_DIR}")
endif()
set(entry "${dir}/${CORPUS_ID}")

set(stream "")
if(EXISTS "${entry}/.ok")
  file(READ "${entry}/.ok" record)
  string(JSON primary ERROR_VARIABLE json_error GET "${record}" primary)
  if(NOT json_error AND NOT primary STREQUAL "" AND NOT primary MATCHES "[/\\\\]" AND EXISTS "${entry}/${primary}")
    set(stream "${entry}/${primary}")
  elseif(json_error)
    file(GLOB files LIST_DIRECTORIES false "${entry}/*")
    list(FILTER files EXCLUDE REGEX "/\\.ok$")
    list(LENGTH files count)
    if(count EQUAL 1)
      set(stream "${files}")
    endif()
  endif()
endif()

if(stream STREQUAL "")
  if("$ENV{OVT_REQUIRE_CORPORA}" STREQUAL "1")
    message(FATAL_ERROR "OVT_REQUIRE_CORPORA=1, but the corpus entry ${CORPUS_ID} is not in ${dir}")
  endif()
  message("corpus file missing: ${CORPUS_ID}")
  return()
endif()

execute_process(COMMAND "${PROGRAM}" "${stream}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "ovt_plugin_hm_smoke ${stream} exited with ${rc}")
endif()
