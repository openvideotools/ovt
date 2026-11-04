# Licensing

> **Not legal advice.** This page explains the project's licensing decisions so that contributors and
> redistributors know what applies. It is not legal advice; if your use depends on it, ask a lawyer. The
> binding text is chapter 15 of the specification (`docs/spec/15-licensing-and-governance.md`).

## The short version

- Open Video Tools as a whole (any official binary containing the `ovt` CLI or OVT Studio) is distributed
  under **GPL-3.0-or-later**: see `LICENSE`.
- The library layer (`core/`, `render/`) is **LGPL-2.1-or-later**, so encoder vendors and other projects can
  embed it. The plugin ABI headers and the FlatBuffers schemas are **BSD-2-Clause**, so anyone can implement
  a plugin or read OVT's output files.
- Each file says its licence in its first lines (`SPDX-License-Identifier:`). Files that cannot carry a
  header (Markdown, JSON, binary assets) are covered by `REUSE.toml`. The full texts are in `LICENSES/`.
  `reuse lint` checks all of this ([REUSE](https://reuse.software/) specification 3.3).
- Documentation prose is **CC-BY-SA-4.0**; code snippets inside the documentation are BSD-2-Clause
  (`docs/LICENSE.md`).

## Licence per directory (chapter 15 §15.1)

| Directory (§0.4) | SPDX licence expression | Why | Allowed inbound code |
|---|---|---|---|
| `core/` (except `core/include/ovt/abi/`) | `LGPL-2.1-or-later` | Embeddable data layer for encoder vendors' CI; relicensable to GPL-2/3 as needed (ADR-002). | Own code; permissive (MIT/BSD/Apache-2.0*/zlib/ISC); LGPL-2.1+ code. **No GPL code, no YUView code, no LGPL-3-only code.** (*Apache-2.0 is incompatible with LGPL-2.1-*only* and compatible with LGPL-3.0; combining Apache-2.0 libraries with `core/` works only because `core/` is `LGPL-2.1-or-later`, so the combined binary is effectively distributed under LGPL-3.0 terms and GPL-2.0-only downstream consumers of such a binary are unsupported — see §15.3. Copied Apache-2.0 *source* in `core/` needs a NOTICE entry; prefer not to copy.) |
| `core/include/ovt/abi/` | `BSD-2-Clause` | Lets proprietary/other-licence decoder vendors implement plugins without copyleft on the header (ADR-002). | Own code only; no third-party code. |
| `core/schema/*.fbs` | `BSD-2-Clause` | Schemas are part of the plugin interface and must be reusable by any consumer (Kotlin, JS, Python). | Own code only. |
| `render/` | `LGPL-2.1-or-later` | Same reasoning as core; links libplacebo (LGPL-2.1+). | As for `core/`. |
| `plugins/ovt-plugin-aom/` | `BSD-2-Clause` (own code) — binary also subject to `LicenseRef-AOM-Patent-License-1.0` | Follows libaom; AOM patent licence must accompany binaries. | Own code; libaom. |
| `plugins/ovt-plugin-de265/` | `LGPL-3.0-or-later` | Statically links libde265 (LGPL-3.0+); patch files are LGPL-3.0-or-later too (derivative of libde265). | Own code; libde265. |
| `plugins/ovt-plugin-ffmpeg/` | `LGPL-2.1-or-later` (becomes `GPL-2.0-or-later` when built with `OVT_FFMPEG_GPL=ON`) | Private static FFmpeg (chapter 13 §13.3.3). | Own code; FFmpeg public API only. |
| `plugins/ovt-plugin-hm/` | `BSD-3-Clause` | Follows HM. | Own code; HM. |
| `cli/` | `GPL-3.0-or-later` | Application code; may use GPL libraries and YUView-derived code (ADR-002). | Anything GPL-3-compatible. |
| `app/` | `GPL-3.0-or-later` | Qt GPL-only modules (Qt Graphs), KDDockWidgets (GPL), YUView-derived code. | Anything GPL-3-compatible. |
| `android/` | `GPL-3.0-or-later` | Part of the application. | Anything GPL-3-compatible. |
| `python/` | `GPL-3.0-or-later` | Application-level bindings; wheel bundles GPL-compatible deps only. | Anything GPL-3-compatible. |
| `tools/`, `tests/`, `.github/`, `cmake/`, `third_party/*.cmake` | `LGPL-2.1-or-later` | Build and test infrastructure must be reusable by downstream LGPL consumers of core. | Own code; permissive. |
| `docs/` | `CC-BY-SA-4.0` for prose; code snippets in docs are `BSD-2-Clause` (stated in `docs/LICENSE.md`) | Documentation is not software; share-alike keeps derived docs open. | — |
| `third_party/patches/`, `plugins/*/patches/` | licence of the patched project (stated in the patch header) | A patch is a derivative work of the upstream project. | — |

`LICENSES/` holds the full texts, taken verbatim from the
[SPDX License List](https://github.com/spdx/license-list-data) 3.29.0 (`text/<id>.txt`, checked against the
`licenseText` field of `json/details/<id>.json`). `LicenseRef-AOM-Patent-License-1.0.txt` is the `PATENTS`
file of the libaom 3.12.1 release archive, byte for byte; its SHA-256 is recorded in the libaom row of
`third_party/VERSIONS.md`.

New files start with the header of their directory, for example in `core/`:

```c
// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
```

The year is the year the file was created and is never updated in bulk. "The Open Video Tools contributors"
stands for everyone in the git history; authorship is tracked by git and the DCO sign-off. Code copied from
another project keeps its original copyright line in addition (chapter 15 §15.1.1).

A change that alters a file's licence, or moves code between directories with different licences, is a
separate pull request titled `licence:` and needs two licence reviewers (chapter 15 §15.7.3).

## What may go where (chapter 15 §15.3)

Rows: the licence of a component you want to add. Columns: where it may go.

| Inbound component licence | `core/`, `render/` (LGPL-2.1+) | `abi/`, `schema/` (BSD-2) | plugins (own licence per dir) | `cli/`, `app/`, `android/`, `python/` (GPL-3+) | Official combined binary (GPL-3) |
|---|---|---|---|---|---|
| Public domain / CC0 / Unlicense / WTFPL | ✔ | ✔ (copy with note) | ✔ | ✔ | ✔ |
| MIT / BSD-2 / BSD-3 / ISC / Zlib / BSL-1.0 | ✔ | ✘ (no third-party code in ABI headers — policy, not law) | ✔ | ✔ | ✔ |
| Apache-2.0 | ✔ link (combined work distributed under LGPL-3.0 terms because `core/` is "or-later"; GPL-2.0-only downstreams unsupported — FSF: Apache-2.0 is incompatible with (L)GPL-2.x-only, compatible with (L)GPL-3.0); ✘ copy source (avoid NOTICE burden; needs 2 reviewers) | ✘ | ✔ | ✔ | ✔ (GPL-3 compatible) |
| MPL-2.0 | ✔ link (file-level copyleft stays on MPL files) | ✘ | ✔ | ✔ | ✔ |
| LGPL-2.1-or-later | ✔ | ✘ | ✔ | ✔ | ✔ |
| LGPL-2.1-only | ✔ (link); copy source only as LGPL-2.1-only files (allowed inside an "or-later" library? No — would pin the library; **✘ copy**) | ✘ | ✔ | ✔ (upgrades to GPL-3 §3? LGPL-2.1 §3 allows conversion to GPL-2+; GPL-2+ combines with GPL-3) | ✔ |
| LGPL-3.0-or-later / -only | **✘** (would make core LGPL-3) | ✘ | ✔ (de265 plugin) | ✔ | ✔ |
| GPL-2.0-or-later | ✘ | ✘ | ✔ only in a plugin declared GPL (`OVT_FFMPEG_GPL`) — then **not** in default download | ✔ (as GPL-3) | ✔ |
| GPL-2.0-only | ✘ | ✘ | ✘ | **✘** (incompatible with GPL-3) | ✘ |
| GPL-3.0-only / -or-later | ✘ | ✘ | ✘ (plugins are loaded by the LGPL core; a GPL plugin would be loaded as user's combination — allowed for *third parties*, not shipped by us except via GPL flag) | ✔ | ✔ |
| AGPL-3.0 (any) | **✘ forbidden** | ✘ | ✘ | **✘ forbidden** (owner policy, ADR-002) | ✘ |
| SSPL, BUSL-1.1, Elastic-2.0, Commons Clause, CC-BY-NC-*, "non-commercial", "modified Apache with NC clause" (GitlHEVCAnalyzer) | ✘ | ✘ | ✘ | ✘ | ✘ |
| Proprietary / no licence / "all rights reserved" | ✘ | ✘ | ✘ (a *user-installed* proprietary plugin is the user's combination; we never ship it) | ✘ | ✘ |
| CC-BY-4.0 / CC-BY-SA-4.0 | docs only | — | — | docs only | ✔ (docs, not code) |

When you add a dependency, find its SPDX id (its `LICENSE`, the vcpkg `copyright` file or
`third_party/VERSIONS.md`), find the row, and check the column of the directory you are changing. If the cell
is ✘ or the licence is not in the table, stop and open an issue labelled `licensing`. The licence gate
(`tools/license_gate.py`, chapter 15 §15.4) enforces the same rules in CI.

## Patents and the codec pack (chapter 15 §15.5.2)

- OVT is free of charge (POL-PAT-1).
- The official downloads contain **no AVC or HEVC decoder** (POL-PAT-2). They contain OVT's own parsers, the
  LGPL FFmpeg build with the `h264`/`hevc` decoders disabled, libaom with its `PATENTS` text, dav1d and
  libvmaf.
- AVC and HEVC decoding comes from **runtime-loaded plugins** distributed as a separate codec pack, with its
  own licence files and a patent notice (POL-PAT-3). Distributors may build a single package with
  `OVT_PATENT_CODECS_IN_CORE=ON`; the project's own releases never do.
- Every artefact that contains libaom ships libaom's `LICENSE` and `PATENTS` texts (POL-PAT-6).
- Companies that redistribute OVT with the AVC/HEVC plugins carry their own patent exposure. The plugin ABI
  lets them substitute a licensed decoder (POL-PAT-7).
- The project does not host or link to proprietary decoder plugins (POL-PAT-5), and contributors do not
  research patents. Issues about specific patent claims are closed with a pointer to this policy
  (POL-PAT-8).

## Plugins and the GPL

The plugin ABI (`core/include/ovt/abi/*.h`, BSD-2-Clause) and the FlatBuffers schemas are a published,
documented and stable interface. A plugin that a user installs and OVT loads at run time is the user's own
combination, not something the project distributes, so a vendor's proprietary decoder plugin is permitted
as a user action (POL-PAT-4). This is accepted practice (GCC plugins before the runtime exception, Linux
kernel modules, Kodi binary add-ons, YUView's external decoders), not a position the FSF endorses.

The full position, with one paragraph per distribution channel, is in the "Plugins and the GPL" section
that WI-0.608 adds to this page.
