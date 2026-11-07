# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FFmpeg `configure` flag lists per flavour and platform (chapter 13 §13.3.3, WI-0.012). Included by
# third_party/ffmpeg.cmake, and by third_party/ffmpeg/test_configure_flags.cmake in script mode (cmake -P), so
# this file must not use targets, options or anything else that needs a project.
#
#   ovt_ffmpeg_configure_flags(<out-var>
#       FLAVOUR core|plugin  PLATFORM linux|macos|android
#       [HW_DECODE <bool>] [PATENT_CODECS_IN_CORE <bool>] [GPL <bool>] [DAV1D <bool>])
#
#   Sets <out-var> to the flavour- and platform-specific `configure` arguments: the common part of the §13.3.3
#   table, then the platform part, then the flavour part. Toolchain arguments (--cc, --prefix, --sysroot,
#   --extra-cflags, the Android cross-compile set of third_party/ffmpeg/android_toolchain.sh) depend on the active
#   build and are added by third_party/ffmpeg.cmake.
#
#   HW_DECODE              OVT_ENABLE_HW_DECODE: the platform hwaccels (VideoToolbox, VA-API, MediaCodec).
#   PATENT_CODECS_IN_CORE  OVT_PATENT_CODECS_IN_CORE: the core flavour keeps the h264/hevc decoders and hwaccels, and
#                          the other pool-covered decoders (VVC, AAC, MPEG-4 Part 2 and its
#                          Microsoft variants msmpeg4v1-v3/wmv1/wmv2, VC-1).
#   GPL                    --enable-gpl for this flavour (ffmpeg.cmake derives it from OVT_FFMPEG_GPL*).
#   DAV1D                  the core flavour links libdav1d (tp::dav1d or the system dav1d is available).
#
# Rules that keep the flavours honest (§13.3.3):
#   * Parsers and bitstream filters are never disabled: the raw h264/hevc and mpegts demuxers select their parsers,
#     and a parser or bitstream filter is not a decoder.
#   * No encoder is enabled in any flavour (--disable-encoders). The muxers kept (null, framemd5, rawvideo,
#     yuv4mpegpipe) carry packets that OVT encodes itself or that are already raw.
#   * VideoToolbox and VA-API exist only as hwaccels, so the core flavour removes h264/hevc from them with
#     --disable-hwaccel=; MediaCodec has real decoder entries, removed with --disable-decoder=.

include_guard(GLOBAL)

# The common part, first in every flavour (the §13.3.3 "common" row, without the toolchain arguments).
set(OVT_FFMPEG_COMMON_FLAGS
    --disable-programs
    --disable-doc
    --disable-avdevice
    --disable-postproc
    --disable-avfilter
    --disable-encoders
    --disable-muxers
    --enable-muxer=null,framemd5,rawvideo,yuv4mpegpipe
    --disable-network
    --disable-debug
    --enable-pic
    --disable-autodetect
    --enable-zlib
    --pkg-config-flags=--static)

# The patent-encumbered decoders (and their hwaccels) that the core flavour leaves out unless
# OVT_PATENT_CODECS_IN_CORE=ON. The plugin flavour enables exactly these.
set(OVT_FFMPEG_PATENT_DECODERS h264 hevc)
set(OVT_FFMPEG_PATENT_MEDIACODEC_DECODERS h264_mediacodec hevc_mediacodec)
set(OVT_FFMPEG_PATENT_HWACCELS_macos h264_videotoolbox hevc_videotoolbox)
set(OVT_FFMPEG_PATENT_HWACCELS_linux h264_vaapi hevc_vaapi)
set(OVT_FFMPEG_PATENT_HWACCELS_android "")

# The other pool-covered decoders the core flavour leaves out (ADR-015, chapter 15 §15.5 POL-PAT-2; Karl's decision on
# #132): H.266/VVC, AAC and HE-AAC (one FFmpeg decoder family: aac, aac_fixed, aac_latm, plus the AudioToolbox and
# MediaCodec wrappers), MPEG-4 Part 2 Visual with its Microsoft pre-standard variants (msmpeg4v1/v2/v3 and wmv1/wmv2,
# i.e. WMV7/8: all five are built from libavcodec/msmpeg4dec.c, configure's msmpeg4dec component, which is then not
# built; the h263 decoder stays), and SMPTE VC-1 (vc1 and wmv3 are the same decoder; *image are its sprite variants).
# No requirement asks the core to decode any of them: they are recognised and demuxed, never decoded,
# because demuxers, parsers and bitstream filters stay on. The plugin flavour does not enable them either.
# OVT_PATENT_CODECS_IN_CORE=ON keeps them, like h264/hevc. Names checked against libavcodec/allcodecs.c and
# hwaccels.h of n7.1.1; the cuvid/qsv/mmal/v4l2m2m/nvdec/vdpau/d3d* variants are never built (--disable-autodetect
# and no --enable for their APIs), so only the hwaccels of the APIs the core enables are listed.
set(OVT_FFMPEG_POOL_DECODERS
    vvc
    aac
    aac_fixed
    aac_latm
    aac_at
    aac_mediacodec
    mpeg4
    mpeg4_mediacodec
    msmpeg4v1
    msmpeg4v2
    msmpeg4v3
    wmv1
    wmv2
    vc1
    vc1image
    wmv3
    wmv3image)
