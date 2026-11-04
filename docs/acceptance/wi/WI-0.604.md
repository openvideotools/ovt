# WI-0.604 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, markdownlint-cli2 0.17.1 (markdownlint 0.37.3),
check-jsonschema with the SchemaStore issue-form schemas, reuse 5.0.2. Date: 2026-11-04. Spec version:
1.0.0-rc1. Commit: the head of the WI-0.604 pull request. Spec question: #25.

## Sources

- `DCO`: the text of chapter 15 §15.7.1, which is DCO 1.1 verbatim.
- `CODE_OF_CONDUCT.md`: `content/version/2/1/code_of_conduct.md` of
  github.com/EthicalSource/contributor_covenant (branch `release`), with the Hugo front matter removed and
  `[INSERT CONTACT METHOD]` replaced by `conduct@ovt.dev` (#25). The first line is a
  `markdownlint-disable MD013` comment, so the upstream paragraphs keep their single-line form.
- `LICENSES/CC-BY-4.0.txt`: SPDX License List 3.29.0 `text/CC-BY-4.0.txt`, equal to the `licenseText` field
  of `json/details/CC-BY-4.0.json`.

## Test 1: an unsigned commit fails the DCO check and the message links `docs/dev/dco.md`

**Pending (#25).** The DCO app is not installed on the repository yet; Karl installs it. Its failure text is
fixed by the app and cannot link a project page. The link to `docs/dev/dco.md` is therefore in the local
hook message and the PR template:

```console
$ printf 'WI-0.604: test\n' > /tmp/msg && pre-commit run --hook-stage commit-msg --commit-msg-filename /tmp/msg dco-signoff
DCO Signed-off-by trailer (missing? commit with `git commit -s`; see docs/dev/dco.md)...Failed
```

## Test 2: `markdownlint` on the governance documents exits 0

```console
$ npx -y markdownlint-cli2@0.17.1 CONTRIBUTING.md SECURITY.md CODE_OF_CONDUCT.md \
    .github/PULL_REQUEST_TEMPLATE.md docs/dev/dco.md docs/dev/getting-started.md
Linting: 6 file(s)
Summary: 0 error(s)
```

The issue forms validate against the SchemaStore `github-issue-forms` and `github-issue-config` schemas
(`check-jsonschema … ok -- validation done`).

## Test 3: a required check named exactly `DCO`

**Pending (#25).** The repository is private on GitHub Free, which has no branch protection, so it has no
required checks. The check becomes required when the repository goes public (WI-0.605 scripts it).
