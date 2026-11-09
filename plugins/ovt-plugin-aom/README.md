<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# ovt-plugin-aom

The AV1 analysis plugin (`libovt_plugin_aom`): libaom's inspection API behind the analysis decoder ABI
(`core/include/ovt/abi/analysis_decoder_v1.h`). Specification: chapter 07 §7.6.1.

- **Upstream:** libaom, pinned in the `libaom` row of `third_party/VERSIONS.md` (v3.12.1) and built by
  `third_party/libaom.cmake` with `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1 CONFIG_SVT_AV1=0`, decoder only, static
  and PIC. The plugin includes the semi-public `av1/decoder/inspection.h` from that source tree, so it is built only
  with the bundled libaom (`OVT_SYSTEM_LIBAOM=OFF`).
- **Licence:** plugin code BSD-2-Clause; the binary also carries libaom (BSD-2-Clause, the ISC `x86inc.asm` on
  x86_64) and the Alliance for Open Media Patent License 1.0 (`PATENTS`, kept with this code).
- **Status:** WI-2.155 skeleton: frame-level `FrameAnalysis` (frame type, shown, `base_q_idx`, sizes) and the
  `Reconstruction` planes of shown frames. WI-2.156a adds the superblock partition-tree reconstruction from the
  per-MI block sizes (`src/partition_reconstruct.cpp`, not wired into the plugin yet); the rest of the block data
  (WI-2.156b), symbol accounting (WI-2.157) and the extension patches (WI-2.159, `patches/`) follow.
- **Tests:** `tests/test_aom_map_mi.cpp` checks the reconstruction against libaom `inspect` layers recorded in
  `tests/data/inspect_reference.json`; regenerate it with `tests/data/make_inspect_reference.py` (usage in the
  script) when the libaom pin moves.
