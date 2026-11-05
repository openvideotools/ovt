# CLAUDE.md: how to work in the OVT repository

OVT (Open Video Tools) is a free, cross-platform video analysis suite for H.264, H.265 and AV1. The
specification in `docs/spec/` is the single source of truth. This file tells every agent session how
to read it, how to work, and when a work item is done. Keep this file stable: volatile status lives
in GitHub issues and milestones, not here.

## 1. Reading rule (do not load the whole spec)

The spec is about 3 MB. Never read all of it. Read only this, in this order:

1. `docs/spec/00-backbone.md` (chapter 0): ADRs, repository layout §0.4, schemas, public
   interfaces, cross-cutting rules §0.7 and the binding decisions D1 to D32.
2. `docs/spec/16-roadmap-and-work-items.md` §16.2 (team workflow), and the §16.3 row of your work
   item if one exists.
3. Your work item block (`### WI-<phase>.<nnn>`), in the chapter that owns it. Find it with
   `grep -n "^### WI-0.000" docs/spec/*.md`.
4. Every section your work item names in its **Interface** line, and the backbone sections it cites.
5. `docs/spec/14-testing.md` §14.1.2 (Definition of Done).

Do not start coding until this reading is done. Use `grep` to look up glossary terms (chapter 2)
and requirement ids (chapter 4) instead of reading those chapters whole.

**Precedence when chapters disagree** (from `00-front-matter.md`): chapter 4 wins on what must be
true; chapter 12 on CLI grammar and exit codes; chapter 11 on key bindings and settings; chapter 6
on the FlatBuffers schema; chapter 16 on scheduling; chapter 13 §13.1 on toolchain pins; chapter
14 §14.9.3 on performance budgets. Otherwise the backbone wins until the chapter is fixed.

## 2. Git and pull request rules

This repository is private on GitHub Free, so **there is no branch protection**. The rules below
are enforced by you, not by GitHub.

- **Never push to `main`.** Always work on a branch and open a pull request against `main`.
- **Branch name:** `wi/<phase>.<nnn>-<kebab-slug>` (for example `wi/0.001-cmake-presets`). If your
  session was assigned a branch name, use that one instead and keep the WI id in the PR title.
- **One work item per pull request.** Do not fold a second WI, a drive-by refactor or a pin bump
  into the same PR. A split WI uses letter suffixes (`WI-1.121a`), never new numbers (§16.2.6).
- **PR size:** at most 1 500 changed lines, excluding generated code, goldens and patches. Larger
  work is split per §16.2.6.
- **Commit message:** first line at most 72 characters and starts with the WI id, for example
  `WI-0.001: add CMakePresets.json`. The body says why. Add a `Signed-off-by:` trailer
  (`git commit -s`; DCO, chapter 15 §15.7).
- **PR title:** `WI-<phase>.<nnn>: <imperative title from the chapter>`.
- **PR body** (§16.2.3): Goal (copied from the WI), an Acceptance tests table (automated test name
  or manual record path, one per Given/When/Then line), Deviations / assumptions (or "none"), Spec
  questions raised (issue links), and `Spec-Version: <version from docs/spec/00-front-matter.md>`.
- **Base:** branch from `main`. Never branch from another WI branch unless that dependency is still
  open; rebase onto `main` once it merges, before review.

### Merging

The implementing session never merges its own PR. A PR is merged (squash merge, linear history)
only when **all** of these hold:

1. CI is green: `gh pr checks <number>` (or the GitHub checks API) shows every check passing and
   none pending. Until the CI workflows exist (WI-0.000 and WI-0.030), the author's local run of
   the fast tests stated in the PR body stands in for CI.
2. A **separate reviewer session**, never the session that wrote the code, has reviewed the diff
   against the WI's acceptance tests and Definition of Done and left a review or comment whose first
   line is `Review verdict: approve`.
3. Changes to `CMakePresets.json`, public C headers under `core/include/`, the FlatBuffers schemas
   under `core/schema/`, the plugin ABI, `LICENSES/` or `tools/license_policy.toml` also need
   Karl's (the owner's) explicit approval.

The merge is done by the coordinator or by Karl, after checking the three conditions above.

## 3. When the spec is ambiguous or wrong

Do not guess silently (§16.2.4).

1. Search first: the glossary (chapter 2), the backbone, your chapter's Interface sections and
   chapter 17 §17.2 (known open questions). Many ambiguities are answered elsewhere.
2. If it is still ambiguous, open a GitHub issue labelled `spec-question`, titled
   `spec: <chapter §> — <one-line question>`. Quote the conflicting sentences with file and line,
   and propose one answer with its consequence.
