#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Assemble "OVT Studio.app" from a built macOS preset (chapter 13 §13.5.2, WI-0.060).
#
#   tools/mac/bundle.sh <preset> [--out DIR] [--skip-notices-build]
#
#   <preset>   macos-arm64-release or macos-x86_64-release (any macos-* preset works; only *-release strips)
#   --out DIR  where the bundle is staged (default build/<preset>/bundle)
#
# The preset must be configured and built first (`cmake --preset <p> && cmake --build --preset <p>`). Environment:
#   QT_ROOT_DIR               Qt install (macdeployqt is $QT_ROOT_DIR/macos/bin/macdeployqt); else macdeployqt on PATH
#   OVT_THIRD_PARTY_PREFIX    prefix of the third_party builds (default build/third_party/<preset>, as cmake/OvtOptions.cmake)
#   OVT_BUNDLE_ADHOC_SIGN     1 (default) signs ad hoc at the end, because an arm64 Mach-O must carry a signature to
#                             run at all. Real signing, entitlements and notarisation are WI-0.061.
#   PYTHON                    interpreter for fix_rpaths.py (default python3)
#
# Steps: (1) copy the built .app skeleton; (2) build the notices and `cmake --install` into Contents/; (3) move the
# install tree to the bundle layout; (4) add the licence notices, PATENTS and MoltenVK; (5) macdeployqt, with every
# plugin passed as -executable because macdeployqt does not see libraries that are only dlopen'ed; (6)
# fix_rpaths.py --fix; (7) strip.sh with the dSYM archive (release only); (8) fix_rpaths.py verification; (9) ad-hoc
# signature; (10) smoke test of both executables. Nothing here uploads or publishes anything.
# Written for the macOS system bash 3.2: no mapfile, no associative arrays.

set -euo pipefail

die() { echo "bundle.sh: $*" >&2; exit 1; }
usage() { sed -n "5,22p" "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

preset=""
out=""
build_notices=1
while [ "$#" -gt 0 ]; do
  case "$1" in
    --out) [ "$#" -ge 2 ] || usage; out=$2; shift 2 ;;
    --skip-notices-build) build_notices=0; shift ;;
    -h|--help) usage ;;
    -*) echo "bundle.sh: unknown option $1" >&2; usage ;;
    *) [ -z "$preset" ] || usage; preset=$1; shift ;;
  esac
done
[ -n "$preset" ] || usage
case "$preset" in
  macos-arm64-*) arch=arm64 ;;
  macos-x86_64-*) arch=x86_64 ;;
  *) die "preset '$preset' is not a macos-arm64-* or macos-x86_64-* preset" ;;
esac
[ "$(uname -s)" = Darwin ] || die "this script runs on macOS only"

repo=$(cd "$(dirname "$0")/../.." && pwd)
build="$repo/build/$preset"
[ -f "$build/CMakeCache.txt" ] || die "$build is not configured: run cmake --preset $preset and build it first"
tp=${OVT_THIRD_PARTY_PREFIX:-$repo/build/third_party/$preset}
py=${PYTHON:-python3}
version=$(tr -d '[:space:]' < "$repo/VERSION")
[ -n "$out" ] || out="$build/bundle"
dsym_dir="$repo/build/dsym/$preset"

macdeployqt=""
if [ -n "${QT_ROOT_DIR:-}" ] && [ -x "$QT_ROOT_DIR/macos/bin/macdeployqt" ]; then
  macdeployqt="$QT_ROOT_DIR/macos/bin/macdeployqt"
elif command -v macdeployqt >/dev/null 2>&1; then
  macdeployqt=$(command -v macdeployqt)
else
  die "macdeployqt not found: set QT_ROOT_DIR (tools/install_qt.py) or put it on PATH"
fi

