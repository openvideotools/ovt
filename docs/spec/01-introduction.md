# 1. Introduction

## 1.1 What this document is

This is the Requirements and Architecture Specification for **Open Video Tools (OVT)**, a free and open-source, cross-platform video analysis suite. It is written to be the single source of truth for an implementation team made up of coding agents, some of which may be running on less capable models. Every chapter therefore follows the same discipline:

- Requirements are single testable sentences with an acceptance test.
- Architecture decisions are recorded with the alternatives that were rejected and why.
- Interfaces are given as code (C, C++20, FlatBuffers schema, JSON Schema, command-line grammar), never only as prose.
- Work items are small (at most one week), name the exact files they produce, and carry Given/When/Then acceptance tests.
- Every domain term is defined in the Glossary (chapter 2) or cited to a specification section.

If you are an implementer: read chapters 1, 2 and 5 first, then the chapter for the module you are assigned, then the work item you are assigned in chapter 16. Do not start a work item whose dependencies are not marked done.

## 1.2 Why OVT exists

Codec engineers, video quality teams and video platform teams rely on commercial stream analyzers, for example ViCueSoft VQ Analyzer and the Elecard StreamEye Studio suite. Tools of this class show, for every frame of an encoded stream, the syntax elements the encoder wrote, the block partitioning and prediction decisions, motion vectors, quantisation, bit allocation, in-loop filter decisions, the decoded planes at each processing stage, container structure, and quality metrics against a reference. They are licensed per seat at commercial prices, and from their public product information we know of none that runs on Android or ChromeOS or that exposes a documented scripting API beyond command-line flags.

Product names in this specification are trademarks of their respective owners and are used only to identify the class of tool and for factual feature comparison (chapter 3). OVT is an independent project: it is not affiliated with, endorsed by or compatible with any of these products, and it does not copy their code, user interfaces, artwork or documentation.

The free and open-source world has pieces but no suite: YUView (GPL-3) is an excellent raw viewer with statistics overlays but has no syntax tree, no hex view, no AV1 block data and no mobile build; aomanalyzer (MIT) shows AV1 blocks and bit accounting but is dormant and AV1-only; hevcesbrowser shows an HEVC syntax tree with hex but does not decode; FFmpeg exports coarse motion vectors and per-block QP for H.264 only; libvmaf computes metrics but has no viewer. Chapter 3 documents the commercial feature set in detail and chapter 15 the licensing landscape.

OVT closes that gap with one architecture: a core library that produces every piece of analysis data as a versioned, documented record; instrumented upstream decoders behind a stable plugin interface; a GPU renderer that displays SDR and HDR (PQ and HLG) content correctly on macOS, Linux and Android; and three front-ends over the same core: a command-line tool, a Qt Quick desktop/tablet application, and a Python module.

## 1.3 Goals

| # | Goal | How success is measured |
|---|---|---|
| G1 | Cover every feature area marked "must" in chapter 3 §3.3 for H.264/AVC, H.265/HEVC and AV1. | Every row marked "must" in chapter 3 §3.3 is implemented and covered by an acceptance test by the end of Phase 4. |
| G2 | Run as a first-class application on macOS, Linux and Android tablets / Chromebooks. | Each phase demo in chapter 16 passes on an M1 MacBook, an Ubuntu 24.04 LTS desktop (`linux-x64-ref`; Ubuntu 22.04 LTS or later is supported, 24.04 is the reference), a Pixel Tablet (SDR) and an HDR-capable Android tablet or Chromebook (`android-hdr`/`chromebook-hdr`, chapter 14 §14.9.2). |
| G3 | Display HDR content correctly. | PQ and HLG streams are shown with correct luminance on an EDR display (macOS) and a PQ-capable Android panel, tone-mapped on SDR displays, with MaxCLL/MaxFALL measurements within 1 nit of a reference computation (chapter 10). |
| G4 | Prefer upstream libraries; stay maintainable. | No dependency is a long-lived fork. Every third-party patch is ≤ 500 changed lines (≤ 2 000 in total per decoder), behind a build flag, changes no decoding result, with an upstream submission recorded (chapter 7). Dependency updates are a version bump in `third_party/VERSIONS.md`. |
| G5 | Make every analysis scriptable and reproducible. | Every GUI view has a CLI subcommand producing byte-identical FlatBuffers/JSON for identical inputs (chapter 12); results carry the exact configuration that produced them. |
| G6 | Extensible metrics. | A new metric (including an HDR metric) is added by implementing one descriptor and one `score` function, with no change to core code (chapter 9). |
| G7 | Serve codec researchers, video quality teams and platform video teams. | Phase ordering (chapter 16) delivers the "what is in this file" workflow first, block-level truth second, quality measurement third. |

