# ADR-CNT-MXF: MXF is opened through libavformat only; own KLV reader deferred

Status: **accepted for phase 1 to 4, with an open question for the owner** (chapter 0 §0.2 "Open question" rule).
Work item: WI-4.211 (chapter 8 §8.10). Related: ADR-006 (container inspection), ADR-009 (two decode paths),
chapter 8 §8.6.7 (the design of the later reader, fixed now so the data model does not change).

## Context

MXF (SMPTE ST 377-1, KLV-coded) is the broadcast and post-production wrapper: XDCAM, AS-11, IMF track files,
AVC-Intra and XAVC OP1a/OP-Atom. OVT's own readers (Role A) give a field-level tree with byte spans; libavformat
(Role B) gives packets for playback and analysis but no structure. An own MXF reader needs a KLV walk (16-byte
universal label keys, BER lengths), partition packs, the primer pack and local-tag header metadata sets
(preface, content storage, material and source packages, descriptors), index table segments and the essence
container mapping to a `SampleTable`. Chapter 8 §8.6.7 sizes it at 3 to 4 weeks.

No broadcast-persona requirement is scheduled before phase 4, and no current requirement needs MXF structure
beyond "open and play".

## Decision

1. `.mxf` files open through **libavformat only** (Role B, `Demuxer` hint `"mxf"`, chapter 8 §8.7.2 rule 5).
2. The sniffer (§8.6.1 step 6) reports `mxf` for a KLV key at offset 0 or a partition pack key inside the
   run-in (first 64 KiB).
3. `core/src/container/mxf_placeholder.cpp` builds a **placeholder** `ContainerTree`: `format == "mxf"`, one
   root `BoxNode` spanning the file with `description = "MXF structure view not implemented (phase 4+)"`, and one
   `TrackInfo` per libavformat stream (handler, `codec`, timescale and duration from the stream time base,
   width and height or channels and sample rate, `avstream_index`). `ovt_container_open` fails with the demuxer's
   status when libavformat rejects the file.
4. The analysis path uses Role B packets for MXF (§8.1.1 rule 4).
5. **No KLV reader is written** until the owner answers the open question below. `core/src/container/mxf/`
   stays absent.

## Why

libavformat's `mxf` demuxer already demuxes the essence OVT analyses (MPEG-2, AVC, HEVC), so the codec
analysis features work on MXF today at no cost. The structure view is the only thing
missing, and its users (broadcast QC) are not a phase 1 to 4 persona. Spending 3 to 4 weeks on it now would
delay the H.264/H.265/AV1 analysis milestones.

## Alternatives considered

- **Own KLV reader now** (as chapter 8 §8.6.7 describes): rejected for now, see Why. The design stays fixed in
  §8.6.7 so the `ContainerTree` model does not change when it is built.
- **Link bmx / libMXF / libMXF++** as the inspection backend: rejected as a dependency for the same reason
  ADR-006 rejects libgpac (no byte-span tree to reuse; OVT would still map every set to `BoxNode`s). bmx stays a
  candidate **test oracle** (`mxf2raw --info --all`).
- **libavformat metadata only, no placeholder**: rejected; the GUI and CLI need a tree root that says why there
  is no structure, instead of an empty tree that looks like a parse failure.

## Reference material

- bmx: <https://github.com/bbc/bmx> (BBC; checked at commit `278a9027580ea5f4a27d5d5040d5dc208c6ef538`,
  2025-09-29). Licence **BSD-3-Clause** (`COPYING`). Tools of interest: `mxf2raw --info --all` (oracle),
  `bmxparse`, `raw2bmx`. Chapter 8 §8.6.7 and the WI-4.211 block say Apache-2.0; that is wrong and is raised as
  spec question #429.
- FFmpeg `libavformat/mxfdec.c` (LGPL-2.1-or-later): the Role B demuxer used today, and the second oracle.
- SMPTE ST 377-1 (MXF file format), ST 379-1/2 (generic container), ST 378 (OP1a), RP 210 / RP 224 registers.

## Open question for the owner

**Should OVT build its own MXF KLV reader, and if so, from scratch or informed by bmx?**

| Option | What it means | Effort | Licence notes |
|---|---|---|---|
| A. Keep deferring (recommended until a broadcast user asks) | Placeholder stays; MXF plays and analyses through libavformat | none | none |
| B. Own reader, written from SMPTE ST 377-1 | KLV walk, partitions, primer, header sets, index tables → `SampleTable`; bmx and FFmpeg as oracles only | 3 to 4 weeks | none beyond the standards' availability |
| C. Own reader informed by bmx | Same output as B, using bmx's set and label tables as a reference (BSD-3-Clause allows reuse with notice) | about 3 weeks | BSD-3-Clause notice in `LICENSES/` if any table is copied |

Trigger to reopen: a broadcast or post-production persona requirement, or user demand (chapter 16 deferred-features
table, row "MXF own reader", trigger "Broadcast persona demand").
