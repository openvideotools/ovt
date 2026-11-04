#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# JM 19.0 H.264 reference software for the oracle image (chapter 14 §14.4.3, §14.3.3; WI-0.554):
#   ldecod  built with TRACE 1 (writes trace_dec.txt in the working directory)
#   lencod  stock (the canonical PAFF generator of §14.3.3 recipe 2)
# Pitfall: JM 19.0 defines globals in headers, so modern GCC (default -fno-common) needs -fcommon.

# shellcheck source-path=SCRIPTDIR source=lib.sh
source "$(dirname "$0")/lib.sh"

JM_VERSION="19.0"
JM_SHA256="0000000000000000000000000000000000000000000000000000000000000000"

work="${1:-/tmp/build/jm}"
mkdir -p "${work}"
cd "${work}" || exit 1
ovt_download "https://iphome.hhi.de/suehring/tml/download/jm${JM_VERSION}.zip" "${JM_SHA256}" jm.zip
unzip -q jm.zip
src="$(dirname "$(find "${work}" -maxdepth 3 -type d -name ldecod | head -n 1)")"
[[ -d "${src}/ldecod" && -d "${src}/lencod" ]] || ovt_die "unexpected JM archive layout"

defines="${src}/ldecod/inc/defines.h"
sed -i 's/^#define TRACE .*/#define TRACE 1/' "${defines}"
grep -Eq '^#define TRACE 1$' "${defines}" || ovt_die "TRACE not enabled in ${defines}"

jm_cc="gcc -fcommon"
make -C "${src}/ldecod" -j "${OVT_ORACLES_JOBS}" CC="${jm_cc}"
make -C "${src}/lencod" -j "${OVT_ORACLES_JOBS}" CC="${jm_cc}"

for tool in ldecod lencod; do
  bin="$(find "${src}/bin" -maxdepth 1 -type f -name "${tool}*" -perm -u+x | head -n 1)"
  [[ -n "${bin}" ]] || ovt_die "${tool} was not built"
  install -D -m 0755 "${bin}" "${OVT_ORACLES_PREFIX}/bin/${tool}"
done

ovt_stamp ldecod "JM ${JM_VERSION} TRACE=1 (jm${JM_VERSION}.zip sha256 ${JM_SHA256})"
ovt_stamp lencod "JM ${JM_VERSION} (jm${JM_VERSION}.zip sha256 ${JM_SHA256})"
rm -rf "${work}"
