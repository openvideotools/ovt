# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/github/apply_protection.sh (WI-0.605) against a fake `gh`; no network."""

from __future__ import annotations

import json
import shutil
import stat
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "github" / "apply_protection.sh"
WANTED = REPO / "tools" / "github" / "branch_protection.json"

pytestmark = pytest.mark.skipif(shutil.which("jq") is None, reason="jq not installed")

# A stand-in for `gh api`: serves GET from $FAKE_DIR/live.json and records any write in $FAKE_DIR/calls.
FAKE_GH = """#!/usr/bin/env bash
set -eu
args=("$@")
[ "${args[0]}" = "api" ] || exit 9
method=GET
path=""
for ((i = 1; i < ${#args[@]}; i++)); do
  case "${args[$i]}" in
    -X) method="${args[$((i + 1))]}"; i=$((i + 1)) ;;
    --input) input="${args[$((i + 1))]}"; i=$((i + 1)) ;;
    *) path="${args[$i]}" ;;
  esac
done
if [ -f "$FAKE_DIR/fail" ]; then echo "HTTP 500" >&2; exit 1; fi
if [ "$method" != GET ]; then
  echo "$method $path" >>"$FAKE_DIR/calls"
  cp "$input" "$FAKE_DIR/body.json"
  exit 0
fi
case "$path" in
  */rulesets) jq '[.]' "$FAKE_DIR/live.json" | { if [ -f "$FAKE_DIR/empty" ]; then echo '[]'; else cat; fi; } ;;
  */rulesets/*) cat "$FAKE_DIR/live.json" ;;
  *) exit 8 ;;
esac
"""


def live_ruleset() -> dict:
    """The wanted file plus the fields GitHub adds, with the rules in another order."""
    live = json.loads(WANTED.read_text())
    live.update({"id": 7, "node_id": "x", "source": "o/r", "created_at": "t", "_links": {}})
    live["rules"] = list(reversed(live["rules"]))
    return live


@pytest.fixture()
def env(tmp_path: Path):
    fake = tmp_path / "gh"
    fake.write_text(FAKE_GH)
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    (tmp_path / "live.json").write_text(json.dumps(live_ruleset()))
    return {"OVT_GH": str(fake), "FAKE_DIR": str(tmp_path), "PATH": "/usr/bin:/bin:/usr/local/bin"}


def run(env: dict, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(SCRIPT), "--repo", "o/r", *args], env=env, capture_output=True, text=True, check=False)


def set_live(env: dict, live: dict) -> None:
    (Path(env["FAKE_DIR"]) / "live.json").write_text(json.dumps(live))


def test_check_no_drift(env):
    r = run(env, "--check")
    assert r.returncode == 0, r.stdout + r.stderr
    assert "no drift" in r.stdout


def test_check_names_a_changed_setting(env):
    live = live_ruleset()
    for rule in live["rules"]:
        if rule["type"] == "pull_request":
            rule["parameters"]["required_approving_review_count"] = 2
    set_live(env, live)
    r = run(env, "--check")
    assert r.returncode == 1
    assert "drift: rules.pull_request.required_approving_review_count live=2 wanted=0" in r.stdout


def test_check_names_a_removed_rule(env):
    live = live_ruleset()
    live["rules"] = [x for x in live["rules"] if x["type"] != "required_linear_history"]
    set_live(env, live)
    r = run(env, "--check")
    assert r.returncode == 1
    assert "rules.required_linear_history.present" in r.stdout


def test_check_names_an_extra_rule_and_a_bypass_actor(env):
    live = live_ruleset()
    live["rules"].append({"type": "required_signatures"})
    live["bypass_actors"] = [{"actor_id": 1, "actor_type": "RepositoryRole", "bypass_mode": "always"}]
    set_live(env, live)
    r = run(env, "--check")
    assert r.returncode == 1
    assert "rules.required_signatures.present" in r.stdout
    assert "bypass_actors.0.actor_id" in r.stdout


def test_check_without_a_ruleset_is_drift(env):
    (Path(env["FAKE_DIR"]) / "empty").write_text("")
    r = run(env, "--check")
    assert r.returncode == 1
    assert "does not exist" in r.stdout


def test_api_error_exits_1(env):
    (Path(env["FAKE_DIR"]) / "fail").write_text("")
    r = run(env, "--check")
    assert r.returncode == 1
    assert "error:" in r.stderr


def test_apply_updates_the_existing_ruleset(env):
    r = run(env)
    assert r.returncode == 0, r.stderr
    assert (Path(env["FAKE_DIR"]) / "calls").read_text().strip() == "PUT repos/o/r/rulesets/7"
    assert json.loads((Path(env["FAKE_DIR"]) / "body.json").read_text()) == json.loads(WANTED.read_text())


def test_apply_creates_a_missing_ruleset(env):
    (Path(env["FAKE_DIR"]) / "empty").write_text("")
    r = run(env)
    assert r.returncode == 0, r.stderr
    assert (Path(env["FAKE_DIR"]) / "calls").read_text().strip() == "POST repos/o/r/rulesets"


def test_bad_usage_exits_2(env):
    assert run(env, "--nope").returncode == 2
