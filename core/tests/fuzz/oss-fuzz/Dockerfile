# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# OSS-Fuzz builder image (chapter 14 §14.7.4, WI-0.569): OVT's source plus the tools its build needs. No Qt.
# The tag matches base_os_version in project.yaml: the untagged image may still be Ubuntu 20.04, which has no xxd package.
FROM gcr.io/oss-fuzz-base/base-builder:ubuntu-24-04

# Host tools of the configure step (found out by the first Mini run, WI-0.569):
#   - Meson >= 1.4: third_party/dav1d.cmake and libvmaf.cmake build through ovt_tp_meson() (cmake/OvtMeson.cmake).
#     Ubuntu's own package is older, so pip provides it. pip also provides cmake >= 3.25 (CMakePresets.json
#     version 6) and ninja, on any base OS version.
#   - nasm: dav1d, libvmaf and FFmpeg assembly on x86_64.
#   - xxd: libvmaf's built-in models (its configure step fails without it).
#   - make, pkg-config, zlib1g-dev: FFmpeg's ./configure (third_party/ffmpeg.cmake).
#   - zip, unzip, curl, autoconf, automake, libtool: vcpkg ports and build.sh's seed corpus archives.
RUN apt-get update && apt-get install -y --no-install-recommends \
      make pkg-config nasm xxd zlib1g-dev zip unzip curl ca-certificates \
      autoconf autoconf-archive automake libtool python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*
RUN pip3 install --break-system-packages 'cmake>=3.25' 'meson>=1.4' ninja \
    || pip3 install 'cmake>=3.25' 'meson>=1.4' ninja

# The vcpkg submodule (third_party/vcpkg) comes with the clone; build.sh bootstraps it and builds the
# libraries from source under the sanitizer flags.
RUN git clone --depth 1 --recurse-submodules --shallow-submodules \
      https://github.com/openvideotools/ovt $SRC/ovt

WORKDIR $SRC/ovt
COPY build.sh $SRC/
