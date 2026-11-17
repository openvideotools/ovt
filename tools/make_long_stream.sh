#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# make_long_stream.sh: the one-hour 1080p H.264 stream of chapter 7 §7.4.6 / WI-1.125 (`syn_h264_1h_1080p`,
# about 108 000 frames) for bench_index_h264_1h_1080p. It is the recipe-27 file of tests/corpora/synthetic/recipes.json
# (chapter 16 §16.3.5: the recipe replaces a hand-written encode), produced through the synthetic generator so that
# the bytes are deterministic (lavfi testsrc2 source, x264 with fixed settings and the pinned tool versions of the
# recipe file) and never committed. The file lands in the corpus cache (or --out) at
# syn_h264_1h_1080p/syn_h264_1h_1080p.264, the layout of every fetched or generated corpus; point $OVT_CORPORA_DIR
# at that cache directory for the benchmark. Nothing is written under the repository.
#
#   tools/make_long_stream.sh [--out <dir>] [--check]
#
# --check compares the produced hashes with the manifest instead of writing them. Needs ffmpeg (lavfi) and x264.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
generator="$here/../tests/corpora/synthetic/generate.py"
out=()
check=()
while [ $# -gt 0 ]; do
  case "$1" in
    --out) out=(--out "$2"); shift 2 ;;
    --check) check=(--check); shift ;;
    -h|--help) sed -n '5,16p' "$0"; exit 0 ;;
    *) echo "make_long_stream.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done
for tool in ffmpeg x264; do
  command -v "$tool" >/dev/null 2>&1 || { echo "make_long_stream.sh: $tool is not installed" >&2; exit 2; }
done
# The recipe needs the 1080p source (300 frames of testsrc2, looped 360 times); the generator makes it first.
python3 -I "$generator" --only syn_h264_1h_1080p --local-only "${out[@]}" "${check[@]}"