built_app=""
for candidate in "$build"/app/*.app; do
  [ -d "$candidate" ] && built_app=$candidate && break
done
[ -n "$built_app" ] || die "no .app under $build/app: build the ovt_studio target of $preset first"

# ---- (1) skeleton -------------------------------------------------------------------------------------------------
rm -rf "$out"
mkdir -p "$out"
app="$out/OVT Studio.app"
ditto "$built_app" "$app"
contents="$app/Contents"

# ---- (2) notices and install ------------------------------------------------------------------------------------
if [ "$build_notices" -eq 1 ]; then
  cmake --build "$build" --target ovt_notices
fi
notices="$build/THIRD_PARTY_NOTICES.md"
[ -f "$notices" ] || die "$notices is missing: build the ovt_notices target (licence obligation: the notices travel with the bundle)"
cmake --install "$build" --prefix "$contents"

# ---- (3) install tree -> bundle layout ---------------------------------------------------------------------------
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/PlugIns/ovt" "$contents/Resources/ovt"
[ -x "$contents/bin/ovt" ] || die "cmake --install produced no bin/ovt: is ovt_cli part of preset $preset?"
mv "$contents/bin/ovt" "$contents/MacOS/ovt"
if [ -d "$contents/lib" ]; then
  find "$contents/lib" -maxdepth 1 \( -type f -o -type l \) -name '*.dylib' -exec mv {} "$contents/Frameworks/" \;
  if [ -d "$contents/lib/ovt/plugins" ]; then
    find "$contents/lib/ovt/plugins" -maxdepth 1 -name '*.dylib' -exec mv {} "$contents/PlugIns/ovt/" \;
  fi
fi
if [ -d "$contents/share/ovt" ]; then
  cp -R "$contents/share/ovt/." "$contents/Resources/ovt/"
fi
rm -rf "${contents:?}/bin" "${contents:?}/lib" "${contents:?}/include" "${contents:?}/share"

# ---- (4) notices, PATENTS, MoltenVK ------------------------------------------------------------------------------
cp "$notices" "$contents/Resources/THIRD_PARTY_NOTICES.md"
if [ -f "$build/PATENT_NOTICE.md" ]; then cp "$build/PATENT_NOTICE.md" "$contents/Resources/"; fi
mkdir -p "$contents/Resources/LICENSES"
cp "$repo"/LICENSES/*.txt "$contents/Resources/LICENSES/"
if [ -e "$contents/PlugIns/ovt/libovt_plugin_aom.dylib" ]; then
  cp "$repo/LICENSES/LicenseRef-AOM-Patent-License-1.0.txt" "$contents/Resources/PATENTS"
fi
if [ ! -e "$contents/Frameworks/libMoltenVK.dylib" ] && [ -f "$tp/moltenvk/lib/libMoltenVK.dylib" ]; then
  cp "$tp/moltenvk/lib/libMoltenVK.dylib" "$contents/Frameworks/"
fi

# ---- (5) macdeployqt ----------------------------------------------------------------------------------------------
dq_args=("$app" "-qmldir=$repo/app/qml" "-executable=$contents/MacOS/ovt" -always-overwrite -verbose=1)
for lib_dir in "$tp"/*/lib; do
  [ -d "$lib_dir" ] && dq_args+=("-libpath=$lib_dir")
done
for plugin in "$contents"/PlugIns/ovt/*.dylib; do
  [ -e "$plugin" ] && dq_args+=("-executable=$plugin")
done
"$macdeployqt" "${dq_args[@]}"

# Bundle icon: best effort, from app/resources/icons/ovt-studio.svg (a bundle without it shows the generic icon).
if command -v rsvg-convert >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
  iconset="$out/ovt-studio.iconset"
  mkdir -p "$iconset"
  for size in 16 32 128 256 512; do
    rsvg-convert -w "$size" -h "$size" "$repo/app/resources/icons/ovt-studio.svg" -o "$iconset/icon_${size}x${size}.png"
    rsvg-convert -w $((size * 2)) -h $((size * 2)) "$repo/app/resources/icons/ovt-studio.svg" -o "$iconset/icon_${size}x${size}@2x.png"
  done
  iconutil -c icns "$iconset" -o "$contents/Resources/ovt-studio.icns"
  rm -rf "$iconset"
else
  echo "bundle.sh: rsvg-convert or iconutil missing: no bundle icon" >&2
fi

# ---- (6) rpaths ---------------------------------------------------------------------------------------------------
"$py" -I "$repo/tools/mac/fix_rpaths.py" "$app" --fix

# ---- (7) strip with dSYM archive (release only) ------------------------------------------------------------------
case "$preset" in
  *-release)
    rm -rf "$dsym_dir"
    "$repo/tools/mac/strip.sh" "$app" "$dsym_dir"
    # gzip -n and a sorted file list keep the archive free of timestamps and ordering noise.
    (cd "$dsym_dir" && find . -mindepth 1 | LC_ALL=C sort | tar -cf - -T - | gzip -n \
      > "$repo/build/dsym/OVT-Studio-$version-$arch.dSYM.tar.gz")
    ;;
  *) echo "bundle.sh: $preset is not a release preset: not stripping" ;;
esac

# ---- (8) verification: AT1 of WI-0.060 ----------------------------------------------------------------------------
"$py" -I "$repo/tools/mac/fix_rpaths.py" "$app"

# Licence notices must name what the bundle ships (FFmpeg, MoltenVK and Qt are the ones with obligations).
check_notice() { # <file in Frameworks> <name in the notices>
  # shellcheck disable=SC2086 # the pattern argument is a glob on purpose
  if ls "$contents"/Frameworks/$1 >/dev/null 2>&1 && ! grep -qi "$2" "$contents/Resources/THIRD_PARTY_NOTICES.md"; then
    die "the bundle ships $1 but THIRD_PARTY_NOTICES.md does not mention $2"
  fi
}
check_notice 'libav*_ovt*.dylib' FFmpeg
check_notice 'libMoltenVK.dylib' MoltenVK
check_notice 'QtCore.framework' 'Qt'

# ---- (9) ad-hoc signature (WI-0.061 replaces this with real signing) -----------------------------------------------
if [ "${OVT_BUNDLE_ADHOC_SIGN:-1}" = 1 ]; then
  sign() { codesign --force --sign - --timestamp=none "$@"; }
  while IFS= read -r -d '' f; do sign "$f"; done < <(find "$contents" -name '*.dylib' -type f -not -path '*.framework/*' -print0)
  while IFS= read -r -d '' f; do sign "$f"; done < <(find "$contents/Frameworks" -maxdepth 1 -name '*.framework' -print0)
  sign "$contents/MacOS/ovt"
  sign "$app"
  codesign --verify --strict "$app"
fi

# ---- (10) smoke test ----------------------------------------------------------------------------------------------
"$contents/MacOS/ovt" --version
"$contents/MacOS/OVT Studio" --version

echo "bundle.sh: $app ($arch, $version)"
