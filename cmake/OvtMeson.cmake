# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Meson wrapper for third_party/*.cmake (chapter 13 §13.3.5, WI-0.011). Include it through
# OvtThirdPartyCommon or OvtThirdParty; third_party/dav1d.cmake is the first consumer.
#
#   ovt_tp_meson(<dep> (URL <url> URL_HASH SHA256=<hex> | SOURCE_DIR <dir>) [SOURCE_SUBDIR <dir>]
#                [OPTIONS <-Dopt=value>...] OUTPUT_LIBS <tp::target=lib/libx.a>... [PATCH_COMMAND <cmd>...]
#                [DEPENDS <target>...] [EXTRA_C_ARGS <flag>...] [EXTRA_LINK_ARGS <flag>...]
#                [PKG_CONFIG_PATH <dir>...])
#       Builds the Meson project <dep> as ExternalProject `tp_<dep>`, installed into
#       ${OVT_THIRD_PARTY_PREFIX}/<dep> (libdir `lib`, static, release, no LTO, position-independent).
#       URL and URL_HASH are the VERSIONS.md row (OVT_TP_<dep>_URL / _SHA256); the archive is already in
#       OVT_TP_DOWNLOADS from ovt_tp_declare(), so ExternalProject does not fetch it again. SOURCE_DIR is an
#       existing tree instead.
#       SOURCE_SUBDIR names the directory of the archive that holds the top-level meson.build (libvmaf keeps
#       its Meson project in `libvmaf/`); patches and licence files still refer to the archive root.
#       OPTIONS are passed to `meson setup` verbatim. Each OUTPUT_LIBS entry `tp::name=relative/lib.a`
#       becomes a STATIC IMPORTED GLOBAL target (ovt_tp_import_static: include dir `include`, licence record
#       from the ovt_tp_declare() of <dep>, so call that first).
#
#       EXTRA_C_ARGS / EXTRA_LINK_ARGS add compiler (C and C++) and linker flags, PKG_CONFIG_PATH directories to
#       the pkg-config search path, for dependencies Meson finds outside the system paths (the vcpkg tree of
#       libplacebo's glslang and lcms2). They go into the cross file, or into CFLAGS/CXXFLAGS/LDFLAGS and
#       PKG_CONFIG_PATH of a native Linux build.
#
#   Cross file: <binary>/third_party/<dep>-cross.ini is written from third_party/meson/cross_template.ini.in
#   with the compilers, archiver, strip tool, flags and system of the active CMake toolchain. It is passed
#   to Meson when the target is Android, or on macOS (where it forces CMAKE_OSX_ARCHITECTURES and the
#   deployment target into c_args). On a native Linux build no cross file is used and the compiler is
#   given through CC/CXX. pkg-config is vcpkg's `pkgconf` host tool when present (the system one is not
#   usable for Android), else the one found on PATH; pkg_config_path points into the vcpkg installed tree.
#
#   OVT_MESON_FORCE_CROSS_FILE=ON (test hook) also writes and uses the cross file on a native Linux build.
#
#   The host tools meson (>= 1.4) and ninja come from tools/bootstrap.sh (tools/requirements.txt pins
#   meson); configure fails here, naming them, when they are missing. Set -DOVT_MESON=<path> to override.

include_guard(GLOBAL)
include(ExternalProject)
include(OvtThirdPartyCommon)

