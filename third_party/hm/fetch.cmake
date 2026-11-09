# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Download step of third_party/hm.cmake (chapter 07 §7.6.4, WI-4.104), run as
#
#   cmake -DGIT=<git> -DURL=<repo> -DTAG=<tag> -DCOMMIT=<40 hex> -DDEST=<dir> -P fetch.cmake
#
# HM publishes no release tarball with a stable hash, so the VERSIONS.md row pins a commit instead (§13.3.7: "a git
# source needs GIT_TAG = full commit SHA"). ExternalProject_Add(GIT_TAG <sha>) would clone HM's whole history; this
# script fetches only the tag, one commit deep, and fails unless the tag points at the pinned commit. Git checks
# every object against its id, so the checked-out tree is the pinned one.

foreach(var GIT URL TAG COMMIT DEST)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "hm fetch.cmake: -D${var}= is missing")
  endif()
endforeach()
string(LENGTH "${COMMIT}" _len)
if(NOT COMMIT MATCHES "^[0-9a-f]+$" OR NOT _len EQUAL 40)
  message(FATAL_ERROR "hm fetch.cmake: the pin '${COMMIT}' is not a full 40-hex commit id")
endif()

function(_hm_git)
  execute_process(
    COMMAND "${GIT}" ${ARGN}
    WORKING_DIRECTORY "${DEST}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "hm fetch.cmake: git ${ARGN} failed in ${DEST}:\n${err}")
  endif()
  set(_hm_out "${out}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")
_hm_git(init -q)
_hm_git(remote add origin "${URL}")
# Transient network failures are retried with backoff, as ovt_tp_declare() does for archives.
set(_delay 5)
foreach(_attempt RANGE 1 5)
  execute_process(COMMAND "${GIT}" fetch -q --depth 1 origin "+refs/tags/${TAG}:refs/tags/${TAG}"
                  WORKING_DIRECTORY "${DEST}" RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(_rc EQUAL 0)
    break()
  endif()
  if(_attempt EQUAL 5)
    message(FATAL_ERROR "hm fetch.cmake: fetching tag ${TAG} from ${URL} failed after 5 attempts:\n${_err}")
  endif()
  message(STATUS "hm fetch.cmake: fetch of ${URL} failed (attempt ${_attempt} of 5), retrying in ${_delay} s")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${_delay})
  math(EXPR _delay "${_delay} * 2")
endforeach()
_hm_git(-c advice.detachedHead=false checkout -q "refs/tags/${TAG}^{commit}")
_hm_git(rev-parse "HEAD^{commit}")
if(NOT _hm_out STREQUAL COMMIT)
  message(FATAL_ERROR "hm fetch.cmake: tag ${TAG} of ${URL} is ${_hm_out}, but third_party/VERSIONS.md pins ${COMMIT}")
endif()
message(STATUS "hm fetch.cmake: ${URL} ${TAG} = ${COMMIT}")