## 1.4 Non-goals (for this specification)

- Windows and iOS builds, and web browser builds. The architecture does not preclude them; no work item targets them.
- Encoding, transcoding or editing of video (except re-muxing/extraction helpers in chapter 12).
- Real-time monitoring of live network streams (live stream monitoring tools). File-based analysis only.
- Audio analysis beyond listing audio tracks and their container metadata.
- Dolby Vision display-side mapping ("true DV" rendering). OVT parses and shows RPU metadata and converts profile 5/8 to PQ via libplacebo, and labels it as such.
- Reimplementing decoders. OVT instruments existing decoders; it never writes a pixel-reconstructing decoder.
- Being a conformance-certification tool. OVT reports conformance diagnostics it can derive (decoded picture hash, level limits, syntax violations) but does not claim completeness.

## 1.5 Personas

| Persona | Typical questions | Primary chapters |
|---|---|---|
| **Codec engineer** (encoder/decoder developer, researcher) | Why did the encoder choose this partition? What are the MVs and references of this CU? How many bits did this superblock cost? Where does my decoder mismatch the reference? What does the SPS say exactly? | 7, 6, 11 (Selection Info, Block Samples), 12 (`ovt analyze`, `ovt verify`) |
| **Video quality engineer** | What is the VMAF/PSNR/SSIM of this encode against the source, per frame and per region? Are the two streams aligned? What is the BD-rate between two encoder versions? Does this UGC clip look bad (UVQ)? | 9, 11 (Metrics panel), 12 (`ovt metrics`, `ovt bdrate`) |
| **Platform video engineer** (social media / streaming) | What exactly did the packager emit? Is the HDR metadata correct? Are fragments and edit lists consistent? Does this file play on this device (a device-compatibility check is **future** work, chapter 17 §17.4 OI-16; phases 0–4 answer it only indirectly through profile/level, HDR signalling and codec-string checks)? Why is this frame so large? | 8, 10, 11 (Container tree, Timeline, HDR panel), 12 (`ovt probe`, `ovt hdr`) |
| **Tool developer / integrator** | How do I add a metric, a decoder plugin, a container format? How do I call OVT from Python or CI? | 5, 6, 7 §6, 9 §2, 12 |

## 1.6 Owner decisions that shape this specification

The project owner made the following decisions before this specification was written; they are binding and are restated here so no chapter re-litigates them.

1. No constraint on language or UI toolkit, but the system must be modular and extensible and work with the libraries it adopts (for example libaom).
2. GPL licensing is acceptable and probably necessary. AGPL is forbidden because of its network clause. Functionality outranks licence encumbrance.
3. Windows and iOS are not targets. macOS is the first development platform; an Android tablet / Chromebook application is developed concurrently.
4. A library-plus-CLI core with a GUI on top is acceptable; optimise for usefulness, extensibility and long-term maintainability.
5. Starter metrics are PSNR, SSIM, VMAF and UVQ; the metric system must be extensible for future metrics including HDR metrics.
6. Prefer upstream APIs; the project should be low-maintenance and not brittle.
7. Order the roadmap as a codec engineer or a member of a social-media video team would find useful.
8. Deliver Markdown first; the owner creates the repository.
9. The implementation team may use a less capable model; the specification must be written accordingly.

## 1.7 Document map

