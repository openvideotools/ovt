#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# The remaining source-built oracles of chapter 14 §14.2.6 (WI-0.554), one per Dockerfile stage:
#   build_extra.sh gpac    MP4Box (§14.4.6), static binary
#   build_extra.sh bento4  mp4dump, mp4encrypt, mp4fragment (§14.3.3 recipes)
#   build_extra.sh daala   dump_psnrhvs, dump_ciede2000 (a Python script upstream; §14.4.13)
#   build_extra.sh mpv     libplacebo (the third_party/VERSIONS.md pin) + mpv (§14.4.15); needs FFmpeg in the prefix

# shellcheck source-path=SCRIPTDIR source=lib.sh disable=SC1091 # followed only when lib.sh is checked too
source "$(dirname "$0")/lib.sh"

GPAC_VERSION="v2.4.0"
GPAC_COMMIT="5d70253ac94e5840be7b86054131dd753af63cc7"
BENTO4_VERSION="v1.6.0-641"
BENTO4_COMMIT="dc264854d1f76c370b65b18d9f303a95f7f21ab1"
DAALA_COMMIT="24f889aaabe33bc390b993d737d830b8dd367e61" # master, 2025-02-05; Daala has no releases
LIBPLACEBO_VERSION="v7.349.0"
LIBPLACEBO_COMMIT="1fd3c7bde7b943fe8985c893310b5269a09b46c5"
MPV_VERSION="v0.39.0"
MPV_COMMIT="a0fba7be57f3822d967b04f0f6b6d6341e7516e7"

tool="${1:?usage: build_extra.sh gpac|bento4|daala|mpv}"
work="/tmp/build/${tool}"
mkdir -p "${work}"
cd "${work}" || exit 1
P="${OVT_ORACLES_PREFIX}"

case "${tool}" in
  gpac)
    ovt_git_fetch https://github.com/gpac/gpac.git "refs/tags/${GPAC_VERSION}" "${GPAC_COMMIT}" src
    cd src || exit 1
    ./configure --prefix="${P}" --static-bin
    make -j "${OVT_ORACLES_JOBS}"
    install -D -m 0755 bin/gcc/MP4Box "${P}/bin/MP4Box"
    ovt_stamp MP4Box "GPAC ${GPAC_VERSION} (${GPAC_COMMIT})"
    ;;
  bento4)
    ovt_git_fetch https://github.com/axiomatic-systems/Bento4.git "refs/tags/${BENTO4_VERSION}" "${BENTO4_COMMIT}" src
    cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j "${OVT_ORACLES_JOBS}" --target mp4dump mp4encrypt mp4fragment
    for t in mp4dump mp4encrypt mp4fragment; do
      install -D -m 0755 "build/${t}" "${P}/bin/${t}"
      # Bento4 prints "Bento4 Version 1.6.0.0" for every 1.6.0-x tag, so the stamp carries the tag.
      ovt_stamp "${t}" "Bento4 ${BENTO4_VERSION} (${BENTO4_COMMIT})"
    done
    ;;
  daala)
    ovt_git_fetch https://gitlab.xiph.org/xiph/daala.git master "${DAALA_COMMIT}" src full
    cd src || exit 1
    ./autogen.sh
    ./configure --disable-player --disable-shared --disable-unit-tests --disable-doc # static: no libtool wrappers
    make -j "${OVT_ORACLES_JOBS}" tools
    install -D -m 0755 tools/dump_psnrhvs "${P}/bin/dump_psnrhvs"
    # dump_ciede2000 is a Python script in Daala (numpy, scikit-image, y4m from requirements.txt).
    sed '1s|.*|#!/usr/bin/env python3|' tools/dump_ciede2000.py >"${P}/bin/dump_ciede2000"
    chmod 0755 "${P}/bin/dump_ciede2000"
    for t in dump_psnrhvs dump_ciede2000; do ovt_stamp "${t}" "Daala ${DAALA_COMMIT}"; done
    ;;
  mpv)
    ovt_git_fetch https://code.videolan.org/videolan/libplacebo.git "refs/tags/${LIBPLACEBO_VERSION}" \
      "${LIBPLACEBO_COMMIT}" libplacebo
    git -C libplacebo submodule update -q --init --depth 1 # glad, jinja, markupsafe, fast_float, Vulkan-Headers
    meson setup libplacebo/build libplacebo --prefix="${P}" --libdir=lib --buildtype=release \
      -Dvulkan=enabled -Dshaderc=enabled -Dglslang=disabled -Dlcms=enabled -Ddemos=false -Dtests=false \
      -Dc_link_args="-Wl,-rpath,${P}/lib" -Dcpp_link_args="-Wl,-rpath,${P}/lib"
    ninja -C libplacebo/build install
    ovt_git_fetch https://github.com/mpv-player/mpv.git "refs/tags/${MPV_VERSION}" "${MPV_COMMIT}" mpv
    meson setup mpv/build mpv --prefix="${P}" --libdir=lib --buildtype=release -Dlibmpv=false \
      -Dvulkan=enabled -Dx11=enabled -Dmanpage-build=disabled -Dhtml-build=disabled \
      -Dc_link_args="-Wl,-rpath,${P}/lib"
    ninja -C mpv/build install
    ;;
  *) ovt_die "unknown tool ${tool}" ;;
esac
rm -rf "${work}"
