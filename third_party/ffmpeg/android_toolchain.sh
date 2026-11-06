#!/bin/sh
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Android toolchain wrapper for FFmpeg's configure (chapter 13 §13.3.3, WI-0.012). third_party/ffmpeg.cmake runs
#
#   android_toolchain.sh --ndk <ndk-root> --abi arm64-v8a|x86_64 --api <level> -- <ffmpeg>/configure <args>...
#
# and this script runs configure with the NDK's clang and LLVM binutils in front of <args>:
#
#   --enable-cross-compile --target-os=android --arch=<arch> --sysroot=<ndk>/toolchains/llvm/prebuilt/<host>/sysroot
#   --cc=<bin>/<triple><api>-clang --cxx=<bin>/<triple><api>-clang++ --nm=<bin>/llvm-nm --ar=<bin>/llvm-ar
#   --ranlib=<bin>/llvm-ranlib --strip=<bin>/llvm-strip
#
# FFmpeg accepts --enable-jni and --enable-mediacodec only with --target-os=android, so those two stay in the
# flavour's flag list (third_party/ffmpeg/configure_flags.cmake) and come after these.
#
# --print writes the toolchain arguments one per line instead of running anything (tests, debugging).
# An NDK whose prebuilt host directory or clang is missing is an error naming the path.

set -eu

usage() {
  echo "usage: $0 --ndk <ndk-root> --abi arm64-v8a|x86_64 --api <level> [--print] [-- <configure> [args...]]" >&2
  exit 2
}

ndk=""
abi=""
api=""
print=0
while [ $# -gt 0 ]; do
  case "$1" in
    --ndk) [ $# -ge 2 ] || usage; ndk=$2; shift 2 ;;
    --abi) [ $# -ge 2 ] || usage; abi=$2; shift 2 ;;
    --api) [ $# -ge 2 ] || usage; api=$2; shift 2 ;;
    --print) print=1; shift ;;
    --) shift; break ;;
    *) echo "android_toolchain.sh: unknown argument '$1'" >&2; usage ;;
  esac
done
if [ -z "$ndk" ] || [ -z "$abi" ] || [ -z "$api" ]; then
  usage
fi
case "$api" in
  *[!0-9]*|'') echo "android_toolchain.sh: --api must be a number (got '$api')" >&2; exit 2 ;;
esac
if [ "$print" -eq 0 ] && [ $# -eq 0 ]; then
  echo "android_toolchain.sh: no configure command after --" >&2
  usage
fi

case "$abi" in
  arm64-v8a) arch=aarch64; triple=aarch64-linux-android ;;
  x86_64) arch=x86_64; triple=x86_64-linux-android ;;
  *) echo "android_toolchain.sh: unsupported ABI '$abi' (arm64-v8a or x86_64, chapter 13 §13.4)" >&2; exit 2 ;;
esac

# NDK r27 ships one prebuilt directory per host; on macOS it is darwin-x86_64 for both Intel and Apple silicon.
case "$(uname -s)" in
  Linux) host_tag=linux-x86_64 ;;
  Darwin) host_tag=darwin-x86_64 ;;
  *) echo "android_toolchain.sh: unsupported build host $(uname -s)" >&2; exit 2 ;;
esac

prebuilt="$ndk/toolchains/llvm/prebuilt/$host_tag"
bin="$prebuilt/bin"
cc="$bin/$triple$api-clang"
if [ ! -d "$prebuilt" ]; then
  echo "android_toolchain.sh: $prebuilt does not exist (is --ndk the NDK root, r27c?)" >&2
  exit 1
fi
if [ ! -e "$cc" ]; then
  echo "android_toolchain.sh: $cc does not exist (API level $api not in this NDK?)" >&2
  exit 1
fi

toolchain() {
  printf '%s\n' \
    --enable-cross-compile \
    --target-os=android \
    "--arch=$arch" \
    "--sysroot=$prebuilt/sysroot" \
    "--cc=$cc" \
    "--cxx=$bin/$triple$api-clang++" \
    "--nm=$bin/llvm-nm" \
    "--ar=$bin/llvm-ar" \
    "--ranlib=$bin/llvm-ranlib" \
    "--strip=$bin/llvm-strip"
}

if [ "$print" -eq 1 ]; then
  toolchain
  exit 0
fi

configure=$1
shift
exec "$configure" \
  --enable-cross-compile \
  --target-os=android \
  "--arch=$arch" \
  "--sysroot=$prebuilt/sysroot" \
  "--cc=$cc" \
  "--cxx=$bin/$triple$api-clang++" \
  "--nm=$bin/llvm-nm" \
  "--ar=$bin/llvm-ar" \
  "--ranlib=$bin/llvm-ranlib" \
  "--strip=$bin/llvm-strip" \
  "$@"
