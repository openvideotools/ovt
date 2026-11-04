# Open Video Tools: specification (1.0-rc1, 2026-11-04)

This folder holds the Requirements and Architecture Specification for Open Video Tools (OVT), one file per chapter. It is the single source of truth for building OVT. Agents follow the reading rule in the repository's `CLAUDE.md` and never load the whole specification.

## Reading order

1. `00-front-matter.md` and `01-introduction.md` explain the goals and how to read the rest.
2. `00-backbone.md` (chapter 0) holds the architecture decisions, repository layout, core schemas, public interfaces and the binding editor decisions D1 to D32.
3. `16-roadmap-and-work-items.md` is where implementers pick up work. It lists all 380 work items with dependencies, milestones and the requirements each one realises.
4. `17-risks-and-open-questions.md` lists the open questions for the owner (Q1 to Q16). Each question has a default the specification already assumes.

## Chapters

| File | Chapter |
|---|---|
| `00-front-matter.md` | Front matter, how to use the document, precedence rules |
| `00-backbone.md` | 0. Backbone (normative summary) |
| `01-introduction.md` | 1. Introduction |
| `02-glossary.md` | 2. Glossary |
| `03-competitive-reference.md` | 3. Competitive reference |
| `04-requirements.md` | 4. Requirements (FR/NFR ids) |
| `05-architecture.md` | 5. Architecture |
| `06-data-model.md` | 6. Data model and FlatBuffers schema |
| `07-bitstream-and-decoders.md` | 7. Bitstream parsing and decoders |
| `08-container.md` | 8. Container analysis |
| `09-metrics.md` | 9. Video quality metrics |
| `10-rendering-and-hdr.md` | 10. Rendering and HDR |
| `11-gui.md` | 11. GUI |
| `12-cli-and-api.md` | 12. CLI and API |
| `13-platforms-build-ci.md` | 13. Platforms, build and CI |
| `14-testing.md` | 14. Testing |
| `15-licensing-and-governance.md` | 15. Licensing and governance |
| `16-roadmap-and-work-items.md` | 16. Roadmap and work items |
| `17-risks-and-open-questions.md` | 17. Risks and open questions |
| `90-appendices.md` | Appendices |

## Size at a glance

| Item | Count |
|---|---|
| Functional requirements | 264 |
| Non-functional requirements | 36 |
| Work items | 380 |
| Glossary entries | about 600 |
| Phases | 0 Foundations, 1 See the stream, 2 Look inside blocks, 3 Measure quality, 4 Deepen and extend, plus the Android/ChromeOS track |

## Changing the specification

Spec changes follow chapter 16 §16.2.8: bump the version in `00-front-matter.md` and add a line to `CHANGELOG.md` in the same PR.
