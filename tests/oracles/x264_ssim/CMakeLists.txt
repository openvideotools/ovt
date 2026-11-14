# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-2.0-or-later
#
# x264_ssim: the vendored x264 SSIM kernel as an oracle executable (chapter 14 §14.4.12, WI-0.565). GPL-2.0-or-later,
# built only in the oracle image and by tools/goldens/gen_metrics.py; never linked into OVT.
#
#   cmake -S tests/oracles/x264_ssim -B build/x264_ssim && cmake --build build/x264_ssim   ->  build/x264_ssim/x264_ssim
#
# pixel_ssim.c is compiled twice, as x264 does for its BIT_DEPTH variants: the 8-bit integer path and the 10-bit float
# path (x264's `#if BIT_DEPTH > 9`). The entry points are renamed per depth so one executable can hold both.

cmake_minimum_required(VERSION 3.25)
project(x264_ssim LANGUAGES C)

# OVT_X264_SSIM_DOUBLE_ACCUM=ON builds from a copy patched by double_accum.patch: the per-window quotient, the row sums
# and the plane mean in double, which is the definition of chapter 9 §9.4.4 (x264 itself does all three in float and
# is off by up to ~1.5e-6 per frame on the Netflix pair). Used only for the Netflix golden (docs/acceptance/wi/
# WI-3.308a.md); the default build is the verbatim x264 kernel.
option(OVT_X264_SSIM_DOUBLE_ACCUM "Patch the vendored kernel to accumulate in double (double_accum.patch)" OFF)
set(_src "${CMAKE_CURRENT_SOURCE_DIR}")
if(OVT_X264_SSIM_DOUBLE_ACCUM)
  set(_src "${CMAKE_CURRENT_BINARY_DIR}/patched")
  file(REMOVE_RECURSE "${_src}")
  file(COPY pixel_ssim.c ssim_functions.c x264_shim.h main.c DESTINATION "${_src}")
  find_program(PATCH_EXECUTABLE patch REQUIRED)
  execute_process(
    COMMAND "${PATCH_EXECUTABLE}" -p1 -i "${CMAKE_CURRENT_SOURCE_DIR}/double_accum.patch"
    WORKING_DIRECTORY "${_src}"
    RESULT_VARIABLE _patch_rc)
  if(NOT _patch_rc EQUAL 0)
    message(FATAL_ERROR "double_accum.patch does not apply")
  endif()
endif()

foreach(depth 8 10)
  add_library(x264_ssim_k${depth} OBJECT "${_src}/ssim_functions.c")
  target_compile_definitions(x264_ssim_k${depth} PRIVATE BIT_DEPTH=${depth} SSIM_SUFFIX=${depth}
                                                         x264_pixel_ssim_wxh=x264_pixel_ssim_wxh_${depth})
endforeach()

add_executable(x264_ssim "${_src}/main.c")
target_sources(x264_ssim PRIVATE $<TARGET_OBJECTS:x264_ssim_k8> $<TARGET_OBJECTS:x264_ssim_k10>)
target_include_directories(x264_ssim PRIVATE "${_src}")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Plain C float arithmetic, no fused multiply-add: the oracle must print what x264's C code prints.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(x264_ssim_k8 PRIVATE -ffp-contract=off)
  target_compile_options(x264_ssim_k10 PRIVATE -ffp-contract=off)
endif()
install(TARGETS x264_ssim RUNTIME DESTINATION bin)
