#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build OVT_Studio-<ver>-<arch>.AppImage from a built Linux preset (chapter 13 §13.6.2, WI-0.070).
#
#   tools/linux/appimage.sh <preset> [--out DIR] [--skip-notices-build]
#
#   <preset>   linux-x64-release or linux-arm64-release (any linux-x64-* / linux-arm64-* preset works)
#   --out DIR  where AppDir and the AppImage are written (default build/<preset>/appimage)
#
# The preset must be configured and built first (`cmake --preset <p> && cmake --build --preset <p>`), on the
# Ubuntu 22.04 CI image so the glibc baseline is 2.35. Environment:
#   QT_ROOT_DIR       Qt install (tools/install_qt.py); its gcc_64/bin/qmake is the QMAKE of the Qt plugin
#   LINUXDEPLOY       linuxdeploy executable (default: linuxdeploy on PATH); the qt and appimage plugins
#                     (linuxdeploy-plugin-qt, linuxdeploy-plugin-appimage) must be on PATH or next to it
#   OVT_APPIMAGE_UPDATE_INFO  override of the zsync update information (default as §13.6.2, per architecture)
#   OVT_APPIMAGE_VERSION      override of the version in the file name (default: VERSION file)
#
# Steps: (1) cmake --install into AppDir/usr; (2) notices, licences and PATENTS beside the binaries; (3) desktop
# file, icon and the custom AppRun; (4) linuxdeploy with the Qt plugin (libvulkan.so.1 stays excluded: the host
# ICDs must load); (5) check that the plugin directory and the AppRun made it into the image. The AppImage is
# not signed and nothing is uploaded: release signing and publishing belong to WI-0.036.

set -euo pipefail

die() { echo "appimage.sh: $*" >&2; exit 1; }
usage() { sed -n "5,12p" "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

preset=""
out=""
build_notices=1
while [ "$#" -gt 0 ]; do
  case "$1" in
    --out) [ "$#" -ge 2 ] || usage; out=$2; shift 2 ;;
    --skip-notices-build) build_notices=0; shift ;;
    -h|--help) usage ;;
    -*) echo "appimage.sh: unknown option $1" >&2; usage ;;
    *) [ -z "$preset" ] || usage; preset=$1; shift ;;
  esac
done
[ -n "$preset" ] || usage
case "$preset" in
  linux-x64-*) arch=x86_64 ;;
  linux-arm64-*) arch=aarch64 ;;
  *) die "preset '$preset' is not a linux-x64-* or linux-arm64-* preset" ;;
esac
[ "$(uname -s)" = Linux ] || die "this script runs on Linux only"

repo=$(cd "$(dirname "$0")/../.." && pwd)
build="$repo/build/$preset"
[ -f "$build/CMakeCache.txt" ] || die "$build is not configured: run cmake --preset $preset and build it first"
version=${OVT_APPIMAGE_VERSION:-$(tr -d '[:space:]' < "$repo/VERSION")}
[ -n "$version" ] || die "empty version"
[ -n "$out" ] || out="$build/appimage"
linuxdeploy=${LINUXDEPLOY:-linuxdeploy}
command -v "$linuxdeploy" >/dev/null 2>&1 || [ -x "$linuxdeploy" ] || die "linuxdeploy not found: set LINUXDEPLOY or put it on PATH"

[ -n "${QT_ROOT_DIR:-}" ] || die "QT_ROOT_DIR is not set (tools/install_qt.py prints it)"
qmake="$QT_ROOT_DIR/gcc_64/bin/qmake"
[ -x "$qmake" ] || qmake="$QT_ROOT_DIR/bin/qmake"
[ -x "$qmake" ] || die "no qmake under $QT_ROOT_DIR/gcc_64/bin or $QT_ROOT_DIR/bin"

# ---- (1) install ---------------------------------------------------------------------------------------------------
appdir="$out/AppDir"
rm -rf "$out"
mkdir -p "$appdir"
if [ "$build_notices" -eq 1 ]; then
  cmake --build "$build" --target ovt_notices
