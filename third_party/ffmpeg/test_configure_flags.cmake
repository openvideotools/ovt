# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Fast test of the FFmpeg flag lists and the Android toolchain wrapper (WI-0.012, chapter 13 §13.3.3), run without
# building FFmpeg:
#
#   cmake -DWORK_DIR=<scratch dir> -P third_party/ffmpeg/test_configure_flags.cmake
#
# Registered as the CTest test `ffmpeg_configure_flags` (label fast) by tests/CMakeLists.txt.

cmake_minimum_required(VERSION 3.25)
include("${CMAKE_CURRENT_LIST_DIR}/configure_flags.cmake")

function(expect_has flags flag context)
  if(NOT "${flag}" IN_LIST flags)
    message(SEND_ERROR "${context}: expected ${flag} in: ${flags}")
  endif()
endfunction()
function(expect_not flags pattern context)
  foreach(f IN LISTS flags)
    if(f MATCHES "${pattern}")
      message(SEND_ERROR "${context}: unexpected ${f} (matches ${pattern})")
    endif()
  endforeach()
endfunction()

foreach(platform linux macos android)
  foreach(hw ON OFF)
    # ---- core, default options (OVT_PATENT_CODECS_IN_CORE=ON since the owner decision of 2026-11-12) ----------
    ovt_ffmpeg_configure_flags(
      f
      FLAVOUR
      core
      PLATFORM
      ${platform}
      HW_DECODE
      ${hw}
      PATENT_CODECS_IN_CORE
      ON
      DAV1D
      ON)
    set(ctx "core/${platform}/hw=${hw}")
    foreach(common IN LISTS OVT_FFMPEG_COMMON_FLAGS)
      expect_has("${f}" "${common}" "${ctx}")
    endforeach()
    list(GET f 0 first)
    if(NOT first STREQUAL "--disable-programs")
      message(SEND_ERROR "${ctx}: the common part must come first (got ${first})")
    endif()
    expect_has("${f}" --enable-shared "${ctx}")
    expect_has("${f}" --disable-static "${ctx}")
    expect_has("${f}" --build-suffix=_ovt "${ctx}")
    expect_has("${f}" --enable-libdav1d "${ctx}")
    expect_has("${f}" --disable-gpl "${ctx}")
    # The default core keeps every decoder and hwaccel FFmpeg builds for the enabled APIs: nothing is disabled by
    # name, no encoder is ever enabled, no decoder is enabled by name, parsers/bsfs are never touched.
    expect_not("${f}" "^--disable-(decoder|hwaccel)=" "${ctx}")
    expect_not("${f}" "^--enable-(encoder|decoder|gpl)" "${ctx}")
    expect_not("${f}" "^--disable-(parser|bsf)" "${ctx}")

    # ---- core, patent-pool-free split build (OVT_PATENT_CODECS_IN_CORE=OFF, no dav1d) -------------------------
    ovt_ffmpeg_configure_flags(
      f
      FLAVOUR
      core
      PLATFORM
      ${platform}
      HW_DECODE
      ${hw}
      PATENT_CODECS_IN_CORE
      OFF)
    set(ctx "core-patent-free/${platform}/hw=${hw}")
    expect_has(
      "${f}"
      --disable-decoder=h264,hevc,h264_mediacodec,hevc_mediacodec,vvc,aac,aac_fixed,aac_latm,aac_at,aac_mediacodec,mpeg4,mpeg4_mediacodec,msmpeg4v1,msmpeg4v2,msmpeg4v3,wmv1,wmv2,vc1,vc1image,wmv3,wmv3image
      "${ctx}")
    expect_has(
      "${f}"
      --disable-hwaccel=h264_videotoolbox,hevc_videotoolbox,h264_vaapi,hevc_vaapi,mpeg4_videotoolbox,mpeg4_vaapi,vc1_vaapi,wmv3_vaapi
      "${ctx}")
    expect_not("${f}" "^--enable-(encoder|decoder|gpl)" "${ctx}")
    expect_not("${f}" "^--disable-(parser|bsf)" "${ctx}")
    expect_not("${f}" "^--enable-libdav1d" "${ctx} without DAV1D")

    # ---- plugin -------------------------------------------------------------------------------------------------
    ovt_ffmpeg_configure_flags(f FLAVOUR plugin PLATFORM ${platform} HW_DECODE ${hw})
    set(ctx "plugin/${platform}/hw=${hw}")
    expect_has("${f}" --disable-shared "${ctx}")
    expect_has("${f}" --enable-static "${ctx}")
    expect_has("${f}" --disable-decoders "${ctx}")
    expect_has("${f}" --disable-hwaccels "${ctx}")
    expect_has("${f}" --disable-gpl "${ctx}")
    expect_not("${f}" "^--build-suffix" "${ctx}")
    expect_not("${f}" "^--enable-encoder" "${ctx}")
    expect_not("${f}" "^--enable-libdav1d" "${ctx}")
    expect_not("${f}" "vvc|aac|mpeg4|vc1|wmv" "${ctx}")
    if(platform STREQUAL "android" AND hw)
      expect_has("${f}" --enable-decoder=h264,hevc,h264_mediacodec,hevc_mediacodec "${ctx}")
    else()
      expect_has("${f}" --enable-decoder=h264,hevc "${ctx}")
    endif()

    # ---- platform part ------------------------------------------------------------------------------------------
    if(platform STREQUAL "macos")
      expect_has("${f}" --disable-audiotoolbox "${ctx}")
      if(hw)
        expect_has("${f}" --enable-videotoolbox "${ctx}")
        expect_has("${f}" --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox "${ctx}")
      else()
        expect_not("${f}" "videotoolbox" "${ctx}")
      endif()
    elseif(platform STREQUAL "linux")
      expect_has("${f}" --disable-vulkan "${ctx}")
      if(hw)
        expect_has("${f}" --enable-vaapi "${ctx}")
        expect_has("${f}" --enable-hwaccel=h264_vaapi,hevc_vaapi "${ctx}")
      else()
        expect_not("${f}" "vaapi" "${ctx}")
      endif()
    else()
      if(hw)
        expect_has("${f}" --enable-mediacodec "${ctx}")
        expect_has("${f}" --enable-jni "${ctx}")
      else()
        expect_not("${f}" "mediacodec|jni" "${ctx}")
      endif()
      expect_not("${f}" "^--enable-hwaccel=" "${ctx}")
    endif()
  endforeach()
