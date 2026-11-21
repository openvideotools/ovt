#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Installs GCC 13 (gcc-13, g++-13 and their runtime libraries) on a GitHub-hosted Ubuntu runner for
# .github/actions/setup-ovt (chapter 13 §13.1: GCC >= 13). Needs sudo; CI only (tools/bootstrap.sh never runs sudo).
#
#   tools/ci/install-gcc13.sh [<deb cache dir>]
#
# Ubuntu 24.04 and later: gcc-13 comes from the Ubuntu archive. Ubuntu 22.04 does not have it, so it comes from the
# ubuntu-toolchain-r/test PPA, by the first of these routes that works:
#   0. the PPA's .debs in <deb cache dir>, saved there by an earlier run (setup-ovt keeps it in the Actions cache);
#   1. the PPA apt repository (ppa.launchpadcontent.net), up to three tries while less than 12 minutes have passed;
#   2. the same PPA's published .debs straight from Launchpad (api.launchpad.net, launchpadlibrarian.net), which
#      stayed up while ppa.launchpadcontent.net returned 503s for hours on 2026-11-17.
# Routes 1 and 2 install the versions the PPA publishes now; route 0 the ones of the run that filled the cache.
# After route 1 or 2, the PPA's .debs are copied to <deb cache dir> and a file `complete` is written there.
#
# OVT_GCC13_SKIP_PPA_APT=1 skips route 1 (to test route 2 while the PPA host is up).
# Exit 0 when gcc-13 and g++-13 run, non-zero otherwise.
set -euo pipefail

cache_dir="${1:-}"
PPA=ppa:ubuntu-toolchain-r/test
PPA_FILES=https://launchpad.net/~ubuntu-toolchain-r/+archive/ubuntu/test/+files
LP_ARCHIVE=https://api.launchpad.net/1.0/~ubuntu-toolchain-r/+archive/ubuntu/test
# The packages `apt-get install gcc-13 g++-13` takes from the PPA on 22.04 (CI logs of 2026-11-17): the compiler and
# the GCC runtime libraries it needs newer than the archive's 12.3. apt takes everything else from the archive. If the
# PPA's gcc-13 starts to depend on another package of its own, routes 0 and 2 fail on the unmet dependency (route 0
# then falls through to route 1): add the package here.
PPA_PACKAGES=(
  gcc-13 g++-13 cpp-13 gcc-13-base gcc-13-x86-64-linux-gnu g++-13-x86-64-linux-gnu cpp-13-x86-64-linux-gnu
  libgcc-13-dev libstdc++-13-dev
  gcc-16-base libgcc-s1 libstdc++6 libcc1-0 libgomp1 libitm1 libatomic1 libasan8 liblsan0 libtsan2 libubsan1
  libhwasan0 libquadmath0
)
APT_OPTS=(-q -o Acquire::Retries=3 -o Acquire::http::Timeout=60 -o Acquire::https::Timeout=60)
CURL_OPTS=(-fsSL --retry 5 --retry-delay 10 --retry-all-errors --max-time 600)

log() { echo "install-gcc13: $*"; }

installed() {
  command -v gcc-13 >/dev/null && command -v g++-13 >/dev/null || return 1
  g++-13 --version | head -n 1
}

install_debs() { # apt installs the local files and takes their other dependencies from the Ubuntu archive
  sudo apt-get install -y "${APT_OPTS[@]}" --no-install-recommends "$@"
}

fill_cache() { # copies the installed PPA packages' .debs from the directories $@ into $cache_dir
  [ -n "$cache_dir" ] || return 0
  rm -rf "$cache_dir"
  mkdir -p "$cache_dir"
  local pkg want dir deb
  for pkg in "${PPA_PACKAGES[@]}"; do
    want=$(dpkg-query -W -f '${Version}' "$pkg" 2>/dev/null || true)
    for dir in "$@"; do
      for deb in "$dir/${pkg}"_*.deb; do
        [ -f "$deb" ] && [ "$(dpkg-deb -f "$deb" Package)" = "$pkg" ] &&
          [ "$(dpkg-deb -f "$deb" Version)" = "$want" ] && cp "$deb" "$cache_dir/" && continue 3
      done
    done
    log "not caching the .debs: no file for $pkg $want"
    rm -rf "$cache_dir"
    return 0
  done
  touch "$cache_dir/complete"
  log "cached ${#PPA_PACKAGES[@]} .debs in $cache_dir"
}

