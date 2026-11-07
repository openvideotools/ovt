# Continuous integration

`.github/workflows/ci.yml` runs on every pull request, on pushes to `main` and on pushes to `ci/**` branches
(chapter 13 §13.7.1, WI-0.030). Later work items add their own jobs to the same file.

| Job | Runner | What it does |
|---|---|---|
| `lint` | `ubuntu-22.04` | pre-commit on the changed files, `reuse lint`, `tools/check_spdx_dirs.py`, markdownlint on the changed Markdown, `tools/check_versions_md.py`, `tools/check_wi_refs.py --table`, `pytest tools/tests` |
| `build-linux-x64` | `ubuntu-22.04` | `cmake --workflow --preset ci-linux-x64` (GCC 13), JUnit and `ovt-linux-x64.tar.zst` as artefacts |
| `build-macos-arm64` | the Mac Mini, `[self-hosted, macOS, ARM64]` | `cmake --workflow --preset ci-macos-arm64`, `tools/ctest.sh pr`, ad-hoc signing; on `main` the `ovt-macos-arm64` artefact |

Until every M0.1 work item is merged, a `lint` step whose tool is not on the branch yet prints a notice and
skips (#27).

## Getting a macOS build

Every push to `main` uploads `ovt-macos-arm64`: a zip with the `ovt` CLI and, once the GUI exists,
`OVT Studio.app`, both ad-hoc signed. Artefacts are kept for 30 days, and only while the repository is
private: a public artefact would be a binary distribution without third-party notices. To download the latest:

```sh
gh run download --repo openvideotools/ovt -n ovt-macos-arm64 \
  "$(gh run list --repo openvideotools/ovt --workflow ci --branch main --status success --limit 1 \
     --json databaseId --jq '.[0].databaseId')"
unzip ovt-macos-arm64.zip && xattr -dr com.apple.quarantine ovt-macos-arm64
```

## Trying several open pull requests together

Merge the branches into a `ci/<name>` branch and push it. CI runs on the push, just as it would on `main`
after the merges. Delete the branch afterwards.

## Caches

`.github/actions/setup-ovt` restores the caches of §13.7.2:

- the vcpkg binary cache (`build/vcpkg-archives`, through `VCPKG_BINARY_SOURCES`);
- the `third_party` install prefix (exact key only) and downloads;
- ccache (2 GB, `compiler_check=content`);
- Qt, once the GUI needs it.

The keys hash `vcpkg.json`, the triplets and overlays, and `third_party/VERSIONS.md` with the dependency
scripts and patches, so a pin change rebuilds what it affects.
