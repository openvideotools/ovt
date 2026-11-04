# Contributing to OVT

## Welcome and scope

OVT (Open Video Tools) is a free, cross-platform analysis suite for H.264, H.265 and AV1: a C++ core library,
the `ovt` command-line tool, the OVT Studio desktop and Android app, and a Python package. The specification
in [`docs/spec/`](docs/spec/) is the single source of truth. Work is cut into work items (`WI-<phase>.<nnn>`),
listed in chapter 16 ([`docs/spec/16-roadmap-and-work-items.md`](docs/spec/16-roadmap-and-work-items.md)).
Issues labelled `good first WI` are a good place to start.

## Before you start

- Search the issues first; someone may already be on it.
- Discuss a large change in an issue that names the `WI-` id before you write the code.
- One work item per pull request. A drive-by refactor or a dependency bump is its own pull request.

## Setup

```sh
tools/bootstrap.sh --no-qt          # checks the host tools, sets up vcpkg and the Python venv
cmake --preset linux-x64-debug      # macOS: macos-arm64-debug
cmake --build --preset linux-x64-debug
ctest --preset pr
```

[`docs/dev/getting-started.md`](docs/dev/getting-started.md) has the details, including the Android setup.

## Coding standards

The summary is in chapter 13 §13.9.3: C++20, no exceptions across the C ABI, no raw `new`/`delete`, a test
file per public class, a fuzz target per parser, logging only through `ovt::log`, and an SPDX header on every
file. `.clang-format`, `.cmake-format.yaml` and ruff (120 columns) do the formatting. Install the hooks once:

```sh
pre-commit install --install-hooks
```

## Licensing of contributions

The licence of your contribution is the one in the SPDX header of the file you change, which follows the
directory table in [`docs/licensing.md`](docs/licensing.md):

- if you touch `core/` or `render/`, your code is **LGPL-2.1-or-later**;
- if you touch `app/`, `cli/`, `android/` or `python/`, it is **GPL-3.0-or-later**;
- the plugin ABI headers and the schemas are **BSD-2-Clause**; each plugin directory follows its upstream.

Do not copy code from a project whose licence is not allowed in that directory (the matrix in
`docs/licensing.md`, chapter 15 §15.3). Code derived from another project keeps its copyright line: add an
extra `SPDX-FileCopyrightText:` line for the original authors (chapter 15 §15.1.1).

Do not discuss patents or specific patent claims in issues or pull requests. Such threads are closed with a
pointer to the patent policy and forwarded to the owner privately (POL-PAT-8).

## Developer Certificate of Origin

There is no CLA and no copyright assignment. Instead, every commit carries a `Signed-off-by:` line, by which
you certify the [Developer Certificate of Origin](DCO): that you have the right to submit the change under
the licence of the file. `git commit -s` adds the line. A pseudonym is fine if you use it consistently.

If you forgot it, `git rebase --signoff origin/main` adds the line to all commits of your branch; then push
with `--force-with-lease`. [`docs/dev/dco.md`](docs/dev/dco.md) explains the details and the DCO check.

## Pull request checklist

The [pull request template](.github/PULL_REQUEST_TEMPLATE.md) repeats this list:

- tests added (unit tests, plus golden and fuzz tests where the change touches a parser, decoder, exporter or
  metric);
- a `changelog.d/` fragment for a user-visible change;
- docs updated, including the spec chapter when the implementation deviates from it;
- CI green;
- a `WI-` or issue reference;
- no new dependency without a `third_party/VERSIONS.md` row and a `tools/license_policy.toml` review.

## Review process

CODEOWNERS assigns the reviewers. Changes to `app/` or `cli/` need one approval. Changes to `core/include/`,
the plugin ABI, `core/schema/`, `LICENSES/`, `tools/license_policy.toml` or `third_party/VERSIONS.md` need
two, and ABI or schema changes stay open at least 72 hours. Pull requests are squash-merged, so the PR title
becomes the commit subject.

## Reporting security issues

Do not open a public issue for a security bug. A crash or memory error on a crafted input file counts as one.
Report it privately as described in [`SECURITY.md`](SECURITY.md); the default disclosure delay is 90 days.

## Code of conduct

Everyone taking part follows the [Contributor Covenant 2.1](CODE_OF_CONDUCT.md). Report unacceptable
behaviour to [conduct@ovt.dev](mailto:conduct@ovt.dev).

## Communication

GitHub issues for now. GitHub Discussions, a Matrix room and a monthly video call (notes in `docs/meetings/`)
start when the repository becomes public.

## Adding plugins and metrics

Chapter 13 §13.9.4 is the checklist for a new analysis decoder plugin and §13.9.5 for a new metric. Whether a
plugin ships in the default download, in the codec pack or not at all is decided by the table in chapter 15
§15.5.4.
