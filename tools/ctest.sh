#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Lane dispatcher for CTest (chapter 13 §13.2.4, WI-0.001).
#
#   tools/ctest.sh <pr|nightly|sanitizers|perf> [configure-preset] [-- extra ctest args]
#
# A CMake test preset is bound to exactly one configure preset, so there is no host-agnostic `pr`
# preset. This script maps a lane to the lane preset of the host (or of the given configure preset):
#   linux-*   -> <lane>            (pr, nightly, sanitizers, perf)
#   macos-*   -> <lane>-macos      (pr-macos, nightly-macos, sanitizers-macos, perf-macos)
#   android-* -> pr-android        (only the pr lane exists for Android)
# and runs `ctest --preset <name>` from the repository root. It prints the command before running it.
#
# OVT_CTEST_HOST overrides `uname -s` (Darwin | Linux); tests use it to check the mapping on any host.
# OVT_CTEST_DRY_RUN=1 prints the command without running it.
# Keep this POSIX bash 3.2 compatible (macOS /bin/bash): no associative arrays, no ${var,,}.

set -euo pipefail

usage() {
  echo "usage: tools/ctest.sh <pr|nightly|sanitizers|perf> [configure-preset] [-- ctest args...]" >&2
  exit 2
}

[ $# -ge 1 ] || usage
lane="$1"
shift
case "$lane" in
  pr | nightly | sanitizers | perf) ;;
  -h | --help) usage ;;
  *)
    echo "tools/ctest.sh: unknown lane '$lane'" >&2
    usage
    ;;
esac

configure_preset=""
if [ $# -ge 1 ] && [ "$1" != "--" ]; then
  configure_preset="$1"
  shift
fi
if [ $# -ge 1 ] && [ "$1" = "--" ]; then
  shift
fi

if [ -n "$configure_preset" ]; then
  case "$configure_preset" in
    linux-*) platform="linux" ;;
    macos-*) platform="macos" ;;
    android-*) platform="android" ;;
    *)
      echo "tools/ctest.sh: cannot tell the platform of configure preset '$configure_preset'" >&2
      exit 2
      ;;
  esac
else
  host="${OVT_CTEST_HOST:-$(uname -s)}"
  case "$host" in
    Darwin) platform="macos" ;;
    Linux) platform="linux" ;;
    *)
      echo "tools/ctest.sh: unsupported host '$host' (Darwin and Linux only)" >&2
      exit 2
      ;;
  esac
fi

case "$platform" in
  linux) test_preset="$lane" ;;
  macos) test_preset="${lane}-macos" ;;
  android)
    if [ "$lane" != "pr" ]; then
      echo "tools/ctest.sh: lane '$lane' has no Android preset (only pr-android exists)" >&2
      exit 2
    fi
    test_preset="pr-android"
    ;;
esac

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
if [ $# -gt 0 ]; then
  echo "+ ctest --preset ${test_preset} $*" >&2
else
  echo "+ ctest --preset ${test_preset}" >&2
fi
if [ "${OVT_CTEST_DRY_RUN:-0}" = "1" ]; then
  exit 0
fi
cd "$repo_root"
exec ctest --preset "$test_preset" "$@"
