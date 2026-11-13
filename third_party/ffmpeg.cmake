# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FFmpeg, two flavours from one pinned source (chapter 13 §13.3.3, WI-0.012). Included by cmake/OvtThirdParty.cmake
# unless OVT_SYSTEM_FFMPEG=ON. The pin (version, URL, SHA-256) is the FFmpeg row of third_party/VERSIONS.md.
#
#   tp::ffmpeg_core     INTERFACE IMPORTED: the shared `core` flavour, linked by ovt_core. LGPL (never --enable-gpl
#                       unless OVT_FFMPEG_GPL_CORE_ACK), libraries renamed by --build-suffix=_ovt (libavcodec_ovt.so,
#                       libavcodec_ovt.pc) so a system FFmpeg in the same process does not clash. Keeps the h264/hevc
#                       decoders, their hwaccels and the other pool-covered decoders unless
#                       OVT_PATENT_CODECS_IN_CORE=OFF (the split build; ON is the default). Per library:
#                       tp::ffmpeg_core_<lib> (SHARED IMPORTED) for <lib> in avformat avcodec swscale swresample avutil.
#   tp::ffmpeg_plugin   STATIC-archive INTERFACE IMPORTED: the `plugin` flavour, linked only by ovt_plugin_ffmpeg.
#                       h264/hevc decoders and the platform h264/hevc hwaccels, nothing else decodes. The shared
#                       object linking it calls ovt_ffmpeg_hide_symbols() so no av* symbol leaves the plugin.
#                       Per library: tp::ffmpeg_plugin_<lib> (STATIC IMPORTED). Built only with
#                       OVT_BUILD_PLUGIN_FFMPEG=ON.
#
# Patches: the plugin flavour applies the series plugins/ovt-plugin-ffmpeg/patches/series (chapter 07 §7.6.3, the
# block-params side data of WI-4.100/4.101) through tools/apply_patches.py (§13.3.6), which records them in
# ffmpeg-plugin/share/ovt/patches.json; the core flavour applies third_party/patches/ffmpeg (none). A changed series
# or patch re-extracts the pristine tarball. SPEC-ASSUMPTION(#331): the plugin flavour is patched whatever
# OVT_FFMPEG_GPL is (the patch is LGPL-2.1-or-later), never the core flavour.
#
# Each flavour is its own ExternalProject (tp_ffmpeg_core, tp_ffmpeg_plugin) extracting the archive into its own
# SOURCE_DIR: two configures in one tree overwrite each other's config.h (WI-0.012 pitfall). Both install into
# OVT_THIRD_PARTY_PREFIX/ffmpeg-<flavour>. The flag lists are in third_party/ffmpeg/configure_flags.cmake; on
# Android, configure runs through third_party/ffmpeg/android_toolchain.sh.
#
# Reproducibility (§13.3.3): configure runs with SOURCE_DATE_EPOCH (the environment's, else 0) and
# -ffile-prefix-map for the source and build trees, so the build path does not reach the binaries, and with
# GIT_CEILING_DIRECTORIES (configure, build and install) so FFmpeg's ffbuild/version.sh cannot pick up the git revision of the OVT checkout around
# the build tree (the release tarball's VERSION file names the version).

include_guard(GLOBAL)
include(OvtThirdPartyCommon)
include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg/configure_flags.cmake")

set(OVT_FFMPEG_PLUGIN_PATCH_SERIES "${CMAKE_CURRENT_LIST_DIR}/../plugins/ovt-plugin-ffmpeg/patches/series"
    CACHE FILEPATH "Series of the patches applied to the FFmpeg plugin flavour (chapter 13 §13.3.6)")
cmake_path(NORMAL_PATH OVT_FFMPEG_PLUGIN_PATCH_SERIES)

# LICENSE stays the row's prose cell; each imported target below records its own SPDX id.
ovt_tp_declare(ffmpeg LICENSE_FILES COPYING.LGPLv2.1 COPYING.GPLv2 LICENSE.md)