endforeach()

# ---- OVT_FFMPEG_CORE_STATIC (OSS-Fuzz build): the core flavour as static archives, same suffix ------------------
ovt_ffmpeg_configure_flags(
  f
  FLAVOUR
  core
  PLATFORM
  linux
  HW_DECODE
  OFF
  PATENT_CODECS_IN_CORE
  ON
  DAV1D
  ON
  STATIC
  ON)
expect_has("${f}" --disable-shared "core/static")
expect_has("${f}" --enable-static "core/static")
expect_has("${f}" --build-suffix=_ovt "core/static")
expect_not("${f}" "^--(enable-shared|disable-static)$" "core/static")

# ---- GPL only where asked ---------------------------------------------------------------------------------------
ovt_ffmpeg_configure_flags(f FLAVOUR plugin PLATFORM linux GPL ON)
expect_has("${f}" --enable-gpl "plugin GPL")
expect_not("${f}" "^--disable-gpl" "plugin GPL")

# ---- Android toolchain wrapper with a fake NDK ------------------------------------------------------------------
if(NOT WORK_DIR)
  set(WORK_DIR "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg_configure_flags_test")
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(host_tag linux-x86_64)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(host_tag darwin-x86_64)
endif()
if(host_tag)
  set(ndk "${WORK_DIR}/fake-ndk")
  set(bin "${ndk}/toolchains/llvm/prebuilt/${host_tag}/bin")
  file(REMOVE_RECURSE "${ndk}")
  file(MAKE_DIRECTORY "${bin}")
  file(TOUCH "${bin}/aarch64-linux-android28-clang")
  # A fake configure that prints its arguments, to check the order the wrapper passes them in.
  set(fake_configure "${WORK_DIR}/fake-configure.sh")
  file(WRITE "${fake_configure}" "#!/bin/sh\nfor a in \"$@\"; do printf '%s\\n' \"$a\"; done\n")
  file(CHMOD "${fake_configure}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
  set(wrapper "${CMAKE_CURRENT_LIST_DIR}/android_toolchain.sh")

  execute_process(COMMAND sh "${wrapper}" --ndk "${ndk}" --abi arm64-v8a --api 28 -- "${fake_configure}"
                          --enable-mediacodec --enable-jni OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(SEND_ERROR "android_toolchain.sh failed (${rc}): ${out}")
  endif()
  string(REPLACE "\n" ";" lines "${out}")
  list(REMOVE_ITEM lines "")
  set(expected
      --enable-cross-compile
      --target-os=android
      --arch=aarch64
      "--sysroot=${ndk}/toolchains/llvm/prebuilt/${host_tag}/sysroot"
      "--cc=${bin}/aarch64-linux-android28-clang"
      "--cxx=${bin}/aarch64-linux-android28-clang++"
      "--nm=${bin}/llvm-nm"
      "--ar=${bin}/llvm-ar"
      "--ranlib=${bin}/llvm-ranlib"
      "--strip=${bin}/llvm-strip"
      --enable-mediacodec
      --enable-jni)
  if(NOT lines STREQUAL expected)
    message(SEND_ERROR "android_toolchain.sh passed:\n  ${lines}\nexpected:\n  ${expected}")
  endif()

  # x86_64 at API 28 has no clang in the fake NDK: the wrapper must fail and name the missing compiler.
  execute_process(COMMAND sh "${wrapper}" --ndk "${ndk}" --abi x86_64 --api 28 --print ERROR_VARIABLE err
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(rc EQUAL 0 OR NOT err MATCHES "x86_64-linux-android28-clang does not exist")
    message(SEND_ERROR "android_toolchain.sh with a missing compiler: rc=${rc} stderr=${err}")
  endif()

  # An ABI outside the two of chapter 13 §13.4 is refused.
  execute_process(COMMAND sh "${wrapper}" --ndk "${ndk}" --abi armeabi-v7a --api 28 --print ERROR_VARIABLE err
                  RESULT_VARIABLE rc OUTPUT_QUIET)
  if(rc EQUAL 0 OR NOT err MATCHES "unsupported ABI")
    message(SEND_ERROR "android_toolchain.sh with armeabi-v7a: rc=${rc} stderr=${err}")
  endif()
endif()