3. Keep working on your proposed answer, unless the ambiguity blocks an interface other WIs consume
   (a public header, a schema, a CLI grammar, a file path). Then stop that part.
4. Record the assumption in the PR body under "Deviations / assumptions" and in the code as
   `// SPEC-ASSUMPTION(#<issue>): …`.

Do not ask spec questions in chat. `spec-question` issues are triaged daily:

- **Low risk and low uncertainty, no public interface change:** the triaging session accepts the
  proposed answer with a comment on the issue. The next spec-amendment PR folds it into
  `docs/spec/` and closes the issue. That PR merges on green CI and a separate reviewer's approve,
  without the owner. The daily digest lists every answer accepted and every spec fix made this way.
- **Uncertain, or changes a public interface** (public C headers under `core/include/`, the plugin
  ABI, the FlatBuffers schemas, the CLI grammar): the issue stays open for the owner. If the owner
  has not answered within 24 hours, the proposed answer stands, except for a public interface
  change, which always waits for the owner.

When in doubt whether a question is low risk, treat it as uncertain.

## 4. Definition of Done (chapter 14 §14.1.2 plus chapter 16 §16.2.2)

A work item is Done only when all of these hold:

1. Unit tests pass on every CI platform (`ctest --preset <platform> -L fast`).
2. Golden tests pass for every module the WI touches (`ctest -L golden`).
3. Every new or modified parser of untrusted bytes has a fuzz target in
   `core/tests/fuzz/CMakeLists.txt` that runs 60 s in CI without a finding.
4. Sanitizer-clean: `linux-x64-asan-ubsan` green; code touching threads also green under
   `linux-x64-tsan`.
5. Docs updated: the `docs/spec/` chapter is amended in the same PR when the implementation
   deviates from it (§16.2.8: bump the spec version and add a `docs/spec/CHANGELOG.md` line).
6. Every Given/When/Then acceptance line is an automated test named in the PR body, or a manual
   record in `docs/acceptance/wi/WI-<phase>.<nnn>.md` (command, output, date, machine).
7. No new warnings under `-Werror` (`ovt::flags`, chapter 13 §13.2.5) and no new clang-tidy
   findings on changed lines.
8. Licence gate green (`tools/license_gate.py`).
9. Every path in the WI's **Produces** line exists.
10. The §16.4 row still describes the WI; a scope change edits the WI block and the §16.4 row in
    the same PR (§16.2.5).

Items that depend on tooling not yet built (fuzzing, sanitizers, licence gate, clang-tidy) apply
from the WI that introduces that tooling onward.

## 5. Build and test commands

The build arrives with WI-0.000 (skeleton) and WI-0.001 (presets). Once they are merged:

```sh
tools/bootstrap.sh --no-qt                      # first time on a machine
cmake --preset linux-x64-debug                  # macOS: macos-arm64-debug
cmake --build --preset linux-x64-debug
ctest --preset pr                               # macOS: pr-macos; or tools/ctest.sh pr
ctest --preset linux-x64-debug -L golden        # when touching a parser, decoder, exporter or metric
```

Preset names are fixed by chapter 13 §13.2.4 (D23). Do not invent new ones.

- **Run only the fast lane locally** (`ctest --preset pr`, plus `-L golden` when relevant). The
  full corpora, long fuzzing, sanitizer matrix and performance runs belong to CI and the Mac Mini
  runner. Do not re-run the whole world after every edit.
- **Determinism:** run your WI's output twice; if the bytes differ, the WI is not Done (§0.7.5).
- Never commit corpus media, goldens over 2 MB uncompressed, or generated FlatBuffers code.
- A third-party pin moves only in its own PR touching `third_party/VERSIONS.md` and the affected
  script (chapter 13 §13.3.7).
- Hardware-bound WIs (§16.5.3, label `needs-hardware:<id>`) run only where that device exists,
  usually the Mac Mini through Remote Control or a device on Karl's desk.

## 6. Working habits

- One session per work item. When the PR is merged, the session ends.
- Do not spawn subagents inside an implementation session. Parallelism comes from separate sessions.
- Do not edit `docs/spec/**` except as rule 5 of the Definition of Done requires. A pure spec change
  is its own PR (§16.2.8 rule 3).
- Never commit secrets, tokens, runner registration tokens or signing material.

## 7. Attribution

End every commit message with the attribution trailers your session provides, after the
`Signed-off-by:` line. Pull request bodies end with the attribution footer your session provides.
