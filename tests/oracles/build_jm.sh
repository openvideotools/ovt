#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# JM 19.0 H.264 reference software for the oracle image (chapter 14 §14.4.3, §14.3.3; WI-0.554):
#   ldecod  built with TRACE 1 (writes trace_dec.txt in the working directory)
#   lencod  stock (the canonical PAFF generator of §14.3.3 recipe 2)
# Pitfall: JM 19.0 defines globals in headers, so modern GCC (default -fno-common) needs -fcommon.
# Source: the JM-19.0 tag of the HHI GitLab, commit-checked. The jm19.0.zip of §14.4.3 is not used because
# iphome.hhi.de serves an incomplete TLS certificate chain (curl error 60).

# shellcheck source-path=SCRIPTDIR source=lib.sh disable=SC1091 # followed only when lib.sh is checked too
source "$(dirname "$0")/lib.sh"

JM_VERSION="19.0"
JM_COMMIT="ef2362d159339d583fc18cea2de4d4bed5a1417c"

work="${1:-/tmp/build/jm}"
mkdir -p "${work}"
cd "${work}" || exit 1
ovt_git_fetch https://vcgit.hhi.fraunhofer.de/jvet/JM.git "refs/tags/JM-${JM_VERSION}" "${JM_COMMIT}" jm
src="${work}/jm"
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

ovt_stamp ldecod "JM ${JM_VERSION} TRACE=1 (${JM_COMMIT})"
ovt_stamp lencod "JM ${JM_VERSION} (${JM_COMMIT})"
rm -rf "${work}"
