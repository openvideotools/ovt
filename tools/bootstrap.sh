#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# First-time setup of a build host (chapter 13 §13.9.1, WI-0.006). Idempotent: run it again at any time.
#
#   tools/bootstrap.sh [--no-qt] [--no-corpora] [--preset <name>] [--print-install]
#
#   1. checks the host tools and versions; when something is missing it prints the exact install commands
#      (`sudo apt-get install …` from tools/apt-packages.txt, `brew install …` from tools/brew-packages.txt)
#      and stops. It never runs sudo itself.
#   2. git submodule update --init third_party/vcpkg, then third_party/vcpkg/bootstrap-vcpkg.sh
#   3. .venv with the pinned tools/requirements.txt (and tools/requirements-test.txt when present)
#   4. Qt through tools/install_qt.py --target desktop into $QT_ROOT_DIR (default ~/Qt/ovt), unless --no-qt
#   5. tools/fetch_corpora.py --tags fast, unless --no-corpora
#   6. pre-commit install
#   7. prints the configure preset to use (or the one given with --preset)
#
#   --print-install  only print the install commands of step 1, one per line, and exit 0 (CI pipes them
#                    to `sudo bash`; .github/workflows/bootstrap-check.yml).
#
# Steps 4 and 5 are skipped with a note while their scripts (WI-0.018, WI-0.551) do not exist yet.
# OVT_BOOTSTRAP_LIB=1 makes the file define its helpers and return, for tools/bootstrap_android.sh.
# Keep it bash 3.2 compatible (macOS /bin/bash): no associative arrays, no ${var,,}, no mapfile.

set -euo pipefail

OVT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

say()  { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

# version_ge <have> <want>: true when dotted version have >= want.
version_ge() {
  local IFS=.
  # shellcheck disable=SC2206
  local a=($1) b=($2) i
  for ((i = 0; i < ${#b[@]}; i++)); do
    local x="${a[i]:-0}" y="${b[i]:-0}"
    x="${x%%[!0-9]*}"; y="${y%%[!0-9]*}"
    if ((10#${x:-0} > 10#${y:-0})); then return 0; fi
    if ((10#${x:-0} < 10#${y:-0})); then return 1; fi
  done
  return 0
}

# first_version <text>: the first dotted version number in text.
first_version() { printf '%s\n' "$1" | grep -Eo '[0-9]+(\.[0-9]+)+' | head -n 1; }

have() { command -v "$1" >/dev/null 2>&1; }

# package_list <file>: the non-comment lines of a package list.
package_list() { grep -Ev '^[[:space:]]*(#|$)' "$1"; }

host_os() {
  case "${OVT_BOOTSTRAP_HOST:-$(uname -s)}" in
    Darwin) echo macos ;;
    Linux) echo linux ;;
    *) echo other ;;
  esac
}

# ubuntu_version: VERSION_ID of /etc/os-release on Ubuntu, empty elsewhere.
ubuntu_version() {
  if [ -r /etc/os-release ] && grep -q '^ID=ubuntu' /etc/os-release; then
    sed -n 's/^VERSION_ID="\{0,1\}\([0-9.]*\)"\{0,1\}$/\1/p' /etc/os-release
  fi
}

# find_python: a python >= 3.11 on PATH, or nothing.
find_python() {
  local p v
  for p in python3 python3.13 python3.12 python3.11; do
    if have "$p"; then
      v="$("$p" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null || true)"
      if [ -n "$v" ] && version_ge "$v" 3.11 && "$p" -c 'import venv, ensurepip' >/dev/null 2>&1; then
        echo "$p"
        return
      fi
    fi
  done
}

# find_compiler: "gcc <version>" or "clang <version>" for the C++ compiler the presets will use
# ($CXX, else c++), or for gcc-13/clang-16+ found on PATH when the default is too old.
find_compiler() {
  local cxx="${CXX:-c++}" out v
  if have "$cxx"; then
    out="$("$cxx" --version 2>/dev/null | head -n 1)"
    v="$(first_version "$out")"
    case "$out" in
      *clang*) if version_ge "$v" 16; then echo "clang $v $cxx"; return; fi ;;
      *) if version_ge "$v" 13; then echo "gcc $v $cxx"; return; fi ;;
    esac
  fi
  for cxx in g++-14 g++-13 clang++-18 clang++-17 clang++-16; do
    if have "$cxx"; then
      echo "alt $(first_version "$("$cxx" --version | head -n 1)") $cxx"
      return
    fi
  done
}

# ---- step 1 -----------------------------------------------------------------------------------------
MISSING_PKGS=""
INSTALL_CMDS=""
add_pkg() { case " $MISSING_PKGS " in *" $1 "*) ;; *) MISSING_PKGS="${MISSING_PKGS:+$MISSING_PKGS }$1" ;; esac; }
add_cmd() { INSTALL_CMDS="${INSTALL_CMDS:+$INSTALL_CMDS
}$1"; }

check_tools_linux() {
  local ubuntu pkg out
  ubuntu="$(ubuntu_version)"
  if have dpkg-query; then
    while IFS= read -r pkg; do
      if ! dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed'; then
        add_pkg "$pkg"
      fi
    done < <(package_list "$OVT_ROOT/tools/apt-packages.txt")
  else
    warn "not a Debian/Ubuntu host: install the equivalents of tools/apt-packages.txt yourself"
  fi
  # cmake >= 3.25: Ubuntu 22.04 ships 3.22, so the Kitware repository is needed there.
  if have cmake && ! version_ge "$(first_version "$(cmake --version)")" 3.25; then
    add_pkg cmake
    if [ "$ubuntu" = "22.04" ]; then
      add_cmd "curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg"
      add_cmd "echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' > /etc/apt/sources.list.d/kitware.list"
    fi
  elif ! have cmake && [ "$ubuntu" = "22.04" ]; then
    add_cmd "curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg"
    add_cmd "echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' > /etc/apt/sources.list.d/kitware.list"
  fi
  # GCC >= 13 or Clang >= 16 (Ubuntu 22.04: gcc-13 from ubuntu-toolchain-r/test).
  out="$(find_compiler)"
  if [ -z "$out" ]; then
    add_pkg g++-13
    if [ "$ubuntu" = "22.04" ]; then
      add_pkg software-properties-common
      add_cmd "add-apt-repository -y ppa:ubuntu-toolchain-r/test"
    fi
  fi
  # Python >= 3.11 (Ubuntu 22.04: 3.10 by default, 3.11 from deadsnakes).
  if [ -z "$(find_python)" ]; then
    if [ "$ubuntu" = "22.04" ]; then
      add_pkg python3.11
      add_pkg python3.11-venv
      add_pkg software-properties-common
      add_cmd "add-apt-repository -y ppa:deadsnakes/ppa"
    else
      add_pkg python3
      add_pkg python3-venv
    fi
  fi
  if [ -n "$MISSING_PKGS" ]; then
    case "$INSTALL_CMDS" in
      *add-apt-repository*|*kitware*)
        # the repositories above need these first
        INSTALL_CMDS="apt-get update && apt-get install -y --no-install-recommends software-properties-common gpg curl ca-certificates
$INSTALL_CMDS" ;;
    esac
    add_cmd "apt-get update"
    add_cmd "apt-get install -y --no-install-recommends $MISSING_PKGS"
  fi
}

