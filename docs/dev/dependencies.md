# Dependencies

OVT gets its libraries from three places (chapter 13 §13.3.1):

| Source | What | Pinned by |
|---|---|---|
| vcpkg manifest (`vcpkg.json`) | fmt, spdlog, flatbuffers (library and the `flatc` host tool), zstd, nlohmann-json, highway, cli11, xxhash, catch2; later zimg, Vulkan, glslang, lcms, libpng, pybind11, benchmark, onnxruntime | `builtin-baseline` in `vcpkg.json` and the `third_party/vcpkg` submodule commit |
| `third_party/*.cmake` ExternalProject scripts | FFmpeg, libaom, dav1d, libde265, HM, libvmaf, libplacebo, MoltenVK | `third_party/VERSIONS.md` (WI-0.010 and later) |
| Prebuilt | Qt 6.8.3 (aqtinstall), Android SDK/NDK | `tools/install_qt.py`, `tools/bootstrap_android.sh` |

This page covers the vcpkg part.

`vcpkg.json` lists only the ports that some target links today. §13.3.2 of the specification shows the
manifest's end state; each later work item adds its ports in the PR that first uses them
(SPEC-ASSUMPTION(#8)).

## How vcpkg is wired in

- `third_party/vcpkg` is a git submodule of https://github.com/microsoft/vcpkg, checked out at the commit
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
