#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# FFmpeg and the encoders it links, for the oracle image (chapter 14 §14.2.6, §14.3.3, §14.4.1, §14.4.10,
# §14.4.11; WI-0.554). Needs libaom in the prefix first (build_libaom_inspect.sh). Builds, in order:
#   x264  (all bit depths and chroma formats; full history, its version string counts commits)
#   x265  (8-bit main library linking the 10- and 12-bit libraries; -DENABLE_HDR10_PLUS=ON)
#   dav1d (library and CLI), libvmaf + `vmaf` CLI (-Denable_float=true)
#   FFmpeg --enable-gpl --enable-libx264 --enable-libx265 --enable-libaom --enable-libvmaf --enable-libdav1d
# The binaries are oracles only: GPL, never linked into OVT. WI-0.563 adds `ffmpeg_mvs` to this script.

# shellcheck source-path=SCRIPTDIR source=lib.sh disable=SC1091 # followed only when lib.sh is checked too
source "$(dirname "$0")/lib.sh"

X264_REVISION="3191"
X264_COMMIT="4613ac3c15fd75cebc4b9f65b7fb95e70a3acce1"
X265_VERSION="4.1"
X265_COMMIT="1d117bed4747758b51bd2c124d738527e30392cb"
DAV1D_VERSION="1.5.1"
DAV1D_COMMIT="42b2b24fb8819f1ed3643aa9cf2a62f03868e3aa"
LIBVMAF_VERSION="3.0.0"
LIBVMAF_COMMIT="17a67b238ce0539bdeafdc95961abac64fa16ea8"
FFMPEG_VERSION="n7.1.1"
FFMPEG_COMMIT="db69d06eeeab4f46da15030a80d539efb4503ca8"

work="${1:-/tmp/build/ffmpeg}"
mkdir -p "${work}"
cd "${work}" || exit 1
P="${OVT_ORACLES_PREFIX}"
rpath="-Wl,-rpath,${P}/lib"

# --- x264 ----------------------------------------------------------------------------------------------
ovt_git_fetch https://code.videolan.org/videolan/x264.git master "${X264_COMMIT}" x264 full
(
  cd x264 || exit 1
  ./configure --prefix="${P}" --enable-shared --enable-pic --bit-depth=all --chroma-format=all \
    --disable-lavf --disable-swscale --disable-ffms --disable-gpac --disable-lsmash --extra-ldflags="${rpath}"
  grep -Eq "X264_REV +${X264_REVISION}\b" x264_config.h || ovt_die "x264 is not r${X264_REVISION}"
  make -j "${OVT_ORACLES_JOBS}"
  make install
)

# --- x265: 12- and 10-bit static libraries linked into the 8-bit shared library and CLI -----------------
ovt_git_fetch https://bitbucket.org/multicoreware/x265_git.git "refs/tags/${X265_VERSION}" "${X265_COMMIT}" x265
x265_common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_HDR10_PLUS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
cmake -S x265/source -B x265/b12 "${x265_common[@]}" -DHIGH_BIT_DEPTH=ON -DMAIN12=ON \
  -DEXPORT_C_API=OFF -DENABLE_SHARED=OFF -DENABLE_CLI=OFF
cmake --build x265/b12 -j "${OVT_ORACLES_JOBS}"
cmake -S x265/source -B x265/b10 "${x265_common[@]}" -DHIGH_BIT_DEPTH=ON \
  -DEXPORT_C_API=OFF -DENABLE_SHARED=OFF -DENABLE_CLI=OFF
cmake --build x265/b10 -j "${OVT_ORACLES_JOBS}"
mkdir -p x265/b8
cp x265/b10/libx265.a x265/b8/libx265_main10.a
cp x265/b12/libx265.a x265/b8/libx265_main12.a
cmake -S x265/source -B x265/b8 "${x265_common[@]}" -DENABLE_SHARED=ON -DLINKED_10BIT=ON -DLINKED_12BIT=ON \
  -DEXTRA_LIB="x265_main10.a;x265_main12.a" -DEXTRA_LINK_FLAGS="-L${work}/x265/b8" \
  -DCMAKE_INSTALL_PREFIX="${P}" -DCMAKE_INSTALL_RPATH="${P}/lib"
cmake --build x265/b8 -j "${OVT_ORACLES_JOBS}"
cmake --install x265/b8

# --- dav1d ---------------------------------------------------------------------------------------------
ovt_git_fetch https://code.videolan.org/videolan/dav1d.git "refs/tags/${DAV1D_VERSION}" "${DAV1D_COMMIT}" dav1d
meson setup dav1d/build dav1d --prefix="${P}" --libdir=lib --buildtype=release -Denable_tests=false \
  -Denable_tools=true -Db_ndebug=true -Dc_link_args="${rpath}"
ninja -C dav1d/build install

# --- libvmaf and the vmaf CLI --------------------------------------------------------------------------
ovt_git_fetch https://github.com/Netflix/vmaf.git "refs/tags/v${LIBVMAF_VERSION}" "${LIBVMAF_COMMIT}" vmaf
meson setup vmaf/libvmaf/build vmaf/libvmaf --prefix="${P}" --libdir=lib --buildtype=release \
  -Denable_float=true -Denable_cuda=false -Denable_avx512=true -Denable_tests=false -Denable_docs=false \
  -Dbuilt_in_models=true -Dc_link_args="${rpath}" -Dcpp_link_args="${rpath}"
ninja -C vmaf/libvmaf/build install

# --- FFmpeg --------------------------------------------------------------------------------------------
ovt_git_fetch https://github.com/FFmpeg/FFmpeg.git "refs/tags/${FFMPEG_VERSION}" "${FFMPEG_COMMIT}" ffmpeg
(
  cd ffmpeg || exit 1
  ./configure --prefix="${P}" --enable-gpl --enable-shared --disable-static --disable-doc --disable-debug \
    --enable-libx264 --enable-libx265 --enable-libaom --enable-libvmaf --enable-libdav1d \
    --extra-cflags="-I${P}/include" --extra-ldflags="-L${P}/lib ${rpath}"
  make -j "${OVT_ORACLES_JOBS}"
  make install
)

rm -rf "${work}"