# shellcheck source=/dev/null
. /etc/os-release
log "runner OS: ${PRETTY_NAME:-unknown}"

if [ "${VERSION_ID:-}" != "22.04" ]; then
  # 24.04 has gcc-13 13.3 in its archive (the 22.04 PPA build is 13.4).
  sudo apt-get update "${APT_OPTS[@]}"
  sudo apt-get install -y "${APT_OPTS[@]}" --no-install-recommends gcc-13 g++-13
  installed
  exit 0
fi

# Route 0: the cached .debs.
if [ -n "$cache_dir" ] && [ -f "$cache_dir/complete" ]; then
  sudo apt-get update "${APT_OPTS[@]}"
  if install_debs "$cache_dir"/*.deb && installed; then
    log "installed from the cached .debs"
    exit 0
  fi
  log "the cached .debs did not install; trying the PPA"
fi

# Route 1: the PPA apt repository. apt keeps finished downloads between tries, so a slow host still gets there.
attempts=3
[ "${OVT_GCC13_SKIP_PPA_APT:-0}" = 1 ] && attempts=0
[ "$attempts" -gt 0 ] && sudo add-apt-repository -y -n "$PPA"
for ((attempt = 1; attempt <= attempts; attempt++)); do
  # apt-get update exits 0 when one source fails ("W: Failed to fetch ... 503"), so look at the candidate instead.
  sudo apt-get update "${APT_OPTS[@]}" || true
  if apt-cache policy gcc-13 | grep -q 'Candidate: [0-9]' &&
    sudo apt-get install -y "${APT_OPTS[@]}" --no-install-recommends gcc-13 g++-13; then
    log "installed from the PPA apt repository (try $attempt)"
    installed
    fill_cache /var/cache/apt/archives
    exit 0
  fi
  if [ "$attempt" -lt "$attempts" ] && [ "$SECONDS" -lt 720 ]; then
    log "PPA try $attempt failed after ${SECONDS} s; retrying in $((attempt * 30)) s"
    sleep $((attempt * 30))
  else
    break
  fi
done

# Route 2: the PPA's published .debs from Launchpad. The failing apt source goes first, so the job's later apt-get
# calls neither print its errors nor wait on it.
log "PPA apt repository unreachable; downloading the PPA's published .debs from Launchpad"
sudo add-apt-repository -y -r -n "$PPA" >/dev/null 2>&1 || true
sudo rm -f /etc/apt/sources.list.d/ubuntu-toolchain-r-ubuntu-test-*
sudo apt-get update "${APT_OPTS[@]}"
debs="${RUNNER_TEMP:-/tmp}/gcc13-debs"
rm -rf "$debs"
mkdir -p "$debs"
for pkg in "${PPA_PACKAGES[@]}"; do
  pub=$(curl "${CURL_OPTS[@]}" -G "$LP_ARCHIVE" \
    --data-urlencode ws.op=getPublishedBinaries --data-urlencode "binary_name=$pkg" \
    --data-urlencode exact_match=true --data-urlencode status=Published \
    --data-urlencode "distro_arch_series=https://api.launchpad.net/1.0/ubuntu/${VERSION_CODENAME}/amd64" |
    jq -r '.entries[0] | select(. != null) | "\(.binary_package_version) \(.architecture_specific)"')
  if [ -z "$pub" ]; then
    log "error: $pkg is not published in the PPA for ${VERSION_CODENAME}/amd64"
    exit 1
  fi
  read -r version arch_specific <<<"$pub"
  arch=amd64
  [ "$arch_specific" = true ] || arch=all
  file="${pkg}_${version#*:}_${arch}.deb" # .deb file names leave out the epoch
  log "$pkg $version"
  curl "${CURL_OPTS[@]}" -o "$debs/$file" "$PPA_FILES/$file" # redirects to launchpadlibrarian.net
done
install_debs "$debs"/*.deb
log "installed from Launchpad's published PPA files"
installed
fill_cache "$debs"
