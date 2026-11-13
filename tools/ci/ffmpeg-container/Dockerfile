# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# ffmpeg 6.1 with libx265 3.5 for the hosted ubuntu-22.04 CI jobs (setup-ovt input `ffmpeg-container`). The committed
# pattern streams (tests/corpora/patterns, tools/gen_hdr_patterns.py) were muxed by ffmpeg 6.x, and the byte-exact
# `gen_hdr_patterns.py --check` of tools/tests cannot pass with 22.04's ffmpeg 4.4, whose MP4 muxer writes other
# bytes. The x265 pin in encoder.json (3.5+1-f0c1022b6) is Ubuntu's own x265, so only a distro build of the same
# series fits; a static third-party build would carry another x265 and make the check skip. The image is built in
# the CI job and used only through a wrapper on PATH; it is never uploaded or shipped (the base is pinned by digest (ubuntu 24.04 as of 2026-11-13); ffmpeg is GPL-2.0-or-later
# in this build; nothing of it enters an OVT artefact).
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5
RUN apt-get update -q \
    && apt-get install -y -q --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*
