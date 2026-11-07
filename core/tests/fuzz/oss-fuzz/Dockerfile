# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# OSS-Fuzz builder image (chapter 14 §14.7.4, WI-0.569): OVT's source plus the tools its build needs. No Qt.
FROM gcr.io/oss-fuzz-base/base-builder

# cmake >= 3.25 (CMakePresets.json version 6), Ninja, pkg-config for vcpkg's ports, zip for the seed corpora.
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build pkg-config zip unzip curl ca-certificates \
      autoconf automake libtool python3 \
    && rm -rf /var/lib/apt/lists/*

# The vcpkg submodule (third_party/vcpkg) comes with the clone; build.sh bootstraps it and builds the
# libraries from source under the sanitizer flags.
RUN git clone --depth 1 --recurse-submodules --shallow-submodules \
      https://github.com/openvideotools/ovt $SRC/ovt

WORKDIR $SRC/ovt
COPY build.sh $SRC/
