# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
# cmake -Dstatus=<ovt/status.h> -Dabi=<ovt/abi/analysis_decoder_v1.h> -P check_status_copies.cmake
# Only one of the two OVT_STATUS_TYPES_DEFINED blocks is ever compiled (#211): fail when they differ textually.
foreach(_h status abi)
  file(READ "${${_h}}" _text)
  string(REGEX MATCH "#ifndef OVT_STATUS_TYPES_DEFINED.*#endif /\\* OVT_STATUS_TYPES_DEFINED \\*/" _block "${_text}")
  if(_block STREQUAL "")
    message(FATAL_ERROR "no OVT_STATUS_TYPES_DEFINED block in ${${_h}}")
  endif()
  string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" _block "${_block}")
  string(REGEX REPLACE "//[^\n]*" "" _block "${_block}")
  string(REGEX REPLACE "[ \t\r\n]+" "" _${_h} "${_block}")
endforeach()
if(NOT _status STREQUAL _abi)
  message(FATAL_ERROR "ovt_status/ovt_Buffer differ between the headers:\n  status.h: ${_status}\n  abi:      ${_abi}")
endif()