fi
notices="$build/THIRD_PARTY_NOTICES.md"
[ -f "$notices" ] || die "$notices is missing: build the ovt_notices target (licence obligation: the notices travel with the AppImage)"
cmake --install "$build" --prefix "$appdir/usr"
[ -x "$appdir/usr/bin/ovt-studio" ] || die "cmake --install produced no usr/bin/ovt-studio: is OVT_BUILD_GUI on in $preset?"
[ -x "$appdir/usr/bin/ovt" ] || die "cmake --install produced no usr/bin/ovt: is OVT_BUILD_CLI on in $preset?"
# GNUInstallDirs picks lib64 on some hosts (Fedora); the AppRun and the plugin path assume usr/lib.
if [ -d "$appdir/usr/lib64" ]; then
  mkdir -p "$appdir/usr/lib"
  cp -a "$appdir/usr/lib64/." "$appdir/usr/lib/"
  rm -rf "${appdir:?}/usr/lib64"
fi

# ---- (2) notices, licences, PATENTS --------------------------------------------------------------------------------
doc="$appdir/usr/share/doc/ovt"
mkdir -p "$doc/LICENSES"
cp "$notices" "$doc/THIRD_PARTY_NOTICES.md"
if [ -f "$build/PATENT_NOTICE.md" ]; then cp "$build/PATENT_NOTICE.md" "$doc/"; fi
cp "$repo"/LICENSES/*.txt "$doc/LICENSES/"
if [ -e "$appdir/usr/lib/ovt/plugins/libovt_plugin_aom.so" ]; then
  cp "$repo/LICENSES/LicenseRef-AOM-Patent-License-1.0.txt" "$doc/PATENTS"
fi

# ---- (3) desktop file, icon, AppRun --------------------------------------------------------------------------------
desktop="$repo/app/resources/org.openvideotools.Studio.desktop" # WI-0.071 adds it; until then the one below
if [ ! -f "$desktop" ]; then
  desktop="$out/org.openvideotools.Studio.desktop"
  cat > "$desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=OVT Studio
GenericName=Video analyzer
Comment=Inspect video bitstreams, containers, motion vectors and quality metrics
Exec=ovt-studio %F
Icon=ovt-studio
Terminal=false
Categories=AudioVideo;Video;Development;
MimeType=video/mp4;video/x-matroska;video/webm;video/mp2t;video/x-h264;video/x-h265;
DESKTOP
fi
icon="$repo/app/resources/icons/ovt-studio.svg"
[ -f "$icon" ] || die "$icon is missing"

# ---- (4) linuxdeploy -----------------------------------------------------------------------------------------------
update_info=${OVT_APPIMAGE_UPDATE_INFO:-"gh-releases-zsync|openvideotools|ovt|latest|OVT_Studio-*-$arch.AppImage.zsync"}
(
  cd "$out"
  export ARCH=$arch
  export QMAKE=$qmake
  export EXTRA_QT_MODULES='svg;quickcontrols2;graphs'
  export QML_SOURCES_PATHS=$repo/app/qml
  export DEPLOY_PLATFORM_THEMES=1
  export LDAI_UPDATE_INFORMATION=$update_info
  export LDAI_OUTPUT="OVT_Studio-$version-$arch.AppImage"
  export LD_LIBRARY_PATH="$appdir/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  # Plugins are dlopen'ed, so linuxdeploy does not see them through the executables: pass each as -l.
  extra=()
  for p in "$appdir"/usr/lib/ovt/plugins/*.so; do
    [ -e "$p" ] && extra+=(--library "$p")
  done
  # libvulkan.so.1 is in linuxdeploy's built-in exclude list; it must stay there (host ICDs). Do not remove it.
  "$linuxdeploy" --appdir "$appdir" \
    --executable "$appdir/usr/bin/ovt-studio" --executable "$appdir/usr/bin/ovt" \
    ${extra[@]+"${extra[@]}"} \
    --desktop-file "$desktop" --icon-file "$icon" --icon-filename ovt-studio \
    --custom-apprun "$repo/packaging/appimage/AppRun" \
    --plugin qt --output appimage
)

# ---- (5) checks ----------------------------------------------------------------------------------------------------
image="$out/OVT_Studio-$version-$arch.AppImage"
[ -f "$image" ] || die "linuxdeploy did not produce $image"
[ -x "$appdir/AppRun" ] || die "AppRun is missing or not executable in $appdir"
cmp -s "$appdir/AppRun" "$repo/packaging/appimage/AppRun" || die "AppDir/AppRun is not packaging/appimage/AppRun"
if [ -e "$appdir/usr/lib/libvulkan.so.1" ]; then
  die "libvulkan.so.1 was bundled: the host Vulkan loader must be used (§13.6.2)"
fi
if [ ! -f "$image.zsync" ]; then
  echo "appimage.sh: warning: no $image.zsync written (update information embedded: $update_info)" >&2
fi

echo "appimage.sh: $image ($arch, $version)"
