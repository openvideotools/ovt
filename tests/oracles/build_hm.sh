#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# HM HEVC reference software for the oracle image (chapter 14 §14.4.4, §14.3.3; WI-0.554):
#   TAppEncoder, TAppDecoder  stock build
#   TAppDecoder_trace         ENC_DEC_TRACE=1 in TypeDef.h (writes TraceDec.txt)
#   hm_cu_dump                the getter dumper of WI-0.563 (hm_cu_dump.cpp), linked against the stock libraries
#   share/hm/cfg              HM's encoder configs (encoder_randomaccess_main.cfg, ...)

# shellcheck source-path=SCRIPTDIR source=lib.sh disable=SC1091 # followed only when lib.sh is checked too
source "$(dirname "$0")/lib.sh"
here="$(cd "$(dirname "$0")" && pwd)"

HM_VERSION="HM-18.0"
HM_COMMIT="fb4486d5ab5d0cd3b6a71659c7d5eb4509f2a4ce"

work="${1:-/tmp/build/hm}"
mkdir -p "${work}"
cd "${work}" || exit 1
ovt_git_fetch https://vcgit.hhi.fraunhofer.de/jvet/HM.git "refs/tags/${HM_VERSION}" "${HM_COMMIT}" stock
# HM builds with -Werror; GCC 13 reports false array-bounds positives in TComLoopFilter.cpp. Warnings stay on.
grep -rl --include=CMakeLists.txt --include='*.cmake' -e '-Werror' stock |
  xargs -r sed -i -E 's/[[:space:]]*-Werror([[:space:]")]|$)/\1/g'
cp -a stock trace

ovt_enable_macro trace/source ENC_DEC_TRACE

# HM's CMake writes the executables under <source>/bin/; find them by exact name.
hm_build() {
  local flags="-Wno-error"
  [[ "$1" == trace ]] && flags+=" -DENC_DEC_TRACE=1"
  cmake -S "$1" -B "$1/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="${flags}"
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

# hm_cu_dump.cpp includes HM's headers as "TLibCommon/..." and "TLibDecoder/..." (the source/Lib directory) and links the
# static libraries of the stock build (not the trace build: ENC_DEC_TRACE would change TDecTop's layout).
# HM-18.0 puts the libraries under <source>/lib or the build tree depending on the generator; find them by name.
hm_lib_dir="$(dirname "$(find stock -type f -name libTLibDecoder.a | head -n 1)")"
[[ -f "${hm_lib_dir}/libTLibDecoder.a" ]] || ovt_die "libTLibDecoder.a was not built in stock"
hm_libs=()
for lib in TLibDecoder TLibCommon libmd5 Utilities; do
  found="$(find stock -type f \( -name "lib${lib}.a" -o -name "${lib}.a" \) | head -n 1)"
  [[ -n "${found}" ]] && hm_libs+=("${found}")
done
g++ -std=c++14 -O2 -Wall -I stock/source/Lib -I stock/source/Lib/TLibCommon -I stock/source/Lib/TLibDecoder \
  -o hm_cu_dump "${here}/hm_cu_dump.cpp" -Wl,--start-group "${hm_libs[@]}" -Wl,--end-group
install -D -m 0755 hm_cu_dump "${OVT_ORACLES_PREFIX}/bin/hm_cu_dump"

# The encoder configs ({hm_cfg} of tests/corpora/synthetic/recipes.json, §14.3.3 rule 4): TAppEncoder -c
# ${OVT_ORACLES_PREFIX}/share/hm/cfg/encoder_randomaccess_main.cfg and the rest of HM's cfg/ tree.
[[ -f stock/cfg/encoder_randomaccess_main.cfg && -f stock/cfg/encoder_lowdelay_main.cfg ]] ||
  ovt_die "HM has no cfg/encoder_*_main.cfg"
mkdir -p "${OVT_ORACLES_PREFIX}/share/hm/cfg"
cp -a stock/cfg/. "${OVT_ORACLES_PREFIX}/share/hm/cfg/"

for tool in TAppEncoder TAppDecoder; do ovt_stamp "${tool}" "${HM_VERSION} (${HM_COMMIT})"; done
ovt_stamp hm_cu_dump "hm_cu_dump (OVT tests/oracles) on ${HM_VERSION} (${HM_COMMIT})"
ovt_stamp TAppDecoder_trace "${HM_VERSION} (${HM_COMMIT}) ENC_DEC_TRACE=1"
rm -rf "${work}"