set(OVT_FFMPEG_POOL_HWACCELS_macos mpeg4_videotoolbox)
set(OVT_FFMPEG_POOL_HWACCELS_linux mpeg4_vaapi vc1_vaapi wmv3_vaapi)

function(ovt_ffmpeg_configure_flags out_var)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "FLAVOUR;PLATFORM;HW_DECODE;PATENT_CODECS_IN_CORE;GPL;DAV1D" "")
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "ovt_ffmpeg_configure_flags: unknown arguments ${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT arg_FLAVOUR MATCHES "^(core|plugin)$")
    message(FATAL_ERROR "ovt_ffmpeg_configure_flags: FLAVOUR must be core or plugin (got '${arg_FLAVOUR}').")
  endif()
  if(NOT arg_PLATFORM MATCHES "^(linux|macos|android)$")
    message(FATAL_ERROR "ovt_ffmpeg_configure_flags: PLATFORM must be linux, macos or android "
                        "(got '${arg_PLATFORM}').")
  endif()
  set(platform ${arg_PLATFORM})

  set(flags ${OVT_FFMPEG_COMMON_FLAGS})

  # ---- platform part --------------------------------------------------------------------------------
  # FFmpeg's configure has no --enable-X=no form: a feature is either named with --enable-X or left out.
  if(platform STREQUAL "macos")
    if(arg_HW_DECODE)
      list(APPEND flags --enable-videotoolbox)
    endif()
    list(APPEND flags --disable-audiotoolbox)
  elseif(platform STREQUAL "linux")
    if(arg_HW_DECODE)
      list(APPEND flags --enable-vaapi)
    endif()
    list(APPEND flags --disable-vulkan)
  else() # android; --enable-cross-compile, --target-os=android, --arch and --sysroot come from android_toolchain.sh
    if(arg_HW_DECODE)
      list(APPEND flags --enable-mediacodec --enable-jni)
    endif()
  endif()

  # ---- flavour part ---------------------------------------------------------------------------------
  if(arg_FLAVOUR STREQUAL "core")
    list(APPEND flags --enable-shared --disable-static --build-suffix=_ovt)
    if(arg_DAV1D)
      list(APPEND flags --enable-libdav1d)
    endif()
    if(NOT arg_PATENT_CODECS_IN_CORE)
      # Always the full list, whatever the platform: disabling a component configure does not build is a no-op, and
      # one list on every platform keeps avcodec_configuration() comparable across builds.
      set(decoders ${OVT_FFMPEG_PATENT_DECODERS} ${OVT_FFMPEG_PATENT_MEDIACODEC_DECODERS} ${OVT_FFMPEG_POOL_DECODERS})
      set(hwaccels ${OVT_FFMPEG_PATENT_HWACCELS_macos} ${OVT_FFMPEG_PATENT_HWACCELS_linux}
                   ${OVT_FFMPEG_POOL_HWACCELS_macos} ${OVT_FFMPEG_POOL_HWACCELS_linux})
      list(JOIN decoders "," decoders)
      list(JOIN hwaccels "," hwaccels)
      list(APPEND flags --disable-decoder=${decoders} --disable-hwaccel=${hwaccels})
    endif()
  else() # plugin: the AVC/HEVC codec pack, nothing else decodes in it
    # SPEC-ASSUMPTION(#128): "--disable-everything-else-via-common" read as --disable-decoders/--disable-hwaccels
    # plus the AVC/HEVC entries; the common part alone disables no decoder.
    list(APPEND flags --disable-shared --enable-static)
    set(decoders ${OVT_FFMPEG_PATENT_DECODERS})
    set(hwaccels "")
    if(arg_HW_DECODE)
      if(platform STREQUAL "android")
        list(APPEND decoders ${OVT_FFMPEG_PATENT_MEDIACODEC_DECODERS})
      else()
        set(hwaccels ${OVT_FFMPEG_PATENT_HWACCELS_${platform}})
      endif()
    endif()
    list(JOIN decoders "," decoders)
    list(APPEND flags --disable-decoders --enable-decoder=${decoders} --disable-hwaccels)
    if(hwaccels)
      list(JOIN hwaccels "," hwaccels)
      list(APPEND flags --enable-hwaccel=${hwaccels})
    endif()
  endif()

  if(arg_GPL)
    list(APPEND flags --enable-gpl)
  else()
    list(APPEND flags --disable-gpl)
  endif()

  set(${out_var} ${flags} PARENT_SCOPE)
endfunction()
