# third_party/vcpkg-overlays/

Overlay ports for vcpkg (chapter 13 §13.3.2). A directory `<port>/` here replaces the port of the same
name from the pinned vcpkg baseline. `vcpkg-configuration.json` and the `VCPKG_OVERLAY_PORTS` variable of
the presets both point here.

Add an overlay only when the upstream port cannot be used as is (a missing feature, a broken build on one
of our triplets). Each overlay port carries a `README.md` saying why it exists and which upstream vcpkg
issue or pull request would let us delete it.

| Port | Why |
|---|---|
| `cli11` | header-only instead of the precompiled, uninstrumented `libCLI11.a` (ASan container-overflow; `cli11/README.md`) |
| `spdlog` | the upstream port's two backport patches are vendored instead of downloaded from github.com (HTTP 503 on Android source rebuilds; `spdlog/README.md`) |
| `fmt` | the upstream port's backport patch is vendored instead of downloaded from github.com (HTTP 503; `fmt/README.md`) |
