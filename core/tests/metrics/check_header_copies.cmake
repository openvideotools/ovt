# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
# cmake -Da=<header> -Db=<header> -Dguard=<GUARD> -P check_header_copies.cmake
# ovt/abi/metric_v1.h repeats ovt_status/ovt_Buffer, ovt_FrameId and ovt_ColorDescription from the LGPL headers
# under shared guards (#156, #211); only one copy is ever compiled, so this fails when two differ textually
# (comments and whitespace ignored). Generalises core/tests/analysis/check_status_copies.cmake.
foreach(_h a b)
  file(READ "${${_h}}" _text)
  string(REGEX MATCH "#ifndef ${guard}.*#endif /\\* ${guard} \\*/" _block "${_text}")
  if(_block STREQUAL "")
    message(FATAL_ERROR "no ${guard} block in ${${_h}}")
  endif()
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _block "${_block}")
  string(REGEX REPLACE "//[^\n]*" "" _block "${_block}")
  string(REGEX REPLACE "[ \t\r\n]+" "" _${_h} "${_block}")
endforeach()
if(NOT _a STREQUAL _b)
  message(FATAL_ERROR "${guard} blocks differ between the headers:\n  ${a}: ${_a}\n  ${b}: ${_b}")
endif()