| Chapter | Title | Read it if you need to… |
|---|---|---|
| 0 | Architecture backbone | see the fixed decisions, repository layout, schema excerpts and interface signatures every chapter conforms to (sections `0.n`) |
| 1 | Introduction | understand goals, personas and how to read the spec |
| 2 | Glossary | look up any term |
| 3 | Competitive reference | know what the commercial tools do and which UX conventions OVT adopts |
| 4 | Requirements | find every FR/NFR with its acceptance test, and the FR → work item traceability table (§4.17) |
| 5 | Architecture | understand layers, data flow, threading, and the 16 architecture decisions |
| 6 | Data model | read or write any analysis record (FlatBuffers schemas, cache, exports) |
| 7 | Bitstream parsing, indexing and decoders | implement parsers, the stream index, the plugin host or a decoder plugin |
| 8 | Container inspection and demuxing | implement the ISOBMFF reader, other containers, or the demux wrapper |
| 9 | Video quality metrics | implement normalisation, kernels, the libvmaf or UVQ host, BD-rate, exports |
| 10 | Rendering, colour management and HDR | implement the video surface, overlays, HDR output or HDR measurements |
| 11 | GUI specification | implement any panel or interaction of `ovt-studio` |
| 12 | CLI, C API and Python API | implement or use the `ovt` command, the C API or the Python module |
| 13 | Platforms, build, CI and distribution | set up the repository, build any target, package or release |
| 14 | Testing | write or run tests, fetch corpora, run oracles, fuzz, benchmark |
| 15 | Licensing, patents and governance | choose a licence header, add a dependency, understand the decoder plugin policy |
| 16 | Roadmap and work items | pick up work; see phase exit criteria and dependencies |
| 17 | Risks and open questions | know what might go wrong and which decisions the owner still has to make |
| A–F | Appendices | commercial feature inventory, library survey, spec cross-references, schema listing, URL index |

## 1.8 Conventions used in this document

- **MUST / MUST NOT / SHOULD / MAY** follow RFC 2119.
- Identifiers: `FR-<AREA>-<nnn>` functional requirement, `NFR-<nnn>` non-functional requirement, `ADR-<nnn>` architecture decision, `WI-<phase>.<nnn>` work item, `V-<nnn>` data validation rule, `D<n>` cross-chapter decision (chapter 0 §0.11), `OI-nn` open item; diagnostic codes are `<FAMILY>(-<QUALIFIER>)?-<nnnn>` with the families `CNT`, `SYN`, `DEC`, `VQ`, `MDL`, `IO`, `BR`, `VER`, `ANA`, `RND`, `GUI`, `CLI` (chapter 5 §5.7), e.g. `CNT-0040`, `SYN-H265-0003`, `VQ-ALIGN-0001`.
- Section references are written "chapter N §N.n.m"; `§0.n` refers to the architecture backbone (chapter 0). Where chapters disagree: chapter 4 wins on what must be true, chapter 12 on CLI grammar and exit codes, chapter 11 on key bindings and settings, chapter 6 on the schema, chapter 16 on scheduling.
- Spec citations: `H.264 §7.3.2.1` means ITU-T Rec. H.264 (08/2021) clause 7.3.2.1; `H.265 §8.3.1` means ITU-T Rec. H.265 (09/2023); `AV1 §5.9.2` means the AV1 Bitstream & Decoding Process Specification v1.0.0 with Errata 1; `14496-12 §8.6.1.2` means ISO/IEC 14496-12 (ISOBMFF). H.264, H.265 and AV1 specifications are freely downloadable; ISO/IEC 14496-12 is a publicly available standard; 14496-15, 23001-7 and 23000-19 are paid standards whose box syntax is mirrored in the public MPEG FileFormatConformance data (chapter 8).
- Codec terminology: H.264 *macroblock*, HEVC *CTU/CU/PU/TU*, AV1 *superblock/block/transform block*. The codec-neutral terms are **block** and **cell** (a 4×4 luma square).
- Units: luma samples in coded-picture coordinates; motion vectors in 1/16 luma sample units in the neutral model; bits from the start of the owning unit; bytes from the start of the file.
- Component names (`libovt-core`, `libovt-render`, `ovt`, `ovt-studio`, `ovt-plugin-*`) are placeholders the owner may rename; they are used consistently throughout.
