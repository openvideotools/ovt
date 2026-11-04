#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# libaom for the oracle image (chapter 14 §14.4.5, WI-0.554). Two builds from one verified archive:
#   1. stock shared libaom + aomenc/aomdec, installed into the prefix (FFmpeg's --enable-libaom links it);
#   2. the inspection build: `inspect` exists only with CONFIG_INSPECTION=1 AND ENABLE_EXAMPLES=1.
# The archive is the one third_party/VERSIONS.md pins (same SHA-256), so oracle and product agree.

# shellcheck source-path=SCRIPTDIR source=lib.sh
source "$(dirname "$0")/lib.sh"

LIBAOM_VERSION="3.12.1"
LIBAOM_SHA256="9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e"
LIBAOM_COMMIT="10aece4157eb79315da205f39e19bf6ab3ee30d0" # tag v3.12.1, recorded for ORACLE_VERSIONS.md

work="${1:-/tmp/build/libaom}"
mkdir -p "${work}"
cd "${work}" || exit 1
ovt_download "https://storage.googleapis.com/aom-releases/libaom-${LIBAOM_VERSION}.tar.gz" "${LIBAOM_SHA256}" aom.tar.gz
tar xzf aom.tar.gz
src="${work}/libaom-${LIBAOM_VERSION}"

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=0 -DENABLE_DOCS=0 -DENABLE_TOOLS=0)

cmake -S "${src}" -B stock "${common[@]}" -DBUILD_SHARED_LIBS=1 -DENABLE_EXAMPLES=1 \
  -DCMAKE_INSTALL_PREFIX="${OVT_ORACLES_PREFIX}" -DCMAKE_INSTALL_RPATH="${OVT_ORACLES_PREFIX}/lib"
cmake --build stock -j "${OVT_ORACLES_JOBS}"
cmake --install stock

cmake -S "${src}" -B inspection "${common[@]}" -DENABLE_EXAMPLES=1 -DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1
cmake --build inspection -j "${OVT_ORACLES_JOBS}" --target inspect
inspect_bin="$(find inspection -type f -name inspect -perm -u+x | head -n 1)"
[[ -n "${inspect_bin}" ]] || ovt_die "inspect was not built"
install -m 0755 "${inspect_bin}" "${OVT_ORACLES_PREFIX}/bin/inspect"

ovt_stamp inspect "libaom v${LIBAOM_VERSION} (${LIBAOM_COMMIT}) CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1"
rm -rf "${work}"
