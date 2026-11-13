#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Strip the Mach-O files that OVT builds itself and keep their debug info as dSYM bundles (chapter 13 §13.5.2,
# WI-0.060).
#
#   tools/mac/strip.sh <OVT Studio.app> <dsym-dir>
#
# Stripped: Contents/MacOS/*, the loose libraries in Contents/Frameworks (libovt_*, libav*_ovt, libMoltenVK, ...)
# and Contents/PlugIns/ovt/*. Not touched: Qt (*.framework and Qt's own plugins), which ships stripped. The dSYM of
# each file goes to <dsym-dir>/<file name>.dSYM; bundle.sh archives that directory next to the DMG so crash
# reports can be symbolicated. stripping invalidates a code signature, so bundle.sh signs afterwards.
# Written for the macOS system bash 3.2: no mapfile, no associative arrays.

set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: strip.sh <OVT Studio.app> <dsym-dir>" >&2
  exit 2
fi
app=$1
dsym_dir=$2
contents="$app/Contents"
if [ ! -d "$contents" ]; then
  echo "strip.sh: $app is not an app bundle" >&2
  exit 2
fi
mkdir -p "$dsym_dir"

is_macho() { file -b "$1" | grep -q 'Mach-O'; }

count=0
while IFS= read -r -d '' f; do
  [ -L "$f" ] && continue
  is_macho "$f" || continue
  name=$(basename "$f")
  # dsymutil prints a warning, not an error, for a file without DWARF (MoltenVK is prebuilt).
  dsymutil "$f" -o "$dsym_dir/$name.dSYM" 2>/dev/null || true
  strip -S -x "$f"
  count=$((count + 1))
done < <(
  {
    find "$contents/MacOS" -type f -print0
    find "$contents/Frameworks" -maxdepth 1 -type f -name '*.dylib' -print0
    if [ -d "$contents/PlugIns/ovt" ]; then find "$contents/PlugIns/ovt" -type f -name '*.dylib' -print0; fi
  }
)

echo "strip.sh: stripped $count file(s), dSYMs in $dsym_dir"
