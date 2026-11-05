# Decoder patches

OVT prefers public decoder APIs. When a decoder exposes no API for the data an analysis view needs, a small
patch adds a read-only accessor (requirement NFR-026; policy in chapter 15 §15.5.5 of the specification). This
page says where patches live, which header they carry, and how `tools/check_patches.py` checks them.

## Where patches live

- `plugins/<plugin>/patches/` for a decoder plugin, for example
  `plugins/ovt-plugin-de265/patches/0001-de265-internals-api.patch`.
- `third_party/patches/` for any other third-party project.

Each patch is also listed in the patch column of `third_party/VERSIONS.md`. `tools/apply_patches.py` applies
them at build time (see [dependencies.md](dependencies.md)).

## Required header

Write every field before the diff. With `git format-patch`, put the fields in the commit message body:

<!-- REUSE-IgnoreStart -->

```text
From: A Developer <dev@example.org>
Date: Wed, 5 Nov 2026 10:00:00 +0000
Subject: [PATCH] export the CTB tree as side data
Upstream-Status: Submitted [https://github.com/strukturag/libde265/pull/123]
SPDX-License-Identifier: LGPL-3.0-or-later
Rationale: libde265 has no public API that exposes the coding tree.
Issue: https://github.com/openvideotools/ovt/issues/99
```

<!-- REUSE-IgnoreEnd -->

| Field | Required | Value |
|---|---|---|
| `From` | always | Author name and e-mail. |
| `Subject` | always | One-line summary, usually `[PATCH] …`. |
| `Upstream-Status` | always | `Submitted [<URL>]`, `Accepted [<version>]`, `Pending` or `Inappropriate [<reason>]`. The brackets may be left out. |
| `SPDX-License-Identifier` | always | The licence of the patched project, not OVT's. |
| `Rationale` | always | Why no public API is enough. |
| `Date` | when `Pending` | `git format-patch` writes it. Hand-written patches add it, as RFC 2822 or `YYYY-MM-DD`. |
| `Issue` | when `Pending` for more than 180 days | A URL or `#<number>` of the issue that tracks the upstreaming. |

The header ends at the first line that is exactly `---`, or that starts with `diff --git`, with `---` followed by
a space, or with `Index:` followed by a space.
Field lines inside the diff do not count.

## Upstreaming

The work item that adds a patch includes the upstreaming attempt as one of its steps. Record the merge request
or mailing-list URL in `Upstream-Status: Submitted [...]` as soon as it exists. `Pending` is meant for a short
time only:

- up to 180 days after `Date:`, the checker prints a warning `W-PATCH-PENDING <file> <days>`;
- after 180 days, the patch fails with `E-PATCH-STALE <file> <days>` unless an `Issue:` line links an issue that
  explains the delay. With the link, the warning stays.

Days are counted from the `Date:` field, never from the file's modification time, because a checkout resets it.

## Running the checker

```sh
python3 -I tools/check_patches.py                              # every patch in the two directories
python3 -I tools/check_patches.py plugins/ovt-plugin-de265/patches/0001-x.patch
python3 -I tools/check_patches.py --max-pending-days 90        # a stricter limit
```

It exits 1 when any patch has an error, and 0 otherwise. It prints one line per finding:

| Message | Meaning |
|---|---|
| `E-PATCH-HEADER <file> missing <field>` | A required field is absent or empty. |
| `E-PATCH-HEADER <file> invalid <field>` | `Upstream-Status` is not one of the four forms, or `Date` cannot be read. |
| `E-PATCH-STALE <file> <days>` | `Pending` for more than the limit, and no `Issue:` link. |
| `W-PATCH-PENDING <file> <days>` | `Pending` and still within the limit, or older with an `Issue:` link. |

The checker runs in the `lint` CI job and as the `check-patches` pre-commit hook on changed `*.patch` files
under a `patches/` directory.
