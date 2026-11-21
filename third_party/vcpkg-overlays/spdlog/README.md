# spdlog overlay: vendored backport patches

The upstream `spdlog` port of the pinned vcpkg baseline (1.17.0, port-version 1) fetches two upstream commits as
patches with `vcpkg_download_distfile` from `https://github.com/gabime/spdlog/commit/<sha>.patch?full_index=1`.
That endpoint intermittently answers HTTP 503. When the vcpkg binary cache has no spdlog entry (a new vcpkg hash
key, a cold cache), the Android jobs (`android-build`, `android-emulator-smoke`) rebuild spdlog from source and
fail on the download.

This overlay is the upstream port with the patches stored here instead, so building spdlog needs only the release
archive (the same one the upstream port uses, same hash):

- `patches/backport-3541-fmt-format-string-copy.patch` is spdlog commit `1685e694c5cd8328280c48f8b94b1281c17c6fee`
  (PR #3541, "Fix deprecated copy constructor usage of fmt::format_string").
- `patches/backport-3543-format-string-propagation.patch` is spdlog commit `d5af52d903ed69f96c7350155e94aeff248b31eb`
  (PR #3543, "Fix format_string propagation").

The patches live in `patches/` so the repository's whitespace hooks leave them byte-exact. Both are exported with `git format-patch --full-index`, so author and message are kept; only the `Upstream-Status`, `SPDX-License-Identifier` and `Rationale` header lines required by `tools/check_patches.py` are added to the message. They are MIT-licensed like
the rest of spdlog (copyright Gabi Melman and spdlog contributors); `REUSE.toml` annotates them. They apply cleanly
to `v1.17.0`. `vcpkg.json` and `usage` are copied unchanged from the baseline.

Delete this overlay when the pinned baseline's spdlog port no longer needs the patches (a spdlog release that
contains both commits) or no longer downloads them.