# ---- platform -------------------------------------------------------------------------------------------
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(_ovt_ff_platform android)
elseif(APPLE)
  set(_ovt_ff_platform macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_ovt_ff_platform linux)
else()
  message(FATAL_ERROR "third_party/ffmpeg.cmake: no FFmpeg flag list for ${CMAKE_SYSTEM_NAME} (chapter 13 §13.3.3).")
endif()

# ---- licence per flavour (chapter 15 §15.4; the options are validated in cmake/OvtOptions.cmake) ---------
# OVT_FFMPEG_GPL=ON alone fails configure there; with OVT_FFMPEG_GPL_PLUGIN_ONLY only the plugin turns GPL, with
# OVT_FFMPEG_GPL_CORE_ACK both do.
set(_ovt_ff_plugin_gpl OFF)
set(_ovt_ff_core_gpl OFF)
if(OVT_FFMPEG_GPL)
  set(_ovt_ff_plugin_gpl ON)
  if(NOT OVT_FFMPEG_GPL_PLUGIN_ONLY)
    set(_ovt_ff_core_gpl ON)
  endif()
endif()

# ---- host tools ---------------------------------------------------------------------------------------
# FFmpeg's build is GNU make (>= 3.81, which is what macOS ships as `make`).
find_program(OVT_MAKE NAMES gmake make DOC "GNU make for the FFmpeg build (third_party/ffmpeg.cmake)")
if(NOT OVT_MAKE)
  message(FATAL_ERROR "third_party/ffmpeg.cmake: GNU make not found; install it (tools/bootstrap.sh) or set "
                      "-DOVT_MAKE=<path>.")
endif()
find_program(OVT_PKG_CONFIG NAMES pkgconf pkg-config DOC "pkg-config for FFmpeg's configure")
if(NOT OVT_PKG_CONFIG)
  message(FATAL_ERROR "third_party/ffmpeg.cmake: pkg-config not found; FFmpeg's configure needs it for dav1d "
                      "and VA-API (tools/bootstrap.sh installs it).")
endif()
cmake_host_system_information(RESULT _ovt_ff_jobs QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT _ovt_ff_jobs OR _ovt_ff_jobs LESS 1)
  set(_ovt_ff_jobs 2)
endif()

# ---- dav1d for the core flavour (libdav1d decoder, chapter 13 §13.3.1) ---------------------------------
set(_ovt_ff_dav1d OFF)
set(_ovt_ff_pkg_dirs "")
set(_ovt_ff_core_depends "")
if(TARGET tp::dav1d)
  set(_ovt_ff_dav1d ON)
  if(NOT OVT_SYSTEM_DAV1D)
    list(APPEND _ovt_ff_pkg_dirs "${OVT_THIRD_PARTY_PREFIX}/dav1d/lib/pkgconfig")
    list(APPEND _ovt_ff_core_depends tp_dav1d)
  endif()
else()
  message(WARNING "OVT: FFmpeg core flavour built without libdav1d, because tp::dav1d does not exist "
                  "(see the dav1d status message above). AV1 then decodes only through the hwaccels.")
endif()

