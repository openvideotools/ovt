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

foreach(depth 8 10)
  add_library(x264_ssim_k${depth} OBJECT ssim_functions.c)
  target_compile_definitions(x264_ssim_k${depth} PRIVATE BIT_DEPTH=${depth} SSIM_SUFFIX=${depth}
                                                         x264_pixel_ssim_wxh=x264_pixel_ssim_wxh_${depth})
endforeach()

add_executable(x264_ssim main.c)
target_sources(x264_ssim PRIVATE $<TARGET_OBJECTS:x264_ssim_k8> $<TARGET_OBJECTS:x264_ssim_k10>)
target_include_directories(x264_ssim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Plain C float arithmetic, no fused multiply-add: the oracle must print what x264's C code prints.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(x264_ssim_k8 PRIVATE -ffp-contract=off)
  target_compile_options(x264_ssim_k10 PRIVATE -ffp-contract=off)
endif()
install(TARGETS x264_ssim RUNTIME DESTINATION bin)
