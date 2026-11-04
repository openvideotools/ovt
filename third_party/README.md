# third_party/

ExternalProject scripts (`<dep>.cmake`), patches (`patches/`), vcpkg triplets and overlays, and the version pins in `VERSIONS.md` (chapter 13 §13.3). Every third-party version and source hash is pinned only in `VERSIONS.md`, never in a `CMakeLists.txt` (chapter 13 §13.2.0).

- `VERSIONS.md`: the pin table; `tools/versions_to_cmake.py` turns it into `build/<preset>/third_party/versions.cmake`.
- `_template.cmake`: the starting point for a new `<dep>.cmake` script.
- `vcpkg/`, `vcpkg-triplets/`, `vcpkg-overlays/`: the vcpkg submodule, the `*-ovt` triplets and overlay ports.

See `docs/dev/dependencies.md`.
