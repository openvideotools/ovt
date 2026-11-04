# Dependencies

OVT gets its libraries from three places (chapter 13 §13.3.1):

| Source | What | Pinned by |
|---|---|---|
| vcpkg manifest (`vcpkg.json`) | fmt, spdlog, flatbuffers (library and the `flatc` host tool), zstd, nlohmann-json, highway, cli11, xxhash, catch2; later zimg, Vulkan, glslang, lcms, libpng, pybind11, benchmark, onnxruntime | `builtin-baseline` in `vcpkg.json` and the `third_party/vcpkg` submodule commit |
| `third_party/*.cmake` ExternalProject scripts | FFmpeg, libaom, dav1d, libde265, HM, libvmaf, libplacebo, MoltenVK | `third_party/VERSIONS.md` |
| Prebuilt | Qt 6.8.3 (aqtinstall), Android SDK/NDK | `tools/install_qt.py`, `tools/bootstrap_android.sh` |

This page covers the vcpkg part first, then the `third_party/` script machinery (see "Own builds" below).

`vcpkg.json` lists only the ports that some target links today. §13.3.2 of the specification shows the
manifest's end state; each later work item adds its ports in the PR that first uses them
(SPEC-ASSUMPTION(#8)).

## How vcpkg is wired in

- `third_party/vcpkg` is a git submodule of <https://github.com/microsoft/vcpkg>, checked out at the commit
  that `vcpkg.json`'s `builtin-baseline` names (tag `2026.10.16`, commit
  `9eec002df1cd109f8f104cd966a763daf9751489`). The two must always be equal.
- The presets pass `third_party/vcpkg/scripts/buildsystems/vcpkg.cmake` as the toolchain file and turn on
  manifest mode, so `cmake --preset <name>` runs `vcpkg install` for the preset's triplet into
  `build/vcpkg_installed/<triplet>/` before the project configures.
- `vcpkg-configuration.json` adds the overlay directories: `third_party/vcpkg-triplets/` (the `*-ovt`
  triplets, see `docs/dev/presets.md`) and `third_party/vcpkg-overlays/` (overlay ports; none yet).
- CMake finds each package with `ovt_find_package()` (`cmake/OvtThirdParty.cmake`), which also records the
  package's SPDX id and version for the licence manifest (`ovt_license_manifest` target).

First time on a machine:

```sh
git submodule update --init third_party/vcpkg
third_party/vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake --preset linux-x64-debug          # or macos-arm64-debug
```

`tools/bootstrap.sh` (WI-0.006) does the first two steps for you. If the submodule is missing, configure
stops with a message that says so.

## Imported targets

| Port | `find_package` | Target OVT links | Linked by |
|---|---|---|---|
| fmt | `fmt` | `fmt::fmt` | `ovt_core` |
| spdlog | `spdlog` | `spdlog::spdlog` | `ovt_core` |
| flatbuffers | `flatbuffers` (config file `FlatBuffersConfig.cmake`) | `flatbuffers::flatbuffers` | `ovt_core` |
| zstd | `zstd` | `zstd::libzstd_static` (falls back to `zstd::libzstd`) | `ovt_core` |
| nlohmann-json | `nlohmann_json` | `nlohmann_json::nlohmann_json` | `ovt_core` |
| highway | `hwy` | `hwy::hwy` | `ovt_core` |
| xxhash | `xxHash` (pkg-config `libxxhash` without vcpkg) | `xxHash::xxhash` | `ovt_core` |
| cli11 | `CLI11` | `CLI11::CLI11` | `ovt_cli` |
| catch2 | `Catch2` | `Catch2::Catch2WithMain` | test executables |

All of them are linked `PRIVATE`. The `*-ovt` triplets build static libraries, so they are absorbed into
`libovt_core` and a downstream `find_package(ovt)` never needs them (chapter 13 §13.2.6).

`flatbuffers` is listed twice in `vcpkg.json`: once with `"host": true` for the `flatc` compiler, which must
run on the build machine, and once for the target library. Android cross builds need both.

## FlatBuffers code generation

`cmake/OvtFlatBuffers.cmake` provides `ovt_add_flatbuffers_schema(<target> <fbs>...)`. `core/schema/CMakeLists.txt`
calls it for every `core/schema/*.fbs`, which creates the target `ovt_schemas`. Building it runs the host `flatc`
and writes, below `build/<preset>/generated/` (never into the source tree):

| Output | Path |
|---|---|
| C++ | `<schema>_generated.h` (`--gen-object-api --gen-compare --reflect-names --scoped-enums`) |
| Python | `python/ovt/fb/<Type>.py` and `__init__.py` (`PYTHONPATH=build/<preset>/generated/python`) |
| Kotlin | `kotlin/` |

Link `ovt_schemas` to use the header: `#include "ovt_model_generated.h"`. The generated directory is a SYSTEM
include, so flatc's code does not trip `-Werror`. `flatc` comes from the vcpkg host port; `-DOVT_FLATC=<path>`
overrides it (for example with `flatbuffers-compiler` from apt when building with `-DOVT_USE_VCPKG=OFF`).

`tools/check_schema_compat.py` compares the schemas with the last release tag and exits 1, naming the field,
when a field was removed or retyped (chapter 6 principle P7). It reports nothing to compare until the first tag.

## Building without vcpkg

`-DOVT_USE_VCPKG=OFF` drops the vcpkg toolchain and resolves the same packages with plain
`find_package`/`pkg-config` from the system. The Flatpak and distro packages use this. On Ubuntu 24.04:

```sh
sudo apt install libfmt-dev libspdlog-dev libflatbuffers-dev flatbuffers-compiler libzstd-dev \
  nlohmann-json3-dev libhwy-dev libcli11-dev libxxhash-dev catch2
cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF
```

System versions are older than the baseline's, and only the vcpkg configuration is what CI tests.

## Binary caching

vcpkg rebuilds a port only when its ABI hash changes. Built packages are cached as zip archives through the
`files` provider (vcpkg's former `x-gha` provider was removed in 2025):

```sh
export VCPKG_BINARY_SOURCES="clear;files,$HOME/.cache/ovt-vcpkg-archives,readwrite"
```

Locally, put this in your shell profile; the default (`~/.cache/vcpkg/archives`) also works. In CI, the
directory is `build/vcpkg-archives`, restored and saved by `actions/cache` with the key
`vcpkg-<os>-<triplet>-<hash of vcpkg.json, triplets and overlays>` (chapter 13 §13.7.2):

```sh
VCPKG_BINARY_SOURCES="clear;files,${GITHUB_WORKSPACE}/build/vcpkg-archives,readwrite"
```

With a warm cache, the vcpkg install step of a configure restores archives instead of building and takes
well under a minute.

## Changing dependencies

- **Add a port:** add it to `vcpkg.json` in the PR of the work item that first uses it, find it with
  `ovt_find_package()` next to the target that links it, and give its SPDX id from the port's `license`
  field (`third_party/vcpkg/ports/<port>/vcpkg.json`).
- **Move the baseline:** `third_party/vcpkg/vcpkg x-update-baseline`, then move the submodule to the same
  commit. This is its own PR (chapter 13 §13.3.7), weekly via Renovate (§13.3.8).
- **Pin one port to another version:** an entry in `"overrides"`; say why in the PR.

## Own builds: `third_party/*.cmake` and `VERSIONS.md`

Libraries that vcpkg cannot build the way OVT needs (FFmpeg flavours, libaom with inspection, patched
libde265, the Meson projects) have their own ExternalProject script in `third_party/<dep>.cmake`
(chapter 13 §13.3.4). Copy `third_party/_template.cmake` to start one.

**`third_party/VERSIONS.md` is the only place a version, URL or hash is written.** At configure time
`tools/versions_to_cmake.py` turns its table into `build/<preset>/third_party/versions.cmake`:

```cmake
set(OVT_TP_libaom_VERSION "3.12.1")
set(OVT_TP_libaom_URL "https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz")
set(OVT_TP_libaom_SHA256 "9e97…")
set(OVT_TP_libaom_LICENSE "BSD-2-Clause AND LicenseRef-AOM-Patent-License-1.0")
```

The `<dep>` part is the row's Name in lower case, with every run of other characters replaced by `_`
(`FFmpeg` → `ffmpeg`, `vcpkg baseline` → `vcpkg_baseline`). There is no other spelling; a script never
writes `3.12.1` itself, and `tools/versions_to_cmake.py --check` fails when one does.

The parser rejects a row whose URL contains a version different from its Version column, a duplicate id, or
a malformed hash.

| Helper (`cmake/OvtThirdPartyCommon.cmake`) | What it does |
|---|---|
| `ovt_tp_declare(<dep> [LICENSE_FILES …])` | Checks the row exists and has a hash, downloads the archive into `OVT_TP_DOWNLOADS` (`build/third_party/downloads/`) and verifies its SHA-256. A mismatch stops configure and names the row. |
| `${OVT_TP_PATCH} <dir>` | `PATCH_COMMAND` prefix: applies `<dir>/*.patch` in lexical order with `tools/apply_patches.py --check`; no-op for an empty or missing directory. |
| `ovt_tp_apply_patches(<var> SERIES <file> INSTALL_DIR <INSTALL_DIR>)` | Same, from a quilt `series` file, and writes `<INSTALL_DIR>/share/ovt/patches.json`. |
| `${OVT_TP_CMAKE_TOOLCHAIN_ARGS}` | Forwards compilers, launcher, the chain-loaded toolchain (Android NDK), macOS architecture and deployment target, and hidden visibility to a CMake-based ExternalProject. Debug presets build third-party code as `RelWithDebInfo`. |
| `ovt_tp_import_static(tp::<x> tp_<dep> lib/<lib>.a include)` | Imported static target with the row's licence and version in the licence manifest (marked modified when the row lists patches). |

`tools/apply_patches.py` checks every patch (`git apply --check`) before applying any, requires an
`Upstream-Status:` header, and writes a `.ovt-patched` marker holding the series hash, so a rebuild does not
apply the patches a second time.

### Filling hashes and building offline

```sh
python3 -I tools/versions_to_cmake.py --update-hashes      # downloads rows still marked `<sha>`, writes the hashes
python3 -I tools/third_party_offline.py --out dl/          # fetches and verifies every archive
cmake --preset linux-x64-release -DOVT_TP_DOWNLOADS=$PWD/dl # configure then uses dl/ without network
```

Never type a hash by hand: `--update-hashes` computes it from the downloaded archive (and, for libaom, the
`PATENTS sha256=` note from the archive's `PATENTS` file). Rows still holding `<sha>` are allowed in the file,
but `ovt_tp_declare()` refuses to build them.
