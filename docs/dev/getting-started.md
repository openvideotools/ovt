# Getting started

From a fresh clone to a passing test run, on macOS or Linux.

## Supported hosts

| Host | Compiler | Notes |
|---|---|---|
| macOS 13 or later, arm64 or x86_64 | Xcode 16 or later (Apple clang 16) | Homebrew for the other tools |
| Ubuntu 22.04 or later (or an equivalent distribution) | GCC 13 or later, or Clang 16 or later | On 22.04, CMake, GCC 13 and Python 3.11 come from extra repositories (see below) |

Every host also needs Python 3.11 or later, Git, curl, about 20 GB of free disk and, ideally, 16 GB of RAM
(chapter 13 §13.1).

## 1. Bootstrap

```sh
git clone git@github.com:openvideotools/ovt.git && cd ovt
tools/bootstrap.sh --no-qt        # drop --no-qt once you want the GUI (installs Qt 6.8.3)
```

`tools/bootstrap.sh` is safe to run again at any time. It:

1. Checks the host tools and their versions. When something is missing it prints the exact commands
   (`brew install …` from `tools/brew-packages.txt`, `sudo apt-get install …` from `tools/apt-packages.txt`)
   and stops. It never runs `sudo` itself. Run the printed commands, then run `tools/bootstrap.sh` again.
2. Checks out the `third_party/vcpkg` submodule and bootstraps vcpkg.
3. Creates `.venv/` with the pinned developer tools of `tools/requirements.txt` (meson, ninja, aqtinstall,
   ruff, mypy, reuse, pre-commit, …).
4. Installs Qt 6.8.3 into `$QT_ROOT_DIR` (default `~/Qt/ovt`) with `tools/install_qt.py`, unless you pass
   `--no-qt`. That script arrives with WI-0.018; until then this step is skipped.
5. Fetches the fast test corpora (`tools/fetch_corpora.py --tags fast`), unless you pass `--no-corpora`.
   That script arrives with WI-0.551; until then tests that need corpora skip.
6. Installs the pre-commit hooks (see below).
7. Prints the configure preset for your machine. `--preset <name>` picks another one.

On Ubuntu 22.04 the printed commands also add three package sources, because the distribution's versions
are too old: the Kitware repository (CMake ≥ 3.25), `ppa:ubuntu-toolchain-r/test` (GCC 13) and
`ppa:deadsnakes/ppa` (Python 3.11). GCC 13 is then installed next to the default GCC 11, so export it
before configuring (bootstrap prints the line):

```sh
export CC=gcc-13 CXX=g++-13
```

`tools/bootstrap.sh --print-install` prints only the install commands, without `sudo`, and exits. The
monthly `bootstrap-check` workflow (`.github/workflows/bootstrap-check.yml`) uses it to prove that a fresh
Ubuntu 22.04 container and a clean macOS 15 runner get from nothing to a successful build.

## 2. Configure, build, test

```sh
cmake --preset linux-x64-debug         # macOS: macos-arm64-debug
cmake --build --preset linux-x64-debug
tools/ctest.sh pr                      # the fast lane of your host
```

The first configure builds the vcpkg dependencies, which takes a while; later configures reuse them.
`docs/dev/presets.md` lists every preset, and `docs/dev/dependencies.md` explains where the libraries come
from and how to build without vcpkg.

## Android

After `tools/bootstrap.sh`, run `tools/bootstrap_android.sh`. It needs JDK 17 and an Android SDK
(`ANDROID_SDK_ROOT`) with the command-line tools. It installs the pinned NDK, platform, build-tools and
CMake through `sdkmanager`, installs the Qt Android kits (once `tools/install_qt.py` exists) and writes
`android/local.properties`. `--fdroid` marks the F-Droid flavour; `--print-only` prints the `sdkmanager`
command instead of running it.

## Pre-commit hooks

`.pre-commit-config.yaml` holds the hooks of chapter 13 §13.9.2 at pinned revisions. Install them once per
clone:

```sh
pip install pre-commit==4.0.1
pre-commit install --install-hooks      # installs the pre-commit and commit-msg hooks
```

On every `git commit` they check the staged files:

| Hook | Files | What it does |
|---|---|---|
| `clang-format` | C, C++, Objective-C++ | Reformats with `.clang-format` (Google base, 120 columns, `int* p`). |
| `cmake-format` | `CMakeLists.txt`, `*.cmake` | Reformats with `.cmake-format.yaml`. |
| `ruff`, `ruff-format` | Python | Lint and format, 120 columns. |
| `mypy` | `tools/`, `python/` | Type check. |
| `check-added-large-files` | all | Rejects files over 2 MB (corpora and big goldens are never committed). |
| `end-of-file-fixer`, `trailing-whitespace` | all | Whitespace fixes. |
| `shellcheck` | shell scripts | Lint. |
| `pretty-format-kotlin` | `*.kt` | ktlint; needs a JDK. |
| `check-versions-md` | `third_party/` | `tools/check_versions_md.py`: every `third_party/*.cmake` script has a `VERSIONS.md` row, and the scripts read their pins from `versions.cmake`. |
| `reuse-lint-file` | source files | SPDX header present and its licence text in `LICENSES/` (`docs/licensing.md`). |

A hook that reformats a file fails the commit and leaves the fix in your working tree. Look at the change,
`git add` it and commit again.

The commit-msg stage checks the message:

- `check-wi-refs`: the message must reference a work item (`WI-0.007: …`) or an issue (`#15`). Merge and
  `fixup!` commits are exempt.
- `dco-signoff`: the message needs a `Signed-off-by:` trailer (DCO, chapter 15 §15.7). The hook does not
  add the trailer for you; it stops the commit and tells you to use `git commit -s`.

The files that existed before WI-0.007 have not been reformatted yet. `pre-commit run --all-files` would
rewrite them, so run the hooks on your own changes instead:

```sh
pre-commit run --from-ref origin/main --to-ref HEAD
```

## Specification checks

`tools/check_wi_refs.py` checks the work-item bookkeeping of chapter 16:

```sh
python3 -I tools/check_wi_refs.py --table                  # §16.4 against the chapters and §4.17 (§16.2.5)
python3 -I tools/check_wi_refs.py --spec-version           # a docs/spec change needs a version bump (§16.2.8)
python3 -I tools/check_wi_refs.py --make-issues --dry-run  # tracker issues still to create (§16.2.1)
```

Run `--table` whenever you change a WI block or a §16.4 row.
