#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Android build host setup (chapter 13 §13.9.1, WI-0.006, WI-0.050); run after tools/bootstrap.sh.
#
#   tools/bootstrap_android.sh [--fdroid] [--print-only]
#
#   1. checks JDK 17 and ANDROID_SDK_ROOT (or ANDROID_HOME) with sdkmanager;
#   2. installs the pinned SDK packages through sdkmanager: the NDK, platform, build-tools, platform-tools
#      and CMake of android/gradle/libs.versions.toml (keys ndk, compileSdk, buildTools, cmake; fallback: the
#      chapter 13 §13.4 pins below if the file is missing);
#   3. installs the Qt Android kits through tools/install_qt.py --target android (WI-0.018), when present;
#   4. writes android/local.properties (sdk.dir, ndk.dir, qt.dir = QT_ROOT_DIR for the Qt Gradle plugin of
#      android/app/build.gradle.kts; with --fdroid also ovt.fdroid=true for the F-Droid flavour, chapter 13
#      §13.6). Then `cd android && ./gradlew :app:assembleFossDebug` builds the per-ABI APKs (WI-0.050).
#
#   --print-only  print the sdkmanager command instead of running it.
#
# Uses the helpers of tools/bootstrap.sh. It never runs sudo. bash 3.2 compatible.

set -euo pipefail
export OVT_BOOTSTRAP_LIB=1
# shellcheck source=bootstrap.sh disable=SC1091  # sourced relative to this script
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bootstrap.sh"

# Fallback pins (chapter 13 §13.4.1 libs.versions.toml: ndk, compileSdk, cmake, buildTools).
FALLBACK_NDK="27.2.12479018"
FALLBACK_COMPILE_SDK="36"
FALLBACK_CMAKE="3.30.5"
FALLBACK_BUILD_TOOLS="36.0.0"

# toml_value <key>: the quoted value of `key = "…"` in android/gradle/libs.versions.toml.
toml_value() {
  local f="$OVT_ROOT/android/gradle/libs.versions.toml"
  [ -f "$f" ] || return 0
  sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$f" | head -n 1
}

fdroid=0
print_only=0
while [ $# -gt 0 ]; do
  case "$1" in
    --fdroid) fdroid=1 ;;
    --print-only) print_only=1 ;;
    -h|--help) sed -n '6,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

step "1/4 JDK and Android SDK"
if ! have java || ! java -version 2>&1 | grep -Eq 'version "17(\.|")'; then
  case "$(host_os)" in
    macos) die "JDK 17 required: brew install --cask temurin@17, then set JAVA_HOME" ;;
    *) die "JDK 17 required: sudo apt-get install -y openjdk-17-jdk-headless, then set JAVA_HOME" ;;
  esac
fi
sdk="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
[ -n "$sdk" ] || die "set ANDROID_SDK_ROOT to the Android SDK (Android Studio: ~/Library/Android/sdk or ~/Android/Sdk)"
sdkmanager=""
for c in "$sdk/cmdline-tools/latest/bin/sdkmanager" "$(command -v sdkmanager || true)"; do
  if [ -n "$c" ] && [ -x "$c" ]; then sdkmanager="$c"; break; fi
done
[ -n "$sdkmanager" ] || die "sdkmanager not found: install the Android command-line tools into $sdk/cmdline-tools/latest"

ndk="$(toml_value ndk)"; ndk="${ndk:-$FALLBACK_NDK}"
api="$(toml_value compileSdk)"; api="${api:-$FALLBACK_COMPILE_SDK}"
cmake_v="$(toml_value cmake)"; cmake_v="${cmake_v:-$FALLBACK_CMAKE}"
build_tools="$(toml_value buildTools)"; build_tools="${build_tools:-$FALLBACK_BUILD_TOOLS}"
[ -f "$OVT_ROOT/android/gradle/libs.versions.toml" ] ||
  warn "android/gradle/libs.versions.toml is missing; using the chapter 13 pins NDK $ndk, API $api, CMake $cmake_v"

step "2/4 SDK packages"
packages="platform-tools platforms;android-$api build-tools;$build_tools ndk;$ndk cmake;$cmake_v"
if [ "$print_only" = 1 ]; then
  say "$sdkmanager --install $packages"
else
  # shellcheck disable=SC2086  # the package list is split on purpose
  yes | "$sdkmanager" --licenses >/dev/null || true
  # shellcheck disable=SC2086
  "$sdkmanager" --install $packages
fi

step "3/4 Qt Android kits"
# install_qt.py prints QT_ROOT_DIR=<dest>/<qt version> as its last stdout line; the Gradle build needs that
# directory (it holds android_arm64_v8a, android_x86_64 and the host kit). An exported QT_ROOT_DIR wins.
qt_root="${QT_ROOT_DIR:-}"
if [ -f "$OVT_ROOT/tools/install_qt.py" ] && [ -x "$OVT_ROOT/.venv/bin/python" ]; then
  qt_line="$("$OVT_ROOT/.venv/bin/python" -I "$OVT_ROOT/tools/install_qt.py" --target android | tail -n 1)"
  case "$qt_line" in QT_ROOT_DIR=*) [ -n "$qt_root" ] || qt_root="${qt_line#QT_ROOT_DIR=}" ;; esac
else
  say "skipped: tools/install_qt.py (WI-0.018) or .venv (run tools/bootstrap.sh first) is missing"
fi
[ -n "$qt_root" ] || warn "QT_ROOT_DIR unknown: set qt.dir in android/local.properties before running Gradle"

step "4/4 android/local.properties"
props="$OVT_ROOT/android/local.properties"
{
  echo "# Written by tools/bootstrap_android.sh; not committed."
  echo "sdk.dir=$sdk"
  echo "ndk.dir=$sdk/ndk/$ndk"
  [ -z "$qt_root" ] || echo "qt.dir=$qt_root"
  [ "$fdroid" = 0 ] || echo "ovt.fdroid=true"
} >"$props"
say "wrote $props"
say "export ANDROID_NDK_ROOT=$sdk/ndk/$ndk"
say "cmake --preset android-arm64-debug     # or the APKs: (cd android && ./gradlew :app:assembleFossDebug)"
