#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Android emulator smoke test (chapter 13 §13.7.1 row android-emulator-smoke, WI-0.035). Runs against a booted
# emulator or device that `adb` can see; the CI job starts the emulator and calls this script.
#
#   tools/android/smoke.sh --apk <foss-x86_64.apk> --out <dir> [--cli <ovt>] [--sample <mini.mp4>] [--libcxx <libc++_shared.so>]
#
# Checks, in order (any failure exits 1 after printing the logcat excerpt that explains it):
#   1. adb install of the APK and `am start` of MainActivity;
#   2. uiautomator dump contains a node with text "OVT Studio" (WI-0.050 acceptance test 2) and the process is alive;
#   3. the CLI is pushed to /data/local/tmp and `ovt --version` exits 0; `ovt info <sample>` exits 0 when this build
#      of the CLI has the `info` subcommand (see SPEC-ASSUMPTION below);
#   4. logcat has no `OVT-FATAL` line, no `FATAL EXCEPTION` and no native `Fatal signal` of the app.
# The output directory receives logcat.txt, window.xml and screenshot.png (also on failure). No secrets, no network.
#
# SPEC-ASSUMPTION(#486): chapter 13 §13.7.1 asks for `ovt info mini.mp4` to exit 0, but the CLI of this tree has no
# `info` subcommand yet (chapter 12 WI-1.5xx). Until it has one the script runs `ovt --version` and prints a notice;
# set OVT_SMOKE_REQUIRE_INFO=1 (done in ci.yml by the work item that adds `ovt info`) to make `info` mandatory.

set -euo pipefail

PKG="org.openvideotools.studio"
ACTIVITY="$PKG/.MainActivity"
REMOTE_DIR="/data/local/tmp/ovt-smoke"
APP_TEXT="OVT Studio"

apk="" out="" cli="" sample="" libcxx=""
while [ $# -gt 0 ]; do
  case "$1" in
    --apk) apk="$2"; shift ;;
    --out) out="$2"; shift ;;
    --cli) cli="$2"; shift ;;
    --sample) sample="$2"; shift ;;
    --libcxx) libcxx="$2"; shift ;;
    -h|--help) sed -n '6,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -f "$apk" ] || { echo "smoke: --apk <file> is required" >&2; exit 2; }
[ -n "$out" ] || { echo "smoke: --out <dir> is required" >&2; exit 2; }
mkdir -p "$out"

fail() {
  echo "::error::smoke: $*"
  excerpt
  exit 1
}

# The part of logcat that explains a failure: the app's own lines and every crash banner.
excerpt() {
  adb logcat -d > "$out/logcat.txt" 2>&1 || true
  adb exec-out screencap -p > "$out/screenshot.png" 2>/dev/null || true
  echo "---- logcat excerpt (OVT, AndroidRuntime, DEBUG, libc, Qt) ----"
  grep -E 'OVT|AndroidRuntime|FATAL|Fatal signal|DEBUG|libc |Qt|org.openvideotools' "$out/logcat.txt" | tail -n 80 || true
  echo "---- end of excerpt (full log: $out/logcat.txt) ----"
}

echo "== 1. install and launch"
adb wait-for-device
# shellcheck disable=SC2016  # the loop runs on the device
adb shell 'while [ "$(getprop sys.boot_completed)" != 1 ]; do sleep 2; done'
adb shell input keyevent 82 || true # dismiss the keyguard
adb install -r -t "$apk" || fail "adb install failed"
adb logcat -c
adb shell am start -W -n "$ACTIVITY" || fail "am start failed"

echo "== 2. uiautomator assertion"
found=0
for _ in $(seq 1 30); do
  if ! adb shell pidof "$PKG" > /dev/null; then fail "the app process is gone (crash on start?)"; fi
  adb shell uiautomator dump /sdcard/window.xml > /dev/null 2>&1 || true
  adb pull /sdcard/window.xml "$out/window.xml" > /dev/null 2>&1 || true
  if grep -Fq "text=\"$APP_TEXT\"" "$out/window.xml" 2> /dev/null ||
     grep -Fq "content-desc=\"$APP_TEXT\"" "$out/window.xml" 2> /dev/null; then
    found=1
    break
  fi
  sleep 2
done
[ "$found" = 1 ] || fail "uiautomator found no node with text \"$APP_TEXT\" within 60 s"
echo "found \"$APP_TEXT\""

echo "== 3. CLI"
if [ -n "$cli" ]; then
  adb shell "mkdir -p $REMOTE_DIR"
  adb push "$cli" "$REMOTE_DIR/ovt" > /dev/null
  adb shell "chmod 755 $REMOTE_DIR/ovt"
  if [ -n "$libcxx" ]; then adb push "$libcxx" "$REMOTE_DIR/libc++_shared.so" > /dev/null; fi
  run_ovt() { adb shell "cd $REMOTE_DIR && LD_LIBRARY_PATH=$REMOTE_DIR ./ovt $*; echo EXIT=\$?" | tr -d '\r'; }
  ver="$(run_ovt --version)"
  echo "$ver"
  printf '%s\n' "$ver" | grep -q '^EXIT=0$' || fail "ovt --version did not exit 0"
  help="$(run_ovt --help)"
  if printf '%s\n' "$help" | grep -Eq '^[[:space:]]+info([[:space:]]|$)'; then
    if [ ! -f "$sample" ]; then fail "ovt has an info subcommand but --sample <mini.mp4> is missing"; fi
    adb push "$sample" "$REMOTE_DIR/mini.mp4" > /dev/null
    info="$(run_ovt info mini.mp4)"
    echo "$info"
    printf '%s\n' "$info" | grep -q '^EXIT=0$' || fail "ovt info mini.mp4 did not exit 0"
  elif [ "${OVT_SMOKE_REQUIRE_INFO:-0}" = 1 ]; then
    fail "OVT_SMOKE_REQUIRE_INFO=1 but this ovt has no info subcommand"
  else
    echo "::notice::smoke: this ovt has no info subcommand yet (SPEC-ASSUMPTION #486); ran --version only"
  fi
else
  echo "no --cli given: skipped"
fi

echo "== 4. logcat check"
excerpt > "$out/excerpt.txt"
if grep -q 'OVT-FATAL' "$out/logcat.txt"; then fail "logcat contains OVT-FATAL"; fi
if grep -q 'FATAL EXCEPTION' "$out/logcat.txt"; then fail "logcat contains a FATAL EXCEPTION"; fi
if grep -Eq "Fatal signal .*(${PKG}|ovt|Qt)" "$out/logcat.txt"; then fail "logcat contains a native crash of the app"; fi
echo "smoke: OK (screenshot: $out/screenshot.png)"
