# fmt overlay: vendored backport patch

The upstream `fmt` port of the pinned vcpkg baseline (12.2.0, port-version 1) fetches one upstream commit as a patch
with `vcpkg_download_distfile` from `https://github.com/fmtlib/fmt/commit/<sha>.patch?full_index=1`. That endpoint
intermittently answers HTTP 503, which fails any job that builds fmt from source because the vcpkg binary cache has no
entry for it. This is the same problem as the spdlog overlay (`../spdlog/README.md`).

This overlay is the upstream port with the patch stored here, so building fmt needs only the release archive (the same
one the upstream port uses, same hash):

- `patches/backport-4813-uint128-bitwise-not.patch` is fmt commit `588b3a0f8f6a8bcf2a959cae882d5b2703e86737`
  (PR #4813, "Fix fallback uint128 bitwise not").

The patch lives in `patches/` so the repository's whitespace hooks leave it byte-exact. It is exported with
`git format-patch --full-index`, so author and message are kept; only the `Upstream-Status`,
`SPDX-License-Identifier` and `Rationale` header lines required by `tools/check_patches.py` are added to the message.
It is MIT-licensed like the rest of fmt (copyright Victor Zverovich and fmt contributors); `REUSE.toml` annotates it.
It applies cleanly to `12.2.0`. `vcpkg.json` and `usage` are copied unchanged from the baseline.

Delete this overlay when the pinned baseline's fmt port no longer needs the patch (a fmt release that contains the
commit) or no longer downloads it.