check_tools_macos() {
  local f out v installed=""
  if ! xcode-select -p >/dev/null 2>&1; then
    add_cmd "xcode-select --install"
  else
    out="$(clang --version 2>/dev/null | head -n 1 || true)"
    v="$(first_version "$out")"
    if [ -z "$v" ] || ! version_ge "$v" 16; then
      die "Apple clang ${v:-?} found; Xcode >= 16 (Apple clang 16) is required. Update Xcode, then run this again."
    fi
  fi
  if have brew; then
    installed=" $(brew list --formula -1 2>/dev/null | tr '\n' ' ') "
  else
    # shellcheck disable=SC2016  # printed for the user to run, not expanded here
    add_cmd '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"'
  fi
  while IFS= read -r f; do
    case "$f" in
      python@*) [ -n "$(find_python)" ] && continue ;;
    esac
    case "$installed" in *" $f "*) ;; *) add_pkg "$f" ;; esac
  done < <(package_list "$OVT_ROOT/tools/brew-packages.txt")
  if [ -n "$MISSING_PKGS" ]; then add_cmd "brew install $MISSING_PKGS"; fi
}

check_common() {
  # Tools every host needs; the package lists above cover them, this catches non-Debian Linux.
  local t
  for t in git curl cmake ninja ccache pkg-config nasm; do
    if ! have "$t" && ! { [ "$t" = pkg-config ] && have pkgconf; }; then
      warn "$t not found"
    fi
  done
  if have git && ! version_ge "$(first_version "$(git --version)")" 2.40; then
    warn "git $(first_version "$(git --version)") is older than 2.40 (chapter 13 §13.1); it works for the build"
  fi
}

check_tools() {
  case "$(host_os)" in
    linux) check_tools_linux ;;
    macos) check_tools_macos ;;
    *) die "unsupported host $(uname -s): OVT builds on macOS and Linux (chapter 13 §13.1)" ;;
  esac
}

# ---- step 7 -----------------------------------------------------------------------------------------
default_preset() {
  local arch
  arch="$(uname -m)"
  case "$(host_os)-$arch" in
    macos-arm64) echo macos-arm64-debug ;;
    macos-x86_64) echo macos-x86_64-debug ;;
    linux-x86_64) echo linux-x64-debug ;;
    linux-aarch64|linux-arm64) echo linux-arm64-release ;;
    *) echo linux-x64-debug ;;
  esac
}

