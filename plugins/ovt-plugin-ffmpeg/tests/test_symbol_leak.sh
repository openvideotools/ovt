#!/bin/sh
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Symbol-leak check of the FFmpeg plugin (WI-0.012 acceptance test 2, chapter 13 §13.3.3):
#
#   test_symbol_leak.sh <libovt_plugin_ffmpeg.so | .dylib>
#
# The shared object must export exactly ovt_analysis_decoder_v1 and ovt_plugin_describe: no av*, sws*, swr* or ff_*
# symbol of its private static FFmpeg, and nothing else. Linker-generated symbols (_init, _fini, _edata, _end,
# __bss_start) are ignored. Exit 0 on success; otherwise prints the unexpected symbols and exits 1.

set -eu

if [ $# -ne 1 ] || [ ! -f "$1" ]; then
  echo "usage: $0 <plugin shared object>" >&2
  exit 2
fi
lib=$1
nm_tool=${NM:-nm}

if [ "$(uname -s)" = "Darwin" ]; then
  # -g external, -U defined only, -j names only; Mach-O prefixes C symbols with an underscore.
  exports=$("$nm_tool" -gUj "$lib" | sed 's/^_//')
else
  exports=$("$nm_tool" -D --defined-only "$lib" | awk 'NF >= 3 { print $3 }')
fi
# Two greps: BSD grep (macOS) rejects an empty alternative such as `(a|)`.
exports=$(printf '%s\n' "$exports" | grep -v -E '^(_init|_fini|_edata|_end|__bss_start)$' | grep -v '^$' | sort -u)

expected=$(printf '%s\n' ovt_analysis_decoder_v1 ovt_plugin_describe)

status=0
leaked=$(printf '%s\n' "$exports" | grep -E '^(av|sws|swr|ff_|avpriv_)' || true)
if [ -n "$leaked" ]; then
  echo "FAIL: $lib exports FFmpeg symbols:" >&2
  printf '%s\n' "$leaked" | sed 's/^/  /' >&2
  status=1
fi
if [ "$exports" != "$expected" ]; then
  echo "FAIL: $lib exports" >&2
  printf '%s\n' "$exports" | sed 's/^/  /' >&2
  echo "expected exactly:" >&2
  printf '%s\n' "$expected" | sed 's/^/  /' >&2
  status=1
fi
if [ "$status" -eq 0 ]; then
  echo "OK: $lib exports only: $(printf '%s\n' "$exports" | tr '\n' ' ')"
fi
exit "$status"
