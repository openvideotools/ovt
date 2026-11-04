# WI-0.608 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, Python 3.13, pytest 8.3.4, markdownlint-cli2 0.17.1
(markdownlint 0.37.3), AppStream 1.0.2. Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the
WI-0.608 pull request.

## Test 1: `markdownlint` on `docs/licensing.md`

The repository has no markdownlint settings yet, so this WI adds `.markdownlint.yaml` with the 120-column
limit the code uses (tables and code blocks exempt). Without it the default 80-column rule fails every
prose line of the file.

```console
$ npx -y markdownlint-cli2@0.17.1 docs/licensing.md
markdownlint-cli2 v0.17.1 (markdownlint v0.37.3)
Finding: docs/licensing.md
Linting: 1 file(s)
Summary: 0 error(s)
$ echo $?
0
```

The section "Plugins and the GPL" has one paragraph per channel of §15.8, each naming the channel id in
brackets: `play`, `fdroid`, `flathub`, `homebrew`, `pypi`, `github`.

## Test 2: `pytest tools/tests/test_listing_texts.py`

```console
$ pytest -q tools/tests/test_listing_texts.py
........................                                                 [100%]
24 passed in 0.15s
```

## Test 3: generated metainfo validates with `project_license` GPL-3.0-or-later

```console
$ python3 -I tools/release/gen_metainfo.py --out /tmp/meta.xml --version 1.0.0 --date 2026-11-04
$ appstreamcli validate --no-net /tmp/meta.xml

✔ Validation was successful: pedantic: 1
$ grep project_license /tmp/meta.xml
  <project_license>GPL-3.0-or-later</project_license>
```

The pedantic hint is `cid-contains-uppercase-letter`: the id `org.openvideotools.Studio` is fixed by chapter 0
D5 and Flathub accepts it. `test_metainfo_project_license` repeats this check and also checks that two runs
give the same bytes.
