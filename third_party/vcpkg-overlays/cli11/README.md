# cli11 overlay: header-only CLI11

The upstream `cli11` port of the pinned vcpkg baseline builds CLI11 2.7.2 as a precompiled static library
(`-DCLI11_PRECOMPILED=ON`) and patches `#define CLI11_COMPILE` into `CLI/CLI.hpp`, so every consumer links
`libCLI11.a`. Our vcpkg triplets build that library without sanitizers, while `ovt_cli_lib` and
`ovt_gen_completions` are built with `-fsanitize=address` in the ASan presets. Both halves instantiate the same
inline `std::vector<std::unique_ptr<CLI::Option>>` members, and the linker keeps one copy of each, so libc++'s
container annotations (on by default on macOS) see a vector grown by uninstrumented code and report a
`container-overflow` in `CLI::App::add_option` when `ovt_gen_completions` runs during the `macos-arm64-asan` build
(nightly `macos-asan`, red since 2026-11-09). AddressSanitizer documents this as the mixed-instrumentation false
positive; the fix is to build all of the code with the sanitizer.

This overlay installs CLI11 header-only (`-DCLI11_PRECOMPILED=OFF`, no `CLI11_COMPILE`), so all CLI11 code is
compiled in OVT's own translation units with OVT's flags, sanitizers included. Same version, same source archive
and hash, same licence (BSD-3-Clause). The pkg-config file goes to `share/pkgconfig`, CLI11's own location for a
header-only build, so the upstream port's `revert-1012-pkgconfig.diff` (which moves it to `lib/pkgconfig` for the
library) is not applied.

Delete this overlay when the upstream port installs a header-only variant (for example a `header-only` feature or a
`precompiled` default feature that can be turned off) or when the vcpkg dependencies are built with the same
sanitizer as OVT.