if [ "${OVT_BOOTSTRAP_LIB:-0}" = 1 ]; then
  # shellcheck disable=SC2317  # exit is reached when the file is executed instead of sourced
  return 0 2>/dev/null || exit 0
fi

usage() {
  sed -n '6,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
}

with_qt=1
with_corpora=1
preset=""
print_install=0
while [ $# -gt 0 ]; do
  case "$1" in
    --no-qt) with_qt=0 ;;
    --no-corpora) with_corpora=0 ;;
    --preset) [ $# -ge 2 ] || usage; preset="$2"; shift ;;
    --print-install) print_install=1 ;;
    -h|--help) usage ;;
    *) say "unknown argument: $1" >&2; usage ;;
  esac
  shift
done

if [ "$print_install" = 1 ]; then
  check_tools
  [ -z "$INSTALL_CMDS" ] || printf '%s\n' "$INSTALL_CMDS"
  exit 0
fi

step "1/7 host tools"
check_tools
check_common
if [ -n "$INSTALL_CMDS" ]; then
  say "Missing host tools. Run these commands, then run tools/bootstrap.sh again:"
  say ""
  printf '%s\n' "$INSTALL_CMDS" | while IFS= read -r cmd; do
    if [ "$(host_os)" != linux ]; then
      say "  $cmd"
    else
      case "$cmd" in
        *'|'*|*'>'*|*'&&'*) say "  sudo sh -c \"$cmd\"" ;;
        *) say "  sudo $cmd" ;;
      esac
    fi
  done
  exit 1
fi
PYTHON="$(find_python)"
compiler="$(find_compiler)"
say "cmake $(first_version "$(cmake --version)"), ninja $(ninja --version), $PYTHON $("$PYTHON" -c 'import platform; print(platform.python_version())'), ${compiler%% *} $(echo "$compiler" | cut -d' ' -f2)"

step "2/7 vcpkg submodule"
cd "$OVT_ROOT"
if [ -f .gitmodules ] && grep -q 'third_party/vcpkg' .gitmodules; then
  git submodule update --init third_party/vcpkg
  if [ ! -x third_party/vcpkg/vcpkg ]; then
    third_party/vcpkg/bootstrap-vcpkg.sh -disableMetrics
  else
    say "vcpkg already bootstrapped"
  fi
else
  warn "no third_party/vcpkg submodule in this checkout; skipped"
fi

step "3/7 Python tools in .venv"
if [ ! -x .venv/bin/python ]; then
  "$PYTHON" -m venv .venv
fi
.venv/bin/python -m pip install --quiet --upgrade pip
.venv/bin/python -m pip install --quiet -r tools/requirements.txt
if [ -f tools/requirements-test.txt ]; then
  .venv/bin/python -m pip install --quiet -r tools/requirements-test.txt
fi
say "installed tools/requirements.txt into .venv (activate with: . .venv/bin/activate)"

step "4/7 Qt"
if [ "$with_qt" = 0 ]; then
  say "skipped (--no-qt): configure builds with OVT_BUILD_GUI=OFF"
elif [ -f tools/install_qt.py ]; then
  QT_ROOT_DIR="${QT_ROOT_DIR:-$HOME/Qt/ovt}" .venv/bin/python -I tools/install_qt.py --target desktop
else
  say "skipped: tools/install_qt.py arrives with WI-0.018; use --no-qt or the Qt online installer meanwhile"
fi

step "5/7 test corpora"
if [ "$with_corpora" = 0 ]; then
  say "skipped (--no-corpora): tests that need corpora skip"
elif [ -f tools/fetch_corpora.py ]; then
  .venv/bin/python -I tools/fetch_corpora.py --tags fast
else
  say "skipped: tools/fetch_corpora.py arrives with WI-0.551; tests that need corpora skip"
fi

step "6/7 pre-commit hooks"
if [ -f .pre-commit-config.yaml ] && git rev-parse --git-dir >/dev/null 2>&1; then
  .venv/bin/pre-commit install
else
  say "skipped: no .pre-commit-config.yaml"
fi

step "7/7 next"
preset="${preset:-$(default_preset)}"
case "$compiler" in
  alt*)
    alt="$(echo "$compiler" | cut -d' ' -f3)"
    say "The default C++ compiler is too old; $alt works. Export it before configuring:"
    case "$alt" in
      clang*) say "  export CC=${alt/clang++/clang} CXX=$alt" ;;
      *) say "  export CC=${alt/g++/gcc} CXX=$alt" ;;
    esac ;;
esac
say "cmake --preset $preset"
say "cmake --build --preset $preset"
say "tools/ctest.sh pr"
