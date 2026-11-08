# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Shared by OvtCorpora.cmake (configure time) and OvtCorpusGate.cmake (test time), WI-0.040.
#
#   ovt_corpus_check_index(<index-file> <tag> <dir> <present-var> <missing-var>)
#       Reads the lines `<id>|<tag>,<tag>...` of <index-file>. <present-var> is TRUE when at least one entry carries
#       <tag> and every such entry has <dir>/<id>/.ok; <missing-var> lists the entries that do not.

cmake_policy(VERSION 3.25) # also loaded by cmake -P scripts

function(ovt_corpus_check_index index tag dir present_var missing_var)
  file(STRINGS "${index}" lines)
  set(found 0)
  set(missing "")
  foreach(line IN LISTS lines)
    string(FIND "${line}" "|" bar)
    string(SUBSTRING "${line}" 0 ${bar} id)
    math(EXPR from "${bar} + 1")
    string(SUBSTRING "${line}" ${from} -1 tag_text)
    string(REPLACE "," ";" tag_list "${tag_text}")
    if(tag IN_LIST tag_list)
      set(found 1)
      if(NOT EXISTS "${dir}/${id}/.ok")
        list(APPEND missing "${id}")
      endif()
    endif()
  endforeach()
  if(found AND NOT missing)
    set(${present_var} TRUE PARENT_SCOPE)
  else()
    set(${present_var} FALSE PARENT_SCOPE)
  endif()
  set(${missing_var} "${missing}" PARENT_SCOPE)
endfunction()
