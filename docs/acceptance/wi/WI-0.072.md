# WI-0.072 acceptance record

Spec version: 1.0-rc17. Written 2026-11-09 in a Linux cloud container without `rpmbuild`, `lintian`, mock or
network access to distro repositories. **Neither acceptance test has been run.** They are owed once the install rules
listed in `docs/dev/packaging.md` ("Status") exist.

## Test 1: Ubuntu 22.04 container, `dpkg-buildpackage -us -uc`, the listed `.deb`s, `lintian` without errors

Pending. Command to run, and paste the output here:

```sh
docker run --rm -v "$PWD:/src" -w /src ubuntu:22.04 bash -c \
  'apt-get update && apt-get install -y build-essential devscripts equivs lintian &&
   cp -a packaging/debian debian && mk-build-deps -ir -t "apt-get -y" debian/control &&
   dpkg-buildpackage -us -uc -b && lintian --fail-on error ../*.changes'
```

Checked here: `dpkg-parsechangelog` reads `packaging/debian/changelog`, `debian/control` lists the nine binary
packages of chapter 13 §13.6.3, and `make -n -f debian/rules override_dh_auto_configure` expands to the
`linux-x64-distro` configure. Nothing was built. The `nightly.yml` job `package-deb` runs this when the repository
variable `OVT_PACKAGING_NIGHTLY` is `on`.

## Test 2: Fedora container, `rpmbuild -ba`, `ovt` without AVC/HEVC; freeworld spec builds in mock

Pending. `ovt.spec` `%check` fails the build when `libovt_plugin_ffmpeg.so` or `libovt_plugin_de265.so` is installed,
which is the automated form of "`ovt plugins` lacks `ffmpeg`". The freeworld spec is a manual mock build
(documented in `docs/dev/packaging.md`), and its result is recorded here when the owner has cleared the patent check.
