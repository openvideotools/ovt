# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/check_patches.py (WI-0.607, acceptance tests 1 and 2)."""

from __future__ import annotations

import datetime as dt
import email.utils
import os
import subprocess
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS / "check_patches.py"
SPDX = "SPDX-" + "License-Identifier"  # split so that REUSE does not read it as this file's licence

DIFF = """\
---
 libde265/decctx.h | 1 +
 1 file changed, 1 insertion(+)

diff --git a/libde265/decctx.h b/libde265/decctx.h
--- a/libde265/decctx.h
+++ b/libde265/decctx.h
@@ -1,3 +1,4 @@
 #ifndef DECCTX_H
+// Upstream-Status: not a header line, it is part of the diff
 #define DECCTX_H
"""


def rfc2822(days_ago: int) -> str:
    when = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=days_ago)
    return email.utils.format_datetime(when)


def patch_text(
    status: str | None = "Submitted [https://github.com/strukturag/libde265/pull/1]",
    *,
    date: str | None = None,
    issue: str | None = None,
    drop: tuple[str, ...] = (),
) -> str:
    """A `git format-patch`-style patch; `drop` removes header fields."""
    lines = [
        "From 0123456789abcdef0123456789abcdef01234567 Mon Sep 17 00:00:00 2001",
        "From: A Developer <dev@example.org>",
    ]
    if date is not None:
        lines.append(f"Date: {date}")
    lines += ["Subject: [PATCH] export CTB side data", "", "Read-only accessor for the CTB tree.", ""]
    if status is not None:
        lines.append(f"Upstream-Status: {status}")
    if issue is not None:
        lines.append(f"Issue: {issue}")
    lines += [
        f"{SPDX}: LGPL-3.0-or-later",
        "Rationale: libde265 has no public API for the coding tree.",
    ]
    lines = [ln for ln in lines if not any(ln.startswith(f"{d}:") for d in drop)]
    return "\n".join(lines) + "\n" + DIFF


def run(tmp_path: Path, text: str, *args: str, name: str = "0001-ctb.patch") -> subprocess.CompletedProcess[str]:
    path = tmp_path / "plugins" / "ovt-plugin-de265" / "patches" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    cmd = [sys.executable, "-I", str(SCRIPT), "--root", str(tmp_path), *args]
    return subprocess.run(cmd, capture_output=True, text=True, check=False)


