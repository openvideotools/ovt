# Signing off your commits (DCO)

OVT uses the [Developer Certificate of Origin 1.1](../../DCO) instead of a contributor licence agreement.
Adding a `Signed-off-by:` line to a commit certifies that you wrote the change, or otherwise have the right to
submit it, under the licence named in the SPDX header of each file it touches. There is no copyright
assignment; you keep your copyright.

## How to sign off

Commit with `-s`:

```sh
git commit -s -m "WI-0.123: add the thing"
```

Git appends a trailer with the `user.name` and `user.email` of your git configuration:

```text
Signed-off-by: Jane Doe <jane@example.org>
```

You may use a pseudonym, as long as it is a known identity that you use consistently. The trailer must
match the commit's author email for the DCO check to accept it.

The `dco-signoff` commit-msg hook (installed by `pre-commit install`) stops a commit without the trailer
before it is made.

## When the DCO check fails on a pull request

The DCO check is the GitHub DCO app. It fails when any commit in the pull request has no `Signed-off-by:`
line matching its author. Fix it in one of two ways.

**Sign off the existing commits** (preferred):

```sh
git rebase --signoff origin/main      # adds the trailer to every commit of your branch
git push --force-with-lease
```

For the last commit only, `git commit --amend -s --no-edit` does the same.

**Add a remediation commit**, if you cannot rewrite the branch: an empty commit, signed off by you, whose
message contains one line per unsigned commit of yours:

```text
I, Jane Doe <jane@example.org>, hereby add my Signed-off-by to this commit: 1a2b3c4d…
```

Only the author of a commit can remediate it. A maintainer or anyone else cannot sign off on your behalf
(`.github/dco.yml` turns third-party remediation off), and maintainers sign off their own commits too.