set(_OVT_MESON_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/meson/cross_template.ini.in")
cmake_path(NORMAL_PATH _OVT_MESON_TEMPLATE)
set(_OVT_MESON_MIN_VERSION 1.4)

# Quotes every item of the list in <var> as a Meson string ('a', 'b') and stores it in <out>.
function(_ovt_meson_quote out)
  set(items "")
  foreach(item IN LISTS ARGN)
    string(REPLACE "\\" "/" item "${item}")
    string(REPLACE "'" "\\'" item "${item}")
    list(APPEND items "'${item}'")
  endforeach()
  list(JOIN items ", " joined)
  set(${out} "${joined}" PARENT_SCOPE)
endfunction()

# Finds meson and ninja once; sets OVT_MESON and OVT_NINJA_PROGRAM in the cache.
function(_ovt_meson_find_tools)
  if(NOT OVT_MESON)
    find_program(OVT_MESON NAMES meson meson.py)
  endif()
  if(NOT OVT_MESON)
    message(FATAL_ERROR "ovt_tp_meson: `meson` not found. Run tools/bootstrap.sh or "
                        "`pip install -r tools/requirements.txt`, or pass -DOVT_MESON=<path>.")
  endif()
  execute_process(COMMAND "${OVT_MESON}" --version OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0 OR version VERSION_LESS _OVT_MESON_MIN_VERSION)
    message(FATAL_ERROR "ovt_tp_meson: ${OVT_MESON} is version '${version}', need >= ${_OVT_MESON_MIN_VERSION}.")
  endif()
  find_program(OVT_NINJA_PROGRAM NAMES ninja ninja-build HINTS "${CMAKE_MAKE_PROGRAM}")
  if(NOT OVT_NINJA_PROGRAM)
    message(FATAL_ERROR "ovt_tp_meson: `ninja` not found (tools/bootstrap.sh installs it).")
  endif()
endfunction()

# Sets <prefix>_SYSTEM, <prefix>_CPU_FAMILY, <prefix>_CPU for the target machine.
function(_ovt_meson_machine prefix)
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(system android)
    set(family aarch64)
    set(cpu aarch64)
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(system android)
    set(family x86_64)
    set(cpu x86_64)
  elseif(ANDROID_ABI)
    message(FATAL_ERROR "ovt_tp_meson: unsupported ANDROID_ABI '${ANDROID_ABI}' (arm64-v8a, x86_64).")
  else()
    if(APPLE)
      set(system darwin)
      set(arch "${CMAKE_OSX_ARCHITECTURES}")
      if(NOT arch)
        set(arch "${CMAKE_SYSTEM_PROCESSOR}")
      endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(system linux)
      set(arch "${CMAKE_SYSTEM_PROCESSOR}")
    else()
      message(FATAL_ERROR "ovt_tp_meson: unsupported target system '${CMAKE_SYSTEM_NAME}'.")
    endif()
    if(arch MATCHES "^(arm64|aarch64|ARM64)$")
      set(family aarch64)
      set(cpu aarch64)
    elseif(arch MATCHES "^(x86_64|AMD64|amd64)$")
      set(family x86_64)
      set(cpu x86_64)
    else()
      message(FATAL_ERROR "ovt_tp_meson: unsupported CPU '${arch}' (arm64, x86_64).")
    endif()
  endif()
  set(${prefix}_SYSTEM ${system} PARENT_SCOPE)
  set(${prefix}_CPU_FAMILY ${family} PARENT_SCOPE)
  set(${prefix}_CPU ${cpu} PARENT_SCOPE)
endfunction()

# The pkg-config of the cross file: vcpkg's pkgconf host tool, else PATH.
function(_ovt_meson_pkg_config out)
  set(candidates "")
  if(VCPKG_INSTALLED_DIR AND VCPKG_HOST_TRIPLET)
    list(APPEND candidates "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/pkgconf/pkgconf")
  endif()
  foreach(c IN LISTS candidates)
    if(EXISTS "${c}")
      set(${out} "${c}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  find_program(_ovt_pkg_config NAMES pkgconf pkg-config)
  if(_ovt_pkg_config)
    set(${out} "${_ovt_pkg_config}" PARENT_SCOPE)
  else()
    set(${out} "pkg-config" PARENT_SCOPE)
  endif()
endfunction()

# Writes the cross file of <dep> and sets <out_file>. Called by ovt_tp_meson().
function(_ovt_meson_write_cross_file dep out_file)
  cmake_parse_arguments(PARSE_ARGV 2 extra "" "" "C_ARGS;LINK_ARGS;PKG_CONFIG_PATH")
  _ovt_meson_machine(m)

  # Compiler command lines: the Android NDK toolchain file names a plain clang and a separate target triple.
  set(c_cmd "${CMAKE_C_COMPILER}")
  set(cxx_cmd "${CMAKE_CXX_COMPILER}")
  if(CMAKE_C_COMPILER_TARGET)
    list(APPEND c_cmd "--target=${CMAKE_C_COMPILER_TARGET}")
  endif()
  if(CMAKE_CXX_COMPILER_TARGET)
    list(APPEND cxx_cmd "--target=${CMAKE_CXX_COMPILER_TARGET}")
  endif()

  set(common_args "")
  if(APPLE)
    if(m_CPU_FAMILY STREQUAL "aarch64")
      list(APPEND common_args -arch arm64)
    else()
      list(APPEND common_args -arch x86_64)
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      list(APPEND common_args "-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(CMAKE_OSX_SYSROOT)
      list(APPEND common_args -isysroot "${CMAKE_OSX_SYSROOT}")
    endif()
  endif()
  separate_arguments(c_flags UNIX_COMMAND "${CMAKE_C_FLAGS}")
  separate_arguments(cxx_flags UNIX_COMMAND "${CMAKE_CXX_FLAGS}")
  separate_arguments(link_flags UNIX_COMMAND "${CMAKE_EXE_LINKER_FLAGS}")

  set(pkg_path "")
  if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    set(pkg_path "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/pkgconfig")
  endif()
  list(APPEND pkg_path ${extra_PKG_CONFIG_PATH})
  _ovt_meson_pkg_config(pkg_config)

  set(ar "${CMAKE_AR}")
  set(strip "${CMAKE_STRIP}")
  if(NOT strip)
    find_program(_ovt_strip NAMES strip llvm-strip)
    set(strip "${_ovt_strip}")
  endif()

  _ovt_meson_quote(OVT_MESON_C ${c_cmd})
  _ovt_meson_quote(OVT_MESON_CXX ${cxx_cmd})
  _ovt_meson_quote(OVT_MESON_AR ${ar})
  _ovt_meson_quote(OVT_MESON_STRIP ${strip})
  _ovt_meson_quote(OVT_MESON_PKG_CONFIG ${pkg_config})
  _ovt_meson_quote(OVT_MESON_C_ARGS ${common_args} ${c_flags} ${extra_C_ARGS})
  _ovt_meson_quote(OVT_MESON_CXX_ARGS ${common_args} ${cxx_flags} ${extra_C_ARGS})
  _ovt_meson_quote(OVT_MESON_LINK_ARGS ${common_args} ${link_flags} ${extra_LINK_ARGS})
  _ovt_meson_quote(OVT_MESON_PKG_CONFIG_PATH ${pkg_path})
  set(OVT_MESON_SYSTEM "${m_SYSTEM}")
  set(OVT_MESON_CPU_FAMILY "${m_CPU_FAMILY}")
  set(OVT_MESON_CPU "${m_CPU}")
  # Android binaries cannot run on the build host; macOS cross-arch (x86_64 on arm64) can through Rosetta.
  if(m_SYSTEM STREQUAL "android")
    set(OVT_MESON_NEEDS_EXE_WRAPPER true)
  else()
    set(OVT_MESON_NEEDS_EXE_WRAPPER false)
  endif()

  set(file "${CMAKE_BINARY_DIR}/third_party/${dep}-cross.ini")
  configure_file("${_OVT_MESON_TEMPLATE}" "${file}" @ONLY)
  set(${out_file} "${file}" PARENT_SCOPE)
endfunction()

function(ovt_tp_meson dep)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;URL_HASH;SOURCE_DIR;SOURCE_SUBDIR"
                        "OPTIONS;OUTPUT_LIBS;PATCH_COMMAND;DEPENDS;EXTRA_C_ARGS;EXTRA_LINK_ARGS;PKG_CONFIG_PATH")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "ovt_tp_meson(${dep}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if((arg_URL AND arg_SOURCE_DIR) OR (NOT arg_URL AND NOT arg_SOURCE_DIR) OR (arg_URL AND NOT arg_URL_HASH))
    message(FATAL_ERROR "ovt_tp_meson(${dep}): give either URL with URL_HASH, or SOURCE_DIR.")
  endif()
  if(NOT arg_OUTPUT_LIBS)
    message(FATAL_ERROR "ovt_tp_meson(${dep}): OUTPUT_LIBS is required (tp::name=lib/libname.a ...).")
  endif()
  if(NOT OVT_THIRD_PARTY_PREFIX)
    message(FATAL_ERROR "ovt_tp_meson(${dep}): OVT_THIRD_PARTY_PREFIX is not set (cmake/OvtOptions.cmake).")
  endif()
  _ovt_meson_find_tools()

  # Cross file: always on Android (the only real cross build) and macOS (arch and deployment target).
  set(cross_args "")
  set(env_args "")
  if(ANDROID_ABI OR APPLE OR OVT_MESON_FORCE_CROSS_FILE)
    _ovt_meson_write_cross_file(${dep} cross_file C_ARGS ${arg_EXTRA_C_ARGS} LINK_ARGS ${arg_EXTRA_LINK_ARGS}
                                PKG_CONFIG_PATH ${arg_PKG_CONFIG_PATH})
    set(cross_args --cross-file "${cross_file}")
  else()
    set(env_args "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}")
    if(arg_EXTRA_C_ARGS)
      list(JOIN arg_EXTRA_C_ARGS " " joined)
      list(APPEND env_args "CFLAGS=${joined}" "CXXFLAGS=${joined}")
    endif()
    if(arg_EXTRA_LINK_ARGS)
      list(JOIN arg_EXTRA_LINK_ARGS " " joined)
      list(APPEND env_args "LDFLAGS=${joined}")
    endif()
    if(arg_PKG_CONFIG_PATH)
      list(JOIN arg_PKG_CONFIG_PATH ":" joined)
      list(APPEND env_args "PKG_CONFIG_PATH=${joined}")
    endif()
  endif()

  set(prefix "${OVT_THIRD_PARTY_PREFIX}/${dep}")
  set(source_args "")
  if(arg_URL)
    set(source_args URL "${arg_URL}" URL_HASH "${arg_URL_HASH}" DOWNLOAD_DIR "${OVT_TP_DOWNLOADS}")
  else()
    set(source_args SOURCE_DIR "${arg_SOURCE_DIR}" DOWNLOAD_COMMAND "")
  endif()
  set(source_subdir "<SOURCE_DIR>")
  if(arg_SOURCE_SUBDIR)
    set(source_subdir "<SOURCE_DIR>/${arg_SOURCE_SUBDIR}")
  endif()
  set(patch_args "")
  if(arg_PATCH_COMMAND)
    set(patch_args PATCH_COMMAND ${arg_PATCH_COMMAND})
  endif()

  set(byproducts "")
  foreach(spec IN LISTS arg_OUTPUT_LIBS)
    string(REGEX MATCH "^[^=]+=(.+)$" _m "${spec}")
    if(NOT _m)
      message(FATAL_ERROR "ovt_tp_meson(${dep}): OUTPUT_LIBS entry '${spec}' is not tp::name=relative/path.a")
    endif()
    list(APPEND byproducts "${prefix}/${CMAKE_MATCH_1}")
  endforeach()

  # Meson refuses an existing build directory without --reconfigure, so a re-run of the ExternalProject
  # configure step wipes it first.
  externalproject_add(
    tp_${dep}
    ${source_args} ${patch_args}
    PREFIX "${prefix}"
    INSTALL_DIR "${prefix}"
    BINARY_DIR "${prefix}/build"
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env ${env_args} "${OVT_MESON}" setup --wipe "<BINARY_DIR>" "${source_subdir}" --prefix
      "<INSTALL_DIR>" --libdir lib --buildtype release --default-library static -Db_lto=false -Db_staticpic=true
      ${cross_args} ${arg_OPTIONS}
    BUILD_COMMAND "${OVT_MESON}" compile -C "<BINARY_DIR>"
    INSTALL_COMMAND "${OVT_MESON}" install -C "<BINARY_DIR>"
    BUILD_BYPRODUCTS ${byproducts}
    DEPENDS ${arg_DEPENDS})

  foreach(spec IN LISTS arg_OUTPUT_LIBS)
    string(REGEX MATCH "^([^=]+)=(.+)$" _m "${spec}")
    ovt_tp_import_static(${CMAKE_MATCH_1} tp_${dep} ${CMAKE_MATCH_2} include)
  endforeach()
endfunction()
