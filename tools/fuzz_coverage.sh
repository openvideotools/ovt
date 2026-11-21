#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Line coverage of core/src/syntax/** reached by fuzz corpora (WI-1.129 acceptance test 2; docs/dev/fuzz.md).
#
#   tools/fuzz_coverage.sh BUILD_DIR OUT_DIR TARGET[:CORPUS_DIR...]...
#
# BUILD_DIR is a Clang build configured with -DOVT_COVERAGE=ON (no sanitizers needed) in which the
# ovt_fuzz_<target>_regress binaries are built. Every TARGET replays core/tests/fuzz/corpus/<target> plus the extra
# corpus directories given after it (colon-separated; e.g. the directory a local libFuzzer run grew). The raw
# profiles are merged into OUT_DIR/fuzz.profdata; OUT_DIR/syntax-report.txt is `llvm-cov report` restricted to
# core/src/syntax/, and its TOTAL line is printed. LLVM_PROFDATA and LLVM_COV override the tool names.
set -euo pipefail

if [ $# -lt 3 ]; then
  sed -n '6,13p' "$0" >&2
  exit 2
fi
build=$1
out=$2
shift 2
root=$(cd "$(dirname "$0")/.." && pwd)
profdata=${LLVM_PROFDATA:-llvm-profdata}
cov=${LLVM_COV:-llvm-cov}

mkdir -p "$out/raw"
rm -f "$out"/raw/*.profraw
objects=()
for spec in "$@"; do
  IFS=: read -r -a parts <<< "$spec"
  target=${parts[0]}
  bin="$build/core/tests/fuzz/ovt_fuzz_${target}_regress"
  [ -x "$bin" ] || { echo "fuzz_coverage.sh: $bin not built" >&2; exit 2; }
  dirs=("$root/core/tests/fuzz/regressions/$target" "$root/core/tests/fuzz/corpus/$target" "${parts[@]:1}")
  LLVM_PROFILE_FILE="$out/raw/$target-%p.profraw" "$bin" "${dirs[@]}" > /dev/null
  objects+=("-object" "$bin")
done
"$profdata" merge -sparse "$out"/raw/*.profraw -o "$out/fuzz.profdata"
# The first -object is the positional binary; the rest follow it.
"$cov" report "${objects[1]}" "${objects[@]:2}" -instr-profile="$out/fuzz.profdata" \
  -path-equivalence=/ovt,"$root" "$root/core/src/syntax/" > "$out/syntax-report.txt"
grep '^TOTAL' "$out/syntax-report.txt"