def test_committed_patches_pass() -> None:
    result = subprocess.run([sys.executable, "-I", str(SCRIPT)], capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr


def test_complete_header_passes(tmp_path: Path) -> None:
    result = run(tmp_path, patch_text())
    assert result.returncode == 0, result.stderr
    assert result.stderr == ""


def test_missing_upstream_status_fails_naming_the_patch(tmp_path: Path) -> None:
    # Acceptance test 1. The `Upstream-Status:` inside the diff must not count as a header line.
    result = run(tmp_path, patch_text(status=None))
    assert result.returncode == 1
    assert "E-PATCH-HEADER plugins/ovt-plugin-de265/patches/0001-ctb.patch missing Upstream-Status" in result.stderr


def test_every_required_field_is_checked(tmp_path: Path) -> None:
    for field in ("From", "Subject", "SPDX-License-Identifier", "Rationale"):
        result = run(tmp_path, patch_text(drop=(field,)))
        assert result.returncode == 1, field
        assert f"missing {field}" in result.stderr


def test_empty_field_counts_as_missing(tmp_path: Path) -> None:
    result = run(
        tmp_path, patch_text().replace("Rationale: libde265 has no public API for the coding tree.", "Rationale:")
    )
    assert result.returncode == 1 and "missing Rationale" in result.stderr


def test_unknown_status_is_invalid(tmp_path: Path) -> None:
    for status in ("Maybe", "Submitted", "Accepted []"):
        result = run(tmp_path, patch_text(status=status))
        assert result.returncode == 1, status
        assert "invalid Upstream-Status" in result.stderr


def test_status_forms_accepted(tmp_path: Path) -> None:
    for status in (
        "Submitted [https://example.org/mr/7]",
        "Submitted https://example.org/mr/7",
        "Accepted [1.0.16]",
        "Inappropriate [OVT instrumentation only]",
    ):
        result = run(tmp_path, patch_text(status=status))
        assert result.returncode == 0, (status, result.stderr)


def test_stale_pending_without_issue_fails(tmp_path: Path) -> None:
    # Acceptance test 2, first half.
    result = run(tmp_path, patch_text(status="Pending", date=rfc2822(200)))
    assert result.returncode == 1
    assert "E-PATCH-STALE plugins/ovt-plugin-de265/patches/0001-ctb.patch 200" in result.stderr


def test_stale_pending_with_issue_warns(tmp_path: Path) -> None:
    # Acceptance test 2, second half.
    issue = "https://github.com/openvideotools/ovt/issues/99"
    result = run(tmp_path, patch_text(status="Pending", date=rfc2822(200), issue=issue))
    assert result.returncode == 0, result.stderr
    assert "W-PATCH-PENDING plugins/ovt-plugin-de265/patches/0001-ctb.patch 200" in result.stderr
    assert "E-PATCH" not in result.stderr


def test_issue_number_counts_as_link(tmp_path: Path) -> None:
    result = run(tmp_path, patch_text(status="Pending", date=rfc2822(400), issue="#99"))
    assert result.returncode == 0 and "W-PATCH-PENDING" in result.stderr


def test_issue_without_link_does_not_count(tmp_path: Path) -> None:
    result = run(tmp_path, patch_text(status="Pending", date=rfc2822(400), issue="will file one"))
    assert result.returncode == 1 and "E-PATCH-STALE" in result.stderr


def test_recent_pending_warns(tmp_path: Path) -> None:
    result = run(tmp_path, patch_text(status="Pending", date=rfc2822(10)))
    assert result.returncode == 0
    assert "W-PATCH-PENDING plugins/ovt-plugin-de265/patches/0001-ctb.patch 10" in result.stderr


def test_max_pending_days_option(tmp_path: Path) -> None:
    text = patch_text(status="Pending", date=rfc2822(40))
    assert run(tmp_path, text).returncode == 0
    result = run(tmp_path, text, "--max-pending-days", "30")
    assert result.returncode == 1 and "E-PATCH-STALE" in result.stderr and " 40" in result.stderr


def test_exactly_max_days_is_not_stale(tmp_path: Path) -> None:
    # "more than 180 days" (§15.5.5): day 180 still passes.
    result = run(tmp_path, patch_text(status="Pending", date=rfc2822(180)))
    assert result.returncode == 0, result.stderr


def test_hand_written_iso_date(tmp_path: Path) -> None:
    old = (dt.datetime.now(dt.timezone.utc).date() - dt.timedelta(days=365)).isoformat()
    result = run(tmp_path, patch_text(status="Pending", date=old))
    assert result.returncode == 1 and "E-PATCH-STALE" in result.stderr and " 365" in result.stderr


def test_pending_without_date_fails(tmp_path: Path) -> None:
    result = run(tmp_path, patch_text(status="Pending"))
    assert result.returncode == 1 and "missing Date" in result.stderr


def test_pending_with_bad_date_fails(tmp_path: Path) -> None:
    result = run(tmp_path, patch_text(status="Pending", date="last Tuesday"))
    assert result.returncode == 1 and "invalid Date" in result.stderr


def test_mtime_is_ignored(tmp_path: Path) -> None:
    # A fresh checkout resets mtimes; an old file with a recent Date: is not stale and vice versa.

    path = tmp_path / "plugins" / "ovt-plugin-de265" / "patches" / "0001-ctb.patch"
    run(tmp_path, patch_text(status="Pending", date=rfc2822(5)))
    old = (dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=1000)).timestamp()
    os.utime(path, (old, old))
    result = subprocess.run(
        [sys.executable, "-I", str(SCRIPT), "--root", str(tmp_path)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 0 and "W-PATCH-PENDING" in result.stderr and " 5" in result.stderr


def test_third_party_patches_dir_and_explicit_paths(tmp_path: Path) -> None:
    bad = tmp_path / "third_party" / "patches" / "dav1d" / "0001-x.patch"
    bad.parent.mkdir(parents=True)
    bad.write_text(patch_text(drop=("SPDX-License-Identifier",)), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "-I", str(SCRIPT), "--root", str(tmp_path)], capture_output=True, text=True, check=False
    )
    assert result.returncode == 1
    assert "third_party/patches/dav1d/0001-x.patch missing SPDX-License-Identifier" in result.stderr
    good = tmp_path / "elsewhere.patch"
    good.write_text(patch_text(), encoding="utf-8")
    result = subprocess.run(
        [sys.executable, "-I", str(SCRIPT), "--root", str(tmp_path), str(good)],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr


def test_hand_written_patch_without_format_patch_preamble(tmp_path: Path) -> None:
    text = (
        "From: A Developer <dev@example.org>\nSubject: [PATCH] fix\nUpstream-Status: Accepted [1.0.16]\n"
        f"{SPDX}: LGPL-3.0-or-later\nRationale: needed\n\n"
        "--- a/x.c\n+++ b/x.c\n@@ -1 +1 @@\n-a\n+b\n"
    )
    assert run(tmp_path, text).returncode == 0
