#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Shared helpers for the oracle build scripts (chapter 14 §14.2.6, WI-0.554). Sourced, never executed.
# Every source is pinned: a git commit that is checked after the fetch, or an archive with a SHA-256.

set -euo pipefail

OVT_ORACLES_PREFIX="${OVT_ORACLES_PREFIX:-/opt/ovt-oracles}"
OVT_ORACLES_STAMPS="${OVT_ORACLES_PREFIX}/share/ovt-oracles/stamps"
OVT_ORACLES_JOBS="${OVT_ORACLES_JOBS:-$(nproc)}"
export PKG_CONFIG_PATH="${OVT_ORACLES_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

ovt_log() { printf '[oracles] %s\n' "$*" >&2; }
ovt_die() {
  printf '[oracles] error: %s\n' "$*" >&2
  exit 1
}

# ovt_git_fetch <url> <tag-or-branch> <commit> <dest> [full]
# Fetches <tag-or-branch> (shallow unless "full": x264 derives its revision number from the history) and
# fails unless the checkout is <commit> (40 hex digits), so a moved tag is detected instead of silently
# building different sources.
ovt_git_fetch() {
  local url="$1" ref="$2" commit="$3" dest="$4" mode="${5:-shallow}" head
  [[ "${commit}" =~ ^[0-9a-f]{40}$ ]] || ovt_die "${url}: pin '${commit}' is not a full commit id"
  rm -rf "${dest}"
  git init -q "${dest}"
  git -C "${dest}" remote add origin "${url}"
  # A tag is stored locally too, so `git describe` (GPAC, FFmpeg version strings) sees it.
  local spec="${ref}"
  [[ "${ref}" == refs/tags/* ]] && spec="+${ref}:${ref}"
  if [[ "${mode}" == full ]]; then
    git -C "${dest}" fetch -q origin "${spec}"
    git -C "${dest}" -c advice.detachedHead=false checkout -q "${commit}"
  else
    git -C "${dest}" fetch -q --depth 1 origin "${spec}"
    git -C "${dest}" -c advice.detachedHead=false checkout -q FETCH_HEAD
  fi
  head="$(git -C "${dest}" rev-parse 'HEAD^{commit}')"
  [[ "${head}" == "${commit}" ]] || ovt_die "${url} ${ref} is ${head}, pinned ${commit}"
  ovt_log "fetched ${url} ${ref} = ${head}"
}

# ovt_download <url> <sha256> <file>
ovt_download() {
  local url="$1" sha="$2" out="$3" actual
  curl -fsSL --retry 5 --retry-delay 5 -o "${out}" "${url}"
  actual="$(sha256sum "${out}" | cut -d' ' -f1)"
  [[ "${actual}" == "${sha}" ]] || ovt_die "${url}: SHA-256 ${actual}, pinned ${sha}"
  ovt_log "downloaded ${url} (sha256 ${actual})"
}

# ovt_stamp <tool> <text>: the version record read by oracle-versions for tools that print no version.
ovt_stamp() {
  mkdir -p "${OVT_ORACLES_STAMPS}"
  printf '%s\n' "$2" >"${OVT_ORACLES_STAMPS}/$1.version"
}