# ---- VA-API (Linux hwaccel) -----------------------------------------------------------------------------
# configure with --enable-vaapi and no libva fails deep inside FFmpeg's config.log; check here and name the fix.
set(_ovt_ff_platform_libs "")
if(_ovt_ff_platform STREQUAL "linux" AND OVT_ENABLE_HW_DECODE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(ovt_ff_libva QUIET IMPORTED_TARGET libva libva-drm)
  if(NOT ovt_ff_libva_FOUND)
    message(
      FATAL_ERROR
        "third_party/ffmpeg.cmake: OVT_ENABLE_HW_DECODE=ON needs VA-API (pkg-config libva, "
        "libva-drm): install libva-dev (tools/apt-packages.txt), or configure with " "-DOVT_ENABLE_HW_DECODE=OFF.")
  endif()
  # Plain library names (va, va-drm), described below, rather than the pkg-config target with absolute paths.
  list(APPEND _ovt_ff_platform_libs ${ovt_ff_libva_LIBRARIES})
  foreach(_ovt_ff_lib IN LISTS ovt_ff_libva_LIBRARIES)
    ovt_set_dependency_license(
      ${_ovt_ff_lib}
      SOURCE
      system
      SPDX
      "MIT"
      VERSION
      "${ovt_ff_libva_VERSION}"
      LINKAGE
      shared)
  endforeach()
endif()

# ---- toolchain arguments shared by both flavours ---------------------------------------------------------
set(_ovt_ff_toolchain "")
set(_ovt_ff_wrapper "")
set(_ovt_ff_cflags "${OVT_TP_SANITIZER_C_FLAGS}")
set(_ovt_ff_ldflags "${OVT_TP_SANITIZER_LINK_FLAGS}")
set(_ovt_ff_env "SOURCE_DATE_EPOCH=$ENV{SOURCE_DATE_EPOCH}")
if("$ENV{SOURCE_DATE_EPOCH}" STREQUAL "")
  set(_ovt_ff_env "SOURCE_DATE_EPOCH=0")
endif()

if(_ovt_ff_platform STREQUAL "android")
  set(_ovt_ff_ndk "${ANDROID_NDK}")
  if(NOT _ovt_ff_ndk)
    set(_ovt_ff_ndk "${CMAKE_ANDROID_NDK}")
  endif()
  if(NOT _ovt_ff_ndk)
    set(_ovt_ff_ndk "$ENV{ANDROID_NDK_ROOT}")
  endif()
  if(NOT _ovt_ff_ndk)
    message(FATAL_ERROR "third_party/ffmpeg.cmake: Android build without ANDROID_NDK / ANDROID_NDK_ROOT.")
  endif()
  set(_ovt_ff_api "${ANDROID_NATIVE_API_LEVEL}")
  if(NOT _ovt_ff_api AND ANDROID_PLATFORM MATCHES "([0-9]+)$")
    set(_ovt_ff_api "${CMAKE_MATCH_1}")
  endif()
  if(NOT _ovt_ff_api)
    set(_ovt_ff_api 28) # minSdk (D5)
  endif()
  set(_ovt_ff_wrapper
      sh
      "${CMAKE_CURRENT_LIST_DIR}/ffmpeg/android_toolchain.sh"
      --ndk
      "${_ovt_ff_ndk}"
      --abi
      "${ANDROID_ABI}"
      --api
      "${_ovt_ff_api}"
      --)
  # Only the pkg-config files of OVT's own builds: the host's .pc files describe libraries for the wrong target.
  list(APPEND _ovt_ff_toolchain "--pkg-config=${OVT_PKG_CONFIG}")
  list(JOIN _ovt_ff_pkg_dirs ":" _ovt_ff_pkg_path)
  list(APPEND _ovt_ff_env "PKG_CONFIG_LIBDIR=${_ovt_ff_pkg_path}" "PKG_CONFIG_PATH=")
  list(APPEND _ovt_ff_platform_libs z m mediandk)
else()
  set(_ovt_ff_cc "${CMAKE_C_COMPILER}")
  if(CMAKE_C_COMPILER_LAUNCHER)
    list(JOIN CMAKE_C_COMPILER_LAUNCHER " " _ovt_ff_launcher)
    set(_ovt_ff_cc "${_ovt_ff_launcher} ${_ovt_ff_cc}")
  endif()
  list(APPEND _ovt_ff_toolchain "--cc=${_ovt_ff_cc}" "--pkg-config=${OVT_PKG_CONFIG}")
  if(_ovt_ff_pkg_dirs)
    list(JOIN _ovt_ff_pkg_dirs ":" _ovt_ff_pkg_path)
    if(NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
      string(APPEND _ovt_ff_pkg_path ":$ENV{PKG_CONFIG_PATH}")
    endif()
    list(APPEND _ovt_ff_env "PKG_CONFIG_PATH=${_ovt_ff_pkg_path}")
  endif()
  if(_ovt_ff_platform STREQUAL "macos")
    # One architecture per build; universal artefacts are lipo'd later (OVT_UNIVERSAL, §13.5.1).
    set(_ovt_ff_arch "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT _ovt_ff_arch)
      set(_ovt_ff_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    list(LENGTH _ovt_ff_arch _ovt_ff_narch)
    if(_ovt_ff_narch GREATER 1)
      message(FATAL_ERROR "third_party/ffmpeg.cmake: CMAKE_OSX_ARCHITECTURES='${_ovt_ff_arch}' names more than one "
                          "architecture; build each and lipo them (OVT_UNIVERSAL, §13.5.1).")
    endif()
    set(_ovt_ff_apple "-arch ${_ovt_ff_arch}")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      string(APPEND _ovt_ff_apple " -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(CMAKE_OSX_SYSROOT)
      string(APPEND _ovt_ff_apple " -isysroot ${CMAKE_OSX_SYSROOT}")
    endif()
    string(APPEND _ovt_ff_cflags " ${_ovt_ff_apple}")
    string(APPEND _ovt_ff_ldflags " ${_ovt_ff_apple}")
    list(APPEND _ovt_ff_toolchain "--arch=${_ovt_ff_arch}")
    list(APPEND _ovt_ff_platform_libs z "-framework CoreFoundation" "-framework CoreMedia" "-framework CoreVideo"
         "-framework CoreServices")
    if(OVT_ENABLE_HW_DECODE)
      list(APPEND _ovt_ff_platform_libs "-framework VideoToolbox" "-framework QuartzCore")
    endif()
  else()
    find_package(Threads REQUIRED)
    list(APPEND _ovt_ff_platform_libs Threads::Threads z m ${CMAKE_DL_LIBS})
  endif()
endif()
string(STRIP "${_ovt_ff_ldflags}" _ovt_ff_ldflags)

# Platform libraries of the static plugin flavour that the licence manifest does not skip by itself
# (cmake/OvtLicenseTags.cmake skips only the C runtime): zlib, and Android's NDK media library.
ovt_set_dependency_license(z SOURCE system SPDX "Zlib" LINKAGE shared)
ovt_set_dependency_license(mediandk SOURCE system SPDX "Apache-2.0" LINKAGE shared)

# The libraries each flavour installs, in static link order.
set(OVT_FFMPEG_LIBS avformat avcodec swscale swresample avutil)

# _ovt_ffmpeg_sh_quote(<out-var> <string>): <string> as one single-quoted POSIX shell word.
function(_ovt_ffmpeg_sh_quote out_var value)
  string(REPLACE "'" "'\\''" value "${value}")
  set(${out_var} "'${value}'" PARENT_SCOPE)
endfunction()

# ---- one flavour ------------------------------------------------------------------------------------------
# _ovt_ffmpeg_flavour(<flavour>) adds tp_ffmpeg_<flavour> and the imported targets.
function(_ovt_ffmpeg_flavour flavour)
  set(prefix "${OVT_THIRD_PARTY_PREFIX}/ffmpeg-${flavour}")
  set(src "${prefix}/src/ffmpeg")
  set(bin "${prefix}/src/build")
  if(flavour STREQUAL "core")
    set(gpl ${_ovt_ff_core_gpl})
    set(depends ${_ovt_ff_core_depends})
    set(patch_command ${OVT_TP_PATCH} ${CMAKE_SOURCE_DIR}/third_party/patches/ffmpeg)
    set(patch_files "")
  else()
    set(gpl ${_ovt_ff_plugin_gpl})
    set(depends "")
    ovt_tp_apply_patches(patch_command SERIES "${OVT_FFMPEG_PLUGIN_PATCH_SERIES}" INSTALL_DIR "${prefix}")
    cmake_path(GET OVT_FFMPEG_PLUGIN_PATCH_SERIES PARENT_PATH patch_dir)
    file(GLOB patch_files CONFIGURE_DEPENDS "${patch_dir}/*.patch")
    list(APPEND patch_files "${OVT_FFMPEG_PLUGIN_PATCH_SERIES}")
  endif()

  ovt_ffmpeg_configure_flags(
    flags
    FLAVOUR
    ${flavour}
    PLATFORM
    ${_ovt_ff_platform}
    HW_DECODE
    ${OVT_ENABLE_HW_DECODE}
    PATENT_CODECS_IN_CORE
    ${OVT_PATENT_CODECS_IN_CORE}
    GPL
    ${gpl}
    DAV1D
    ${_ovt_ff_dav1d})

  set(cflags "-ffile-prefix-map=${src}=ffmpeg -ffile-prefix-map=${bin}=ffmpeg-build ${_ovt_ff_cflags}")
  string(STRIP "${cflags}" cflags)
  set(configure_args ${_ovt_ff_toolchain} "--prefix=${prefix}" "--extra-cflags=${cflags}")
  if(_ovt_ff_ldflags)
    list(APPEND configure_args "--extra-ldflags=${_ovt_ff_ldflags}")
  endif()
  if(flavour STREQUAL "core" AND _ovt_ff_platform STREQUAL "linux")
    # libavcodec_ovt.so needs libswresample_ovt.so etc. from its own directory. An executable's DT_RUNPATH does not
    # apply to the dependencies of its libraries, so each library carries RUNPATH $ORIGIN (relocatable, no build
    # path). configure and make both re-evaluate `$`, so the flag travels in a compiler response file instead.
    set(rsp "${prefix}/ovt-rpath.rsp")
    file(WRITE "${rsp}" "-Wl,-rpath,$ORIGIN\n")
    list(APPEND configure_args "--extra-ldsoflags=@${rsp}")
  endif()
  list(APPEND configure_args ${flags})

  set(byproducts "")
  foreach(lib IN LISTS OVT_FFMPEG_LIBS)
    if(flavour STREQUAL "core")
      list(APPEND byproducts "${prefix}/lib/lib${lib}_ovt${CMAKE_SHARED_LIBRARY_SUFFIX}")
    else()
      list(APPEND byproducts "${prefix}/lib/lib${lib}.a")
    endif()
  endforeach()

  # The configure step runs a generated script rather than a command line: each argument is single-quoted there, so
  # values with spaces, commas or `$` reach configure unchanged whatever shell the build tool uses, and the script
  # is the exact, re-runnable configure invocation of the flavour (ffmpeg-<flavour>/ovt-configure.sh).
  set(script "${prefix}/ovt-configure.sh")
  set(lines
      "#!/bin/sh"
      "# Generated by third_party/ffmpeg.cmake (WI-0.012): configure of the FFmpeg ${flavour} flavour."
      "set -e"
      "cd '${bin}'"
      "# configure keeps an existing Makefile (test -e Makefile ||), and one restored from a cache made in another"
      "# checkout would include that checkout's source tree."
      "rm -f Makefile")
  foreach(var IN LISTS _ovt_ff_env ITEMS "GIT_CEILING_DIRECTORIES=${prefix}/src")
    _ovt_ffmpeg_sh_quote(quoted "${var}")
    list(APPEND lines "export ${quoted}")
  endforeach()
  set(command ${_ovt_ff_wrapper} "${src}/configure" ${configure_args})
  set(quoted_command "")
  foreach(arg IN LISTS command)
    _ovt_ffmpeg_sh_quote(quoted "${arg}")
    string(APPEND quoted_command " \\\n  ${quoted}")
  endforeach()
  string(REGEX REPLACE "^ \\\\\n  " "" quoted_command "${quoted_command}")
  list(APPEND lines "exec ${quoted_command}")
  list(JOIN lines "\n" content)
  # Written only when the content changes, so an unchanged configure line does not re-run configure.
  set(old_content "")
  if(EXISTS "${script}")
    file(READ "${script}" old_content)
  endif()
  if(NOT old_content STREQUAL "${content}\n")
    file(WRITE "${script}" "${content}\n")
  endif()

  externalproject_add(
    tp_ffmpeg_${flavour}
    URL ${OVT_TP_ffmpeg_URL} # already in DOWNLOAD_DIR, verified by ovt_tp_declare()
    URL_HASH SHA256=${OVT_TP_ffmpeg_SHA256}
    DOWNLOAD_DIR ${OVT_TP_DOWNLOADS}
    PREFIX ${prefix}
    SOURCE_DIR ${src}
    BINARY_DIR ${bin}
    INSTALL_DIR ${prefix}
    DEPENDS ${depends}
    PATCH_COMMAND ${patch_command}
    CONFIGURE_COMMAND sh ${script}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${prefix}/src" ${OVT_MAKE} -j${_ovt_ff_jobs}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env "GIT_CEILING_DIRECTORIES=${prefix}/src" ${OVT_MAKE} install
    BUILD_BYPRODUCTS ${byproducts}
    LOG_CONFIGURE ON
    LOG_OUTPUT_ON_FAILURE ON)

  externalproject_add_stepdependencies(tp_ffmpeg_${flavour} configure "${script}")
  if(patch_files)
    externalproject_add_stepdependencies(tp_ffmpeg_${flavour} download ${patch_files})
  endif()

  # INTERFACE_INCLUDE_DIRECTORIES must exist at generate time, before the build installs anything.
  file(MAKE_DIRECTORY "${prefix}/include")

  if(flavour STREQUAL "core")
    set(spdx "LGPL-2.1-or-later")
    if(gpl)
      set(spdx "GPL-2.0-or-later")
    endif()
    set(linkage shared)
  else()
    set(spdx "LGPL-2.1-or-later")
    if(gpl)
      set(spdx "GPL-2.0-or-later")
    endif()
    set(linkage static)
  endif()

  add_library(tp::ffmpeg_${flavour} INTERFACE IMPORTED GLOBAL)
  set(components "")
  foreach(lib IN LISTS OVT_FFMPEG_LIBS)
    set(t tp::ffmpeg_${flavour}_${lib})
    if(flavour STREQUAL "core")
      add_library(${t} SHARED IMPORTED GLOBAL)
      set(location "${prefix}/lib/lib${lib}_ovt${CMAKE_SHARED_LIBRARY_SUFFIX}")
    else()
      add_library(${t} STATIC IMPORTED GLOBAL)
      set(location "${prefix}/lib/lib${lib}.a")
    endif()
    set_target_properties(${t} PROPERTIES IMPORTED_LOCATION "${location}" INTERFACE_INCLUDE_DIRECTORIES
                                                                          "${prefix}/include")
    add_dependencies(${t} tp_ffmpeg_${flavour})
    ovt_set_dependency_license(
      ${t}
      SOURCE
      third_party
      SPDX
      "${spdx}"
      VERSION
      "${OVT_TP_ffmpeg_VERSION}"
      LINKAGE
      ${linkage})
    list(APPEND components ${t})
  endforeach()

  if(flavour STREQUAL "core")
    target_link_libraries(tp::ffmpeg_core INTERFACE ${components})
  else()
    # Static archives need the platform libraries; the symbol hiding of §13.3.3 is applied by the shared object
    # that links them, through ovt_ffmpeg_hide_symbols() (cmake/OvtThirdParty.cmake).
    target_link_libraries(tp::ffmpeg_plugin INTERFACE ${components} ${_ovt_ff_platform_libs})
  endif()
  ovt_set_dependency_license(
    tp::ffmpeg_${flavour}
    SOURCE
    third_party
    SPDX
    "${spdx}"
    VERSION
    "${OVT_TP_ffmpeg_VERSION}"
    LINKAGE
    ${linkage})
endfunction()

_ovt_ffmpeg_flavour(core)
if(OVT_BUILD_PLUGIN_FFMPEG)
  _ovt_ffmpeg_flavour(plugin)
endif()

message(STATUS "OVT: FFmpeg ${OVT_TP_ffmpeg_VERSION} core (shared, _ovt) and plugin (static) flavours for "
               "${_ovt_ff_platform}; patent codecs in core: ${OVT_PATENT_CODECS_IN_CORE}; hwaccels: "
               "${OVT_ENABLE_HW_DECODE}; GPL core/plugin: ${_ovt_ff_core_gpl}/${_ovt_ff_plugin_gpl}")
