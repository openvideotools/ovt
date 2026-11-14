#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Apply or check the GitHub ruleset that protects `main` (chapter 15 §15.7.3, WI-0.605).
#
#   tools/github/apply_protection.sh [--check] [--repo <owner/name>]
#
# Reads tools/github/branch_protection.json (the body of a repository ruleset) and uses `gh api`.
#   (no flag)  create the ruleset, or update the one with the same name, so GitHub matches the file;
#              needs repository admin rights, so the owner runs it.
#   --check    change nothing; compare the live ruleset with the file.
# Exit 0 = applied, or no drift. Exit 1 = drift (one `drift: <key> live=<v> wanted=<v>` line each) or an
# API error. Exit 2 = bad usage. --repo defaults to the `origin` remote.
# OVT_GH overrides the `gh` binary (the tests put a fake there).
# Keep this POSIX bash 3.2 compatible (macOS /bin/bash): no mapfile, no associative arrays.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
wanted_file="$here/branch_protection.json"
gh_bin="${OVT_GH:-gh}"

usage() {
  echo "usage: tools/github/apply_protection.sh [--check] [--repo <owner/name>]" >&2
  exit 2
}

check=0
repo=""
while [ $# -gt 0 ]; do
  case "$1" in
    --check) check=1 ;;
    --repo)
      [ $# -ge 2 ] || usage
      repo="$2"
      shift
      ;;
    *) usage ;;
  esac
  shift
done

command -v jq >/dev/null 2>&1 || { echo "error: jq is required" >&2; exit 1; }

if [ -z "$repo" ]; then
  url="$(git -C "$here" remote get-url origin 2>/dev/null || true)"
  repo="$(printf '%s' "$url" | sed -E 's#\.git$##; s#^.*[:/]([^/:]+/[^/]+)$#\1#')"
fi
case "$repo" in
  */*) ;;
  *) echo "error: cannot work out the repository; pass --repo <owner/name>" >&2; exit 1 ;;
esac

# Only the keys the file controls: GitHub adds ids, timestamps and links. Rules become an object keyed by
# type, so the order GitHub returns them in does not matter and a rule without parameters still counts.
normalise='{name, target, enforcement, conditions, bypass_actors,
  rules: ([.rules[] | {(.type): ({present: true} + (.parameters // {}))}] | add // {})}'

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! "$gh_bin" api "repos/$repo/rulesets" >"$tmp/list.json" 2>"$tmp/err"; then
  echo "error: cannot list rulesets of $repo: $(cat "$tmp/err")" >&2
  exit 1
fi
name="$(jq -r .name "$wanted_file")"
id="$(jq -r --arg n "$name" '[.[] | select(.name == $n)][0].id // empty' "$tmp/list.json")"

if [ "$check" -eq 0 ]; then
  if [ -n "$id" ]; then
    method=PUT
    path="repos/$repo/rulesets/$id"
  else
    method=POST
    path="repos/$repo/rulesets"
  fi
  if ! "$gh_bin" api -X "$method" "$path" --input "$wanted_file" >/dev/null 2>"$tmp/err"; then
    echo "error: $method $path failed: $(cat "$tmp/err")" >&2
    exit 1
  fi
  echo "applied ruleset '$name' to $repo ($method)"
  exit 0
fi

if [ -z "$id" ]; then
  echo "drift: ruleset '$name' does not exist in $repo"
  exit 1
fi
if ! "$gh_bin" api "repos/$repo/rulesets/$id" >"$tmp/live.json" 2>"$tmp/err"; then
  echo "error: cannot read ruleset $id of $repo: $(cat "$tmp/err")" >&2
  exit 1
fi

# Flatten both sides to `dotted.key -> scalar` and print every key whose value differs or is on one side only.
drift="$(jq -r -n --slurpfile live "$tmp/live.json" --slurpfile want "$wanted_file" "
  def flat: [paths(scalars) as \$p | {key: (\$p | map(tostring) | join(\".\")), value: getpath(\$p)}]
            | map({(.key): .value}) | add // {};
  (\$live[0] | $normalise | flat) as \$l | (\$want[0] | $normalise | flat) as \$w
  | ((\$l | keys) + (\$w | keys) | unique)[]
  | select(\$l[.] != \$w[.])
  | \"drift: \(.) live=\(\$l[.] | tojson) wanted=\(\$w[.] | tojson)\"
")"
if [ -n "$drift" ]; then
  echo "$drift"
  exit 1
fi
echo "no drift: ruleset '$name' of $repo matches $(basename "$wanted_file")"
