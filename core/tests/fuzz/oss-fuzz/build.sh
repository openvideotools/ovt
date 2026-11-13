#!/bin/bash -eu
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# OSS-Fuzz build script (chapter 14 §14.7.4, WI-0.569). Contract: $SRC, $OUT, $WORK, $CC, $CXX, $CFLAGS,
# $CXXFLAGS, $LIB_FUZZING_ENGINE. There is no oss-fuzz CMake preset (D23); this script configures directly.
#
# Local test, from a checkout of google/oss-fuzz with this directory copied to projects/ovt/:
#   python infra/helper.py build_image ovt
#   python infra/helper.py build_fuzzers --sanitizer address ovt <path to an OVT checkout>
#   python infra/helper.py check_build --sanitizer address ovt

cd "$SRC/ovt"

build_dir="$WORK/build-oss-fuzz"
triplet_dir="$WORK/vcpkg-triplets"
mkdir -p "$triplet_dir"

# Third-party libraries are built from source under the same flags as OVT (no prebuilt binaries). vcpkg ignores
# CFLAGS/CXXFLAGS, so a triplet carries them. The flags are written into the file here, once, from the environment.
cat > "$triplet_dir/x64-linux-ossfuzz.cmake" <<TRIPLET
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS "${CFLAGS}")
set(VCPKG_CXX_FLAGS "${CXXFLAGS}")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
TRIPLET

third_party/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_OVERLAY_PORTS="$PWD/third_party/vcpkg-overlays"
export VCPKG_FEATURE_FLAGS="manifests,versions"

# The sanitizer, coverage and fuzzer-no-link flags arrive in CFLAGS/CXXFLAGS, so OVT_ENABLE_SANITIZERS stays
# OFF. OVT_FUZZ_ENGINE is $LIB_FUZZING_ENGINE: AFL++ and honggfuzz link their own driver. The tests stay ON only
# because the fuzz targets live in core/tests/fuzz; the analysis plugins and everything with a GUI stay off.
cmake -S . -B "$build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_CXX_FLAGS="$CXXFLAGS" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/third_party/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_MANIFEST_MODE=ON -DVCPKG_INSTALLED_DIR="$WORK/vcpkg_installed" \
  -DVCPKG_OVERLAY_TRIPLETS="$triplet_dir" -DVCPKG_TARGET_TRIPLET=x64-linux-ossfuzz \
  -DOVT_ENABLE_FUZZ=ON -DOVT_FUZZ_ENGINE="$LIB_FUZZING_ENGINE" \
  -DOVT_ENABLE_SANITIZERS=OFF -DOVT_WERROR=OFF -DOVT_ENABLE_LTO=OFF \
  -DOVT_BUILD_TESTS=ON \
  -DOVT_BUILD_GUI=OFF -DOVT_BUILD_CLI=OFF -DOVT_BUILD_RENDER=OFF -DOVT_BUILD_PYTHON=OFF \
  -DOVT_BUILD_PLUGIN_AOM=OFF -DOVT_BUILD_PLUGIN_DE265=OFF -DOVT_BUILD_PLUGIN_FFMPEG=OFF \
  -DOVT_BUILD_PLUGIN_HM=OFF -DOVT_BUILD_METRIC_XPSNR=OFF

# Build the libFuzzer executables only: ovt_fuzz_<name>, not the *_regress replay binaries or the unit tests.
targets=$(cmake --build "$build_dir" --target help | sed -n 's/^\(ovt_fuzz_[A-Za-z0-9_]*\): phony.*/\1/p' \
          | grep -v '_regress$' || true)
if [ -z "$targets" ]; then
  echo "build.sh: no ovt_fuzz_* target found; is OVT_ENABLE_FUZZ=ON and the compiler clang?" >&2
  exit 1
fi
# shellcheck disable=SC2086  # word splitting of the target list is intended
cmake --build "$build_dir" --target $targets -j"$(nproc)"

cp "$build_dir"/fuzz/ovt_fuzz_* "$OUT/"

# Dictionaries: <target>.dict next to the binary named ovt_fuzz_<target>. Only some targets have one; the
# directory does not exist until the first parser WI adds a dictionary.
shopt -s nullglob
for dict in core/tests/fuzz/dictionaries/*.dict; do
  cp "$dict" "$OUT/"
done

# Seed corpora: core/tests/fuzz/corpus/<target>/ becomes ovt_fuzz_<target>_seed_corpus.zip.
for dir in core/tests/fuzz/corpus/*/; do
  name=$(basename "$dir")
  files=("$dir"*)
  if [ "${#files[@]}" -gt 0 ]; then
    zip -q -j "$OUT/ovt_fuzz_${name}_seed_corpus.zip" "${files[@]}"
  fi
done
