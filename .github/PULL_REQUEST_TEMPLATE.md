<!-- markdownlint-disable-file MD041 -->
<!-- Title: `WI-<phase>.<nnn>: <imperative title from the chapter>`, or `#<issue>: …` for a fix without a WI. -->

## Goal

<!-- Copy the Goal line of the work item. -->

## Acceptance tests

| # | Given / When / Then | Automated test or manual record |
|---|---|---|
| 1 | | |

## Deviations / assumptions

<!-- Where the code departs from the spec, with the `SPEC-ASSUMPTION(#issue)` comments it adds. Or "none". -->

## Spec questions raised

<!-- Links to `spec-question` issues, or "none". -->

## Checklist

- [ ] I certify the [DCO](https://github.com/openvideotools/ovt/blob/main/DCO): every commit has a
      `Signed-off-by:` line (`git commit -s`; see [docs/dev/dco.md](https://github.com/openvideotools/ovt/blob/main/docs/dev/dco.md)).
- [ ] The PR references a work item (`WI-…`) or an issue (`#…`) and covers only that one.
- [ ] Tests added: unit tests, plus golden and fuzz tests where the change touches a parser, decoder, exporter
      or metric.
- [ ] A `changelog.d/` fragment describes the user-visible change (or the change has none).
- [ ] Docs updated, including the spec chapter if the implementation deviates from it.
- [ ] CI is green.
- [ ] No new dependency without a `third_party/VERSIONS.md` row and a `tools/license_policy.toml` review.

Spec-Version: <!-- from docs/spec/00-front-matter.md -->
