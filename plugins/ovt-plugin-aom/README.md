<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# ovt-plugin-aom

The AV1 analysis plugin (`libovt_plugin_aom`): libaom's inspection API behind the analysis decoder ABI
(`core/include/ovt/abi/analysis_decoder_v1.h`). Specification: chapter 07 §7.6.1.

- **Upstream:** libaom, pinned in the `libaom` row of `third_party/VERSIONS.md` (v3.12.1) and built by
  `third_party/libaom.cmake` with `CONFIG_INSPECTION=1 CONFIG_ACCOUNTING=1 CONFIG_SVT_AV1=0`, decoder only, static
  and PIC. The plugin includes the semi-public `av1/decoder/inspection.h` and the internal `av1/decoder/decoder.h`
  (for the frame-header values `insp_frame_data` lacks) from that source tree, so it is built only with the bundled
  libaom (`OVT_SYSTEM_LIBAOM=OFF`). libaom decodes on one thread: its accounting is not thread-safe (#360).
- **Licence:** plugin code BSD-2-Clause; the binary also carries libaom (BSD-2-Clause, the ISC `x86inc.asm` on
  x86_64) and the Alliance for Open Media Patent License 1.0 (`PATENTS`, kept with this code).
- **Status:** frame-level `FrameAnalysis` (WI-2.155); the cell grid, the superblock partition tree rebuilt from
  the per-MI block sizes (`src/partition_reconstruct.cpp`), luma TXBs, motion vectors, the eight AV1 reference
  slots, tiles and `Av1FilterParams` (WI-2.156, `src/map_mi.cpp`); the `Reconstruction` planes of shown frames
  (WI-2.158, `src/planes.cpp`); symbol accounting (WI-2.157, `src/map_accounting.cpp`). The extension patches
  with the other plane stages and hidden-frame planes (WI-2.159, `patches/`) follow.
- **Accounting:** libaom's `Accounting` records are copied in the inspection callback and aggregated into one
  `SymbolCost` per `(x4, y4, symbol_id)`, with libaom's dictionary as `symbol_names`. Each record is charged to the
  leaf block owning the cell of its accounting context (as libaom recorded it, even when it was read before the next
  block set its context); records with context −1 or in libaom's padding columns and rows go to the frame root only
  and are not in `symbols` (V-024). `Block.bits_x8` sums a block's records and its children; `grid.bits_x8`
  spreads each leaf's bits over its cells. libaom resets the accounting at each tile group, so a frame with several
  tile groups (`av1-1-b8-22-svc-L1T2` has three) gets the Info note "accounting covers last tile group only" and no
  `grid.bits_x8`.
- **Planes:** decoded pictures live in the plugin's own libaom frame buffers (`FramePool`,
  `aom_codec_set_frame_buffer_functions`), which stay alive after libaom releases them for as long as a pulled
  `FrameAnalysis` names them. `plane()` hands out the samples in place, so `PlaneRef.stride_bytes` is libaom's
  stride, and a pull without the `Reconstruction` stage never touches the samples. Planes are before film grain
  synthesis (`AV1D_SET_SKIP_FILM_GRAIN`; frames with `apply_grain = 1` carry the Info note "film grain present (not
  applied)") and, for super-resolution frames, `UpscaledWidth` wide while the grid covers `FrameWidth`
  (`Av1FilterParams.use_superres`, `FrameWidth`, `UpscaledWidth`).
- **Tests:** `tests/test_aom_planes.cpp` checks the planes against the vectors' `.md5` files and `aomdec
  --skip-film-grain`; `tests/test_aom_map_mi.cpp` compares the plugin's grid with libaom `inspect` layers recorded in
  `tests/data/inspect_reference.json`; regenerate it with `tests/data/make_inspect_reference.py` (usage in the
  script) when the libaom pin moves. `tests/test_aom_accounting.cpp` compares `symbols` with libaom's records and
  those with `inspect -a` output in `tests/data/accounting_reference.json` (`tests/data/make_accounting_reference.py`).
