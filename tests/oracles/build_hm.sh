#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# HM HEVC reference software for the oracle image (chapter 14 §14.4.4, §14.3.3; WI-0.554):
#   TAppEncoder, TAppDecoder  stock build
#   TAppDecoder_trace         ENC_DEC_TRACE=1 in TypeDef.h (writes TraceDec.txt)
# WI-0.563 adds the getter dumper `hm_cu_dump` on top of the stock libraries built here.

# shellcheck source-path=SCRIPTDIR source=lib.sh
source "$(dirname "$0")/lib.sh"

HM_VERSION="HM-18.0"
HM_COMMIT="0000000000000000000000000000000000000000"

work="${1:-/tmp/build/hm}"
mkdir -p "${work}"
cd "${work}" || exit 1
ovt_git_fetch https://vcgit.hhi.fraunhofer.de/jvet/HM.git "refs/tags/${HM_VERSION}" "${HM_COMMIT}" stock
cp -a stock trace

typedef="trace/source/Lib/TLibCommon/TypeDef.h"
sed -i -E 's/^(#define[[:space:]]+ENC_DEC_TRACE[[:space:]]+)0/\11/' "${typedef}"
grep -Eq '^#define[[:space:]]+ENC_DEC_TRACE[[:space:]]+1' "${typedef}" || ovt_die "ENC_DEC_TRACE not enabled"

# HM's CMake writes the executables under <source>/bin/; find them by exact name.
hm_build() {
  cmake -S "$1" -B "$1/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "$1/build" -j "${OVT_ORACLES_JOBS}" --target "${@:2}"
}
hm_install() {
  local bin
  bin="$(find "$1" -type f -name "$2" -perm -u+x | head -n 1)"
  [[ -n "${bin}" ]] || ovt_die "$2 was not built in $1"
  install -D -m 0755 "${bin}" "${OVT_ORACLES_PREFIX}/bin/$3"
}

hm_build stock TAppEncoder TAppDecoder
hm_install stock TAppEncoder TAppEncoder
hm_install stock TAppDecoder TAppDecoder
hm_build trace TAppDecoder
hm_install trace TAppDecoder TAppDecoder_trace

for tool in TAppEncoder TAppDecoder; do ovt_stamp "${tool}" "${HM_VERSION} (${HM_COMMIT})"; done
ovt_stamp TAppDecoder_trace "${HM_VERSION} (${HM_COMMIT}) ENC_DEC_TRACE=1"
rm -rf "${work}"
