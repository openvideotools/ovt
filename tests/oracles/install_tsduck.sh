#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# TSDuck (tsp, tstables, tsanalyze), the MPEG-TS oracle of chapter 8 WI-4.205 to WI-4.207 and WI-4.597,
# from the pinned upstream Ubuntu 24.04 .deb (chapter 14 §14.2.6, D27, WI-0.554). BSD-2-Clause.
# apt resolves the .deb's dependencies from the image's pinned Ubuntu snapshot.

# shellcheck source-path=SCRIPTDIR source=lib.sh disable=SC1091 # followed only when lib.sh is checked too
source "$(dirname "$0")/lib.sh"

TSDUCK_VERSION="3.45-4798"
TSDUCK_SHA256="0023689f76e75b64e45253771208bcf8ca95b91fbfd0900da097c729f3c39fde"

deb="/tmp/tsduck_${TSDUCK_VERSION}.ubuntu24_amd64.deb"
ovt_download "https://github.com/tsduck/tsduck/releases/download/v${TSDUCK_VERSION}/tsduck_${TSDUCK_VERSION}.ubuntu24_amd64.deb" \
  "${TSDUCK_SHA256}" "${deb}"
apt-get install -y --no-install-recommends "${deb}"
rm -f "${deb}"
tsp --version 2>&1 | grep -q "${TSDUCK_VERSION}" || ovt_die "tsp does not report ${TSDUCK_VERSION}"
