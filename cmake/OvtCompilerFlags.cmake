# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The ovt::flags INTERFACE target (chapter 13 §13.2.5). Every OVT target links it; third-party builds
# never do. This file is the single source of the warning set (D24): there is no other warning function.

include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(OVT_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ovt_ipo_ok OUTPUT _ovt_ipo_msg LANGUAGES C CXX)
  if(_ovt_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(WARNING "OVT_ENABLE_LTO=ON but IPO is not supported here: ${_ovt_ipo_msg}")
  endif()
endif()

add_library(ovt_flags INTERFACE)
add_library(ovt::flags ALIAS ovt_flags)
target_compile_features(ovt_flags INTERFACE cxx_std_20 c_std_11)

set(_ovt_cxx "$<COMPILE_LANGUAGE:CXX>")
set(_ovt_release "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")

# ---- Common -------------------------------------------------------------------------------------
target_compile_options(ovt_flags INTERFACE
  -fvisibility=hidden
  $<${_ovt_cxx}:-fvisibility-inlines-hidden>
  -fno-strict-aliasing
  -fstack-protector-strong
  -fPIC
  "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/ovt")
target_compile_definitions(ovt_flags INTERFACE $<${_ovt_release}:_FORTIFY_SOURCE=2>)

# ---- Warnings (all compilers) ------------------------------------------------------------------
target_compile_options(ovt_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  $<${_ovt_cxx}:-Wold-style-cast>
  -Wcast-align -Wcast-qual -Wundef -Wunused
  $<${_ovt_cxx}:-Woverloaded-virtual>
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
  $<${_ovt_cxx}:-Wnon-virtual-dtor>
  -Wmisleading-indentation)

# GCC additions.
target_compile_options(ovt_flags INTERFACE
  "$<$<COMPILE_LANG_AND_ID:C,GNU>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op>"
  "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wduplicated-cond;-Wduplicated-branches;-Wlogical-op;-Wuseless-cast>")

# Clang / AppleClang additions.
target_compile_options(ovt_flags INTERFACE
  "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wthread-safety;-Wextra-semi;-Wunreachable-code-aggressive>"
  "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wthread-safety;-Wextra-semi;-Wunreachable-code-aggressive;-Wrange-loop-analysis>")

if(OVT_WERROR)
  target_compile_options(ovt_flags INTERFACE -Werror)
endif()

# ---- Release -----------------------------------------------------------------------------------
# -O2, not -O3 (§13.2.5). The flag comes after CMake's per-config flags, so it wins over a default -O3.
target_compile_options(ovt_flags INTERFACE "$<${_ovt_release}:-O2;-g1>")
target_compile_definitions(ovt_flags INTERFACE $<${_ovt_release}:NDEBUG>)
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
  target_link_options(ovt_flags INTERFACE "LINKER:-z,max-page-size=16384")
endif()
if(APPLE)
  target_link_options(ovt_flags INTERFACE $<${_ovt_release}:LINKER:-dead_strip>)
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
  target_link_options(ovt_flags INTERFACE
    "$<${_ovt_release}:LINKER:--as-needed;LINKER:-z,relro,-z,now;LINKER:--gc-sections>")
endif()

# ---- Sanitizers (OVT_SANITIZER) ----------------------------------------------------------------
if(OVT_ENABLE_SANITIZERS)
  if(OVT_SANITIZER STREQUAL "asan-ubsan")
    set(_ovt_san -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1 -g)
    set(_ovt_san_link -fsanitize=address,undefined)
  elseif(OVT_SANITIZER STREQUAL "tsan")
    set(_ovt_san -fsanitize=thread -O1 -g)
    set(_ovt_san_link -fsanitize=thread)
  else() # fuzz: instrumentation only; the fuzz executables add ${OVT_FUZZ_ENGINE} (WI-0.557).
    set(_ovt_san -fsanitize=fuzzer-no-link,address,undefined)
    set(_ovt_san_link -fsanitize=address,undefined)
  endif()
  target_compile_options(ovt_flags INTERFACE ${_ovt_san})
  target_link_options(ovt_flags INTERFACE ${_ovt_san_link})
  target_compile_definitions(ovt_flags INTERFACE OVT_SANITIZED=1)
  unset(_ovt_san)
  unset(_ovt_san_link)
endif()

# ---- Coverage (OVT_COVERAGE, Clang only) -------------------------------------------------------
if(OVT_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(ovt_flags INTERFACE -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(ovt_flags INTERFACE -fprofile-instr-generate)
endif()

unset(_ovt_cxx)
unset(_ovt_release)
