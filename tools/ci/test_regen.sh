#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# WI-0.083 acceptance test 1: touching core/schema/ovt_model.fbs and rebuilding ovt_core regenerates
# build/<preset>/generated/ovt_model_generated.h (newer mtime) and the embedded .bfbs, and writes nothing into
# the source tree. Also checks that the configure-time kModelSchemaSha256 equals tools/schema_hash.py.
#
#   tools/ci/test_regen.sh [<configure/build preset>]     (default linux-x64-debug)
#
# The build tree must already be configured and built once. Exit 0 on success, 1 on a failed check.
set -euo pipefail

preset="${1:-linux-x64-debug}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$root"
build="build/${preset}"
gen="${build}/generated"
header="${gen}/ovt_model_generated.h"
bfbs="${gen}/ovt_model.bfbs"
fbs="core/schema/ovt_model.fbs"

fail() {
  echo "test_regen: FAIL: $*" >&2
  exit 1
}
mtime() { python3 -I -c 'import os, sys; print(os.stat(sys.argv[1]).st_mtime_ns)' "$1"; }

[ -f "$header" ] || fail "$header missing; configure and build preset ${preset} first"
[ -f "$bfbs" ] || fail "$bfbs missing; configure and build preset ${preset} first"

# Everything git sees in the source tree (tracked changes and untracked, non-ignored files) before the rebuild.
before="$(git status --porcelain=v1 --untracked-files=all)"
header_before="$(mtime "$header")"
bfbs_before="$(mtime "$bfbs")"

sleep 1 # coarse-mtime file systems
touch "$fbs"
cmake --build --preset "$preset" --target ovt_core

header_after="$(mtime "$header")"
bfbs_after="$(mtime "$bfbs")"
[ "$header_after" -gt "$header_before" ] || fail "$header was not regenerated"
[ "$bfbs_after" -gt "$bfbs_before" ] || fail "$bfbs was not regenerated"

after="$(git status --porcelain=v1 --untracked-files=all)"
[ "$before" = "$after" ] || fail "the rebuild changed the source tree:
$(diff <(echo "$before") <(echo "$after") || true)"

expected="$(python3 -I tools/schema_hash.py "$fbs" | cut -d' ' -f1)"
grep -q "\"${expected}\"" "${gen}/model/schema_hash.inc" || fail "kModelSchemaSha256 is not ${expected}"

echo "test_regen: OK (${header} and ${bfbs} regenerated, source tree unchanged, schema hash ${expected})"
