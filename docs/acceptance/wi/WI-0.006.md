# WI-0.006 acceptance record

Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.006 pull request.

Both acceptance tests are runs of `.github/workflows/bootstrap-check.yml`: monthly, by hand, and on pull
requests that change the bootstrap files. This PR changes them, so its own checks run both jobs. The results
are linked from the PR. The local runs below were made in the cloud container used for development.

## Test 1: fresh Ubuntu 22.04 container with only git and sudo

Job `bootstrap-check (ubuntu-22.04)`. It uses the `ubuntu:22.04` image with only `git sudo ca-certificates`
installed, then runs:

```sh
tools/bootstrap.sh --print-install | sudo bash
tools/bootstrap.sh --no-qt --no-corpora
CC=gcc-13 CXX=g++-13 cmake --preset linux-x64-debug
cmake --build --preset linux-x64-debug
```

Result: pending on the job's first run (see the PR checks).

Local run on Ubuntu 24.04 (cloud container, root, GCC 13.3, CMake 3.28, Python 3.13). The first run printed
the missing packages and stopped:

```console
$ tools/bootstrap.sh --no-qt; echo $?
==> 1/7 host tools
Missing host tools. Run these commands, then run tools/bootstrap.sh again:

  sudo apt-get update
  sudo apt-get install -y --no-install-recommends autoconf-archive python3-venv libvulkan-dev mesa-vulkan-drivers libwayland-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb-xkb-dev libx11-xcb-dev libgl1-mesa-dev libegl1-mesa-dev libva-dev
1
$ tools/bootstrap.sh --print-install | bash          # as root
$ rm -rf .venv && tools/bootstrap.sh --no-qt; echo $?
==> 1/7 host tools
cmake 3.28.3, ninja 1.11.1, python3 3.13.18, gcc 13.3.0

==> 2/7 vcpkg submodule
vcpkg already bootstrapped

==> 3/7 Python tools in .venv
installed tools/requirements.txt into .venv (activate with: . .venv/bin/activate)

==> 4/7 Qt
skipped (--no-qt): configure builds with OVT_BUILD_GUI=OFF

==> 5/7 test corpora
skipped: tools/fetch_corpora.py arrives with WI-0.551; tests that need corpora skip

==> 6/7 pre-commit hooks
pre-commit installed at .git/hooks/pre-commit
pre-commit installed at .git/hooks/commit-msg

==> 7/7 next
cmake --preset linux-x64-debug
cmake --build --preset linux-x64-debug
tools/ctest.sh pr
0
```

The container cannot download vcpkg port sources (GitHub archive downloads are blocked), so the configure
with the vcpkg toolchain was not run here. The `-DOVT_USE_VCPKG=OFF` build of this tree is recorded in
`docs/acceptance/wi/WI-0.002.md`.

## Test 2: clean macos-15 runner with only the Xcode command-line tools

Job `bootstrap-check (macos-15)`: the same steps with `macos-arm64-debug`. Result: pending on the job's first
run (see the PR checks).

The macOS branch of the host check was exercised on Linux through the host override:

```console
$ OVT_BOOTSTRAP_HOST=Darwin tools/bootstrap.sh --print-install
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install cmake ninja ccache pkg-config nasm autoconf autoconf-archive automake libtool
```

## Lint

`shellcheck` (pre-commit hook, shellcheck 0.10.0) passes on `tools/bootstrap.sh` and
`tools/bootstrap_android.sh`. `bash -n` passes. The scripts use no bash 4 features (no associative arrays,
`${var,,}` or `mapfile`).
