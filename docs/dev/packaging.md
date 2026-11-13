# Distro packaging

Skeletons for packagers live in `packaging/` (chapter 13 §13.6.3, WI-0.072). They are starting points for a
maintainer, not published packages. Nothing here has been submitted to Debian, Ubuntu, Fedora, RPM Fusion or the
AUR, and submitting needs the owner's legal check (see "Patent-encumbered codecs").

| Distro | Files | Package split |
|---|---|---|
| Debian / Ubuntu | `packaging/debian/` | source `ovt`; binaries `libovt-core1`, `libovt-render1`, `libovt-dev`, `ovt-cli`, `ovt-studio`, `ovt-plugin-aom`, `ovt-plugin-ffmpeg`, `ovt-plugin-de265`, `python3-ovt` |
| Fedora | `packaging/rpm/ovt.spec` | `ovt`, `ovt-libs`, `ovt-devel`, `python3-ovt`; built against `ffmpeg-free`, so no AVC/HEVC |
| RPM Fusion | `packaging/rpm/ovt-codecs-freeworld.spec` | `ovt-codecs-freeworld` (also `Provides: ovt-plugins-freeworld`): the FFmpeg and libde265 plugins |
| Arch / AUR | `packaging/arch/PKGBUILD` | single package `ovt` (an `ovt-git` variant changes only the source) |

All of them configure with the `linux-x64-distro` preset (system libraries, no vcpkg) and install with
`cmake --install`. The preset turns `OVT_PATENT_CODECS_IN_CORE` ON, which suits Debian and Arch, whose FFmpeg
already decodes AVC and HEVC. The Fedora spec turns it OFF again.

## Patent-encumbered codecs (for the owner)

H.264/AVC and H.265/HEVC are covered by patent pools, and RPM Fusion's full FFmpeg enables more (AAC, MPEG-4 Part 2,
VC-1). This is why Fedora gets a split (ADR-015):

* `ovt.spec` builds without `ovt_plugin_ffmpeg` and `ovt_plugin_de265`; its `%check` fails if either is installed.
* `ovt-codecs-freeworld.spec` builds exactly those two plugins. It carries a banner telling the reader not to submit
  it. **Do not submit it to RPM Fusion, and do not publish the Debian or Arch packages, before the owner has made
  the legal check.** Writing and reviewing the specs is fine.
* The Debian `ovt-plugin-ffmpeg` and `ovt-plugin-de265` packages and the Arch `ovt` package pull in the same
  codecs through the distro FFmpeg and libde265. A Debian maintainer decides whether they belong in `main`.
* `ovt-plugin-aom` ships the AOMedia `PATENTS` file with the plugin (ADR-015: AV1 is in every build).

## Debian

dpkg expects `debian/` at the source root, so copy the directory first:

```sh
cp -a packaging/debian debian
dpkg-buildpackage -us -uc -b
lintian ../ovt_*.changes
```

`debian/README.source` declares the static libaom convenience copy: Debian's libaom lacks `CONFIG_INSPECTION`, so the
preset sets `OVT_SYSTEM_LIBAOM=OFF` and the plugin carries its own static build.

## Fedora and RPM Fusion

```sh
git archive --prefix=ovt-0.1.0/ -o ~/rpmbuild/SOURCES/ovt-0.1.0.tar.gz HEAD
rpmbuild -ba packaging/rpm/ovt.spec
```

The freeworld spec is built by hand in mock with the RPM Fusion Free repositories enabled, never in CI:

```sh
mock -r fedora-41-x86_64-rpmfusion_free --buildsrpm --spec packaging/rpm/ovt-codecs-freeworld.spec --sources ~/rpmbuild/SOURCES
```

## Arch

```sh
cd packaging/arch && makepkg -s
```

`sha256sums` is `SKIP` until a release is tagged.

## CI

`nightly.yml` has two jobs, `package-deb` (Ubuntu 22.04 container, `dpkg-buildpackage`, `lintian`) and `package-rpm`
(Fedora container, `rpmbuild -ba packaging/rpm/ovt.spec`). Both are off until the repository variable
`OVT_PACKAGING_NIGHTLY` is `on`. The container images are pinned by digest.

## Status

These are skeletons, and they have not been built in a container. What is known to be missing:

* `ovt_studio` and the Python module have no `install()` rules yet, and there is no `.desktop`, AppStream or icon
  file (FR-LNX-005, WI-0.070). The `ovt-studio` and `python3-ovt` file lists are written for the install tree
  chapter 13 §13.6.3 describes; `dh_install` fails on them until those rules land.
* Ubuntu 22.04's libplacebo and libvmaf are older than the pins, so the Debian job cannot pass there as is.
* The second Debian orig tarball for libaom (see `README.source`) and `debian/watch` are not wired.
* The Debian `copyright` file summarises `tools/license_policy.toml`; regenerate it from `reuse spdx` before upload.
* Acceptance tests 1 and 2 of WI-0.072 (a real container build and `lintian`) are owed: record them in
  `docs/acceptance/wi/WI-0.072.md` when the missing install rules exist.
