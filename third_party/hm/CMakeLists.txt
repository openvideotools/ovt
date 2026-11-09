# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# OVT's build of the HM decoder libraries (chapter 07 §7.6.4 build row, WI-4.104). third_party/hm.cmake runs this
# list as the configure step of the tp_hm ExternalProject with -DHM_SOURCE_DIR=<the pinned, unpatched HM checkout>.
#
# Why not HM's own CMakeLists.txt: it adds -msse4.1 to every file on every UNIX target (an error for arm64 clang) and
# turns warnings into errors (newer GCC and clang reject HM's code). Patching it would contradict §7.6.4 ("no
# patches to HM"), so this list compiles HM's unmodified sources itself, the way HM's per-library lists do:
#
#   TLibCommon   source/Lib/TLibCommon/*.cpp plus source/Lib/libmd5/*.cpp (HM links libmd5 into it, too)
#   TLibDecoder  source/Lib/TLibDecoder/*.cpp
#   Utilities    source/Lib/Utilities/*.cpp (TVideoIOYuv, program_options_lite)
#
# SPEC-ASSUMPTION(#345): HM-18.0 has no TLibVideoIO library (§7.6.4 names one); its YUV writer lives in Utilities.
#
# Settings:
#   * RExt__HIGH_BIT_DEPTH_SUPPORT=1 (16-bit Pel, §7.6.4), PUBLIC: it changes the layout of HM's classes, so every
#     consumer must see it too (hm.cmake puts it on the imported targets).
#   * ENC_DEC_TRACE stays 0, the value TypeDef.h hard-codes; it is not passed again, because a second definition
#     would be a macro redefinition in every consumer.
#   * -w: HM's code is not OVT's; its warnings are switched off for these libraries only (WI-4.104 pitfall). The
#     sanitizer flags arrive in CMAKE_CXX_FLAGS from OVT_TP_CMAKE_TOOLCHAIN_ARGS and are kept.
#   * C++11, as HM itself builds; HM's x86 intrinsics are guarded by __SSE2__, so arm64 builds the C fallback.
#   * Position-independent code, so the plugin (a MODULE library, WI-4.105) can link the static libraries (the
#     -fPIC note of the superseded WI-0.016).
#
# Installed: lib/libTLibCommon.a, lib/libTLibDecoder.a, lib/libUtilities.a and HM's headers under
# include/hm/<library>/, laid out as in source/Lib so HM's relative includes keep working.

cmake_minimum_required(VERSION 3.21)
project(ovt_hm LANGUAGES CXX)

if(NOT HM_SOURCE_DIR OR NOT EXISTS "${HM_SOURCE_DIR}/source/Lib/TLibDecoder/TDecTop.h")
  message(FATAL_ERROR "ovt_hm: -DHM_SOURCE_DIR must name an HM checkout (got '${HM_SOURCE_DIR}')")
endif()
set(_lib "${HM_SOURCE_DIR}/source/Lib")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)

file(GLOB _common_src CONFIGURE_DEPENDS "${_lib}/TLibCommon/*.cpp" "${_lib}/libmd5/*.cpp")
file(GLOB _decoder_src CONFIGURE_DEPENDS "${_lib}/TLibDecoder/*.cpp")
file(GLOB _utilities_src CONFIGURE_DEPENDS "${_lib}/Utilities/*.cpp")

add_library(TLibCommon STATIC ${_common_src})
target_include_directories(TLibCommon PUBLIC "${_lib}/TLibCommon" "${_lib}" "${_lib}/libmd5")
target_link_libraries(TLibCommon PUBLIC Threads::Threads)

add_library(TLibDecoder STATIC ${_decoder_src})
target_include_directories(TLibDecoder PUBLIC "${_lib}/TLibDecoder")
target_link_libraries(TLibDecoder PUBLIC TLibCommon)

add_library(Utilities STATIC ${_utilities_src})
target_include_directories(Utilities PUBLIC "${_lib}/Utilities")
target_link_libraries(Utilities PUBLIC TLibCommon)

foreach(_target TLibCommon TLibDecoder Utilities)
  target_compile_definitions(${_target} PUBLIC RExt__HIGH_BIT_DEPTH_SUPPORT=1)
  if(MSVC)
    target_compile_options(${_target} PRIVATE /w)
  else()
    target_compile_options(${_target} PRIVATE -w)
  endif()
endforeach()

install(TARGETS TLibCommon TLibDecoder Utilities ARCHIVE DESTINATION lib)
foreach(_dir TLibCommon TLibDecoder Utilities libmd5)
  install(DIRECTORY "${_lib}/${_dir}/" DESTINATION include/hm/${_dir} FILES_MATCHING PATTERN "*.h")
endforeach()
install(FILES "${HM_SOURCE_DIR}/COPYING" DESTINATION share/doc/hm)
