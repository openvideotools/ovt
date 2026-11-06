# Open Video Tools (OVT) — Requirements and Architecture Specification

| | |
|---|---|
| **Status** | Release candidate for owner review |
| **Version** | 1.0-rc5 (2026-11-06) |
| **Audience** | The implementation team (coding agents and their human reviewers), the project owner, and future contributors |
| **Scope** | A free and open-source, cross-platform (macOS, Linux, Android/ChromeOS) video analysis suite for H.264/AVC, H.265/HEVC and AV1 with HDR (PQ/HLG) display, bitstream inspection, container inspection, block-level visualisation, per-plane views and video quality metrics |
| **Licence of this document** | CC-BY-4.0 (owner decision of 2026-11-04, issue #20; the rest of `docs/` stays CC-BY-SA-4.0, chapter 15 §15.1) |

## How to use this document

This specification is the single source of truth for building OVT. It is organised so that an implementer reads the general chapters once and then works from the chapter that owns their module and from the work item index in chapter 16.

1. **Everyone** reads chapter 1 (Introduction), chapter 2 (Glossary, as a reference) and chapter 5 (Architecture) before writing code.
2. **Module implementers** read their owning chapter (6–15) in full. Each owning chapter contains the interfaces (as code), the algorithms, the test oracles and the work items (WI) for that module.
3. **Pick work** from chapter 16, which lists every work item with its resolved dependencies, phase and milestone, and resolves the places where two chapters specified overlapping deliverables.
4. **The backbone** (chapter 0, reprinted after chapter 17 in the assembled document — it was written first and every chapter was authored against it; its sections are `0.n`) is the normative summary every chapter conforms to: architecture decisions, repository layout, core schema excerpts and public interface signatures. When a chapter and the backbone disagree, the chapter owner fixes the chapter; until then the backbone wins.
5. **Appendices** hold the commercial feature inventory, the library survey, specification cross-references, the schema listing and the URL index.

Chapter 17 lists the open questions the owner still has to answer; each has a default the specification assumes.

**Precedence when chapters disagree.** Chapter 4 wins on *what must be true* (requirement ids are defined only there); chapter 12 wins on CLI grammar and exit codes; chapter 11 wins on key bindings and the settings file; chapter 6 wins on the FlatBuffers schema; chapter 16 wins on scheduling (which WI delivers what, when); chapter 14 §14.9.3 and `tests/perf/budgets.json` hold the performance budgets (D19; chapter 0 §0.7.4 mirrors the headline numbers) and chapter 13 §13.1 the toolchain pins; chapter 14 §14.3 is the fixture registry (D26) and chapter 14 §14.9.2 the reference-hardware table (D29). Section references use the form "chapter N §N.n.m".

### Chapter ownership of identifiers

| Identifier | Defined in |
|---|---|
| `FR-<AREA>-nnn`, `NFR-nnn` | chapter 4 |
| `ADR-nnn` | chapter 5 (summary in chapter 0 §0.2) |
| `D<n>` cross-chapter decisions | chapter 0 §0.11 (D1–D32), cited by chapters |
| Diagnostic-code format `<FAMILY>(-<QUALIFIER>)?-<nnnn>` and the family list | chapter 5 §5.7 (D2); each family's registry: `CNT` chapter 8 §8.5, `SYN`/`DEC`/`IO`/`BR` chapter 7, `VQ` chapter 9, `MDL`/`VER`/`ANA` chapters 6/7, `RND` chapter 10, `GUI` chapter 11, `CLI` chapter 12 §12.1.3 |
| FR → WI traceability (and its inverse) | chapter 4 §4.17; chapter 16 §16.4 `FRs` column |
| Exit codes, `--format`, subcommand list | chapter 12 §12.1.3, §12.1.2, §12.2.0 |
| Keyboard map, settings file | chapter 11 §11.4.3, §11.2.5 |
| Performance budgets and version pins | chapter 14 §14.9.3 / `tests/perf/budgets.json` (D19; chapter 0 §0.7.4 and D5 mirror the headline numbers), chapter 13 §13.1 |
| Reference hardware ids, fixture registry (corpus ids `<source>_<name>`) | chapter 14 §14.9.2, §14.3 (D26, D29) |
| Specification version and changelog | this front matter, `docs/spec/CHANGELOG.md` (chapter 16 §16.2.8, D28) |
| FlatBuffers schemas, `V-nnn` validation rules | chapter 6 |
| `SYN-*`, `DEC-*`, `IO-*`, `BR-*` diagnostics, plugin ABI | chapter 7 |
| `CNT-nnnn` diagnostics | chapter 8 |
| Metric ids, `NormalizationConfig` | chapter 9 |
| Overlay colours and layouts | chapter 10 |
| Panels, shortcuts, `SelectionModel` | chapter 11 |
| `ovt` subcommands, C API, Python API | chapter 12 |
| CMake targets, presets, `third_party/VERSIONS.md` | chapter 13 |
| Definition of done, corpora, oracles | chapter 14 |
| `WI-<phase>.nnn` (defined in chapters 6–15, indexed in 16) | chapter 16 |
| `R-nn` risks, `Qn` open questions, `OI-nn` open items | chapter 17 (§17.1, §17.2, §17.4) |

### Revision history

| Version | Change |
|---|---|
| 0.9 | Draft v1 assembled from the chapter drafts and the backbone. |
| 0.9.2 | Review round 1 (6 reviewers, about 1 000 findings) applied; decisions D1–D18. |
| 0.9.3 | Review round 2 (4 reviewers, about 170 findings) applied; decisions D19–D32. |
| 1.0-rc1 (2026-11-04) | Final consistency pass (cross-chapter requests, dependency table, fixture registry, open items); release candidate for owner review. |
| 1.0-rc2 (2026-11-04) | Accepted answers of the M0.1 `spec-question` issues #2, #3, #4, #6, #8, #11, #13, #15, #17, #20, #22, #25 and #27 folded into chapters 12–16 (see `CHANGELOG.md`). |
| 1.0-rc3 (2026-11-06) | Accepted answers of `spec-question` #119 (synthetic generator reproducibility, chapter 14 §14.3.3) and #123 (licence gate `--corpora` mode, chapter 14 §14.3.4) folded in (see `CHANGELOG.md`). |
| 1.0-rc4 (2026-11-06) | WI-0.552 corpora manifest: `vcs.rev`, `upstream_path`, `note` fields; url-less `unverified` entries for names absent at their origin (see `CHANGELOG.md`). |
| 1.0-rc5 (2026-11-06) | CI build artefacts are uploaded only while the repository is private (chapter 13 §13.7.1; see `CHANGELOG.md`). |
