# 6. Data model

> **Status:** normative. This chapter owns the three FlatBuffers schema files (`core/schema/ovt_model.fbs`, `core/schema/ovt_container.fbs`, `core/schema/ovt_metrics.fbs`), the coordinate and unit rules, the in-memory plane store, the on-disk cache format, the export formats and the validation rules. Every other chapter MUST use the names defined here exactly. Where this chapter repeats text from the backbone (§0.5.1–§0.5.7) that text is verbatim; additions are marked `// --- extension (ch. 6) ---` inside the schemas.
>
> **Reader:** an implementer who knows C++ and has read the Glossary, but who may never have opened a codec specification. Every codec term below is either defined in the Glossary (`G:`) or cited to a spec section (`H.264 §7.4.5`, `H.265 §7.3.8.5`, `AV1 §5.11.5`).
>
> **Sources:** chapter 0 §0.5/§0.7; Appendix D (per-decoder field inventories behind the unified model); Appendix F.5 (YUView statistics model and CSV format).

---

## 6.1 Principles

| # | Principle | Consequence for implementers |
|---|---|---|
| P1 | **Codec-neutral core + lossless per-codec extensions.** Every view that does not need codec knowledge (block grid, MV arrows, QP map, bits heat-map, reference graph, statistics) reads only the neutral tables (`CellGrid`, `Block`, `MotionVector`, `RefSlot`, `PartitionInfo`). Everything the decoder read for a block is additionally stored, with its exact spec name, in a codec extension table (`AvcMb`, `HevcCu`, `Av1Block`, …) nested in `Block.ext`. | A renderer MUST compile without including any codec header. A plugin MUST NOT drop a syntax element because it has no neutral slot; it goes in the extension table. |
| P2 | **Structure-of-arrays 4×4 cell grid.** The smallest addressable analysis unit is the *cell*: a 4×4 luma-sample square in coded-picture coordinates (H.264 MV/intra granularity, HEVC minimum TB/PB, AV1 MI unit are all 4×4). Per-cell attributes are stored as parallel vectors (`CellGrid.qp[i]`, `CellGrid.mv0_x[i]`, …) indexed `i = y4 * cells_w + x4`. | Overlays are built with one linear pass; GPU upload is a `memcpy` per attribute; zstd compresses the vectors 5–20×. Never store an array-of-structs per cell. |
| P3 | **FlatBuffers everywhere (ADR-010).** The same bytes are used in memory, across the plugin ABI, in the disk cache and in CLI output. JSON and YUView CSV are *exports* generated from the FlatBuffer, never a second source of truth. | Every buffer crossing a trust boundary (plugin → host, disk → host, network → host) MUST pass `flatbuffers::Verifier` before any field is read. |
| P4 | **Determinism (§0.7.5).** Identical input bytes + identical plugin id/version → byte-identical FlatBuffers. No pointers, timestamps, hostnames, absolute paths or hash-map iteration order inside any record. Vectors are written in the canonical orders defined in §6.3.7. | Tests compare SHA-256 of buffers. Builders MUST use `ForceDefaults(false)` (the FlatBuffers default) so scalars equal to their default are not serialised; this is part of the byte-identity contract. |
| P5 | **Smallest adequate scalar.** `ubyte` for codes < 256, `short` for MVs (1/16-pel of ±2048 luma samples covers every legal H.264 and AV1 MV; HEVC MVs are nominally ±2^15 quarter-samples and are clamped, see chapter 6 §6.2.3), `uint` for ids/offsets within a frame, `ulong` only for file offsets. | Do not "future-proof" with wider types; widen with a schema change if a codec ever needs it. |
| P6 | **Sentinels, not optionals.** FlatBuffers scalar fields are always present with a default. The default *is* the "unset / not applicable" value: `-1` for signed types, `0xFF` (255) for `ubyte`, `0xFFFF` (65535) for `ushort`, `0xFFFFFFFF` for `uint` where noted. Vectors that a plugin does not provide are **empty** (length 0), never partially filled. | A consumer tests `x < 0` / `x == 0xFF` / `vec.size() == 0`; it never tests "field present". The sentinel of every field is stated in its `//` comment when it differs from the type rule. |
| P7 | **Versioning: additive within a major.** `schema_version` (in `StreamIndex`, `FrameAnalysis`, `ContainerTree`, `MetricsResult`) is the **major** version and starts at 1. Within a major: new fields are appended at the end of a table with a default; new enum values are appended; nothing is removed, renamed, retyped or renumbered; FlatBuffers `struct`s (`Rect`, `ByteSpan`, `BitSpan`, `FrameId`) are frozen forever (a new struct is a new field). A breaking change increments `schema_version`, changes the 4-character `file_identifier` and is a new ADR. | A reader built from schema 1.x reads any 1.y buffer. The cache stores the writer's exact schema revision (git SHA of the `.fbs`) in `meta.json` for diagnostics only; invalidation keys on the major. |
| P8 | **Exact spec names.** Every field that mirrors a syntax element uses the spec's identifier verbatim (`cu_skip_flag`, `mb_qp_delta`, `use_filter_intra`), including spelling the spec uses for derived variables (`IntraPredModeY`, `QpY`, `TxSize`) in `PascalCase`/mixed case as the spec writes them, converted to `snake_case` only when the FlatBuffers style rule in §0.4 applies to *OVT-invented* fields. Spec names are never renamed. | Implementers grep the spec for the field name and find the semantics. The validator checks the enum tables in §6.3 against the spec numbering in the unit tests. |

### 6.1.1 File identifiers and root types

| Schema file | `root_type` | `file_identifier` | Other root tables and their identifier (passed explicitly to `Finish()`) |
|---|---|---|---|
| `ovt_model.fbs` | `FrameAnalysis` | `"OVTA"` | `StreamIndex` → `"OVTI"`, standalone `SyntaxNode` → `"OVTS"`, `CliEnvelope` → `"OVTE"`, `PluginList` → `"OVTP"` (CLI report tables, §6.3 end; chapter 12) |
| `ovt_container.fbs` | `ContainerRoot` | `"OVTC"` | — (`ContainerTree` is nested in `ContainerRoot.tree`; §6.4) |
| `ovt_metrics.fbs` | `MetricsReport` | `"OVTR"` | `MetricsResult` → `"OVTM"` |

Codec extension tables nested in `Block.ext`, `FrameAnalysis.filter_params` and `FrameInfo.ref_state` are complete FlatBuffers **without** a file identifier; the sibling `*_type` string names the root table. All buffers are written **size-prefixed** (`FinishSizePrefixed`) when stored in the cache or streamed on stdout, and **not** size-prefixed when passed through the C ABI (`ovt_Buffer` carries the size).

---

## 6.2 Coordinate, unit and identity rules

These rules expand backbone §0.1.9 and §0.5.1. They are normative for every producer (plugin) and consumer (renderer, exporter, validator).

### 6.2.1 Coded vs. cropped coordinates

1. All `Rect`, `CellGrid` indices, `MotionVector.rect`, `PlaneRef` dimensions and `SymbolCost.x4/y4` are in **coded picture coordinates**: origin at the top-left luma sample of the *decoded* picture before any cropping, in luma samples.
2. The coded picture size is `FrameInfo.coded_size` (`x = y = 0`). Per codec: H.264 `PicWidthInSamplesL = (pic_width_in_mbs_minus1+1)*16`, `PicHeightInSamplesL = (pic_height_in_map_units_minus1+1)*16*(2-frame_mbs_only_flag)` for frames (§7.4.2.1.1); HEVC `pic_width_in_luma_samples × pic_height_in_luma_samples` (§7.4.3.2.1); AV1 `FrameWidth × FrameHeight` (AV1 §5.9.5 `frame_size()`, §5.9.8 `superres_params()`, §7.16 — i.e. the size the blocks are coded at, **before** super-resolution upscaling; `Av1FilterParams.superres` records `UpscaledWidth`). The only AV1 structures that use `UpscaledWidth` are the reconstruction `PlaneRef`s of a super-resolution frame (rule V-023 exception; the renderer stretches the overlay horizontally by `UpscaledWidth / FrameWidth`).
3. `FrameInfo.crop` is the rectangle the viewer displays: H.264 frame cropping (§7.4.2.1.1, `CropUnitX/Y` applied), HEVC conformance window (§7.4.3.2.1, `SubWidthC/SubHeightC` applied), AV1 `RenderWidth × RenderHeight` (AV1 §5.9.6 `render_size()`) placed at `(0,0)`. For H.264/HEVC `crop` is a sub-rectangle of `coded_size` (rule V-002). For AV1 the render size is **independent** of the coded size (it may be larger or smaller; it is a display hint, not a crop): `crop.x = crop.y = 0`, `crop.w, crop.h ≥ 1`, and a consumer that draws in display space scales the coded picture to `crop.w × crop.h` instead of clipping. A consumer that draws in display space for H.26x subtracts `crop.x/crop.y` and clips to `crop.w/crop.h`.
4. `CellGrid.cells_w = ceil(coded_size.w / 4)`, `cells_h = ceil(coded_size.h / 4)`. For H.264 and HEVC `coded_size` is a multiple of the minimum block size (16 resp. `MinCbSizeY ≥ 8`), so no cell extends past the coded picture. For AV1 `FrameWidth`/`FrameHeight` are arbitrary (≥ 1), so the last cell column/row may extend up to 3 luma samples past the coded picture; such a cell is a valid cell whose values are those of the block covering its top-left sample (libaom's MI grid pads to 4×4 the same way). A cell that lies outside `crop` is still a valid cell.
5. Blocks may extend beyond the coded picture (HEVC CTUs on the right/bottom border, AV1 superblocks, H.264 never). `Block.rect` stores the **nominal** block rectangle (e.g. 64×64 CTU) and may exceed `coded_size`; cells exist only inside `coded_size`. Rule V-012 (§6.9) checks that every cell maps to a block whose rect contains the cell.

### 6.2.2 Chroma handling

Chroma is never stored in chroma-sample coordinates in the neutral model. The neutral model stores luma positions and the consumer derives chroma positions with the factors below (`chroma_format` from `FrameInfo`):

| `chroma_format` | Name | `SubWidthC` | `SubHeightC` | Chroma plane size for coded `W × H` |
|---|---|---|---|---|
| 0 | 4:0:0 | — | — | no chroma planes (`PlaneRef` for plane 1/2 absent) |
| 1 | 4:2:0 | 2 | 2 | `ceil(W/2) × ceil(H/2)` |
| 2 | 4:2:2 | 2 | 1 | `ceil(W/2) × H` |
| 3 | 4:4:4 | 1 | 1 | `W × H` |

A luma rect `(x, y, w, h)` covers chroma rect `(x/SubWidthC, y/SubHeightC, w/SubWidthC, h/SubHeightC)` (integer division; blocks are always aligned so this is exact for sizes ≥ 8; for 4×4 luma cells in 4:2:0 two cells share one 2×2 chroma area, which is why chroma TU/cbf attributes are reported at the 8×8 luma granularity by HEVC and AV1 and the cell vector repeats the same value over the sharing cells).

Chroma-specific syntax (HEVC `intra_chroma_pred_mode`, 4:2:2 second chroma TB cbf, AV1 `uv_mode`, CfL alphas, AV1 chroma `TxSize` of the 4×4-luma "chroma reference block") lives in the codec extension tables; the neutral `CellGrid.intra_mode_c` holds the *derived* chroma mode in codec numbering (HEVC `IntraPredModeC` after the Table 8-3 4:2:2 remap; AV1 `uv_mode`; H.264 `intra_chroma_pred_mode`).

### 6.2.3 Motion vector units and conversion factors

The neutral unit is **1/16 luma sample**, stored as `short` (range ±2047.9375 luma samples). H.264 MVs (Annex A: horizontal ±2048 luma samples, vertical at most ±512 luma samples, quarter-sample units) and AV1 MVs (`MV_LOW`/`MV_UPP` = ±2^14 in 1/8 units = ±2048 luma samples) fit after conversion. **HEVC** `mvLX` components are in quarter-luma-sample units and, after the modulo-2^16 wrap of `uLX`, in the range −2^15..2^15−1 (H.265 §8.5.3.2.1, i.e. up to ±8192 luma samples), so ×4 can overflow `short`: the plugin MUST clamp the converted value to ±32767 and emit Diagnostic `MDL-GEN-0003` (Warning, once per frame) when a clamp occurred; conformance streams never need this but the clamp is mandatory.

| Codec | Spec MV unit (luma) | Spec variable | Factor to 1/16 | Chroma MV (derived by consumer, not stored) |
|---|---|---|---|---|
| H.264 | 1/4 luma sample (§8.4.1.4; `mvLX` components in quarter-sample units) | `mvL0`, `mvL1` | **×4** | 4:2:0: same integer value interpreted in 1/8 chroma-sample units (§8.4.1.4) → in neutral units `mvC = mv` with the chroma plane scaled by 1/2; field pictures see §6.2.4 |
| HEVC | 1/4 luma sample (H.265 §8.5.3.2.1; range −2^15..2^15−1) | `mvL0`, `mvL1` | **×4** | `mvCLX = mvLX * 2 / SubWidthC` horizontally and `* 2 / SubHeightC` vertically, in 1/8 chroma-sample units (§8.5.3.2.10; the chroma sample interpolation that consumes it is §8.5.3.3.3.3) |
| AV1 | 1/8 luma sample (§5.11.32, `Mv[]` after `read_mv`; `allow_high_precision_mv = 0` means the low bit is always 0) | `Mv[refList][comp]` | **×2** | chroma uses the same MV with the plane subsampling applied in the prediction process (§7.11.3.3) |
| AV1 intra block copy (`use_intrabc`) | whole luma samples, signalled as integer MV (§5.11.24, values multiples of 8 in 1/8 units) | `Mv` | **×2** (stored value is a multiple of 16) | — |

Rules:
- `MotionVector.mv_x/mv_y` and `CellGrid.mv0_*/mv1_*` always hold the **derived** (final) motion vector in 1/16 luma units. `MotionVector.mvd_x/mvd_y` hold the **signalled** difference (`mvd_l0`, `mvd_coding`, `read_mv` delta) converted with the **same factor**. The sentinel for "MVD not available" is `mvd_x = mvd_y = -32768` (`short` minimum), chosen because no legal MVD maps to that value after conversion. Plugins that cannot provide MVDs (phase-1 FFmpeg H.264 plugin) set both to `-32768`.
- The sign convention is the spec's: positive x → right, positive y → down.
- AV1 compound prediction: `mv0/ref0` is `Mv[0]/RefFrame[0]`, `mv1/ref1` is `Mv[1]/RefFrame[1]`. H.26x: list 0 → `mv0/ref0`, list 1 → `mv1/ref1`.
- Global or warped motion (AV1 `GLOBALMV`, `motion_mode = LOCALWARP`): `MotionVector.mv_*` holds the block-centre translational MV the decoder derived (libaom `mi->mv`), `flags` bit 2 is set, and the warp parameters are in `Av1Block.LocalWarpParams`/`gm_params`.
- OBMC/sub-block motion (`flags` bit 3): one `MotionVector` per prediction block; neighbours' MVs used for blending are not duplicated.
- Weighted prediction (`flags` bit 1): weights live in `PartitionInfo.header` (H.26x pred weight table) or in `Av1Block` (`compound_idx`, distance weights); the neutral record only flags it.

### 6.2.4 Fields and MBAFF (H.264; HEVC field_seq SEI)

1. **Field pictures** (H.264 `field_pic_flag = 1`; HEVC pictures flagged as fields by `pic_struct` in picture timing SEI or `field_seq_flag`): each coded field is its **own `FrameInfo` and `FrameAnalysis`** with `structure = TopField | BottomField`. `coded_size.h` is the field height (`PicHeightInMbs*16`). All rects, cells and MVs are in **field coordinates**. The two fields of a complementary field pair share one `display_index`; `decode_index` differs. A renderer that shows the interleaved frame maps field row `r` to frame row `2r + parity` (`parity = 0` top, `1` bottom) and multiplies vertical MV components by 2 for display only. The data is **not** pre-converted.
2. **MBAFF** (`mb_adaptive_frame_field_flag = 1` and `field_pic_flag = 0`): `structure = Mbaff`, coordinates are frame coordinates. Macroblocks come in vertical pairs (16×32). For a **frame MB pair** the two `Block`s have rects `(x, y, 16, 16)` and `(x, y+16, 16, 16)` as usual. For a **field MB pair** (`mb_field_decoding_flag = 1`): the top-field MB `Block` has `rect = (x, y, 16, 32)` and the bottom-field MB `Block` has `rect = (x, y, 16, 32)` too (identical rects), `AvcMb.mb_field_decoding_flag = 1`, `AvcMb.pair_parity = 0/1`. Cells are assigned by *frame* row: cells with `y4 < (y+16)/4` belong to the top MB, the rest to the bottom MB (this is an approximation documented for the renderer; the exact interleave is recoverable from `pair_parity`). Vertical MV components of field MBs are stored **as decoded, in field units ×4** (quarter-field-sample → 1/16 field-sample), with `MotionVector.flags` bit 4 set; the consumer doubles them for frame display. `MotionVector.rect` for a field MB partition is the pair rect sub-divided by partition in field coordinates then stretched ×2 vertically (so a 16×8 field partition covers 16×16 frame rows). Rule V-018 enforces `flags bit4 == 1 ⇔ owning Block has mb_field_decoding_flag = 1 or structure is a field`.
3. `PictureStructure.Frame` is used for every progressive picture and for AV1 always.

### 6.2.5 Frame identity: `decode_index`, `display_index`, POC / `order_hint`

| Field | Definition | H.264 | HEVC | AV1 |
|---|---|---|---|---|
| `FrameId.decode_index` | 0-based position of the `FrameInfo` in `StreamIndex.frames` (decode order). One entry per **coded picture** (frame or field) *and* per AV1 `show_existing_frame = 1` frame header (such an entry has `show_existing_of = decode_index of the shown frame`, `size_bytes` = size of that header OBU, `units` = that OBU only, and no `FrameAnalysis` is produced for it). | every primary coded picture (field or frame); redundant coded pictures are **not** frames (they are `Unit`s with `frame.decode_index` of the primary picture) | every picture (`first_slice_segment_in_pic_flag = 1` starts one) | every `frame_header_obu`/`OBU_FRAME` (hidden frames included) and every `show_existing_frame` header; `OBU_REDUNDANT_FRAME_HEADER` does not start a frame |
| `FrameId.display_index` | 0-based position among **output** frames in output order; `-1` for frames never output (AV1 `show_frame = 0` and not later shown via `show_existing_frame`; HEVC `pic_output_flag = 0` or RASL skipped; H.264 non-existing frames). The two fields of a complementary field pair share one `display_index` (chapter 7 §7.3.9); `show_existing_of` is never set for H.264 fields. For AV1 a hidden frame (`show_frame = 0`) keeps `display_index = -1` even when it is later shown; the `show_existing_frame` entry that shows it carries the `display_index` (chapter 7 §7.4.2 S9). | output order per C.4 bumping | output order per C.5.2 | order of `show_frame = 1` / `show_existing_frame = 1` headers |
| `FrameInfo.poc` | codec-native ordering value, `long` | `PicOrderCnt(CurrPic)` (H.264 §8.2.1); for a field the field's `TopFieldOrderCnt`/`BottomFieldOrderCnt` | `PicOrderCntVal` (H.265 §8.3.1) | `OrderHint` (AV1 §5.9.2); when `enable_order_hint = 0` (`OrderHintBits = 0`) the plugin stores `decode_index` so that `poc` stays strictly increasing (chapter 7 §7.4.2 S4) |
| `StreamIndex.display_order` | `display_order[d] = decode_index` of the frame output at slot `d`; for field pairs the **first decoded** field's index; for AV1 `show_existing_frame` the index of the `show_existing_frame` entry (not the hidden frame) | | | |

The neutral `FrameType` is slice-majority for H.26x (I if all slices are I, P if any P and no B, B otherwise) and the AV1 `frame_type` for AV1 (`Key = KEY_FRAME`, `Inter = INTER_FRAME`, `IntraOnly = INTRA_ONLY_FRAME`, `Switch = SWITCH_FRAME`). `is_rap` is true for H.264 IDR (and recovery-point SEI pictures when `--treat-recovery-as-rap` is given, default off), HEVC IRAP (`nal_unit_type` 16–23), AV1 `KEY_FRAME` with `show_frame = 1` (a hidden key frame becomes a RAP at its `show_existing_frame`: the `show_existing_frame` entry has `is_rap = true`, the hidden frame itself has `is_rap = false`; chapter 7 §7.4.2 S9).

### 6.2.6 Time bases

- `FrameInfo.pts`, `dts` are integers in `timebase_num / timebase_den` seconds. Derived seconds = `pts * timebase_num / timebase_den` as `double`.
- **Containers:** ISOBMFF → `timebase = 1 / mdhd.timescale`, `dts` = decode time from `stts`/`trun`, `pts = dts + ctts offset` (after edit-list shift when the CLI flag `--apply-edits` is set; default is **unshifted** so that values match `mp4dump`), IVF → `1 / (frame_rate_numerator / frame_rate_denominator)` from the IVF header (i.e. `timebase_num = frame_rate_denominator`, `timebase_den = frame_rate_numerator`), `pts` = IVF frame timestamp field, `dts = pts`. Matroska/TS: per Container chapter.
- **Elementary streams without container timing:** if VUI/timing info exists (H.264 `num_units_in_tick/time_scale` with `fixed_frame_rate_flag`, HEVC `vui_num_units_in_tick/vui_time_scale`, AV1 `timing_info` `num_units_in_display_tick/time_scale`) then `timebase_num = num_units_in_tick`, `timebase_den = time_scale` (H.264: `pts = display_index * 2` because a frame is two field ticks per H.264 §E.2.1 `DeltaTfiDivisor`; HEVC: `pts = display_index * (elemental_duration_in_tc_minus1 + 1)` with `elemental_duration_in_tc_minus1` from `vui_hrd_parameters`/`vui_poc_proportional_to_timing_flag` when present, else `pts = display_index` in units of one clock tick (H.265 §E.3.1); AV1: `pts = display_index * (num_ticks_per_picture_minus_1 + 1)` only when `equal_picture_interval = 1`, otherwise `timebase_num = 1`, `timebase_den = 1` and `pts = display_index` because per-frame `frame_presentation_time` is a decoder-model quantity, not a presentation time). Otherwise `timebase_num = 1`, `timebase_den = 1`, `pts = display_index`, `dts = decode_index` and the index emits Diagnostic `SYN-GEN-0104 "no timing information; frame-count time base"` once.
- `timebase_den` MUST be > 0 in every `FrameInfo` (rule V-002).

### 6.2.7 Bit and byte offsets

- `ByteSpan.offset` is 0-based from the start of the **file** as opened (for DASH/HLS virtual files, from the start of the virtual concatenation, see `DashHlsContext`).
- `BitSpan.bit_offset` is 0-based from the start of the owning **unit** (§0.1.9), defined exactly as chapter 7 §7.2 does: for an H.26x NAL unit, bit 0 is `forbidden_zero_bit` of the NAL unit header and the count continues over the **RBSP** (emulation prevention bytes removed; the header bytes are never subject to EPB, so header offsets are raw offsets); `Unit.epb_positions` (byte offsets inside `Unit.file_span`) maps RBSP positions back to raw file bytes: `raw_byte = rbsp_byte + |{p ∈ epb_positions : p ≤ raw_byte}|` (iterate). For an AV1 OBU, bit 0 is `obu_forbidden_bit` of `obu_header()`, i.e. the span covers the **whole OBU including `obu_header` and `obu_size`** (AV1 has no emulation prevention, so raw == RBSP and `epb_positions` is empty); the phrase "OBU payload" in the §0.5.1 comment is to be read as these bytes (chapter 7 §7.2 is normative). For `Field` in the container tree it is relative to `BoxNode.file_span.offset` in **raw** bytes (no RBSP concept).
- `PartitionInfo.file_span` for a slice is the slice NAL's `Unit.file_span`; `data_bit_offset` is the RBSP bit position where `slice_data()` starts (after `byte_alignment()` for CABAC).

---

### 6.2.8 `source_hash` (single definition)

`StreamIndex.source_hash`, every `blob_id` (§6.6.2), the cache directory name (§6.7) and the index cache key (chapter 7 §7.5.5) use **one** value: `source_hash = SHA-256( file[0 : min(size, 1 MiB)] ∥ LE64(size) ∥ LE64(mtime_ns) )` where `size` is the file size in bytes, `mtime_ns` the modification time in nanoseconds since the Unix epoch as reported by the opened file (`fstat`; `0` when the platform exposes none, e.g. an Android `content://` descriptor without `last_modified`), both as 8-byte little-endian integers. For a DASH/HLS virtual file the bytes are those of the virtual concatenation and `size`/`mtime_ns` are those of the manifest. Chapter 7 §7.4.2 step S10 and §6.7.3 rule I1 refer to this definition; no chapter restates the recipe.

## 6.3 `core/schema/ovt_model.fbs`

The complete file follows. Lines inside `// ===== backbone §0.5.x (verbatim) =====` fences are copied from the backbone unchanged; `// --- extension (ch. 6) ---` marks appended fields. Enumerations use the spec numbering given in their comment; where OVT must merge several spec tables into one enum (H.264 `mb_type` differs per slice type) the merge rule is given in the enum comment and in §6.3.1.

```fbs
// core/schema/ovt_model.fbs — Open Video Tools neutral analysis model + codec extensions.
// SPDX-License-Identifier: BSD-2-Clause
// schema_version (major) = 1. Rules: additive-only within a major (append fields / enum values; never
// remove, rename, retype, renumber; structs are frozen). See docs/spec/06-data-model.md §6.1 P7.

namespace ovt.fb;

// ===== backbone §0.5.1 (verbatim) =====
enum Codec : ubyte { Unknown = 0, H264 = 1, H265 = 2, AV1 = 3, VP9 = 4, H266 = 5, MPEG2 = 6 }

struct Rect { x:int; y:int; w:int; h:int; }            // luma samples, coded-picture coordinates

struct ByteSpan { offset:ulong; length:ulong; }          // file byte range
struct BitSpan  { bit_offset:ulong; bit_length:uint; }   // bits from start of the owning unit: NAL header bit 0 + RBSP (H.26x), or obu_header bit 0 (AV1, whole OBU) — see §6.2.7 / ch. 7 §7.2

// Frame identity. decode_index counts every coded frame (AV1 hidden frames included);
// display_index counts only shown/output frames in output order. -1 = not shown / unknown.
struct FrameId { decode_index:int; display_index:int; }

// ===== backbone §0.5.2 (verbatim) =====
enum SyntaxValueKind : ubyte { None, UInt, Int, Bool, Bytes, String, Enum, Fixed /* fixed-point as int+scale */ }

table SyntaxNode {
  name:string (required);         // exact spec syntax element name, e.g. "log2_max_pic_order_cnt_lsb_minus4"
  descriptor:string;              // spec descriptor, e.g. "ue(v)", "u(8)", "f(1)", "le(n)", "ns(n)", "su(1+6)"
  kind:SyntaxValueKind;
  uvalue:ulong; ivalue:long; bvalue:bool; bytes:[ubyte]; svalue:string;
  enum_name:string;               // human name for enum values, e.g. "PART_2NxN"
  derived:string;                 // optional derived/explained value, e.g. "CtbSizeY = 64"
  span:BitSpan;                   // position inside the owning unit (see Unit.epb_positions for raw mapping)
  spec_ref:string;                // e.g. "H.265 7.3.2.2.1"
  children:[SyntaxNode];
  array_index:int = -1;           // when this node is an element of a syntax array
  diagnostics:[Diagnostic];
}

enum Severity : ubyte { Info, Warning, Error, Fatal }
table Diagnostic { severity:Severity; code:string; message:string; span:BitSpan; spec_ref:string; }

// ===== backbone §0.5.3 (verbatim) =====
table Unit {                       // one NAL unit (H.26x) or OBU (AV1) or raw sample
  index:uint;                      // 0-based in decode order
  file_span:ByteSpan;              // raw bytes in the file, start code / length prefix excluded
  prefix_span:ByteSpan;            // start code or length field
  type:ubyte;                      // nal_unit_type / obu_type
  type_name:string;                // "IDR_W_RADL", "OBU_FRAME", ...
  layer_id:ubyte; temporal_id:ubyte;
  epb_positions:[uint];            // byte offsets (within file_span) of removed emulation-prevention bytes; empty for AV1
  frame:FrameId;                   // owning frame, or decode_index = -1 for non-VCL units
  header:SyntaxNode;               // parsed header (parameter set, slice header, OBU header + frame header, SEI...)
  sample_index:int = -1;           // container sample this unit belongs to, when demuxed from a container
  diagnostics:[Diagnostic];        // diagnostics raised while scanning or parsing THIS unit (chapter 7 §7.3); StreamIndex.diagnostics holds stream-level ones
}

enum FrameType : ubyte { I, P, B, Key, Inter, IntraOnly, Switch, Unknown }
enum PictureStructure : ubyte { Frame, TopField, BottomField, Mbaff }

table FrameInfo {
  id:FrameId;
  type:FrameType;                  // codec-native; I/P/B for H.26x (slice-majority), Key/Inter/IntraOnly/Switch for AV1
  shown:bool; show_existing_of:int = -1;
  structure:PictureStructure;
  temporal_id:ubyte; layer_id:ubyte;
  is_rap:bool;                     // random access point (IDR/CRA/BLA/Key)
  poc:long;                        // PicOrderCntVal / frame_num-derived / order_hint
  pts:long; dts:long; timebase_num:int; timebase_den:int;
  size_bytes:ulong;                // AU size: Σ over the frame's units of (file_span.length + prefix_span.length), i.e. start codes / length prefixes INCLUDED and the AUD/SEI/parameter-set units that precede the first VCL unit of the frame included (ch. 7 §7.4.2 step S7 is normative); the payload-only sum is Σ units[i].file_span.length
  units:[uint];                    // Unit.index list
  coded_size:Rect; crop:Rect;      // crop = conformance window / render size
  chroma_format:ubyte;             // 0=400,1=420,2=422,3=444
  bit_depth_luma:ubyte; bit_depth_chroma:ubyte;
  qp_base:short;                   // slice QP of first slice / base_q_idx
  declared_refs:[int];             // decode_index of frames referenced per headers (ref lists / ref_frame_idx)
  dpb_after:[int];                 // decode_index of frames in DPB after decoding this frame (if derivable)
  // --- extension (ch. 6) ---
  ref_state:[ubyte];               // nested FlatBuffer: HevcRps / Av1RefState / AvcRefMarking (header-derived reference structure)
  ref_state_type:string;           // "HevcRps" | "Av1RefState" | "AvcRefMarking" | "" (none)
  frame_num:int = -1;              // H.264 frame_num (7.4.3); AV1 current_frame_id when frame_id_numbers_present_flag; -1 otherwise
  slice_count:ushort;              // number of slices / slice segments / tile groups in this frame (0 = unknown)
}

table StreamIndex {
  codec:Codec;
  schema_version:uint;             // = 1
  source_hash:[ubyte];             // 32-byte SHA-256 per §6.2.8 (first 1 MiB ∥ LE64 size ∥ LE64 mtime_ns) — cache key; the ONLY definition
  units:[Unit];
  frames:[FrameInfo];              // decode order
  display_order:[int];             // decode_index per display slot
  parameter_sets:[Unit];           // convenience: all VPS/SPS/PPS/sequence headers
  sei:[Unit];                      // all SEI / metadata OBUs
  diagnostics:[Diagnostic];
}

// ===== backbone §0.5.4 (verbatim) =====
enum PredKind : ubyte { None, Intra, Inter, Skip, IntraBC, PCM, Palette }

// Structure-of-arrays grid over 4x4 luma cells. All vectors have length cells_w * cells_h
// (row-major) unless empty (= not provided by this plugin). -1 / 0xFF mean "not applicable".
table CellGrid {
  cells_w:uint; cells_h:uint;                  // ceil(coded_w/4), ceil(coded_h/4)
  block_id:[uint];                             // leaf Block owning this cell
  partition_id:[ushort];                       // slice / tile index
  pred:[ubyte];                                // PredKind
  qp:[short];                                  // final luma QP (H.26x) or qindex (AV1)
  intra_mode_y:[short]; intra_mode_c:[short];  // codec-numbered
  mv0_x:[short]; mv0_y:[short]; mv1_x:[short]; mv1_y:[short];   // 1/16 luma sample units
  ref0:[byte]; ref1:[byte];                    // RefSlot.index into FrameAnalysis.refs; -1 none. AV1: equals the reference type LAST_FRAME..ALTREF_FRAME (1..7) because refs[0] is the INTRA placeholder (6.3.1); never 0 for a cell
  tx_log2_w:[ubyte]; tx_log2_h:[ubyte]; tx_type:[ubyte]; cbf:[ubyte];   // cbf bit0=Y bit1=Cb bit2=Cr
  bits_x8:[uint];                              // estimated entropy cost ×8 for the owning block, stored at every cell of the block divided by cell count (so the sum over cells = block cost)
  deblock_bs_v:[ubyte]; deblock_bs_h:[ubyte];  // boundary strength on left / top edge of the cell
  filter_idx:[ubyte];                          // SAO type (HEVC) / cdef_idx (AV1) / 0
}

enum BlockKind : ubyte { MB, SubMB, CTU, CU, PU, TU, SB, Block, TXB, Other }

table Block {
  id:uint; parent:int = -1; children:[uint];
  kind:BlockKind; rect:Rect; level:ubyte;
  partition_type:ubyte;            // codec numbering (mb_type/sub_mb_type, PartMode, PARTITION_*)
  pred:PredKind; skip:bool; merge_or_direct:bool;
  qp:short; bits_x8:uint;          // exact-as-available cost for this block incl. children
  ext_type:string;                 // "AvcMb", "HevcCu", "HevcPu", "HevcTu", "Av1Block", "Av1Txb"
  ext:[ubyte];                     // nested FlatBuffer of the codec extension table named by ext_type
}

table MotionVector {
  block_id:uint; list:ubyte;       // 0 = L0 / first ref, 1 = L1 / second ref
  ref:byte;                        // index into refs
  rect:Rect;                       // the prediction block
  mv_x:short; mv_y:short;          // 1/16 luma sample units
  mvd_x:short; mvd_y:short;        // signalled difference (if available), same units
  predictor:string;                // "merge[2]", "amvp[0]", "spatial_direct", "NEARESTMV", "GLOBALMV", ...
  flags:ushort;                    // bit0 bi-pred, bit1 weighted, bit2 global/warped, bit3 sub-block (affine/OBMC), bit4 field
}

table RefSlot { index:ubyte; frame:FrameId; name:string; /* "L0[0]", "LAST", "ALTREF", "INTRA" */ is_long_term:bool; usage_cells:uint;
                slot:byte = -1;   /* AV1 only: the DPB slot ref_frame_idx[index - 1] (0..7) this reference type was mapped to; -1 for H.26x and for the INTRA placeholder (6.3.1) */ }

enum PlaneStage : ubyte { Reconstruction, Prediction, Residual, PreDeblock, PreSaoOrCdef, PreLoopRestoration, Coefficients }

table PlaneRef {                   // planes are not stored in the FlatBuffer; this describes a shared-memory / file blob
  stage:PlaneStage; plane:ubyte;   // 0=Y 1=U 2=V
  width:uint; height:uint; stride_bytes:uint; bit_depth:ubyte; sample_bytes:ubyte; is_signed:bool;   // `signed` is a reserved word in several generated languages
  blob_id:string;                  // key into the Picture store
}

table SymbolCost { x4:ushort; y4:ushort; symbol_id:ushort; bits_x8:uint; count:uint; }

table FrameAnalysis {
  schema_version:uint;             // = 1
  plugin_id:string; plugin_version:string;
  frame:FrameInfo;
  grid:CellGrid;
  blocks:[Block];                  // blocks[0] children are the top-level CTUs/SBs/MBs in raster order
  mvs:[MotionVector];
  refs:[RefSlot];
  partitions:[PartitionInfo];      // slices / tiles / tile groups with byte ranges and headers
  filter_params:[ubyte]; filter_params_type:string;   // nested codec table: HevcFilterParams / Av1FilterParams / AvcFilterParams
  planes:[PlaneRef];
  symbols:[SymbolCost]; symbol_names:[string];
  qp_hist:[uint];                  // 256 bins
  diagnostics:[Diagnostic];
}

// =====================================================================================
// ch. 6 additions — partitions
// =====================================================================================

enum PartitionKind : ubyte {
  Slice = 0,          // H.264 slice (7.3.3); HEVC independent slice segment that starts a slice
  SliceSegment = 1,   // HEVC dependent slice segment (dependent_slice_segment_flag = 1)
  Tile = 2,           // HEVC tile (6.3.1) / AV1 tile (6.10.1)
  TileGroup = 3,      // AV1 tile_group_obu (5.11.1)
  Substream = 4       // HEVC WPP substream delimited by entry_point_offset_minus1 (7.4.7.1)
}

table PartitionInfo {
  kind:PartitionKind;
  index:ushort;                    // 0-based in decode order within the frame; equals CellGrid.partition_id values for Slice/SliceSegment/Tile
  parent:short = -1;               // index of the enclosing partition (Tile -> TileGroup, Substream -> SliceSegment, SliceSegment -> Slice); -1 none
  unit_index:uint;                 // Unit.index carrying this partition (slice NAL / OBU_TILE_GROUP or OBU_FRAME)
  file_span:ByteSpan;              // exact bytes of this partition's data in the file (slice NAL payload / tile bytes incl. tile_size_minus_1 field)
  data_bit_offset:uint;            // RBSP bit position where slice_data()/tile data starts inside unit (after slice header + byte_alignment); 0 for AV1 tiles
  bit_count:ulong;                 // EXACT bit length of the entropy-coded data (slice_data incl. trailing bits; AV1 tile size*8). Not an estimate.
  header:SyntaxNode;               // slice_segment_header() / slice_header() / tile_group_obu header; null for Tile/Substream
  slice_type:ubyte = 255;          // codec numbering: H.264 slice_type 0..9 (7.4.3); HEVC slice_type 0=B 1=P 2=I (7.4.7.1); AV1: 255 (n/a)
  rect:Rect;                       // Tile: exact rectangle (luma); Slice/SliceSegment: bounding box of its CTBs/MBs (may include cells of other slices)
  first_block_addr:uint;           // first_mb_in_slice / slice_segment_address (CTB raster) / MiRowStart*MiCols+MiColStart (AV1) 
  block_count:uint;                // number of MBs / CTBs / 4x4 MI units in the partition
  qp:short = -1;                   // H.264 SliceQPY = 26 + pic_init_qp_minus26 + slice_qp_delta (7.4.3); HEVC SliceQpY = 26 + init_qp_minus26 + slice_qp_delta (7.4.7.1); AV1 base_q_idx
  qp_delta:short = -1;             // slice_qp_delta as signalled; AV1: 0
  deblocking_disable:ubyte = 255;  // H.264 disable_deblocking_filter_idc 0..2; HEVC slice_deblocking_filter_disabled_flag 0/1; AV1 255
  deblock_alpha_or_beta_offset_div2:byte = 0;  // H.264 slice_alpha_c0_offset_div2 (7.4.3) / HEVC slice_beta_offset_div2 (7.4.7.1); 0 when not present
  deblock_beta_or_tc_offset_div2:byte = 0;     // H.264 slice_beta_offset_div2 / HEVC slice_tc_offset_div2; 0 when not present
  loop_filter_across_slices:ubyte = 255;       // HEVC slice_loop_filter_across_slices_enabled_flag; 255 n/a
  sao_luma:ubyte = 255; sao_chroma:ubyte = 255;  // HEVC slice_sao_luma_flag / slice_sao_chroma_flag
  tile_row:ushort = 65535; tile_col:ushort = 65535;  // Tile only: TileRow / TileCol (AV1 6.10.1; HEVC tile scan 6.5.1)
  num_entry_points:ushort;         // HEVC num_entry_point_offsets; AV1 NumTiles in the tile group; 0 otherwise
  cabac_init:ubyte = 255;          // H.264 cabac_init_idc 0..2; HEVC cabac_init_flag 0/1; AV1 255
  max_num_merge_cand:ubyte = 255;  // HEVC MaxNumMergeCand = 5 - five_minus_max_num_merge_cand (7.4.7.1)
  num_ref_idx_active:[ubyte];      // [L0, L1] after override: num_ref_idx_l0_active_minus1+1, num_ref_idx_l1_active_minus1+1; empty for AV1
  diagnostics:[Diagnostic];
}

// =====================================================================================
// ch. 6 additions — H.264 / AVC extension tables
// Spec: ITU-T H.264 (08/2021). Sentinels: -1 (signed), 255 (ubyte), 65535 (ushort).
// =====================================================================================

// Unified mb_type numbering. The spec numbers mb_type per slice type (Tables 7-11, 7-13, 7-14); OVT stores the
// raw value in AvcMb.mb_type_raw and this unified code in AvcMb.mb_type. Mapping from (slice_type, raw):
//   I/SI slice: unified = raw (SI slice raw 0 -> SI, raw>=1 -> raw-1 as I type)
//   P/SP slice: raw <  5 -> unified = raw + 26 ; raw >= 5 -> unified = raw - 5  (I type, Table 7-13 note)
//   B slice   : raw < 23 -> unified = raw + 31 ; raw >= 23 -> unified = raw - 23 (I type, Table 7-14 note)
//   inferred  : P_Skip = 54, B_Skip = 55 (mb_skip_flag / mb_skip_run, 7.4.4), SI = 56 (Table 7-12)
enum AvcMbType : ubyte {
  I_NxN = 0,                                                             // Table 7-11 (I_4x4 or I_8x8 per transform_size_8x8_flag)
  I_16x16_0_0_0 = 1, I_16x16_1_0_0 = 2, I_16x16_2_0_0 = 3, I_16x16_3_0_0 = 4,
  I_16x16_0_1_0 = 5, I_16x16_1_1_0 = 6, I_16x16_2_1_0 = 7, I_16x16_3_1_0 = 8,
  I_16x16_0_2_0 = 9, I_16x16_1_2_0 = 10, I_16x16_2_2_0 = 11, I_16x16_3_2_0 = 12,
  I_16x16_0_0_1 = 13, I_16x16_1_0_1 = 14, I_16x16_2_0_1 = 15, I_16x16_3_0_1 = 16,
  I_16x16_0_1_1 = 17, I_16x16_1_1_1 = 18, I_16x16_2_1_1 = 19, I_16x16_3_1_1 = 20,
  I_16x16_0_2_1 = 21, I_16x16_1_2_1 = 22, I_16x16_2_2_1 = 23, I_16x16_3_2_1 = 24,  // I_16x16_<predmode>_<cbpchroma>_<cbpluma 0|15>
  I_PCM = 25,
  P_L0_16x16 = 26, P_L0_L0_16x8 = 27, P_L0_L0_8x16 = 28, P_8x8 = 29, P_8x8ref0 = 30,       // Table 7-13 raw 0..4
  B_Direct_16x16 = 31, B_L0_16x16 = 32, B_L1_16x16 = 33, B_Bi_16x16 = 34,                  // Table 7-14 raw 0..22
  B_L0_L0_16x8 = 35, B_L0_L0_8x16 = 36, B_L1_L1_16x8 = 37, B_L1_L1_8x16 = 38,
  B_L0_L1_16x8 = 39, B_L0_L1_8x16 = 40, B_L1_L0_16x8 = 41, B_L1_L0_8x16 = 42,
  B_L0_Bi_16x8 = 43, B_L0_Bi_8x16 = 44, B_L1_Bi_16x8 = 45, B_L1_Bi_8x16 = 46,
  B_Bi_L0_16x8 = 47, B_Bi_L0_8x16 = 48, B_Bi_L1_16x8 = 49, B_Bi_L1_8x16 = 50,
  B_Bi_Bi_16x8 = 51, B_Bi_Bi_8x16 = 52, B_8x8 = 53,
  P_Skip = 54, B_Skip = 55, SI = 56,
  AvcMbType_Unknown = 255
}

// Unified sub_mb_type: P types Table 7-17 raw 0..3 -> 0..3 ; B types Table 7-18 raw 0..12 -> raw + 4.
enum AvcSubMbType : ubyte {
  P_L0_8x8 = 0, P_L0_8x4 = 1, P_L0_4x8 = 2, P_L0_4x4 = 3,
  B_Direct_8x8 = 4, B_L0_8x8 = 5, B_L1_8x8 = 6, B_Bi_8x8 = 7,
  B_L0_8x4 = 8, B_L0_4x8 = 9, B_L1_8x4 = 10, B_L1_4x8 = 11, B_Bi_8x4 = 12, B_Bi_4x8 = 13,
  B_L0_4x4 = 14, B_L1_4x4 = 15, B_Bi_4x4 = 16,
  AvcSubMbType_Unknown = 255
}

enum AvcIntra4x4PredMode : ubyte {      // Intra4x4PredMode / Intra8x8PredMode, Table 8-2 / 8-3
  Vertical = 0, Horizontal = 1, DC = 2, Diagonal_Down_Left = 3, Diagonal_Down_Right = 4,
  Vertical_Right = 5, Horizontal_Down = 6, Vertical_Left = 7, Horizontal_Up = 8, AvcIntra4x4_Unknown = 255
}
enum AvcIntra16x16PredMode : ubyte { I16_Vertical = 0, I16_Horizontal = 1, I16_DC = 2, I16_Plane = 3, AvcIntra16x16_Unknown = 255 }   // Table 8-4
enum AvcIntraChromaPredMode : ubyte { Chroma_DC = 0, Chroma_Horizontal = 1, Chroma_Vertical = 2, Chroma_Plane = 3, AvcIntraChroma_Unknown = 255 }   // Table 7-16

// One macroblock. Block.kind = MB. Block.partition_type = AvcMb.mb_type (unified).
table AvcMb {
  mb_addr:uint;                            // CurrMbAddr (7.4.4), raster in frame / field; MBAFF: 2*pairAddr + (bottom?1:0)
  mb_type_raw:ubyte = 255;                 // mb_type as signalled (7.3.5); 255 for skipped MBs (not signalled)
  mb_type:AvcMbType = AvcMbType_Unknown;   // unified (see enum comment)
  mb_skip_flag:ubyte = 255;                // 7.3.4 (CABAC) or inferred from mb_skip_run (CAVLC); 0/1
  mb_field_decoding_flag:ubyte = 255;      // 7.3.4; MBAFF only; 255 when not MBAFF
  pair_parity:ubyte = 255;                 // MBAFF field MB pair: 0 = top field MB, 1 = bottom field MB (OVT-derived, see 6.2.4); 255 otherwise
  transform_size_8x8_flag:ubyte = 255;     // 7.3.5; 255 = not present (inferred 0)
  coded_block_pattern:ubyte = 255;         // 7.4.5: CodedBlockPatternLuma = cbp % 16 (bit n = 8x8 block n), CodedBlockPatternChroma = cbp / 16 (0..2)
  mb_qp_delta:byte = 0;                    // 7.4.5; 0 when not present
  QPY:byte = -1;                           // derived QPY (7.4.5 eq. 7-37), 0..51 (-QpBdOffsetY..51 for high bit depth)
  QPC_cb:byte = -1; QPC_cr:byte = -1;      // QPC per component via Table 8-15 with chroma_qp_index_offset / second_chroma_qp_index_offset
  sub_mb_type:[AvcSubMbType];              // 4 entries for P_8x8 / P_8x8ref0 / B_8x8 (7.3.5.2); empty otherwise
  intra4x4_pred_mode:[AvcIntra4x4PredMode];   // 16 entries, Intra4x4PredMode[luma4x4BlkIdx] (8.3.1.1), luma4x4BlkIdx order (6.4.3); empty unless I_NxN with transform_size_8x8_flag = 0
  intra8x8_pred_mode:[AvcIntra4x4PredMode];   // 4 entries, Intra8x8PredMode[luma8x8BlkIdx] (8.3.2.1); empty unless I_NxN with transform_size_8x8_flag = 1
  prev_intra_pred_mode_flag:[ubyte];       // 16 or 4 entries: prev_intra4x4_pred_mode_flag / prev_intra8x8_pred_mode_flag (7.3.5.1); "predicted mode used" overlay
  rem_intra_pred_mode:[ubyte];             // 16 or 4 entries: rem_intra4x4_pred_mode / rem_intra8x8_pred_mode; 255 where prev flag = 1
  Intra16x16PredMode:AvcIntra16x16PredMode = AvcIntra16x16_Unknown;  // derived from mb_type (7.4.5 Table 7-11)
  intra_chroma_pred_mode:AvcIntraChromaPredMode = AvcIntraChroma_Unknown;   // 7.3.5.1 / Table 7-16
  ref_idx_l0:[byte]; ref_idx_l1:[byte];    // 4 entries each (per mbPartIdx / 8x8), ref_idx_lX (7.3.5.1); -1 = not used for that list
  mvd_l0_x:[short]; mvd_l0_y:[short];      // 16 entries each (per 4x4, luma4x4BlkIdx order): mvd_l0[mbPartIdx][subMbPartIdx] replicated over the 4x4s it covers; quarter-sample units (NOT ×4) ; -32768 = n/a
  mvd_l1_x:[short]; mvd_l1_y:[short];      // same for list 1
  mvL0_x:[short]; mvL0_y:[short];          // 16 entries: derived mvL0 per 4x4 (8.4.1), quarter-sample units (NOT ×4); -32768 = predFlagL0 = 0
  mvL1_x:[short]; mvL1_y:[short];          // 16 entries: derived mvL1
  pred_flag:[ubyte];                       // 16 entries: bit0 = predFlagL0, bit1 = predFlagL1 (8.4.1.2)
  direct_8x8:[ubyte];                      // 4 entries: 1 if the 8x8 is B_Direct (B_Direct_16x16 or sub_mb_type B_Direct_8x8), 0 otherwise
  direct_spatial_mv_pred_flag:ubyte = 255; // from slice header 7.3.3; applies to direct MBs
  total_coeff:[ubyte];                     // 16 (luma 4x4) + 8 (chroma 4x4 of Cb then Cr, 4:2:0) entries: TotalCoeff(coeff_token) for CAVLC, number of non-zero levels for CABAC; empty when unknown
  i16_dc_nonzero:ubyte = 255;              // I_16x16: 1 if the Intra16x16DCLevel block has any non-zero coefficient
  pcm_sample_luma:[ushort];                // I_PCM only: 256 pcm_sample_luma values (7.3.5); empty otherwise
  pcm_sample_chroma:[ushort];              // I_PCM only: 2*MbWidthC*MbHeightC values
  bs_vertical:[ubyte];                     // 16 entries: bS of the LEFT edge of each 4x4 (8.7.2.1), luma4x4BlkIdx order; 255 = unknown
  bs_horizontal:[ubyte];                   // 16 entries: bS of the TOP edge of each 4x4
  filterLeftMbEdgeFlag:ubyte = 255; filterTopMbEdgeFlag:ubyte = 255;   // 8.7
  bit_position_start:ulong = 0; bit_position_end:ulong = 0;   // CAVLC: exact RBSP bit positions of macroblock_layer(); CABAC: arithmetic decoder read-pointer positions (8 * bytes consumed - bits in range register) — an estimate; 0/0 = unknown
  slice_index:ushort;                      // PartitionInfo.index of the owning slice
}

// Sub-macroblock partition (one 8x8 of a P_8x8/B_8x8 MB, or a 16x8/8x16 partition). Block.kind = SubMB,
// Block.partition_type = AvcSubMb.sub_mb_type (unified) or 255 for 16x8/8x16 halves.
table AvcSubMb {
  mbPartIdx:ubyte;                         // 0..3 (6.4.2.1)
  sub_mb_type:AvcSubMbType = AvcSubMbType_Unknown;   // 255 for 16x8/8x16 partitions of a non-8x8 MB
  ref_idx_l0:byte = -1; ref_idx_l1:byte = -1;        // for this partition
  NumSubMbPart:ubyte = 1;                  // Table 7-17 / 7-18 column (1, 2, 4)
  SubMbPartWidth:ubyte; SubMbPartHeight:ubyte;       // Table 7-17 / 7-18
}

// Per-slice deblocking parameters and picture-level filter state for H.264. FrameAnalysis.filter_params_type = "AvcFilterParams".
table AvcFilterParams {
  deblocking_filter_control_present_flag:ubyte;   // PPS 7.3.2.2
  disable_deblocking_filter_idc:[ubyte];          // per slice (PartitionInfo.index order), 7.3.3
  slice_alpha_c0_offset_div2:[byte];              // per slice
  slice_beta_offset_div2:[byte];                  // per slice
  chroma_qp_index_offset:byte;                    // PPS 7.4.2.2
  second_chroma_qp_index_offset:byte;             // PPS 7.4.2.2 (equal to chroma_qp_index_offset when not present)
  FilterOffsetA:[byte]; FilterOffsetB:[byte];     // per slice: slice_alpha_c0_offset_div2 << 1, slice_beta_offset_div2 << 1 (8.7.2.2)
  film_grain_characteristics:SyntaxNode;          // film grain characteristics SEI (D.2.20 / D.3.20) if present (parsed), else null
}

// Decoded reference picture marking state after this picture (H.264 8.2.5). FrameInfo.ref_state_type = "AvcRefMarking".
table AvcMmco {
  memory_management_control_operation:ubyte;      // 7.4.3.3 Table 7-9: 1..6
  difference_of_pic_nums_minus1:int = -1;
  long_term_pic_num:int = -1;
  long_term_frame_idx:int = -1;
  max_long_term_frame_idx_plus1:int = -1;
}
table AvcRefPic {                                 // one DPB entry
  frame:FrameId;
  FrameNum:int; FrameNumWrap:int;                 // 8.2.4.1
  PicNum:int = -1; LongTermPicNum:int = -1;       // 8.2.4.1 (field-aware when field decoding)
  is_long_term:bool; is_reference:bool; is_output_needed:bool;
  structure:PictureStructure;                     // which fields are reference (Frame = both)
  TopFieldOrderCnt:int; BottomFieldOrderCnt:int;  // 8.2.1
}
table AvcRefMarking {
  frame_num:uint;                                 // 7.4.3
  idr_pic_id:int = -1;                            // IDR pictures only
  no_output_of_prior_pics_flag:ubyte = 255; long_term_reference_flag:ubyte = 255;   // IDR dec_ref_pic_marking (7.3.3.3)
  adaptive_ref_pic_marking_mode_flag:ubyte = 255;
  mmco:[AvcMmco];                                 // MMCO operations in signalled order (empty for sliding window)
  RefPicList0:[byte]; RefPicList1:[byte];         // indices into dpb_after, after modification (8.2.4.3); first slice's lists
  ref_pic_list_modification_l0:SyntaxNode; ref_pic_list_modification_l1:SyntaxNode;   // 7.3.3.1 as parsed (null if absent)
  dpb_after:[AvcRefPic];                          // DPB content after marking (8.2.5) and before bumping
  max_num_ref_frames:ubyte;                       // SPS 7.4.2.1.1
  gaps_in_frame_num_detected:bool;                // 8.2.5.2 invoked
}

// =====================================================================================
// ch. 6 additions — H.265 / HEVC extension tables
// Spec: ITU-T H.265 (09/2023).
// =====================================================================================

enum HevcPartMode : ubyte {                      // Table 7-10
  PART_2Nx2N = 0, PART_2NxN = 1, PART_Nx2N = 2, PART_NxN = 3,
  PART_2NxnU = 4, PART_2NxnD = 5, PART_nLx2N = 6, PART_nRx2N = 7, HevcPartMode_Unknown = 255
}
enum HevcPredMode : ubyte { MODE_INTER = 0, MODE_INTRA = 1, MODE_SKIP = 2, HevcPredMode_Unknown = 255 }   // 7.4.9.5 (MODE_SKIP = cu_skip_flag)
enum HevcInterPredIdc : ubyte { PRED_L0 = 0, PRED_L1 = 1, PRED_BI = 2, HevcInterPredIdc_Unknown = 255 }   // Table 7-11
// IntraPredModeY / IntraPredModeC: 0 = INTRA_PLANAR, 1 = INTRA_DC, 2..34 = INTRA_ANGULAR2..34 (Table 8-1). Stored as ubyte, 255 = n/a.
enum HevcSaoType : ubyte { SAO_NOT_APPLIED = 0, SAO_BAND = 1, SAO_EDGE = 2, HevcSaoType_Unknown = 255 }   // SaoTypeIdx, Table 7-8
enum HevcMergeCandSource : ubyte {                // OVT-derived "why this MV": origin of the selected merge / AMVP candidate (8.5.3.2.2 / 8.5.3.2.6)
  MC_Unknown = 0, MC_SpatialA0 = 1, MC_SpatialA1 = 2, MC_SpatialB0 = 3, MC_SpatialB1 = 4, MC_SpatialB2 = 5,
  MC_Temporal = 6, MC_CombinedBi = 7, MC_Zero = 8, MC_AmvpA = 9, MC_AmvpB = 10, MC_AmvpTemporal = 11, MC_AmvpZero = 12
}

// Coding tree unit. Block.kind = CTU. Block.partition_type = 255.
table HevcCtu {
  CtbAddrInRs:uint; CtbAddrInTs:uint;             // 6.5.1
  slice_segment_idx:ushort; tile_idx:ushort = 65535;   // PartitionInfo.index of slice segment / tile
  end_of_slice_segment_flag:ubyte = 255;          // 7.3.8.1
  // SAO per component (7.3.8.3), index 0 = Y, 1 = Cb, 2 = Cr. Vectors have 3 entries or are empty (SAO off / unknown).
  sao_merge_left_flag:ubyte = 255; sao_merge_up_flag:ubyte = 255;
  SaoTypeIdx:[HevcSaoType];                       // derived (merge resolved)
  sao_offset_abs:[ubyte];                         // 3 * 4 entries, [cIdx*4 + i]
  sao_offset_sign:[ubyte];                        // 3 * 4 entries (band offset only; 0 for edge)
  SaoOffsetVal:[short];                           // 3 * 4 entries derived (7.4.9.3.2: offsetSign * sao_offset_abs << log2OffsetScale, with log2OffsetScale = log2_sao_offset_scale_luma / _chroma from the PPS range extension, 0 without it)
  sao_band_position:[ubyte];                      // 3 entries (band only)
  sao_eo_class:[ubyte];                           // 3 entries: SaoEoClass 0..3 (edge only; chroma shares one value stored at 1 and 2)
  bit_count_x8:uint;                              // estimated CABAC cost of coding_tree_unit() in 1/8 bits (0 = unknown)
}

// Coding unit. Block.kind = CU. Block.partition_type = HevcCu.part_mode.
table HevcCu {
  CtDepth:ubyte;                                  // 7.4.9.4 (0..3)
  log2CbSize:ubyte;                               // 3..6
  cu_transquant_bypass_flag:ubyte = 255;          // 7.3.8.5
  cu_skip_flag:ubyte = 255;                       // 7.3.8.5
  pred_mode_flag:ubyte = 255;                     // 7.3.8.5 as signalled (255 when inferred)
  CuPredMode:HevcPredMode = HevcPredMode_Unknown; // derived (7.4.9.5)
  palette_mode_flag:ubyte = 255;                  // 7.3.8.5 (SCC)
  part_mode:HevcPartMode = HevcPartMode_Unknown;  // PartMode (Table 7-10) derived from part_mode + CuPredMode
  pcm_flag:ubyte = 255;                           // 7.3.8.5
  pcm_sample_luma:[ushort]; pcm_sample_chroma:[ushort];   // 7.3.8.7, when pcm_flag = 1
  rqt_root_cbf:ubyte = 255;                       // 7.3.8.5 (255 when not present -> inferred 1)
  cu_qp_delta_abs:ubyte = 255; cu_qp_delta_sign_flag:ubyte = 255;   // 7.3.8.14 (first TU with cbf in the quantization group)
  CuQpDeltaVal:short = 0;                         // 7.4.9.14
  IsCuQpDeltaCoded:ubyte = 255;                   // 7.4.9.14
  qPY_PRED:byte = -1;                             // 8.6.1 eq. 8-284 (predicted luma QP for the quantization group)
  QpY:byte = -1;                                  // 8.6.1 eq. 8-285 (-QpBdOffsetY..51)
  QpY_prime:byte = -1;                            // Qp'Y = QpY + QpBdOffsetY (8.6.1)
  QpC_cb_prime:byte = -1; QpC_cr_prime:byte = -1; // Qp'Cb / Qp'Cr (8.6.1, Table 8-10 for ChromaArrayType 1)
  cu_chroma_qp_offset_flag:ubyte = 255; cu_chroma_qp_offset_idx:ubyte = 255;   // 7.3.8.14 (RExt)
  CuQpOffsetCb:byte = 0; CuQpOffsetCr:byte = 0;   // 7.4.9.14
  quant_group_x:ushort; quant_group_y:ushort;     // top-left of the quantization group (8.6.1, Log2MinCuQpDeltaSize)
  intra_chroma_pred_mode:[ubyte];                 // 7.3.8.5: 1 entry (ChromaArrayType != 3) or 4 entries (ChromaArrayType = 3 with PART_NxN); values 0..4
  IntraPredModeC:[ubyte];                         // derived (8.4.3, Table 8-2/8-3), same count
  palette:[ubyte];                                // nested HevcPalette FlatBuffer when palette_mode_flag = 1 (SCC); empty otherwise
  bit_count_x8:uint;                              // estimated CABAC cost of coding_unit() incl. its PUs/TUs in 1/8 bits (0 = unknown)
}

// SCC palette (7.3.8.13). Nested in HevcCu.palette.
table HevcPalette {
  palette_predictor_run:[uint]; num_signalled_palette_entries:ubyte; new_palette_entries:[ushort];   // [cIdx][i] flattened per component
  palette_escape_val_present_flag:ubyte; num_palette_indices_minus1:uint; palette_index_idc:[ubyte];
  copy_above_indices_for_final_run_flag:ubyte; palette_transpose_flag:ubyte;
  CurrentPaletteEntries:[ushort];                 // [cIdx][idx] flattened, numComps * CurrentPaletteSize
  PaletteIndexMap:[ubyte];                        // nCbS*nCbS indices, raster
}

// Prediction unit. Block.kind = PU. Block.partition_type = partIdx (0..3).
table HevcPu {
  partIdx:ubyte;                                  // 0..3 (7.3.8.6 invocation order)
  // intra (7.3.8.5; luma PB)
  prev_intra_luma_pred_flag:ubyte = 255; mpm_idx:ubyte = 255; rem_intra_luma_pred_mode:ubyte = 255;
  IntraPredModeY:ubyte = 255;                     // derived 8.4.2 (0..34)
  candModeList:[ubyte];                           // 3 entries: the MPM candidates derived in 8.4.2 (for "predicted mode used" overlay); empty if unknown
  // inter (7.3.8.6)
  merge_flag:ubyte = 255; merge_idx:ubyte = 255;
  inter_pred_idc:HevcInterPredIdc = HevcInterPredIdc_Unknown;
  ref_idx_l0:byte = -1; ref_idx_l1:byte = -1;
  mvd_l0_x:short = -32768; mvd_l0_y:short = -32768;   // mvd_coding(x0,y0,0) result MvdL0 (7.4.9.9), quarter-sample units (NOT ×4); -32768 n/a. Note mvd_l1_zero_flag forces MvdL1 = 0 for PRED_BI.
  mvd_l1_x:short = -32768; mvd_l1_y:short = -32768;
  mvp_l0_flag:ubyte = 255; mvp_l1_flag:ubyte = 255;
  mvL0_x:short = -32768; mvL0_y:short = -32768;   // derived mvL0 (8.5.3.2), quarter-sample units (NOT ×4)
  mvL1_x:short = -32768; mvL1_y:short = -32768;
  predFlagL0:ubyte = 255; predFlagL1:ubyte = 255; // 8.5.3.2.1
  refIdxL0:byte = -1; refIdxL1:byte = -1;         // derived (merge may differ from signalled ref_idx)
  merge_cand_source:HevcMergeCandSource = MC_Unknown;     // origin of the selected merge candidate (or of mvpLX for AMVP)
  ref_poc_l0:int = -2147483648; ref_poc_l1:int = -2147483648;  // PicOrderCntVal of the referenced pictures (INT_MIN = none)
  // weighted prediction actually applied (8.5.3.3.4.3): per component luma, cb, cr
  weighted:ubyte = 255;                           // 0 default, 1 explicit WP applied
  LumaWeightL0:short = -32768; luma_offset_l0:short = -32768; LumaWeightL1:short = -32768; luma_offset_l1:short = -32768;
  ChromaWeightL0:[short]; ChromaOffsetL0:[short]; ChromaWeightL1:[short]; ChromaOffsetL1:[short];   // 2 entries (Cb, Cr) or empty
}

// Transform unit (one node of the transform tree). Block.kind = TU. Block.partition_type = trafoDepth.
table HevcTu {
  trafoDepth:ubyte;                               // 7.3.8.8
  log2TrafoSize:ubyte;                            // 2..5
  blkIdx:ubyte;                                   // 0..3 within parent
  split_transform_flag:ubyte = 255;               // 7.3.8.8 (255 = inferred, see interSplitFlag / intra NxN)
  interSplitFlag:ubyte = 255;                     // 7.4.9.8 derived
  cbf_luma:ubyte = 255;                           // 7.3.8.8
  cbf_cb:[ubyte]; cbf_cr:[ubyte];                 // 1 entry, or 2 entries for ChromaArrayType = 2 (second chroma TB at y0 + (1 << (log2TrafoSize-1))); empty = not present/inferred 0
  transform_skip_flag:[ubyte];                    // 3 entries [Y, Cb, Cr] (7.3.8.11); 255 where not present
  explicit_rdpcm_flag:[ubyte]; explicit_rdpcm_dir_flag:[ubyte];   // 3 entries each (RExt 7.3.8.11)
  last_sig_coeff_x:[ubyte]; last_sig_coeff_y:[ubyte];   // 3 entries: LastSignificantCoeffX/Y (7.4.9.11) per component; 255 if cbf = 0
  num_nonzero:[ushort];                           // 3 entries [Y, Cb, Cr]: count of non-zero TransCoeffLevel per component (4:2:2: both chroma TBs of a component summed)
  coeff_abs_sum:[uint];                           // 3 entries: sum of |TransCoeffLevel| per component (coefficient energy proxy)
  sign_data_hiding_used:[ubyte];                  // 3 entries: 1 if signHidden was 1 in any sub-block (7.4.9.11)
  dst_used:ubyte = 255;                           // 1 when the 4x4 luma intra DST-VII (8.6.4.2 trType = 1) was applied
  cross_component_prediction_alpha:[byte];        // 2 entries (Cb, Cr): ResScaleVal (RExt 7.4.9.12); empty if disabled
  scaling_list_used:ubyte = 255;                  // 1 if scaling_list_enabled_flag and not transform skip / bypass (8.6.4.2 m[x][y] != 16)
  coeff_blob_id:string;                           // PlaneStage.Coefficients blob holding this TU's dequantised coefficients (empty if not exported)
  bit_count_x8:uint;                              // estimated CABAC cost of transform_unit() incl. residual_coding() in 1/8 bits (0 = unknown)
}

// Picture-level filter parameters for HEVC. FrameAnalysis.filter_params_type = "HevcFilterParams".
table HevcFilterParams {
  // deblocking (7.4.3.3.1 PPS, 7.4.7.1 slice)
  pps_deblocking_filter_disabled_flag:ubyte; pps_beta_offset_div2:byte; pps_tc_offset_div2:byte;
  deblocking_filter_override_enabled_flag:ubyte;
  slice_deblocking_filter_disabled_flag:[ubyte];  // per slice segment (PartitionInfo.index order)
  slice_beta_offset_div2:[byte]; slice_tc_offset_div2:[byte];   // per slice segment
  pps_loop_filter_across_slices_enabled_flag:ubyte; loop_filter_across_tiles_enabled_flag:ubyte;
  pcm_loop_filter_disabled_flag:ubyte;            // SPS 7.4.3.2.1
  // SAO (7.4.3.2.1 / 7.4.7.1)
  sample_adaptive_offset_enabled_flag:ubyte;
  slice_sao_luma_flag:[ubyte]; slice_sao_chroma_flag:[ubyte];   // per slice segment
  // deblocking edge map: 1 bit per 8x8 grid edge, 0 = not filtered. Layout: edges_v has (cells_w/2) x (cells_h/2) bits raster for vertical edges at x multiple of 8, edges_h same for horizontal edges.
  edge_flags_v:[ubyte]; edge_flags_h:[ubyte];     // bit-packed, LSB first; empty = unknown (per-cell bS is in CellGrid.deblock_bs_*)
  film_grain_characteristics:SyntaxNode;          // film grain characteristics SEI (D.2.13 / D.3.13) if present
}

// Reference picture set state for HEVC (8.3.2). FrameInfo.ref_state_type = "HevcRps".
table HevcRpsPic { frame:FrameId; PicOrderCntVal:int; used_by_curr_pic:bool; is_long_term:bool; delta_poc_msb_present_flag:ubyte = 255; }
table HevcRps {
  PicOrderCntVal:int;                             // 8.3.1
  short_term_ref_pic_set_sps_flag:ubyte = 255; short_term_ref_pic_set_idx:ubyte = 255;   // 7.3.6.1
  st_ref_pic_set:SyntaxNode;                      // st_ref_pic_set(num_short_term_ref_pic_sets) as parsed when signalled in the slice header
  NumPicTotalCurr:ubyte;                          // 7.4.7.2
  RefPicSetStCurrBefore:[HevcRpsPic]; RefPicSetStCurrAfter:[HevcRpsPic]; RefPicSetStFoll:[HevcRpsPic];
  RefPicSetLtCurr:[HevcRpsPic]; RefPicSetLtFoll:[HevcRpsPic];    // 8.3.2 (entries with decode_index = -1 are "no reference picture")
  RefPicList0:[HevcRpsPic]; RefPicList1:[HevcRpsPic];            // 8.3.4, after list modification; first independent slice segment
  collocated_from_l0_flag:ubyte = 255; collocated_ref_idx:ubyte = 255;   // 7.3.6.1 (TMVP collocated picture)
  NoRaslOutputFlag:ubyte = 255; pic_output_flag:ubyte = 255;     // 8.1.3 / 7.4.7.1
  dpb_fullness_after:ubyte;                       // number of pictures in the DPB after this picture (C.5.2.2)
  sps_max_dec_pic_buffering_minus1:ubyte; sps_max_num_reorder_pics:ubyte; sps_max_latency_increase_plus1:uint;   // for HighestTid (7.4.3.2.1)
}

// =====================================================================================
// ch. 6 additions — AV1 extension tables
// Spec: AV1 Bitstream & Decoding Process Specification v1.0.0 Errata 1. Constants per spec Section 3 / 6.10.
// =====================================================================================

enum Av1Partition : ubyte {                       // 6.10.4
  PARTITION_NONE = 0, PARTITION_HORZ = 1, PARTITION_VERT = 2, PARTITION_SPLIT = 3,
  PARTITION_HORZ_A = 4, PARTITION_HORZ_B = 5, PARTITION_VERT_A = 6, PARTITION_VERT_B = 7,
  PARTITION_HORZ_4 = 8, PARTITION_VERT_4 = 9, Av1Partition_Unknown = 255
}
enum Av1BlockSize : ubyte {                       // 6.10.4 (BLOCK_* / subSize)
  BLOCK_4X4 = 0, BLOCK_4X8 = 1, BLOCK_8X4 = 2, BLOCK_8X8 = 3, BLOCK_8X16 = 4, BLOCK_16X8 = 5, BLOCK_16X16 = 6,
  BLOCK_16X32 = 7, BLOCK_32X16 = 8, BLOCK_32X32 = 9, BLOCK_32X64 = 10, BLOCK_64X32 = 11, BLOCK_64X64 = 12,
  BLOCK_64X128 = 13, BLOCK_128X64 = 14, BLOCK_128X128 = 15, BLOCK_4X16 = 16, BLOCK_16X4 = 17, BLOCK_8X32 = 18,
  BLOCK_32X8 = 19, BLOCK_16X64 = 20, BLOCK_64X16 = 21, Av1BlockSize_Unknown = 255
}
enum Av1YMode : ubyte {                           // 6.10.22 (YMode) — intra 0..12, UV_CFL_PRED 13 (uv only), inter 13..24 (same numeric space as the spec)
  DC_PRED = 0, V_PRED = 1, H_PRED = 2, D45_PRED = 3, D135_PRED = 4, D113_PRED = 5, D157_PRED = 6, D203_PRED = 7,
  D67_PRED = 8, SMOOTH_PRED = 9, SMOOTH_V_PRED = 10, SMOOTH_H_PRED = 11, PAETH_PRED = 12,
  NEARESTMV = 13, NEARMV = 14, GLOBALMV = 15, NEWMV = 16,
  NEAREST_NEARESTMV = 17, NEAR_NEARMV = 18, NEAREST_NEWMV = 19, NEW_NEARESTMV = 20, NEAR_NEWMV = 21, NEW_NEARMV = 22,
  GLOBAL_GLOBALMV = 23, NEW_NEWMV = 24, Av1YMode_Unknown = 255
}
// UVMode shares 0..12 with Av1YMode and adds UV_CFL_PRED = 13 (6.10.22).
enum Av1FilterIntraMode : ubyte { FILTER_DC_PRED = 0, FILTER_V_PRED = 1, FILTER_H_PRED = 2, FILTER_D157_PRED = 3, FILTER_PAETH_PRED = 4, Av1FilterIntraMode_Unknown = 255 }   // 6.10.23
enum Av1InterIntraMode : ubyte { II_DC_PRED = 0, II_V_PRED = 1, II_H_PRED = 2, II_SMOOTH_PRED = 3, Av1InterIntraMode_Unknown = 255 }   // 6.10.28
enum Av1MotionMode : ubyte { SIMPLE = 0, OBMC = 1, LOCALWARP = 2, Av1MotionMode_Unknown = 255 }   // 6.10.27
enum Av1CompoundType : ubyte { COMPOUND_WEDGE = 0, COMPOUND_DIFFWTD = 1, COMPOUND_AVERAGE = 2, COMPOUND_INTRA = 3, COMPOUND_DISTANCE = 4, Av1CompoundType_Unknown = 255 }   // 6.10.31
enum Av1CompRefType : ubyte { UNIDIR_COMP_REFERENCE = 0, BIDIR_COMP_REFERENCE = 1, Av1CompRefType_Unknown = 255 }   // 6.10.25
enum Av1InterpFilter : ubyte { EIGHTTAP = 0, EIGHTTAP_SMOOTH = 1, EIGHTTAP_SHARP = 2, BILINEAR = 3, SWITCHABLE = 4, Av1InterpFilter_Unknown = 255 }   // 6.8.9 / 6.10.33
enum Av1TxSize : ubyte {                          // 6.10.16 / Section 3 constants (TX_*)
  TX_4X4 = 0, TX_8X8 = 1, TX_16X16 = 2, TX_32X32 = 3, TX_64X64 = 4, TX_4X8 = 5, TX_8X4 = 6, TX_8X16 = 7, TX_16X8 = 8,
  TX_16X32 = 9, TX_32X16 = 10, TX_32X64 = 11, TX_64X32 = 12, TX_4X16 = 13, TX_16X4 = 14, TX_8X32 = 15, TX_32X8 = 16,
  TX_16X64 = 17, TX_64X16 = 18, Av1TxSize_Unknown = 255
}
enum Av1TxType : ubyte {                          // 6.10.19 (TxType)
  DCT_DCT = 0, ADST_DCT = 1, DCT_ADST = 2, ADST_ADST = 3, FLIPADST_DCT = 4, DCT_FLIPADST = 5, FLIPADST_FLIPADST = 6,
  ADST_FLIPADST = 7, FLIPADST_ADST = 8, IDTX = 9, V_DCT = 10, H_DCT = 11, V_ADST = 12, H_ADST = 13, V_FLIPADST = 14, H_FLIPADST = 15,
  WHT_WHT = 16,                                   // OVT: lossless (Lossless = 1 uses the Walsh-Hadamard transform, 7.13.2.10); not a TxType symbol
  Av1TxType_Unknown = 255
}
enum Av1RefFrame : byte { NONE = -1, INTRA_FRAME = 0, LAST_FRAME = 1, LAST2_FRAME = 2, LAST3_FRAME = 3, GOLDEN_FRAME = 4, BWDREF_FRAME = 5, ALTREF2_FRAME = 6, ALTREF_FRAME = 7 }   // 6.10.24
enum Av1RestorationType : ubyte { RESTORE_NONE = 0, RESTORE_WIENER = 1, RESTORE_SGRPROJ = 2, RESTORE_SWITCHABLE = 3, Av1RestorationType_Unknown = 255 }   // 6.10.15 (FrameRestorationType); per-unit restoration_type uses 0..2
enum Av1GmType : ubyte { IDENTITY = 0, TRANSLATION = 1, ROTZOOM = 2, AFFINE = 3 }   // 6.8.17 (GmType)
enum Av1FrameType : ubyte { KEY_FRAME = 0, INTER_FRAME = 1, INTRA_ONLY_FRAME = 2, SWITCH_FRAME = 3 }   // 6.8.2

// One AV1 block (decode_block, 5.11.5). Block.kind = SB for the superblock node (ext_type "Av1Block" with is_superblock = 1),
// Block for every other partition node and leaf. Block.partition_type = Av1Block.partition (the partition symbol read AT this node;
// PARTITION_NONE for leaves).
table Av1Block {
  MiRow:uint; MiCol:uint;                         // 5.11.5 (4x4 units == cell coordinates)
  bsize:Av1BlockSize = Av1BlockSize_Unknown;      // MiSize (6.10.5)
  partition:Av1Partition = Av1Partition_Unknown;  // partition symbol read at this square node (5.11.4); PARTITION_NONE for leaves
  is_superblock:ubyte = 0;                        // 1 for the 64x64/128x128 root node (use_128x128_superblock, 5.5.2)
  tile_idx:ushort = 65535;                        // PartitionInfo.index of the tile
  // mode info (5.11.6–5.11.8)
  skip_mode:ubyte = 255;                          // 5.11.10
  skip:ubyte = 255;                               // 5.11.11 (no residual)
  segment_id:ubyte = 255; seg_id_predicted:ubyte = 255;   // 5.11.9 / 5.11.19
  is_inter:ubyte = 255;                           // 5.11.20
  use_intrabc:ubyte = 255;                        // 5.11.7
  CurrentQIndex:short = -1;                       // 6.10.13 after delta_qindex (0..255); equals base_q_idx when delta_q_present = 0
  delta_qindex:byte = 0;                          // delta_q_abs/sign resolved (5.11.12) read at this block (0 if none)
  DeltaLF:[byte];                                 // 4 entries: DeltaLF[0..3] after delta_lf (5.11.13); empty if delta_lf_present = 0
  qindex_dc_y:short = -1; qindex_ac_y:short = -1; // Dc_Qlookup/Ac_Qlookup results for this block's qindex (7.12.2) — the actual quantizer step
  Lossless:ubyte = 255;                           // LosslessArray[segment_id] (6.8.10)
  // intra (5.11.22–5.11.24, 5.11.42–5.11.45)
  YMode:Av1YMode = Av1YMode_Unknown;              // intra_frame_y_mode / y_mode (6.10.22)
  AngleDeltaY:byte = 0; AngleDeltaUV:byte = 0;    // angle_delta_y / angle_delta_uv (-3..3), 5.11.42
  UVMode:ubyte = 255;                             // uv_mode 0..13 (13 = UV_CFL_PRED)
  CflAlphaU:byte = 0; CflAlphaV:byte = 0;         // derived from cfl_alpha_signs / cfl_alpha_u / cfl_alpha_v (5.11.44), -16..16
  cfl_alpha_signs:ubyte = 255;
  use_filter_intra:ubyte = 255; filter_intra_mode:Av1FilterIntraMode = Av1FilterIntraMode_Unknown;   // 5.11.24
  PaletteSizeY:ubyte = 0; PaletteSizeUV:ubyte = 0;   // 5.11.45 (0 = no palette; 2..8)
  palette:[ubyte];                                // nested Av1Palette FlatBuffer when PaletteSizeY or PaletteSizeUV > 0
  use_intrabc_mv_x:short = -32768; use_intrabc_mv_y:short = -32768;   // intra block copy Mv in 1/8 units (multiples of 8)
  // inter (5.11.25–5.11.33)
  RefFrame:[byte];                                // 2 entries: RefFrame[0], RefFrame[1] (Av1RefFrame; -1 NONE)
  comp_mode:ubyte = 255;                          // 5.11.25: 0 SINGLE_REFERENCE, 1 COMPOUND_REFERENCE
  comp_ref_type:Av1CompRefType = Av1CompRefType_Unknown;
  drl_mode_idx:ubyte = 255;                       // RefMvIdx after read_drl_mode (5.11.23)
  NumMvFound:ubyte = 255;                         // 7.10.2 (size of the MV stack) for "why this MV" views
  Mv:[short];                                     // 4 entries: Mv[0].row, Mv[0].col, Mv[1].row, Mv[1].col — 1/8 luma units (NOT ×2). Empty for intra.
  Mvd:[short];                                    // 4 entries, same order: signalled diff from read_mv (5.11.32) in 1/8 units; -32768 where not NEWMV
  mv_joint:[ubyte];                               // up to 2 entries (per ref list that used NEWMV): MvJoint (6.10.29)
  interintra:ubyte = 255; interintra_mode:Av1InterIntraMode = Av1InterIntraMode_Unknown; wedge_interintra:ubyte = 255;
  wedge_index:ubyte = 255; wedge_sign:ubyte = 255;   // 5.11.28 / 5.11.31
  mask_type:ubyte = 255;                          // COMPOUND_DIFFWTD mask_type (5.11.31)
  motion_mode:Av1MotionMode = Av1MotionMode_Unknown;   // 5.11.27
  LocalValid:ubyte = 255; LocalWarpParams:[int];  // 6 entries (7.11.3.8 warpEstimation) when motion_mode = LOCALWARP
  comp_group_idx:ubyte = 255; compound_idx:ubyte = 255;   // 5.11.31
  compound_type:Av1CompoundType = Av1CompoundType_Unknown;   // derived CompoundType (6.10.31)
  FwdWeight:ubyte = 255; BckWeight:ubyte = 255;   // COMPOUND_DISTANCE weights (7.11.3.15), 0..16
  interp_filter:[Av1InterpFilter];                // 2 entries: interp_filter[dir] as read in inter_block_mode_info (5.11.23; both equal when enable_dual_filter = 0 or is_filter_switchable = 0)
  is_global:ubyte = 255;                          // 1 when the MV came from gm_params (GLOBALMV / GLOBAL_GLOBALMV with GmType > TRANSLATION)
  // transform (5.11.15–5.11.17)
  TxSize:Av1TxSize = Av1TxSize_Unknown;           // luma TxSize at the block level (read_block_tx_size / read_tx_size)
  tx_depth:ubyte = 255;                           // 5.11.16 (intra / non-var-tx)
  txfm_split_tree:[ubyte];                        // var-tx tree (5.11.17): pre-order list of txfm_split flags; empty if not used
  TxType:Av1TxType = Av1TxType_Unknown;           // first luma TXB's TxType (per-TXB values in Av1Txb)
  // CDEF index for the 64x64 containing this block (5.11.56)
  cdef_idx:byte = -1;
  // reference order hints of the block's refs, for display ("LAST (hint 12)")
  RefOrderHint:[int];                             // 2 entries (-1 when RefFrame NONE/INTRA)
  bit_count_x8:uint;                              // accounting: sum of AccountingSymbol.bits for symbols with context (MiRow, MiCol) in this block, 1/8 bits (0 = unknown)
}

// Palette colours and index map (5.11.45, 5.11.49). Nested in Av1Block.palette.
table Av1Palette {
  palette_colors_y:[ushort];                      // PaletteSizeY entries (palette_colors_y[], 5.11.45)
  palette_colors_u:[ushort]; palette_colors_v:[ushort];   // PaletteSizeUV entries each
  ColorMapY:[ubyte];                              // bw*bh indices (7.11.4), raster, block-sized; empty if not exported
  ColorMapUV:[ubyte];                             // chroma block indices
}

// One transform block (transform_block, 5.11.35 / coeffs 5.11.39). Block.kind = TXB. Block.partition_type = plane (0 Y, 1 U, 2 V).
table Av1Txb {
  plane:ubyte;                                    // 0 Y, 1 U, 2 V
  startX:uint; startY:uint;                       // in samples of the plane (5.11.35)
  txSz:Av1TxSize = Av1TxSize_Unknown;             // TxSize of this TXB (chroma: get_tx_size(plane, txSz), 5.11.37)
  TxType:Av1TxType = Av1TxType_Unknown;           // compute_tx_type result (5.11.40)
  all_zero:ubyte = 255;                           // 5.11.39
  eob:ushort = 0;                                 // end of block position (number of coefficients coded), 5.11.39
  dc_sign:ubyte = 255;                            // sign of the DC coefficient (0 +, 1 -), 255 when dc = 0
  dc_level:int = 0;                               // Dequant[0][0] (dequantised DC)
  coeff_abs_sum:uint = 0;                         // sum |Quant[i]| (pre-dequant levels) — energy proxy
  num_nonzero:ushort = 0;
  qmLevel:ubyte = 255;                            // quantizer matrix level in use (qm_y/qm_u/qm_v) when using_qmatrix; 255 none
  coeff_blob_id:string;                           // PlaneStage.Coefficients blob for this TXB (empty if not exported)
  bit_count_x8:uint;                              // accounting cost of this TXB's symbols (0 = unknown)
}

// Loop filter / CDEF / LR / superres / film grain for one AV1 frame. FrameAnalysis.filter_params_type = "Av1FilterParams".
table Av1LrUnit {                                 // one loop restoration unit (5.11.57 / 5.11.58)
  plane:ubyte; unitRow:ushort; unitCol:ushort;    // 7.17 (unit grid per plane, LoopRestorationSize)
  rect:Rect;                                      // luma-coordinate rectangle of the unit (chroma units scaled to luma for display)
  restoration_type:Av1RestorationType = Av1RestorationType_Unknown;   // RESTORE_NONE / WIENER / SGRPROJ (per-unit, 5.11.57)
  LrWiener:[short];                               // 6 entries: LrWiener[plane][unitRow][unitCol][pass 0..1][0..2] (7 symmetric taps; 5.11.58)
  LrSgrSet:ubyte = 255;                           // 5.11.58 (0..15)
  LrSgrXqd:[short];                               // 2 entries: LrSgrXqd[..][0..1]
}
table Av1CdefParams {
  cdef_damping_minus_3:ubyte; cdef_bits:ubyte;    // 5.9.19
  CdefDamping:ubyte;                              // = cdef_damping_minus_3 + 3
  cdef_y_pri_strength:[ubyte]; cdef_y_sec_strength:[ubyte];   // (1 << cdef_bits) entries each; sec strength 3 means 4 (5.9.19)
  cdef_uv_pri_strength:[ubyte]; cdef_uv_sec_strength:[ubyte];
  cdef_idx:[byte];                                // one entry per 64x64 in raster order (ceil(w/64)*ceil(h/64)); -1 = not signalled (all skip)
  cdef_dir:[ubyte];                               // derived direction per 8x8 (7.15.2), raster, (cells_w/2)*(cells_h/2) entries; empty if not exported
  cdef_var:[uint];                                // derived variance per 8x8 (7.15.2); empty if not exported
}
table Av1GmParams { ref:Av1RefFrame; GmType:Av1GmType; gm_params:[int]; }   // 6 entries gm_params[ref][0..5] (5.9.24, 7.11.3.1 global motion warp)
table Av1FilmGrainParams {                        // 5.9.30 / 6.8.20
  apply_grain:ubyte; grain_seed:ushort; update_grain:ubyte = 255; film_grain_params_ref_idx:ubyte = 255;
  num_y_points:ubyte; point_y_value:[ubyte]; point_y_scaling:[ubyte];
  chroma_scaling_from_luma:ubyte; num_cb_points:ubyte; point_cb_value:[ubyte]; point_cb_scaling:[ubyte];
  num_cr_points:ubyte; point_cr_value:[ubyte]; point_cr_scaling:[ubyte];
  grain_scaling_minus_8:ubyte; ar_coeff_lag:ubyte;
  ar_coeffs_y_plus_128:[ubyte]; ar_coeffs_cb_plus_128:[ubyte]; ar_coeffs_cr_plus_128:[ubyte];
  ar_coeff_shift_minus_6:ubyte; grain_scale_shift:ubyte;
  cb_mult:ubyte; cb_luma_mult:ubyte; cb_offset:ushort; cr_mult:ubyte; cr_luma_mult:ubyte; cr_offset:ushort;
  overlap_flag:ubyte; clip_to_restricted_range:ubyte;
}
table Av1FilterParams {
  // deblocking (5.9.11 loop_filter_params)
  loop_filter_level:[ubyte];                      // 4 entries: [0] Y vertical, [1] Y horizontal, [2] U, [3] V
  loop_filter_sharpness:ubyte;
  loop_filter_delta_enabled:ubyte; loop_filter_delta_update:ubyte;
  loop_filter_ref_deltas:[byte];                  // 8 entries (INTRA_FRAME..ALTREF_FRAME)
  loop_filter_mode_deltas:[byte];                 // 2 entries
  delta_lf_present:ubyte; delta_lf_res:ubyte; delta_lf_multi:ubyte;   // 5.9.18
  lvl_per_cell:[ubyte];                           // derived filter level per 4x4 for Y vertical edges (7.14.4); empty if not exported
  // CDEF (5.9.19)
  enable_cdef:ubyte; cdef:Av1CdefParams;
  // loop restoration (5.9.20)
  enable_restoration:ubyte;
  FrameRestorationType:[Av1RestorationType];      // 3 entries per plane (6.10.15)
  LoopRestorationSize:[ushort];                   // 3 entries: 64 / 128 / 256 per plane
  lr_units:[Av1LrUnit];                           // every unit of every plane, plane-major then raster
  // superres (5.9.8)
  use_superres:ubyte; coded_denom:ubyte = 255; SuperresDenom:ubyte = 8; FrameWidth:uint; UpscaledWidth:uint;   // FrameWidth = downscaled coded width
  // quantization (5.9.12) — frame level
  base_q_idx:ubyte; DeltaQYDc:byte; DeltaQUDc:byte; DeltaQUAc:byte; DeltaQVDc:byte; DeltaQVAc:byte;
  using_qmatrix:ubyte; qm_y:ubyte = 255; qm_u:ubyte = 255; qm_v:ubyte = 255;
  delta_q_present:ubyte; delta_q_res:ubyte;      // 5.9.17
  // segmentation (5.9.14)
  segmentation_enabled:ubyte; segmentation_update_map:ubyte = 255; segmentation_temporal_update:ubyte = 255;
  FeatureEnabled:[ubyte];                         // 8 segments * 8 features, [seg*8 + feature]; feature ids per Section 3 constants: SEG_LVL_ALT_Q = 0, SEG_LVL_ALT_LF_Y_V = 1, ALT_LF_Y_H = 2, ALT_LF_U = 3, ALT_LF_V = 4, SEG_LVL_REF_FRAME = 5, SEG_LVL_SKIP = 6, SEG_LVL_GLOBALMV = 7
  FeatureData:[short];                            // 64 entries, same layout
  // TX mode / reference modes (5.9.21–5.9.23)
  TxMode:ubyte;                                   // 0 ONLY_4X4, 1 TX_MODE_LARGEST, 2 TX_MODE_SELECT (6.8.21)
  reference_select:ubyte; skip_mode_present:ubyte; SkipModeFrame:[byte];   // 2 entries (6.8.22) or empty
  allow_warped_motion:ubyte; reduced_tx_set:ubyte;
  interpolation_filter:Av1InterpFilter = Av1InterpFilter_Unknown;   // frame-level (5.9.10)
  // global motion (5.9.24)
  gm:[Av1GmParams];                               // 7 entries (LAST_FRAME..ALTREF_FRAME)
  // film grain (5.9.30)
  film_grain:Av1FilmGrainParams;                  // null when film_grain_params_present = 0 or apply_grain = 0
}

// Reference slot state after this frame (7.20 reference frame update). FrameInfo.ref_state_type = "Av1RefState".
table Av1RefSlot {
  slot:ubyte;                                     // 0..7
  RefValid:ubyte; RefFrameId:int = -1; RefFrameType:Av1FrameType; RefOrderHint:int = -1;   // 7.20
  frame:FrameId;                                  // the frame stored in this slot (decode_index -1 if invalid)
  RefUpscaledWidth:uint; RefFrameHeight:uint; RefRenderWidth:uint; RefRenderHeight:uint; RefMiCols:uint; RefMiRows:uint;
  RefBitDepth:ubyte; RefSubsamplingX:ubyte; RefSubsamplingY:ubyte;
  saved_gm_valid:bool; saved_segmentation:bool; saved_film_grain:bool;   // which side state was saved (7.20)
}
table Av1RefState {
  frame_type:Av1FrameType; show_frame:ubyte; showable_frame:ubyte; show_existing_frame:ubyte; frame_to_show_map_idx:ubyte = 255;
  error_resilient_mode:ubyte; disable_cdf_update:ubyte; primary_ref_frame:ubyte;   // 5.9.2 (primary_ref_frame 7 = PRIMARY_REF_NONE)
  refresh_frame_flags:ubyte;                      // 5.9.2 bit mask (after this frame)
  OrderHint:int; ref_order_hint:[int];            // 8 entries ref_order_hint[i] when signalled (error resilient), else empty
  ref_frame_idx:[ubyte];                          // 7 entries: slot used for LAST_FRAME..ALTREF_FRAME (5.9.2); empty for intra frames
  OrderHints:[int];                               // 8 entries: OrderHints[refFrame] for INTRA_FRAME..ALTREF_FRAME (6.8.2); -1 when not applicable
  RefFrameSignBias:[ubyte];                       // 8 entries (6.8.2, via get_relative_dist() 5.9.3): 0 = forward (past) reference, 1 = backward (future)
  use_ref_frame_mvs:ubyte; allow_high_precision_mv:ubyte; force_integer_mv:ubyte;
  SkipModeFrame:[byte];                           // 2 entries or empty (6.8.22)
  slots_after:[Av1RefSlot];                       // 8 entries: slot state after reference frame update (7.20)
  current_frame_id:int = -1;                      // 5.9.2 when frame_id_numbers_present_flag
  temporal_id:ubyte; spatial_id:ubyte;            // from obu_extension_header
}

// =====================================================================================
// CLI / API report tables (consumed by chapter 12; defined here because chapter 06 owns every schema).
// Root tables carry their own file_identifier (§6.1.1); the others are nested or streamed as records
// inside a CliEnvelope stream (chapter 12 §12.1.4).
// =====================================================================================
table CliEnvelope {                              // used by chapter 12 §12.1.4 (first record of every fb/json stream)
  ovt_version:string; schema_version:uint; command:string; input_path:string; input_size:ulong; source_hash:[ubyte];
  record_count:uint;                             // number of result records that follow
  diagnostics:[Diagnostic]; status:string;
}
table FrameHash     { frame:FrameId; md5:string; }                                     // used by chapter 12 §12.2.6 (`ovt dump --md5`)
table DumpOutput    { stage:PlaneStage; path:string; container:string; width:uint; height:uint; chroma_format:ubyte; bit_depth:ubyte;
                      frames_written:uint; md5:[FrameHash];
                      sequence_md5:string; }                                           // used by chapter 12 §12.2.6; sequence_md5 = MD5 over all per-frame hashed bytes in output order (`--md5` last line `sequence,<md5>`)
table DumpManifest  { outputs:[DumpOutput]; decoder_id:string; decoder_version:string; order:string; crop:bool; }   // used by chapter 12 §12.2.6
table ExtractManifest { path:string; format:string; units:[uint]; bytes:ulong; frames:[FrameId]; decode_chain_length:uint; }   // used by chapter 12 §12.2.7
table PluginInfo    { id:string; kind:string /* "analysis","metric","decoder" */; codec:Codec; version:string; license:string;
                      abi_version:uint; path:string /* "(built-in)" for static */; sha256:string; plane_stages:[string]; grid_fields:[string];
                      bit_exact:bool; has_symbol_accounting:bool; notes:string; }      // used by chapter 12 §12.2.14, §12.3.11
table PluginList    { plugins:[PluginInfo]; search_paths:[string]; }                   // root table, file_identifier "OVTP"; used by chapter 12 §12.2.14
table HdrFrameMeasure { frame:FrameId; max_nits:float; fall_nits:float; p99_nits:float; min_nits:float; pct_above_threshold:float; histogram:[uint]; }   // used by chapter 12 §12.2.11
table HdrDynamicRecord { frame:FrameId; kind:string /* "hdr10plus","dv_rpu" */; present:bool; profile:ubyte; level:ubyte; raw_size:uint; }          // used by chapter 12 §12.2.11
table HdrReport     { declared:[ubyte] /* ovt_ColorDescription as little-endian struct image, sizeof = 44 bytes for the §0.5.6 layout with the leading uint32_t struct_size (D4) */; declared_json:string; sources:[string];
                      conflicts:[Diagnostic]; measures:[HdrFrameMeasure]; max_cll_measured:float; max_fall_measured:float;
                      dynamic:[HdrDynamicRecord]; threshold_nits:float; }              // used by chapter 12 §12.2.11 (`ovt hdr`)
table VerifyFrame   { frame:FrameId; ok:bool; plane_mismatch_mask:ubyte; max_abs_diff:uint; first_mismatch_x:int; first_mismatch_y:int;
                      declared_hash:string; computed_hash:string; }                    // used by chapter 12 §12.2.12
table VerifyCheck   { name:string /* "hash", "levels", "against" */; status:string /* "pass" | "fail" | "skipped" */; detail:string; }   // used by chapter 12 §12.2.12
table VerifyReport  { mode:string /* "hash_sei","against" */; plugin:string; oracle:string; tolerance:uint; frames:[VerifyFrame];
                      frames_ok:uint; frames_failed:uint; checks:[VerifyCheck]; }       // used by chapter 12 §12.2.12 (`ovt verify`); one VerifyCheck per requested check
table CacheEntry    { key:string; kind:string; source_basename:string; size_bytes:ulong; created:string; last_used:string; }   // used by chapter 12 §12.2.13
table CacheReport   { dir:string; entries:[CacheEntry]; total_bytes:ulong; max_bytes:ulong; hits:ulong; misses:ulong; }        // used by chapter 12 §12.2.13 (`ovt cache`)
table HrdPoint      { decode_index:uint; init_arrival_time_90k:ulong; final_arrival_time_90k:ulong; removal_time_90k:ulong;
                      cpb_fullness_bits:ulong; overflow:bool; underflow:bool; }                 // one access unit of the CPB trace; times in 90 kHz ticks (chapter 7 §7.4.8)
table HrdTrace      { frame:[HrdPoint]; bit_rate_bps:ulong; cpb_size_bits:ulong; sched_sel_idx:ubyte; nal_hrd:bool /* false = VCL HRD */;
                      cbr:bool; low_delay_hrd:bool; conformance_type:ubyte /* 1 = type I, 2 = type II */; underflow_count:uint; overflow_count:uint;
                      diagnostics:[Diagnostic]; }                                      // used by chapter 12 §12.2.21 (`ovt hrd`); built by chapter 7 WI-4.112
table DoctorReport  { build_json:string; plugins:PluginList; hw_decoders:[string]; gpu_json:string; displays_json:string; cpu_features:[string];
                      env:[string]; cache:CacheReport; }                                // used by chapter 12 §12.2.16 (`ovt doctor`)

root_type FrameAnalysis;
file_identifier "OVTA";
file_extension "fa";
```

### 6.3.1 Encoding of the neutral `CellGrid` / `Block` fields per codec

The neutral vectors are "codec-numbered": the consumer needs `FrameInfo`-level codec knowledge only to *label* values, never to *place* them. The following tables are the single source for those labels (the renderer's legend strings come from `ovt::model::label_for(field, codec, value)` which implements exactly these tables).

**`CellGrid.pred` / `Block.pred` (`PredKind`)**

| PredKind | H.264 | HEVC | AV1 |
|---|---|---|---|
| `Intra` | I_NxN, I_16x16_* | `CuPredMode = MODE_INTRA`, not PCM/palette | `is_inter = 0`, not intrabc/palette |
| `Inter` | P_*/B_* not skip | `MODE_INTER`, `cu_skip_flag = 0` | `is_inter = 1`, `skip_mode = 0` |
| `Skip` | P_Skip / B_Skip | `cu_skip_flag = 1` | `skip_mode = 1` (note: AV1 `skip` = "no residual" is **not** PredKind.Skip; it is `Block.skip`). Stock libaom inspection does not expose `skip_mode` (chapter 7 §7.6.1), so without the OVT extension patch `ovt-plugin-aom` reports such blocks as `Inter` and raises a Warning Diagnostic (empty code) `"skip_mode not available"` on the frame (`Block` has no `derived` field, issue #360); the GUI legend says so |
| `IntraBC` | — | `pps_curr_pic_ref_enabled_flag` CU predicting from the current picture | `use_intrabc = 1` |
| `PCM` | I_PCM | `pcm_flag = 1` | — |
| `Palette` | — | `palette_mode_flag = 1` | `PaletteSizeY > 0` or `PaletteSizeUV > 0` |

`Block.skip` = H.264 skipped MB; HEVC `cu_skip_flag`; AV1 `skip` (no residual). `Block.merge_or_direct` = H.264 direct (B_Direct_16x16 / B_Direct_8x8 / B_Skip); HEVC `merge_flag = 1`; AV1 `YMode ∈ {NEARESTMV, NEARMV, GLOBALMV, NEAREST_NEARESTMV, NEAR_NEARMV, GLOBAL_GLOBALMV}` (MV fully predicted, no `NEWMV` component).

**`CellGrid.intra_mode_y` (`short`, −1 = not intra)**

| Codec | Value |
|---|---|
| H.264 | `0..8` = `Intra4x4PredMode`/`Intra8x8PredMode` of the 4×4/8×8 containing the cell; `16 + Intra16x16PredMode` (16..19) for I_16x16; `32` for I_PCM |
| HEVC | `IntraPredModeY` 0..34 of the PB containing the cell; `64` for PCM; `65` for palette |
| AV1 | `YMode` 0..12 (intra); `32` for intrabc; `33` for palette-coded luma (the `YMode` is still stored in `Av1Block.YMode`) |

**`CellGrid.intra_mode_c`**: H.264 `intra_chroma_pred_mode` 0..3; HEVC `IntraPredModeC` 0..34; AV1 `UVMode` 0..13 (13 = UV_CFL_PRED). −1 when not intra or 4:0:0.

**`CellGrid.qp`**: H.264 `QPY` (0..51; may be negative for high bit depth); HEVC `QpY` (−QpBdOffsetY..51); AV1 `CurrentQIndex` (0..255). `FrameAnalysis.qp_hist` bins are `qp + 128` clamped to 0..255 for H.26x so negatives fit, and `qindex` directly for AV1.

**`CellGrid.tx_log2_w/h`**: log2 of the transform block covering the cell (H.264: 2 or 3; HEVC: 2..5; AV1: luma `TxSize` width/height log2, 2..6). `0xFF` when no transform (skip / PCM / `all_zero` with no TXB); AV1 keeps the signalled `TxSize` for skip blocks (chapter 7 §7.6.1, issue #360).

**`CellGrid.tx_type` (`ubyte`)**

| Codec | Value |
|---|---|
| H.264 | `0` = 4×4 integer DCT, `1` = 8×8 integer DCT, `2` = I_16x16 (4×4 + Hadamard DC), `3` = PCM (none), `0xFF` unknown |
| HEVC | `0` = DCT-II, `1` = DST-VII (4×4 intra luma, §8.6.4.2 `trType = 1`), `2` = `transform_skip_flag = 1`, `3` = `cu_transquant_bypass_flag = 1`, `0xFF` unknown |
| AV1 | `Av1TxType` 0..15, `16` = WHT (lossless), `0xFF` unknown / no TXB |

**`CellGrid.cbf`**: bit0 luma, bit1 Cb, bit2 Cr — H.264: from `coded_block_pattern` (the 8×8 quadrant bit for luma; chroma bits set when `CodedBlockPatternChroma > 0`); HEVC: `cbf_luma`, `cbf_cb`, `cbf_cr` of the TU containing the cell (4:2:2: OR of the two chroma TBs); AV1: `!all_zero` per plane of the TXB containing the cell. `0xFF` = unknown.

**`CellGrid.filter_idx`**: HEVC `SaoTypeIdx[0]` (luma) of the CTB (0..2); AV1 `cdef_idx` of the 64×64 (0..7, `0xFE` = −1 not signalled); H.264 `0`. `0xFF` = unknown.

**`CellGrid.deblock_bs_v/h`**: H.264 bS 0..4 (§8.7.2.1); HEVC bS 0..2 (§8.7.2.4); AV1: the derived filter size for the edge (`filterSize` of AV1 §7.14.3: 0 = no filter, 4, 6 (chroma only), 8, 16) — not a strength, documented as such in the legend. `0xFF` = unknown.

**`CellGrid.partition_id`**: `PartitionInfo.index` of the innermost `Slice`/`SliceSegment` (H.26x) or `Tile` (AV1 and HEVC with tiles: HEVC uses the slice segment; the tile index is in `HevcCtu.tile_idx`).

**`Block.partition_type`**

| `BlockKind` | H.264 | HEVC | AV1 |
|---|---|---|---|
| `MB` | unified `AvcMbType` | — | — |
| `SubMB` | unified `AvcSubMbType` (255 for 16×8 / 8×16 halves) | — | — |
| `CTU` | — | 255 | — |
| `CU` | — | `HevcPartMode` | — |
| `PU` | — | `partIdx` 0..3 | — |
| `TU` | — | `trafoDepth` | — |
| `SB` | — | — | `Av1Partition` read at the SB root |
| `Block` | — | — | `Av1Partition` read at this node (`PARTITION_NONE` for leaves) |
| `TXB` | — | — | plane 0..2 |
| `Other` | 255 (the virtual root `blocks[0]`) | 255 | 255 |

**`Block.level`**: depth in the tree, root = 0, top-level MB/CTU/SB = 1.

**`MotionVector.predictor` strings** (exact spellings; the renderer shows them verbatim):

| Codec | Strings |
|---|---|
| H.264 | `"mvp"` (median prediction + mvd), `"spatial_direct"`, `"temporal_direct"`, `"skip"` (P_Skip inferred), `"pcm"` never (no MV) |
| HEVC | `"merge[<merge_idx>]:<source>"` where source ∈ `A0,A1,B0,B1,B2,Tmvp,CombBi,Zero` (e.g. `"merge[2]:B1"`), `"amvp[<mvp_lX_flag>]:<source>"` with source ∈ `A,B,Tmvp,Zero` |
| AV1 | the `Av1YMode` name (`"NEARESTMV"`, `"NEW_NEARMV"`, …) followed by `"[<drl_mode_idx>]"` when `drl_mode_idx` is valid, and `"+global"` when `is_global = 1`, `"+warp"` when `motion_mode = LOCALWARP`, `"+obmc"` when OBMC. Example: `"NEARMV[1]+obmc"` |

**`RefSlot.name` and AV1 slot convention**: H.264/HEVC `"L0[i]"`, `"L1[i]"` (one slot per list entry of the **first** slice; later slices with different lists are described in their `PartitionInfo.header` and plugins MUST append extra slots named `"S<slice>:L0[i]"` when the lists differ); `RefSlot.slot = -1`. **AV1:** `refs[]` always has exactly 8 entries indexed by the spec's reference type (AV1 §6.10.24 `ref_frame`): `refs[0] = {name "INTRA", frame.decode_index = -1, slot = -1, usage_cells = 0}` is a placeholder so that `grid.ref0 = RefFrame[0]` numerically for inter cells, and `refs[t]` for `t = 1..7` is `"LAST"`, `"LAST2"`, `"LAST3"`, `"GOLDEN"`, `"BWDREF"`, `"ALTREF2"`, `"ALTREF"` with `slot = ref_frame_idx[t − 1]` (the DPB slot 0..7 of AV1 §5.9.2) and `frame` = the frame currently in that slot (host DPB model, chapter 7 §7.4.4); on intra frames `slot = -1` and `frame.decode_index = -1` for all seven. Cells never reference index 0: intra cells have `ref0 = ref1 = -1` (rule V-026). Consumers that need the DPB slot (reference-structure graphs) read `RefSlot.slot`; consumers that need the type (LAST…ALTREF colouring) use the index. Chapter 7 §7.6.1 maps libaom `mi.ref_frame[]` onto this. `RefSlot.usage_cells` = number of cells whose `ref0` or `ref1` equals this slot (computed by the plugin; rule V-016 verifies it).

### 6.3.2 Block tree construction rules

1. `blocks[0]` is the virtual root: `id = 0`, `parent = -1`, `kind = Other`, `rect = coded_size`, `level = 0`, `partition_type = 255`, `ext_type = ""`. Its `children` are the top-level units in **raster order** (MBs for H.264 — in MBAFF, pair order top then bottom; CTUs in **tile-scan order** `CtbAddrInTs` for HEVC; superblocks in tile order then raster within the tile for AV1).
2. Every block's `id` equals its index in `blocks` (rule V-004). Children have ids greater than their parent (pre-order numbering; rule V-005).
3. Per codec the tree is: H.264 `MB → SubMB*` (SubMB nodes only for P_8x8/B_8x8 (4) and 16×8/8×16 (2); 16×16 MBs have no children). HEVC `CTU → CU+ → (PU+ , TU+ with nested TU for the transform tree)`: a CU's `children` list first its PUs (`partIdx` order) then its root TU (`trafoDepth = 0`); TUs with `split_transform_flag = 1` have 4 TU children. AV1 `SB → Block (partition nodes) → Block (leaves) → TXB*`: a non-leaf `Block` node has `partition != PARTITION_NONE` and 2, 3 or 4 children per the partition type (`HORZ_4`/`VERT_4` have 4; `*_A`/`*_B` have 3); leaf blocks have `partition = PARTITION_NONE` and TXB children (luma TXBs in raster order then U then V) when the plugin exports them.
4. `CellGrid.block_id[i]` is the **leaf-most block of kind MB/SubMB (H.264), CU (HEVC), Block (AV1)** containing the cell — never a PU/TU/TXB (rule V-011). PU/TU/TXB membership is found through `Block.children`.
5. `Block.bits_x8` of an inner node ≥ sum of its children's `bits_x8` (the difference being the node's own syntax: `split_cu_flag`, `partition`, …); rule V-019.
6. Blocks of a kind the plugin cannot produce are omitted (e.g. phase-1 FFmpeg H.264: MB nodes only, no SubMB; `ext` present but mostly sentinels). The validator does not require any particular kind.

### 6.3.3 Nested extension buffers

`Block.ext` is a complete FlatBuffer (built with its own `FlatBufferBuilder`, `Finish()` without identifier, not size-prefixed) whose root table is the one named by `ext_type`. Allowed `ext_type` per `BlockKind`: `MB → "AvcMb"`, `SubMB → "AvcSubMb"`, `CTU → "HevcCtu"`, `CU → "HevcCu"`, `PU → "HevcPu"`, `TU → "HevcTu"`, `SB → "Av1Block"`, `Block → "Av1Block"`, `TXB → "Av1Txb"`, `Other → ""` (rule V-021). Same for `FrameAnalysis.filter_params` (`"AvcFilterParams" | "HevcFilterParams" | "Av1FilterParams"`), `FrameInfo.ref_state` (`"AvcRefMarking" | "HevcRps" | "Av1RefState"`), `HevcCu.palette` (`HevcPalette`) and `Av1Block.palette` (`Av1Palette`). The host verifies every nested buffer with the matching `Verify<T>Buffer` before exposing it (rule V-022). Helper: `ovt::model::ext_as<T>(const Block&) -> const T*` returns `nullptr` when `ext_type` does not match `T::GetFullyQualifiedName()` or verification fails. Alignment: a nested buffer is only 4-byte aligned inside the outer `[ubyte]` vector unless the producer forces it, and `AvcMb` has `ulong` fields; producers therefore call `ForceVectorAlignment(size, 1, nested.GetBufferMinAlignment())` before creating the vector, `ext_as` verifies with alignment checked relative to the nested buffer (as FlatBuffers does for `nested_flatbuffer` fields), and `canonicalize()` (WI-0.085) keeps that alignment.

### 6.3.4 Bit accounting fields

`bits_x8` values are **estimates** for CABAC and AV1 range-coded data (§0.9 "bits per block (estimated)"), exact only for CAVLC and at `PartitionInfo.bit_count`. Producers use: libaom `AccountingSymbol.bits` (already 1/8-bit units); libde265 patch: `(bitstream_curr - bitstream_start) * 64 - (bits_needed adjustment)` deltas at CU boundaries, in 1/8 bits (×8); FFmpeg H.264 phase 1: not available → `bits_x8` vectors empty. `CellGrid.bits_x8` distributes each **leaf** `Block.bits_x8` over its cells: cell value = `floor(block_bits_x8 / n_cells)` with the remainder `block_bits_x8 mod n_cells` added to the first `remainder` cells in raster order, so that the sum over cells equals the block's value exactly (rule V-020). Partition-level exact counts (`PartitionInfo.bit_count`) and the sum of top-level block estimates will differ; the GUI shows both. **AV1:** `bits_x8` of every `Block` (SB, Block, TXB) is derived from accounting only — the sum of `AccountingSymbol.bits` of the symbols whose accounting context (`context.x` = MI column, `context.y` = MI row) lies inside the block's MI rectangle; libaom v3.12.1 has no per-superblock bit field (`insp_mi_data` has no `sb_bits`, D30). A libaom build without `CONFIG_ACCOUNTING=1` yields `bits_x8 = 0` on every AV1 block and an empty `grid.bits_x8`.

### 6.3.5 `SymbolCost` and `symbol_names`

`FrameAnalysis.symbol_names[k]` is the dictionary (libaom `AccountingDictionary` names, or OVT's CABAC syntax element names such as `"split_cu_flag"`, `"sig_coeff_flag"`); `SymbolCost.symbol_id` indexes it. One `SymbolCost` per `(x4, y4, symbol_id)` with `count` occurrences and summed `bits_x8`. Sorted by `(y4, x4, symbol_id)` (canonical order §6.3.7). Empty when the plugin lacks accounting (`ovt_AnalysisCaps.has_symbol_accounting = 0`).

### 6.3.6 `PlaneRef` conventions

One `PlaneRef` per `(stage, plane)` exported for the frame; the `blob_id` follows §6.6.2. `PlaneStage.Coefficients` planes are `int32` little-endian, full coded size (`width = coded_w`, `height = coded_h`), holding the dequantised coefficient at the sample position of the transform block's coefficient scan position `(u, v)` (i.e. coefficient `(u, v)` of a TB at `(x0, y0)` is stored at `(x0 + u, y0 + v)`), zero elsewhere; this is the "coefficient view" layout StreamEye uses. `Residual` planes are `int16` signed (prediction-subtracted reconstruction, before clipping). All other stages are unsigned samples at the stream's bit depth.

### 6.3.7 Canonical vector orders (determinism)

| Vector | Order |
|---|---|
| `StreamIndex.units` | decode (file) order, `Unit.index` ascending |
| `StreamIndex.frames` | decode order |
| `StreamIndex.parameter_sets`, `sei` | `Unit.index` ascending |
| `FrameAnalysis.blocks` | pre-order depth-first traversal of the tree starting at the root, children in the order of §6.3.2 rule 1/3 |
| `FrameAnalysis.mvs` | by `block_id` ascending, then `list` (0 before 1), then `rect.y`, `rect.x` |
| `FrameAnalysis.refs` | `RefSlot.index` ascending (0..n−1, contiguous) |
| `FrameAnalysis.partitions` | `PartitionInfo.index` ascending |
| `FrameAnalysis.planes` | `stage` ascending then `plane` ascending |
| `FrameAnalysis.symbols` | `(y4, x4, symbol_id)` ascending |
| `Diagnostic` vectors | emission order (deterministic by construction: single-threaded parse order) |
| `Av1FilterParams.lr_units` | plane 0,1,2, then `unitRow`, then `unitCol` |
| `HevcCtu.*` SAO vectors | component 0,1,2 |

Strings are UTF-8 without BOM; `ext` buffers are built with `FlatBufferBuilder` default alignment; no `ForceDefaults`; no shared strings (`FlatBufferBuilder::CreateSharedString` is **not** used, so that byte identity does not depend on insertion order of a string pool).

---

## 6.4 `core/schema/ovt_container.fbs`

This section is the **single normative text** of `ovt_container.fbs`. Chapter 08 (§8.1.3, §8.4.1–§8.4.6, §8.6.7) specifies how every field is derived and validated and does not repeat the schema; chapters 10/11/12 consume the names below exactly. The field names mirror ISO/IEC 14496-12 where a field is a copy of a box field, written in `snake_case` (backbone §0.4 naming rules: `PascalCase` tables, `snake_case` fields). Every table carries a `///` doc comment that `flatc --reflect-names` keeps in the binary schema.

```fbs
// core/schema/ovt_container.fbs — container tree and derived views (ISOBMFF first; IVF/Annex B/OBU/Y4M/EBML/TS reuse the same nodes).
// SPDX-License-Identifier: BSD-2-Clause
// schema_version (major) = 1. Same evolution rules as ovt_model.fbs (§6.1).
// Normative text: chapter 06 §6.4. Derivation and validation rules: chapter 08 §8.4.

include "ovt_model.fbs";   // SyntaxValueKind, ByteSpan, BitSpan, Diagnostic, SyntaxNode, Codec, FrameId

namespace ovt.fb;

// ===================================== backbone §0.5.5 (verbatim) =====================================

/// One decoded field of a box payload (ISOBMFF, EBML element or TS header); children hold nested structures and array entries.
table Field { name:string; kind:SyntaxValueKind; uvalue:ulong; ivalue:long; svalue:string; bytes:[ubyte];
              span:BitSpan /* relative to BoxNode.file_span.offset */; spec_ref:string; children:[Field]; }
/// One box (or EBML element / TS structure) of the container tree with its byte span, decoded fields and children.
table BoxNode {
  fourcc:string; uuid:[ubyte];     // uuid set when fourcc == "uuid"
  file_span:ByteSpan;              // whole box incl. header
  header_length:ubyte;             // 8, 16 (largesize), +16 for uuid
  version:short = -1; flags:int = -1;   // FullBox
  fields:[Field];                  // decoded payload fields; empty for undecoded boxes
  payload_span:ByteSpan;           // undecoded remainder for hex display
  children:[BoxNode];
  spec_ref:string; description:string;   // from registry
  diagnostics:[Diagnostic];
}
/// The container tree: top-level boxes plus the per-track summaries that `ovt_stream_open` selects from (chapter 8 §8.1.1).
table ContainerTree { format:string /* "isobmff","ivf","annexb","obu","y4m","raw","ebml","mpegts" */; root:[BoxNode]; tracks:[TrackInfo]; diagnostics:[Diagnostic]; }

// ===================================== Tracks (chapter 8 §8.1.3) =====================================

/// hdlr.handler_type mapped to a closed enum; `handler_fourcc` on TrackInfo keeps the written value.
enum HandlerType : ubyte { Unknown = 0, Video = 1, Audio = 2, Subtitle = 3, Text = 4, Metadata = 5, Hint = 6, Timecode = 7, Auxiliary = 8 }

/// One codec configuration child of a sample entry; `bytes` is exactly what `ovt_AnalysisDecoderApi.create()` receives as codec_private.
table CodecPrivate { fourcc:string;        /* "avcC", "hvcC", "av1C", "vpcC", "esds", "dOps", "dvcC", "dvvC", "dvwC" */
                     bytes:[ubyte];         /* box payload WITHOUT the 8/16-byte box header */
                     box_offset:ulong; }    /* file offset of the box, for hex linking */

/// One `tref` reference type and the track_IDs it points to.
table TrackReference { type:string; /* "cdsc", "hint", "vdep", "tmcd", "chap", "auxl", "subt", "font", "hind", "vplx", "sbas", "scal" */ track_ids:[uint]; }

/// Per-track summary built from `moov` (chapter 8 §8.1.3, WI-1.217). Derived views (SampleTable, EditList, FragmentTimeline,
/// EncryptionInfo, HdrMetadata) are separate tables keyed by `track_id` and fetched through `ovt_container_*` (chapter 12 §12.3).
table TrackInfo {
  index:uint;                      // 0-based position among trak boxes in moov order (what ovt_stream_open takes)
  track_id:uint;                   // tkhd.track_ID
  handler:HandlerType;
  handler_fourcc:string;           // hdlr.handler_type as written ("vide", "soun", "sbtl", "subt", "text", "meta", "hint", "tmcd", "auxv")
  codec:Codec;                     // ovt.fb.Codec (backbone §0.5.1); Unknown for non-video
  sample_entry_fourcc:string;      // "avc1", "hev1", "dvh1", "av01", "mp4a", "Opus", ...
  original_format:string;          // frma.data_format when sample entry is enc*/resv; else same as sample_entry_fourcc
  sample_entry_count:uint;         // stsd.entry_count
  codec_string:string;             // RFC 6381 codecs= value of the first sample entry, e.g. "avc1.640028", "hvc1.2.4.L153.B0", "av01.0.08M.10.0.110.09.16.09.0" (rules: chapter 8 §8.1.3)
  timescale:uint;                  // mdhd.timescale
  duration:ulong;                  // mdhd.duration in timescale units (0xFFFFFFFF / all-ones = unknown)
  duration_seconds:double;         // derived; -1 if unknown
  movie_timescale:uint;            // mvhd.timescale
  tkhd_duration:ulong;             // tkhd.duration (movie timescale)
  language:string;                 // ISO 639-2/T from mdhd (3 letters) or elng if present
  enabled:bool; in_movie:bool; in_preview:bool;   // tkhd.flags bits 0,1,2
  width:double; height:double;     // tkhd 16.16 → double (presentation size)
  coded_width:uint; coded_height:uint;             // VisualSampleEntry.width/height
  matrix:[int];                    // tkhd.matrix, 9 entries, raw (16.16 / 2.30 fixed point)
  rotation:ushort;                 // derived from matrix: 0, 90, 180 or 270 = CLOCKWISE rotation to apply when displaying (FFmpeg `rotate` tag convention: rotation = (360 - av_display_rotation_get(matrix)) mod 360, rounded to a multiple of 90). tkhd (a,b,c,d) in 16.16: (1,0,0,1) -> 0; (0,1,-1,0) -> 90 (portrait phone recording); (-1,0,0,-1) -> 180; (0,-1,1,0) -> 270. Any other matrix (flip, skew, scale) -> 0 and the raw `matrix` is left to the consumer
  pixel_aspect:[uint];             // pasp hSpacing, vSpacing (2 entries) or empty
  clean_aperture:[uint];           // clap: cleanApertureWidthN/D, HeightN/D, horizOffN/D, vertOffN/D (8 entries) or empty
  channel_count:uint; sample_rate:double; sample_size:uint;   // audio
  sample_count:ulong;              // stsz/stz2 sample_count + sum of trun sample_count over all fragments
  bitrate_avg_bps:uint; bitrate_max_bps:uint;     // btrt avgBitrate/maxBitrate if present, else derived from sizes and durations (0 when unknown)
  is_fragmented:bool;              // movie has mvex/trex for this track_ID
  is_encrypted:bool;               // sample entry is enc*/ sinf present
  edit_list_present:bool;
  codec_private:[CodecPrivate];    // in stsd child order; first entry is what create() receives
  avstream_index:int = -1;         // libavformat stream index after §8.1.4 matching; -1 = unmatched
  references:[TrackReference];     // from tref
  trak_offset:ulong;               // file offset of the trak box (link into the tree)
  diagnostics:[Diagnostic];
}

// ===================================== Sample table (chapter 8 §8.4.1) =====================================

/// One sbgp/sgpd pair expanded to a per-sample description index.
table GroupMembership { grouping_type:string; grouping_type_parameter:uint;
                        description_index:[uint];   /* per sample; 0 = none; ≥ 0x10001 = fragment-local (index − 0x10000 into the traf's sgpd) */
                        sgpd_offset:ulong; }         /* file offset of the sgpd that defines the entries */

/// Per-sample table, structure-of-arrays (one entry per sample in every vector unless stated). Sample indices are 0-based in OVT
/// (ISOBMFF sample_number = index + 1). Built once per track (chapter 8 §8.4.1); fragments are appended in file order.
table SampleTable {
  track_id:uint; timescale:uint;
  count:ulong;
  dts:[long];                  // decode time, track timescale, from 0 (moov) or from tfdt (fragments)
  cts_offset:[int];            // CTS − DTS (composition offset); 0 when no ctts/trun offsets
  duration:[uint];
  offset:[ulong];              // absolute file offset of the first byte of the sample
  size:[uint];
  is_sync:[bool];              // stss / !sample_is_non_sync_sample; all true when stss absent and not fragmented
  is_partial_sync:[bool];      // stps (QuickTime), empty if no stps
  sample_description_index:[ushort];  // 1-based into stsd
  chunk_index:[uint];          // moov tables: stsc-derived chunk number (1-based); fragments: 0
  fragment_index:[int];        // -1 for moov samples; else index into FragmentTimeline.fragments
  sdtp:[ubyte];                // raw sdtp byte or trun sample_flags bits 4..11 packed the same way; empty if neither present
  degradation_priority:[ushort];      // stdp / trun sample_degradation_priority; empty if absent
  groups:[GroupMembership];
  has_subsamples:bool;         // subs present
  composition_to_dts_shift:long;      // cslg or derived min(cts_offset) negated when negative
  diagnostics:[Diagnostic];
}

// ===================================== Edit list (chapter 8 §8.4.2) =====================================

/// One elst entry resolved onto the presentation timeline and the samples it covers.
table EditSegment { segment_duration:ulong /* movie timescale */; media_time:long /* track timescale; -1 empty */; media_rate:double;
                    presentation_start:ulong; presentation_end:ulong;  /* movie timescale, cumulative */
                    first_sample:long; last_sample:long;               /* sample indices covered, -1 if empty edit */
                    kind:string; /* "normal", "empty", "dwell", "rate" */ }
/// The edit list of one track with the derived presentation offset (chapter 8 §8.4.2).
table EditList { track_id:uint; segments:[EditSegment]; total_presentation_duration:ulong;
                 implied_start_offset:long;  /* media_time of first normal segment in track timescale; 0 = none */
                 negative_cts_compensation:bool; /* true when the first edit removes exactly the initial composition delay */
                 diagnostics:[Diagnostic]; }

// ===================================== Fragments (chapter 8 §8.4.3) =====================================

/// One trun with its resolved first-sample offset and sums.
table TrunInfo { trun_offset:ulong; flags:uint; sample_count:uint; data_offset:int; first_sample_offset:ulong /* absolute */; duration_sum:ulong; size_sum:ulong; }
/// One traf (tfhd + tfdt + truns) with defaults resolved against trex and the base-offset rule applied.
table TrafInfo { track_id:uint; tfhd_flags:uint; base_data_offset:ulong; default_sample_duration:uint; default_sample_size:uint; default_sample_flags:uint;
                 has_tfdt:bool; tfdt:ulong; computed_dts_start:ulong; /* when no tfdt */ truns:[TrunInfo]; senc_offset:ulong; saio_resolved:[ulong]; }
/// One moof + mdat pair (14496-12 §8.8) with its sidx link and DTS continuity check.
table FragmentInfo { index:uint; moof_offset:ulong; moof_size:ulong; mdat_offset:ulong; mdat_size:ulong; sequence_number:uint; trafs:[TrafInfo];
                     styp_offset:long; /* -1 none */ sidx_reference_index:int; /* -1 none */ expected_dts:ulong; dts_gap:long; /* tfdt − expected; 0 ok */ }
/// One sidx reference with the absolute byte range it resolves to and the measured size/duration (−1 unknown).
table SidxRef { reference_type:ubyte; referenced_size:uint; subsegment_duration:uint; starts_with_sap:bool; sap_type:ubyte; sap_delta_time:uint;
                abs_offset:ulong; abs_end:ulong; actual_size:long; /* -1 unknown */ actual_duration:long; }
/// One sidx box.
table SidxInfo { sidx_offset:ulong; reference_id:uint; timescale:uint; earliest_presentation_time:ulong; first_offset:ulong;
                 refs:[SidxRef]; }
/// All fragments of one track in file order plus the sidx boxes that reference it.
table FragmentTimeline { track_id:uint; fragments:[FragmentInfo]; sidx:[SidxInfo]; diagnostics:[Diagnostic]; }

// ===================================== Common encryption (chapter 8 §8.4.4; ISO/IEC 23001-7) =====================================

/// One clear/protected pair of a subsample-encrypted sample.
table SubsampleRange { clear_bytes:uint; protected_bytes:uint; }
/// Per-sample encryption record after seig/tenc resolution (senc or saio/saiz addressed).
table SampleEncryption { sample_index:ulong; is_protected:bool; kid:[ubyte]; iv:[ubyte]; /* 8 or 16 bytes (constant IV copied) */
                         scheme:string; /* cenc,cbc1,cens,cbcs */ crypt_byte_block:ubyte; skip_byte_block:ubyte;
                         subsamples:[SubsampleRange]; senc_entry_offset:ulong; /* file offset of this sample's IV in senc/aux data */ }
/// One pssh box (moov or moof). `kids` is the concatenation of `kid_count` 16-byte KIDs (FlatBuffers has no vector of vectors).
table PsshInfo { box_offset:ulong; system_id:[ubyte]; /* 16 bytes */ system_name:string; /* "Widevine","PlayReady","FairPlay","Common" from a fixed table, else "" */
                 version:ubyte; kid_count:uint; kids:[ubyte]; /* 16 × kid_count */ data_size:uint; summary:string; location:string; /* "moov" | "moof[<index>]" */ }
/// Track-level encryption summary (schm/tenc/frma/pssh) plus the per-sample map for the ranges requested so far.
table EncryptionInfo { track_id:uint; scheme_type:string; scheme_version:uint; original_format:string; default_kid:[ubyte];
                       default_iv_size:ubyte; default_is_protected:bool; default_constant_iv:[ubyte]; pattern_crypt:ubyte; pattern_skip:ubyte;
                       pssh:[PsshInfo]; samples:[SampleEncryption]; /* may be partial: only requested ranges */ diagnostics:[Diagnostic]; }

// ===================================== HDR / colour summary (chapter 8 §8.4.6) =====================================

/// Final HDR classification of a track (rules: chapter 8 §8.4.6 step 3).
enum HdrKind : ubyte { Unknown = 0, SDR = 1, HDR10 = 2, HDR10Plus = 3, HLG = 4, DolbyVision = 5, PQ_NoStatic = 6 }
/// One place where HDR/colour information was found or looked for (box or bitstream).
table HdrSignal { source:string; /* "colr", "mdcv", "clli", "dvcC", "dvvC", "av1C", "vpcC", "sps_vui", "sei_137", "sei_144", "sei_t35_hdr10plus", "nal_62_rpu", "obu_metadata_1", "obu_metadata_2", "obu_metadata_4" */
                  box_offset:long; /* -1 for bitstream */ sample_index:long; /* -1 for box */ unit_index:long; present:bool; summary:string; }
/// Container-level HDR/colour summary of one track, merged from boxes and a bounded bitstream probe (WI-1.223). Consumed by the GUI HDR
/// panel (chapter 11 §11.3.14), `ovt probe --hdr` / `ovt info` (chapter 12) and the render colour pipeline (chapter 10).
table HdrMetadata {
  track_id:uint;
  kind:HdrKind;                       // final classification (rules in chapter 8 §8.4.6)
  color:[ubyte];                      // ovt_ColorDescription (backbone §0.5.6) as little-endian struct image, 44 bytes (incl. the leading uint32_t struct_size, D4); same convention as HdrReport.declared (§6.3) and NormalizationConfig (§6.5). Boxes win over bitstream for primaries/transfer/matrix/full_range when colr exists.
  dv_profile:ubyte; dv_level:ubyte; dv_bl_compat_id:ubyte; dv_rpu_present:bool; dv_el_present:bool; dv_bl_present:bool;
  hdr10plus_application_version:ubyte; hdr10plus_num_windows:ubyte;
  signals:[HdrSignal];                // every place HDR info was found or looked for
  box_vs_bitstream_mismatch:bool;
  probed_samples:uint;                // how many samples were inspected (0 = box-level only)
  diagnostics:[Diagnostic];
}

// ===================================== Raw video / Y4M format (chapter 8 §8.6.5–§8.6.6) =====================================

/// Pixel format of a headerless raw YUV file or a Y4M stream (grammar of `pix_fmt`: chapter 8 §8.6.6); one per file.
table RawVideoFormat { width:uint; height:uint; pix_fmt:string; fps_num:uint; fps_den:uint;
                       chroma_format:ubyte; bit_depth:ubyte; little_endian:bool; planar:bool; interleaved_uv:bool; /* nv12/nv21/p010 */
                       frame_bytes:ulong; frame_count:ulong; header_bytes:ulong; /* skipped prefix, e.g. 0 */
                       color:[ubyte]; /* ovt_ColorDescription struct image (44 bytes, §0.5.6 with struct_size); empty when unknown */ }

// ===================================== HEIF / AVIF items (chapter 8 §8.3.6, WI-1.214 / WI-4.213; phase 4) =====================================

/// One `iref` edge: this item references `to_item_id` with the 4CC `type` (`dimg`, `thmb`, `auxl`, `cdsc`, …).
table ItemReference { type:string; to_item_id:uint; }
/// One `meta` item (`iinf`/`infe` + `iloc` + `ipma`/`ipco`), derived once per file; `ContainerRoot.items`.
table ItemInfo { item_id:uint; type:string; /* infe item_type 4CC, "mime" or "uri " */ name:string;
                 properties:[string];        /* ipco property 4CCs associated through ipma, in association order */
                 extents:[ByteSpan];         /* iloc extents resolved to absolute file spans (construction_method 0/1) */
                 references:[ItemReference]; is_primary:bool; is_hidden:bool;
                 derived_of:[uint];          /* `dimg` source item ids for grid/iden/iovl items */
                 width:uint; height:uint; }  /* from ispe when present; 0 otherwise */

// ===================================== MPEG-TS derived views (chapter 8 §8.6.7, WI-4.206/4.207; phase 4) =====================================

table TsPcrPoint   { packet_index:ulong; pcr:ulong; }                      /* 27 MHz PCR at the given 188/192/204-byte packet */
table TsPtsPoint   { pid:ushort; packet_index:ulong; pts:long = -1; dts:long = -1; }   /* 90 kHz; -1 absent */
table TsPidBitrate { pid:ushort; window_ms:uint; bps:[uint]; }             /* bitrate per window */
/// PCR/PTS/DTS timing and per-PID bitrate of one transport stream (`ContainerRoot.ts_timing`).
table TsTiming { pcr:[TsPcrPoint]; pts_dts:[TsPtsPoint]; bitrate_per_pid:[TsPidBitrate]; pcr_pid:ushort; packet_size:ubyte; }
table TsCheck { id:string; /* ETSI TR 101 290 check id, e.g. "1.1" */ name:string; count:ulong; first_packet_index:ulong; first_offset:ulong; }
/// TR 101 290 priority-1/2 report of one transport stream (`ovt probe --tr101290`, chapter 12 §12.2.2; `ContainerRoot.ts_report`).
table TsReport { checks:[TsCheck]; packets:ulong; diagnostics:[Diagnostic]; }

// ===================================== DASH / HLS virtual file (chapter 8 §8.6.7, phase 4) =====================================

/// Parsed manifest (DASH MPD or HLS master + media playlists) before representation selection; `DashHlsContext.manifest` (WI-4.208/4.209).
table ManifestSegment { url:string; byte_range:ByteSpan; /* length 0 = whole resource */ start_time:long = -1; duration:long = -1; number:long = -1; is_init:bool; }
table ManifestEncryption { method:string; /* "cenc", "cbcs", "AES-128", "SAMPLE-AES", … */ key_uri:string; iv:[ubyte]; default_kid:[ubyte]; }
table ManifestRepresentation { id:string; bandwidth:uint; codecs:string; mime_type:string; width:uint; height:uint; frame_rate:string;
                               video_range:string; /* HLS VIDEO-RANGE: "SDR", "PQ", "HLG" */ timescale:uint; segments:[ManifestSegment]; encryption:[ManifestEncryption]; }
table ManifestAdaptationSet { id:string; content_type:string; lang:string; representations:[ManifestRepresentation]; }
table ManifestPeriod { id:string; start:long = -1; duration:long = -1; adaptation_sets:[ManifestAdaptationSet]; }
table Manifest { kind:string; /* "dash-mpd" | "hls-m3u8" */ url:string; periods:[ManifestPeriod]; /* HLS: one period, one adaptation set per media type */
                 encryption:[ManifestEncryption]; /* manifest-level keys (HLS #EXT-X-KEY, DASH ContentProtection) */ diagnostics:[Diagnostic]; }

/// One segment of a virtual file: where it came from and where it sits in the concatenated byte space.
table SegmentRef { url:string; byte_range:ByteSpan; /* length 0 = whole resource */ virtual_span:ByteSpan; /* position in the virtual concatenated file */
                   kind:ubyte; /* 0 init, 1 media, 2 index (sidx) */ time:long = -1; duration:long = -1; /* segment timeline, in timescale */ number:long = -1; /* $Number$ */ }
/// How a logical stream was assembled from a DASH MPD or HLS playlist (null for local files).
table DashHlsContext {
  manifest_url:string; manifest_kind:string;      // "dash-mpd" | "hls-m3u8"
  period_id:string; adaptation_set_id:string; representation_id:string;   // DASH
  variant_uri:string; rendition_group_id:string;  // HLS
  timescale:uint; presentation_time_offset:long;
  bandwidth:uint; codecs:string; mime_type:string;
  segments:[SegmentRef];                           // in virtual-file order; segments[0] is the init segment
  manifest_text:string;                            // the MPD / playlist as fetched (for display); may be truncated to 1 MiB
  manifest:Manifest;                               // parsed form (phase 4, WI-4.208/4.209); null until the virtual file layer is built
  fetched_at_unix:long;                            // metadata only (not part of any analysis record)
}

// ===================================== Root =====================================

/// File-level root written to the cache (`container.fb.zst`, §6.7) and emitted by `ovt probe -f fb`. Derived views are stored once per
/// track that has them and are matched to `tree.tracks[]` by `track_id`; absent vectors mean "not built yet" (lazy, chapter 8 §8.4).
table ContainerRoot {
  schema_version:uint;             // = 1
  tree:ContainerTree;
  file_size:ulong; file_name:string;  // basename only (no directories, determinism)
  source_hash:[ubyte];             // same 32-byte hash as StreamIndex.source_hash
  brands:[string];                 // ftyp major_brand then compatible_brands; styp brands excluded
  is_fragmented:bool; is_cmaf:bool; has_sidx:bool; has_mfra:bool; is_encrypted:bool;
  sample_tables:[SampleTable];
  edit_lists:[EditList];
  fragment_timelines:[FragmentTimeline];
  encryption:[EncryptionInfo];
  hdr:[HdrMetadata];
  raw_format:RawVideoFormat;       // y4m / raw only; null otherwise
  dash_hls:DashHlsContext;         // null for local files
  items:[ItemInfo];                // HEIF/AVIF `meta` items (chapter 8 §8.3.6); empty for non-HEIF files
  ts_timing:TsTiming;              // MPEG-TS only (phase 4); null otherwise
  ts_report:TsReport;              // MPEG-TS TR 101 290 report (phase 4); null otherwise
  diagnostics:[Diagnostic];
}

root_type ContainerRoot;
file_identifier "OVTC";
file_extension "fc";
```

**Table list (for `tools/check_schema_in_spec.py`, WI-0.081):** `Field`, `BoxNode`, `ContainerTree`, `HandlerType` (enum), `CodecPrivate`, `TrackReference`, `TrackInfo`, `GroupMembership`, `SampleTable`, `EditSegment`, `EditList`, `TrunInfo`, `TrafInfo`, `FragmentInfo`, `SidxRef`, `SidxInfo`, `FragmentTimeline`, `SubsampleRange`, `SampleEncryption`, `PsshInfo`, `EncryptionInfo`, `HdrKind` (enum), `HdrSignal`, `HdrMetadata`, `RawVideoFormat`, `ItemReference`, `ItemInfo`, `TsPcrPoint`, `TsPtsPoint`, `TsPidBitrate`, `TsTiming`, `TsCheck`, `TsReport`, `ManifestSegment`, `ManifestEncryption`, `ManifestRepresentation`, `ManifestAdaptationSet`, `ManifestPeriod`, `Manifest`, `SegmentRef`, `DashHlsContext`, `ContainerRoot`.

Notes for implementers: (1) `SampleTable` is SoA like `CellGrid` so a 1-hour file (≈ 216 000 samples) costs ≈ 9 MB uncompressed; (2) `ContainerTree.tracks` remains the list consumed by `ovt_stream_open` (`TrackInfo.index`); `ContainerRoot` adds file-level data and the derived views without touching the verbatim §0.5.5 tables; (3) the derived views are **not** nested inside `TrackInfo` so that `ovt_container_track_info` stays cheap and each view can be built lazily and cached separately (chapter 8 §8.4); (4) the codec configuration record is exposed raw (`CodecPrivate.bytes`) — its parsed fields are `Field` children of the `avcC`/`hvcC`/`av1C` `BoxNode` in the tree (chapter 8 §8.3 group C), not a second copy; (5) `HdrMetadata.color` is a struct image, not a FlatBuffers table, so that render and metrics consume the identical 44-byte `ovt_ColorDescription` (§0.5.6 with the leading `struct_size`, D4) everywhere (OI-02).

---

## 6.5 `core/schema/ovt_metrics.fbs`

```fbs
// core/schema/ovt_metrics.fbs — metric descriptors, per-frame scores, maps, pooling, BD-rate, scenes.
// SPDX-License-Identifier: BSD-2-Clause
// schema_version (major) = 1.

include "ovt_model.fbs";   // FrameId, Diagnostic

namespace ovt.fb;

// ===== backbone §0.5.7 (verbatim) =====
enum MetricKind : ubyte { FullReference, NoReference, ReducedReference, Bitstream }
enum WorkingDomain : ubyte { CodeValue, LinearLight, PU21, ICtCp, ModelRGB }
table MetricDescriptor { id:string /* "psnr", "ssim", "ssim_x264", "ms_ssim", "vmaf", "vmaf_neg", "uvq", "xpsnr", "cambi", "de_itp", ... */;
  display_name:string; kind:MetricKind; domain:WorkingDomain; per_plane:bool; higher_is_better:bool;
  range_min:double; range_max:double; temporal_unit_frames:uint /* 1 = per frame; N = per N-frame chunk (UVQ sets N = round(fps), one 1-second chunk; chapter 0 §0.5.7); 0 = per sequence */;
  outputs:[string] /* e.g. ["score","map16"] */; license:string; version:string; }
table FrameScore { frame:FrameId; values:[double] /* per plane or [combined] */; map_blob_id:string; map_block_size:ushort; }
table MetricsResult { descriptor:MetricDescriptor; normalization:string /* JSON of NormalizationConfig */; frames:[FrameScore];
  pooled_mean:[double]; pooled_harmonic:[double]; pooled_min:[double]; pooled_p1:[double]; pooled_p5:[double]; pooled_std:[double];
  alignment_offset_frames:int; alignment_confidence:double; engine:string /* "libvmaf 3.0.0", "ovt-kernels 1.0", "onnxruntime 1.2x" */;
  // --- extension (ch. 6) ---
  normalization_config:NormalizationConfig;   // structured form of `normalization` (both MUST describe the same configuration; the JSON is for exports)
  maps:[MapBlob];                             // descriptors of every per-frame map referenced by FrameScore.map_blob_id
  pooling:[PoolingResult];                    // additional pooling methods beyond the fixed pooled_* vectors
  scenes:[SceneSegment];                      // scene segmentation used for per-scene pooling (empty = none)
  schema_version:uint;                        // = 1
  frames_total:uint; frames_scored:uint;      // frames_scored < frames_total when frames were skipped (decode errors / mismatched lengths)
  distorted_source_hash:[ubyte]; reference_source_hash:[ubyte];   // 32-byte hashes (reference empty for NoReference)
  diagnostics:[Diagnostic];
}

// =====================================================================================
// ch. 6 additions
// =====================================================================================

enum ScaleFilter : ubyte { Bicubic = 0, Lanczos = 1, Spline36 = 2, Bilinear = 3, Point = 4 }   // zimg resize filters
enum AlignmentMode : ubyte { NoAlignment = 0, FixedOffset = 1, AutoPsnrSearch = 2, AutoHashSearch = 3 }
enum BitDepthPolicy : ubyte { PromoteToReference = 0, PromoteToDistorted = 1, PromoteTo16 = 2, KeepIfEqualElseError = 3 }

// Describes how reference and distorted frames were brought into the metric's WorkingDomain (ch. 9 normalisation stage).
table NormalizationConfig {
  target_width:uint; target_height:uint;       // 0,0 = reference size
  scale_filter:ScaleFilter = Lanczos;          // used when distorted size != target
  scale_which:ubyte;                           // 0 scale distorted to reference, 1 scale reference to distorted, 2 both to target
  crop_reference:Rect; crop_distorted:Rect;    // applied before scaling (w = h = 0 => none); coded coordinates
  chroma_format_out:ubyte;                     // 0 400, 1 420, 2 422, 3 444 (444 when the metric needs full chroma)
  chroma_siting:ubyte;                         // ovt_ColorDescription.chroma_siting value used for upsampling
  bit_depth_policy:BitDepthPolicy = PromoteToReference;
  bit_depth_out:ubyte;                         // resulting bit depth (8/10/12/16)
  domain:WorkingDomain;                        // target domain (= MetricDescriptor.domain)
  // colour conversion (H.273 codes) applied to EACH input to reach the common domain
  primaries_out:ubyte; transfer_out:ubyte; matrix_out:ubyte; full_range_out:ubyte;
  reference_color:[ubyte]; distorted_color:[ubyte];   // serialised ovt_ColorDescription (little-endian struct image; sizeof = 44 bytes for the §0.5.6 layout with the leading uint32_t struct_size, D4) as seen by the stage
  peak_luminance_nits:float = 10000;           // for LinearLight / PU21 / ICtCp: nominal peak used by the EOTF (PQ 10000, HLG per BT.2100 with system gamma)
  hlg_system_gamma:float = 1.2; hlg_display_peak_nits:float = 1000;
  sdr_white_nits:float = 100;                  // mapping of SDR code values to nits in absolute-light domains
  pu21_variant:string;                         // "banding", "banding_glare", "peaks", "peaks_glare" (PU21 encodings), "" if unused
  alignment:AlignmentMode = NoAlignment; alignment_offset_frames:int; alignment_search_range:ushort;
  frame_range_start:int; frame_range_end:int = -1;   // display_index range scored (end -1 = to the end)
  frame_step:ushort = 1;                       // score every n-th frame (1 = all)
  dither:bool;                                 // dithering applied when reducing bit depth (never for metrics; always false here, present for completeness)
  zimg_version:string; engine_options_json:string;   // the plugin's options_json as passed to ovt_MetricApi.create (canonicalised: sorted keys, no whitespace)
}

// Descriptor of a per-frame quality map stored as a plane blob (§6.6.2 blob_id grammar, stage "map-<metric_id>").
enum MapSampleType : ubyte { U8 = 0, U16 = 1, F32 = 2 }
table MapBlob {
  blob_id:string;                              // "<source_hash>/<decode_index>/map-<metric_id>/<plane>"
  frame:FrameId; metric_id:string; plane:ubyte;   // plane 0 = luma / combined; 1,2 chroma; 255 = combined-from-all-planes
  width:uint; height:uint;                     // map grid size (= ceil(frame_w / block_size) x ceil(frame_h / block_size))
  block_size:ushort;                           // samples per map cell on the normalised frame (1 = per pixel; 4 = per cell; 16 = x264 SSIM grid)
  sample_type:MapSampleType; stride_bytes:uint;
  value_min:double; value_max:double;          // physical range for U8/U16 quantisation: value = min + raw * (max - min) / (2^bits - 1)
  nominal_range_min:double; nominal_range_max:double;   // for the colour LUT (= descriptor range by default)
}

enum PoolingMethod : ubyte { Mean = 0, HarmonicMean = 1, GeometricMean = 2, Min = 3, Max = 4, Median = 5, Percentile = 6, LogMean = 7 /* 10*log10(mean of 10^(x/10)), for PSNR */, MinkowskiP4 = 8, PerScene = 9 }
table PoolingResult {
  method:PoolingMethod; percentile:float;      // percentile 0..100 for Percentile method; ignored otherwise
  values:[double];                             // per plane or [combined] (same layout as FrameScore.values)
  frames_used:uint;                            // excludes frames with NaN / skipped frames
  scene_index:int = -1;                        // for PerScene: which SceneSegment
}

// Rate-distortion point and BD-rate (Bjøntegaard delta) per ch. 9 §9.8.
enum Interpolator : ubyte { Cubic = 0 /* classic BD: cubic polynomial fit through 4 points (least squares for > 4) */, Pchip = 1 /* piecewise cubic Hermite, JVET/AOM CTC default */, Akima = 2 }
table RdPoint { bitrate_kbps:double; quality:double; label:string; /* e.g. "crf=28" */ source_hash:[ubyte]; metrics_result_index:int = -1; }   // used by chapter 9 §9.8.4 and chapter 12 §12.2.10 (chapter 9's former `rate_kbps`/`metric_value` and chapter 12's former `BdPoint` are these fields)
table BdRateResult {
  metric_id:string; plane:ubyte = 255;         // which value of FrameScore.values was pooled (255 = combined)
  pooling:PoolingMethod;
  anchor:[RdPoint]; test:[RdPoint];            // sorted by bitrate ascending (writer sorts; reader MUST NOT re-sort)
  interpolator:Interpolator = Pchip;
  bd_rate_percent:double;                      // negative = test saves bitrate at equal quality
  bd_quality:double;                           // metric-unit delta at equal bitrate (BD-PSNR when metric is psnr)
  overlap_low:double; overlap_high:double;     // integration interval in the quality axis (bd_rate) — log10(bitrate) axis for bd_quality
  overlap_fraction:double;                     // fraction of the anchor's quality span covered by the overlap (< 0.5 => warning)
  log_base_bitrate:bool = true;                // integration performed over log10(bitrate)
  diagnostics:[Diagnostic];                    // e.g. "VQ-BD-0002 fewer points than the interpolator needs", "VQ-BD-0003 non-monotonic RD curve" (registry chapter 9 §9.11.4, D2); these replace the former `warnings:[string]` of chapters 9/12
  anchor_name:string; test_name:string;        // curve labels (file basename or user name); used by chapter 9 §9.8.4 and the `ovt bdrate` csv `test` column (chapter 12 §12.2.10). One BdRateResult per (anchor, test) pair; several tests => several records / MetricsReport.bd_rates entries
  range_mode:string;                           // "overlap" | "anchor" (chapter 12 `--range`)
}

// Scene segmentation (shot boundaries) used for per-scene pooling and for the GUI timeline.
table SceneSegment { index:uint; first_display_index:int; last_display_index:int; /* inclusive */ boundary_score:float; /* detector score at the cut (0 for the first segment) */ method:string; /* "ffmpeg-scdet", "hist-diff", "manual" */ }

// Root for a whole metrics run (one or more metrics over one pair of streams).
/// One pooled row for a subset of frames selected by `ovt metrics --group-by type,tid,scene` (chapter 9 §9.8.2–§9.8.3, chapter 12 §12.2.9).
/// Export-level: computed by the exporters of WI-3.312 from `MetricsResult.frames` and the `StreamIndex`/`SceneSegment`s; never stored in the analysis cache.
table PoolingGroup {
  key:string;                                  // "type:I" | "type:P" | "type:B" | "tid:<n>" | "scene:<index>"
  result_index:uint;                           // index into MetricsReport.results this row belongs to
  frame_count:uint;                            // frames in the group (NaN excluded)
  pooled_mean:[double]; pooled_harmonic:[double]; pooled_min:[double]; pooled_p1:[double]; pooled_p5:[double]; pooled_std:[double];   // same definitions and per-component layout as MetricsResult.pooled_*
}
table MetricsReport {
  schema_version:uint;                         // = 1
  results:[MetricsResult];
  bd_rates:[BdRateResult];
  groups:[PoolingGroup];                       // per frame type / temporal layer / scene pooled rows (chapter 9 §9.8.3, `--group-by`); empty when not requested
  reference_name:string; distorted_name:string;   // basenames only
  ovt_version:string;                          // "ovt 0.3.0+gabcdef" — allowed here because a report is an export, not an analysis record
  diagnostics:[Diagnostic];
}

root_type MetricsReport;
file_identifier "OVTR";
file_extension "fm";
```

`MetricsResult.normalization` (JSON string, backbone) is generated from `normalization_config` by the JSON exporter (§6.8.1) with sorted keys and no whitespace, so the two are always consistent (rule V-030).

---

## 6.6 `Picture` and the plane blob store

Planes (decoded samples, prediction, residual, pre-filter snapshots, coefficient views, metric maps) are **never** inside a FlatBuffer. They live in the `ovt::model::PictureStore` and are referenced by `blob_id` from `PlaneRef`, `MapBlob`, `HevcTu.coeff_blob_id` and `Av1Txb.coeff_blob_id`.

### 6.6.1 In-memory layout

```cpp
// core/include/ovt/cxx/model/plane.hpp  (C++ view; the C ABI mirror is ovt_PlaneView in core/include/ovt/decode.h)
namespace ovt::model {
enum class SampleFormat : uint8_t { U8 = 0, U16LE = 1, S16LE = 2, S32LE = 3 };
struct PlaneView {
  const uint8_t* data;      // first byte of row 0; rows are contiguous with `stride_bytes` pitch; data is 64-byte aligned when owned by the store
  uint32_t width, height;   // in samples of THIS plane (chroma planes already subsampled)
  uint32_t stride_bytes;    // >= width * sample_bytes; multiple of 64 when owned by the store (zimg/libplacebo friendly)
  uint8_t  bit_depth;       // 8..16 for samples; 16 for S16LE residual; 32 for S32LE coefficients
  uint8_t  sample_bytes;    // 1, 2 or 4
  SampleFormat format;
  uint8_t  plane;           // 0 Y, 1 U, 2 V (3 = alpha, future)
  uint8_t  stage;           // ovt.fb.PlaneStage, or 0xFE for metric maps
};
struct Picture {            // what ovt_decoder_next and ovt_analysis_plane hand out
  ovt::fb::FrameId id;
  ovt_ColorDescription color;
  PlaneView planes[3];      // planes[1..2] have width = height = 0 for 4:0:0
  uint8_t chroma_format;    // 0..3
  ovt::fb::Rect crop;
  std::shared_ptr<const void> owner;   // keeps the backing memory alive (store entry or decoder frame)
};
}
```

Rules:
1. **Planar only**, one `PlaneView` per plane; no packed or semi-planar (NV12) layouts cross the C API. Decoders that produce NV12 (hardware paths) are converted in `decode` before exposure.
2. **Sample encoding**: 8-bit → `U8`; 9–16-bit samples → `U16LE` with the value in the low `bit_depth` bits, upper bits zero (rule: `v < (1 << bit_depth)`); `Residual` stage → `S16LE` (range −(2^bit_depth) .. 2^bit_depth − 1); `Coefficients` stage → `S32LE`. Little-endian is mandated regardless of host byte order (all target platforms are little-endian; a big-endian host MUST byte-swap on export).
3. `stride_bytes` is the row pitch; consumers MUST use it (never assume `width * sample_bytes`). Rows beyond `height` do not exist. Padding bytes within a row are unspecified and MUST be ignored by hashing/export (export writes exactly `width * sample_bytes` bytes per row).
4. Hidden/non-output frames (AV1 ALTREF) have planes only when the analysis plugin can export them: stock libaom returns no `aom_image_t` for a hidden frame, so `ovt-plugin-aom` delivers hidden-frame planes only when built with the OVT snapshot patch (chapter 7 §7.6.1); otherwise `planes[]` is empty for that frame and the host records Diagnostic Info `ANA-0001`. When present they are exposed by `decode_index` like any other frame.
5. The **`Reconstruction`** stage is the decoder's final output *before* film grain synthesis (AV1 `apply_grain`, H.26x film grain SEI are display-side; the renderer applies grain optionally). Hashes (HEVC decoded picture hash SEI) are computed on this stage.

### 6.6.2 `blob_id` grammar

```abnf
blob-id      = source-hash "/" decode-index "/" stage "/" plane
source-hash  = 64LOWERHEX                      ; full SHA-256 of StreamIndex.source_hash, lowercase hex
decode-index = 1*DIGIT                         ; FrameId.decode_index, no padding, no sign (never -1), <= INT32_MAX
stage        = "recon" / "pred" / "resid" / "predeblock" / "presaocdef" / "prelr" / "coeff"
             / ("map-" metric-id)              ; metric maps (ovt_metrics.fbs MapBlob)
             / ("tb-" 1*DIGIT)                 ; per-TU/TXB coefficient blob: "tb-<Block.id>" (HevcTu/Av1Txb.coeff_blob_id); no leading zeros, <= UINT32_MAX
metric-id    = 1*32( ALPHA / DIGIT / "_" )     ; MetricDescriptor.id
plane        = "y" / "u" / "v" / "a"
LOWERHEX     = DIGIT / "a" / "b" / "c" / "d" / "e" / "f"
```

Stage tokens map 1:1 to `PlaneStage`: `Reconstruction → recon`, `Prediction → pred`, `Residual → resid`, `PreDeblock → predeblock`, `PreSaoOrCdef → presaocdef`, `PreLoopRestoration → prelr`, `Coefficients → coeff`. Example: `3b7f…e1/142/pred/u`. `blob_id` is the only key; `ovt_analysis_plane(…, blob_id, …)` and `ovt_AnalysisDecoderApi.plane` resolve it. Plugins never learn the stream's source hash (the analysis ABI of chapter 7 §7.5 passes none), so a plugin emits the **plugin-local form** `decode-index "/" stage "/" plane` (the same tokens; for example `12/recon/u`) and resolves that form in `plane()`. The host (WI-2.151 `Session`) prefixes `<source-hash>/` when it copies the planes into the picture store and rewrites `PlaneRef.blob_id` accordingly; the host rejects plugin ids that do not match the local form. Rule V-023 applies to the `FrameAnalysis` as the host stores and returns it. Each open stream (`ovt_Analysis`) owns its own `PictureStore`, so per-instance budgets are per-stream budgets; for an A/B comparison the two streams differ in `source-hash`, so the `blob_id`s of two streams never collide and planes from both can be looked up by id.

### 6.6.3 Lifetime and LRU (backbone §0.7.3)

| Object | Budget | Eviction key | Where it goes when evicted |
|---|---|---|---|
| `FrameAnalysis` buffers (grid + blocks + ext, no planes) | **64 frames** per open stream (LRU) | last access time (any read through the host API) | disk cache `frames/` (write-through on creation; eviction is a free) |
| `Reconstruction` planes | **4 frames** per open stream (LRU), counted per frame regardless of planes | last access | disk cache `planes/` **only if** the user enabled "cache planes" (default off on Android, on for desktop when the cache is on an internal drive) |
| Other stages (`pred`, `resid`, `predeblock`, `presaocdef`, `prelr`, `coeff`) | share the 4-frame budget; a frame with 5 stages counts as 1 frame but memory is checked against the hard cap | last access | never written to disk by default (regenerated by re-decoding the GOP with the stage mask); written when "cache planes" is on |
| Metric maps | 16 frames per `MetricsResult` | last access | disk cache `maps/` always (small) |
| Hard memory cap | 2 GB RSS desktop / 1 GB Android per open stream (§0.7.3); the store trims LRU entries beyond the frame budgets **and** whenever `resident_bytes > 0.6 × cap`; a single frame's planes exceeding the cap (8K with all stages) is refused with `OVT_E_NOMEM` and Diagnostic `MDL-GEN-0004` | | |

Pinning: the frame currently displayed and the A/B comparison frames are pinned. `PictureStore::pin(blob_id)` pins the **whole frame** the blob belongs to (frames are the LRU unit); pins are counted per `blob_id`, may be taken before the blob is put, and `unpin` of an id that holds no pin is a no-op. Pinned entries are never evicted and may keep a category over its frame budget (display plus A/B pins are at most about three frames); a request that cannot be satisfied under the **byte cap** without evicting a pinned entry fails with `OVT_E_NOMEM`. Accounting: `FrameAnalysis` bytes count toward `resident_bytes`; after each put the store trims first the category's frame budget and then the store-wide LRU across categories until `resident_bytes ≤ 0.6 × cap`, never touching the frame just written; a put fails with `OVT_E_NOMEM` (C++ `ovt::model::NoMemError`, no side effect) when the frame alone would exceed the cap or when the unpinned bytes cannot bring the store under it. Planes are put one by one, so an oversized frame is refused on the first plane that does not fit. `Limits::hard_cap_bytes` defaults to 2 GiB (desktop, §0.7.3); every limit is non-zero. `PlaneBlob` (`plane.hpp`) is the immutable stored unit — `blob_id()`, `view()` (a `PlaneView` into owned memory) and `size_bytes()` — built by `PlaneBlob::copy_of` (64-byte-aligned rows, zeroed padding) or `PlaneBlob::adopt` (keeps the caller's stride and alignment; `src.data` must equal the adopted pointer). `stats().evictions` counts evicted **frames** of any category. The store is thread-safe (one mutex; planes are immutable once published; `Picture.owner` keeps evicted-but-in-use memory alive until the last `Picture` is destroyed). The `NoMemError → OVT_E_NOMEM` mapping lives in the first C entry point that reaches the store (`ovt_analysis_plane`, chapter 12 §12.3.10).

---

## 6.7 On-disk cache format

### 6.7.1 Directory layout

```
<cache_root>/                                   see the cache-root rule below this block
  cache.json                                    cache-wide: {"cache_format": 1, "size_cap_bytes": N, "entries": [{"source_hash": "...", "last_used": "<ISO-8601 UTC>", "bytes": N}]}
  <source_hash>/                                64 lowercase hex chars
    meta.json                                   see 6.7.2
    index.fb                                    StreamIndex FlatBuffer, size-prefixed, identifier "OVTI", UNCOMPRESSED (mmap-ed on open)
    container.fb.zst                            ContainerRoot FlatBuffer ("OVTC"), zstd
    frames/
      <decode_index>.fa.zst                     FrameAnalysis ("OVTA"), size-prefixed, zstd level 3; <decode_index> = 8-digit zero-padded decimal, e.g. 00000142.fa.zst
    planes/
      <decode_index>/
        <stage>-<plane>.pl.zst                  raw plane bytes (exactly height rows of width*sample_bytes bytes, no stride padding), zstd level 1; e.g. 00000142/recon-y.pl.zst
                                                dimensions come from the frame's PlaneRef; a plane file without a matching PlaneRef is garbage and deleted
    maps/
      <metric_id>/<decode_index>-<plane>.pl.zst metric maps, same raw layout; dimensions from MapBlob in the MetricsResult
    metrics/
      <run_id>.fm.zst                           MetricsReport ("OVTR"); run_id = first 16 hex of SHA-256(reference_hash || distorted_hash || canonical options JSON)
    reports/
      <kind>.fb.zst                             one FlatBuffer per `ovt_cache_put_report` kind ("metrics" MetricsReport, "hdr" HdrReport, "verify" VerifyReport; chapter 12 §12.3.14), size-prefixed, zstd level 3;
                                                kind matches [A-Za-z0-9_-]{1,32}; a later put of the same kind replaces the file (metrics/<run_id>.fm.zst stays the per-run file the chapter 09 metrics job writes)
```

**Cache root (single definition; chapters 5, 9, 11, 12, 13 and 14 refer here, D32).** `<cache_root>` is, in this order of precedence: (1) the `--cache-dir` CLI option or the GUI cache-directory setting; (2) the environment variable `OVT_CACHE_DIR` (chapter 12 §12.1.2); (3) the platform default — macOS `~/Library/Caches/org.openvideotools.ovt/`; Linux `$XDG_CACHE_HOME/ovt/`, i.e. `~/.cache/ovt/` when `XDG_CACHE_HOME` is unset or empty; Android the app cache directory `Context.getCacheDir()` + `/ovt/` (passed to core by the Android shell at start-up); Flatpak resolves `$XDG_CACHE_HOME` inside the sandbox (`~/.var/app/org.openvideotools.Studio/cache/ovt/`). `ovt cache path` (chapter 12 §12.2.13) prints the resolved root. The directory is created with mode `0700` on first write.

### 6.7.2 `meta.json`

Written atomically (write `meta.json.tmp`, `fsync`, rename). JSON Schema (draft 2020-12), all fields required unless noted:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://openvideotools.org/schema/cache-meta-1.json",
  "type": "object",
  "required": ["cache_format", "source_hash", "created", "ovt_version", "schemas", "codec", "frame_count", "plugins"],
  "properties": {
    "cache_format": { "const": 1 },
    "source_hash": { "type": "string", "pattern": "^[0-9a-f]{64}$" },
    "source_name": { "type": "string", "description": "basename at creation (informational)" },
    "source_size": { "type": "integer", "minimum": 0 },
    "created": { "type": "string", "format": "date-time", "description": "ISO-8601 UTC, e.g. 2026-11-03T14:05:09Z" },
    "last_used": { "type": "string", "format": "date-time" },
    "ovt_version": { "type": "string", "description": "e.g. 0.3.0+gabcdef" },
    "schemas": {
      "type": "object",
      "required": ["ovt_model", "ovt_container", "ovt_metrics"],
      "properties": {
        "ovt_model":     { "$ref": "#/$defs/schemaRev" },
        "ovt_container": { "$ref": "#/$defs/schemaRev" },
        "ovt_metrics":   { "$ref": "#/$defs/schemaRev" }
      }
    },
    "codec": { "type": "string", "enum": ["H264", "H265", "AV1", "VP9", "H266", "MPEG2", "Unknown"] },
    "frame_count": { "type": "integer", "minimum": 0 },
    "index_complete": { "type": "boolean" },
    "plugins": {
      "type": "array",
      "description": "one entry per analysis plugin whose FrameAnalysis files exist under frames/",
      "items": { "type": "object", "required": ["plugin_id", "plugin_version", "frames"],
        "properties": {
          "plugin_id": { "type": "string" }, "plugin_version": { "type": "string" },
          "plane_stage_mask": { "type": "integer" },
          "frames": { "type": "integer", "description": "number of frames/*.fa.zst written by this plugin" } } }
    },
    "planes_cached": { "type": "boolean" }
  },
  "$defs": { "schemaRev": { "type": "object", "required": ["major", "sha256"],
    "properties": { "major": { "type": "integer", "minimum": 1 }, "sha256": { "type": "string", "pattern": "^[0-9a-f]{64}$", "description": "SHA-256 of the .fbs file text the writer was built with (tools/schema_hash.py); informational" } } } }
}
```

Example:

```json
{ "cache_format": 1, "source_hash": "3b7f0c…e1", "source_name": "tears_of_steel_1080p.mp4", "source_size": 734003200,
  "created": "2026-11-03T14:05:09Z", "last_used": "2026-11-03T15:00:00Z", "ovt_version": "0.3.0+g1a2b3c4",
  "schemas": { "ovt_model": {"major": 1, "sha256": "9c…"}, "ovt_container": {"major": 1, "sha256": "7a…"}, "ovt_metrics": {"major": 1, "sha256": "11…"} },
  "codec": "H265", "frame_count": 17982, "index_complete": true,
  "plugins": [ { "plugin_id": "ovt-plugin-de265", "plugin_version": "1.0.0+de265-1.0.15", "plane_stage_mask": 1, "frames": 240 } ],
  "planes_cached": false }
```

Only one `FrameAnalysis` per `decode_index` is kept: writing with a different plugin id/version than the one recorded in `meta.json.plugins` for that frame first deletes the old file. (Rationale: two plugins for one codec, e.g. de265 and HM oracle, are compared in memory by the verify job, not via the cache.)

### 6.7.3 Invalidation rules

On `ovt_stream_open` the cache entry `<cache_root>/<source_hash>/` is used only if **all** hold; otherwise the directory is deleted and recreated:

| # | Check | Failure action |
|---|---|---|
| I1 | `source_hash` recomputed from the file (first 1 MiB + size + mtime, §6.2.8) equals the directory name and `meta.source_hash` | different hash ⇒ simply a different entry (old one ages out by LRU) |
| I2 | `meta.cache_format == 1` | delete entry |
| I3 | `meta.schemas.ovt_model.major == ovt::model::kModelSchemaMajor` (compiled-in, = 1) and likewise container/metrics (`reports/` is covered by the same majors as `frames/`) | delete entry |
| I4 | `index.fb` passes `VerifySizePrefixedStreamIndexBuffer` and its `schema_version` equals the major; `codec` matches `meta.codec` | delete entry |
| I5 | For each frame file requested: `.fa.zst` decompresses, verifies, `frame.id.decode_index` equals the file name, `plugin_id`/`plugin_version` equal the **currently selected** plugin's id and version (`SelectedPlugin`, passed to `Cache::open_entry`; `std::nullopt` = no plugin check) | delete that frame file (not the entry); re-analyse |
| I6 | For each plane file requested: a `PlaneRef` with matching stage/plane exists in the frame's `FrameAnalysis` and the decompressed size equals `height * width * sample_bytes` | delete that plane file; regenerate |
| I7 | `meta.ovt_version` differs from the running version | **no action** (the schema major decides); logged at info level |

Which accessor applies these rules: `ovt::cache::Cache::open_entry()` (the stream-session accessor, WI-0.092) checks I1–I4 on a directory that is not currently open (a missing or malformed `meta.json` counts as I2), deletes and recreates the entry on failure, logs I7 at info, and switches the entry to invalidating reads (`get_frame`/`get_plane` delete a file that fails I5/I6 and return a miss). `Cache::entry()` and the C object API of chapter 12 §12.3.14 stay raw: no deletion, a bad file is `OVT_E_CORRUPT`. An open entry is never re-checked (it was checked when created); a read-only cache throws `OVT_E_CORRUPT` for a failing entry. The "on `ovt_stream_open`" wording elsewhere means the `open_entry` path. A `--no-cache` CLI flag / GUI setting bypasses reads and writes; `ovt cache clear [--source <file>]` deletes entries; `ovt cache list` prints `cache.json` (chapter 12 §12.2.13).

### 6.7.4 Size cap policy

- Default cap: **20 GiB** desktop, **2 GiB** Android (`cache.json.size_cap_bytes`, user-settable; NFR-010, chapter 5 §5.6, `ovt_cache_open` option `max_bytes` in chapter 12 §12.3.14).
- Accounting: `cache.json.entries[].bytes` is updated after every write (sum of file sizes, computed incrementally). `last_used` is updated at most once per minute per entry while open.
- Eviction (triggered after any write that pushes the total above the cap, and at startup): repeat until total ≤ 0.9 × cap: (1) among entries **not currently open**, pick the one with the oldest `last_used`; (2) delete its `planes/` directory first; if still above 0.9 × cap delete `maps/`, then `reports/`, then `frames/`, then the whole entry (every step targets 0.9 × cap). Open entries are never evicted; if only open entries remain above the cap, the cap stays exceeded and a warning Diagnostic `MDL-CCH-0002` is logged once per `Cache`. Cache state is shared **per root per process**: every `Cache` handle (and every `ovt_Cache` of chapter 12 §12.3.14) on one root shares one implementation, so the open-entry protection of eviction and of `ovt_cache_clear` is process-wide, not per handle; `read_only` is a property of the handle.
- Hard per-entry limit: planes for at most 64 frames are cached per entry even when "cache planes" is on (planes dominate size: 4K 10-bit recon ≈ 25 MB per frame uncompressed, 10–15 MB zstd).

---

## 6.8 Export formats

Exports are produced by `ovt::model::exporters` and reachable from the CLI (`ovt analyze --format json|yuview|aom-inspect|fb`, `ovt index --format json|fb`, `ovt metrics --format json|csv|fb`). All exports are deterministic functions of the FlatBuffer (P4).

### 6.8.1 JSON (FlatBuffers reflection, field-for-field)

Grammar: the output is the FlatBuffers text format generated by `flatbuffers::GenerateText` against the compiled binary schema (`ovt_model.bfbs`, shipped in the binary as a resource), with these fixed `IDLOptions`:

| Option | Value | Effect |
|---|---|---|
| `strict_json` | `true` | keys are double-quoted → valid RFC 8259 JSON |
| `output_default_scalars_in_json` | `true` | every scalar field appears, including sentinels (a consumer never has to know defaults) |
| `indent_step` | `2` | pretty-printed, 2 spaces; `--compact` sets `-1` (single line) |
| `output_enum_identifiers` | `true` | enums as strings (`"kind": "CU"`, `"pred": "Intra"`); unknown values as integers |
| `natural_utf8` | `true` | strings emitted as UTF-8, not `\u` escapes |
| `allow_non_utf8` | `false` | |
| vector of `ubyte` | default (array of numbers) **except** the nested-buffer fields listed below | |
| floats/doubles | FlatBuffers' shortest round-trip formatting (`%.9g` / `%.17g` fallback), `NaN`/`Infinity` written as `"nan"`/`"inf"` strings when `--json-strict-numbers` is given, else as FlatBuffers emits them (non-standard tokens) | |

Post-processing (the only departures from pure reflection): the fields `Block.ext`, `FrameAnalysis.filter_params`, `FrameInfo.ref_state`, `HevcCu.palette`, `Av1Block.palette` are **replaced** by the JSON of the nested table (same options), and the sibling `*_type` field is kept. Byte vectors that are hashes (the `.fbs` field names `source_hash`, `reference_source_hash`, `distorted_source_hash`, `default_kid`, `system_id`, `uuid`, `iv`, `kid`, `kids`, `default_constant_iv`, `bytes`) are emitted as **lowercase hex strings** under the same key (`"source_hash": "3b7f…"`). Field order is schema order. Example fragment (from §6.10):

```json
{
  "schema_version": 1,
  "plugin_id": "ovt-plugin-de265",
  "plugin_version": "1.0.0+de265-1.0.15",
  "frame": { "id": { "decode_index": 0, "display_index": 0 }, "type": "I", "shown": true, "show_existing_of": -1, "structure": "Frame", ... },
  "grid": { "cells_w": 4, "cells_h": 4, "block_id": [2,2,5,5, 2,2,5,5, 8,8,11,11, 8,8,11,11], ... },
  "blocks": [ { "id": 0, "parent": -1, "children": [1], "kind": "Other", "rect": {"x":0,"y":0,"w":16,"h":16}, "level": 0, "partition_type": 255, "pred": "None", "skip": false, "merge_or_direct": false, "qp": 0, "bits_x8": 0, "ext_type": "", "ext": null }, ... ]
}
```

The reverse direction (`ovt cache import <file.json>`, chapter 12 §12.2.13) uses `flatbuffers::Parser` with the same schema; the round trip FlatBuffer → JSON → FlatBuffer MUST be byte-identical for every golden fixture (WI-0.088 acceptance).

### 6.8.2 YUView statistics CSV

Target: files that **YUView 2.14** (`YUViewLib/src/statistics/StatisticsFileCSV.cpp`) loads without error and renders over the decoded YUV of the same stream. Appendix F.5 lists the YUView sources that define the format family; the exact grammar OVT writes is below (semicolon-separated, `\n` line endings, ASCII, no BOM). Header lines start with `%;`. **Verification note:** the keyword set below is taken from YUView's wiki page "Statistics Extension Syntax"; the WI-0.089 acceptance test loads the output in YUView and in OVT's own importer, and any keyword YUView rejects is fixed in the exporter, not worked around in this spec.

```abnf
file        = header *data-row
header      = version-line seq-line 1*type-block
version-line= "%;syntax-version;v1.2" LF
seq-line    = "%;seq-specs;" basename ";" layer ";" width ";" height ";" fps LF     ; layer = 0 ; fps = frames per second as decimal (e.g. "25" or "29.97"), "0" if unknown
type-block  = type-line *type-attr
type-line   = "%;type;" type-id ";" type-name ";" ("map" / "range" / "vector") LF
type-attr   = map-color / default-range / range-line / scale-block / vector-color / scale-factor
map-color   = "%;mapColor;" value ";" r ";" g ";" b [";" a] LF                      ; one per legal value (map types)
default-range = "%;defaultRange;" min ";" max ";" colormap LF                       ; range types; colormap ∈ "jet" "heat" "hsv" "hot" "cool" "spring" "summer" "autumn" "winter" "gray" "bone" "copper" "pink" "lines" "col3_gblr" "col3_gwr" "col3_bblg" "col3_tyr" "col3_bwr" "col3_gbr" "col3_rgb"
range-line  = "%;range;" min ";" max LF                                             ; optional explicit value range when it differs from defaultRange
scale-block = "%;scaleToBlockSize;" ("0" / "1") LF                                  ; vectors: 1 = scale arrow length to block size
vector-color= "%;vectorColor;" r ";" g ";" b [";" a] LF
scale-factor= "%;scaleFactor;" n LF                                                 ; vectors: value / n = pixels (OVT writes 16)
data-row    = poc ";" x ";" y ";" w ";" h ";" type-id ";" value [";" vec-y] LF      ; map/range: one value; vector: value = vecX, vec-y = vecY
```

**POC column:** `FrameId.display_index` by default (YUView matches rows to the frame index of the YUV it displays; hidden AV1 frames and fields are **skipped**); `ovt analyze --format yuview --yuview-poc-column poc` (chapter 12 §12.2.8) writes `FrameInfo.poc` instead (useful when YUView decodes the bitstream itself and reports true POCs). **Coordinates:** coded picture coordinates (YUView expects the decoded, uncropped size; `seq-specs` width/height are `coded_size`). **Ordering:** rows sorted by `poc` ascending, then `type-id` ascending, then `y`, then `x` (one pass per type; deterministic).

**Fixed OVT type-id table** (ids are stable across releases; new types are appended):

| id | name | kind | Source | Row granularity | Header attributes written |
|---|---|---|---|---|---|
| 0 | `OVT_PredMode` | map | `Block.pred` (leaf blocks of kind MB/CU/Block) | per leaf block | `mapColor` 0 (None) 128,128,128; 1 (Intra) 255,0,0; 2 (Inter) 0,0,255; 3 (Skip) 0,255,0; 4 (IntraBC) 255,0,255; 5 (PCM) 255,255,0; 6 (Palette) 0,255,255 |
| 1 | `OVT_QP` | range | `Block.qp` | per leaf block | `defaultRange 0 51 jet` (H.26x) / `defaultRange 0 255 jet` (AV1) |
| 2 | `OVT_IntraModeY` | map | `CellGrid.intra_mode_y` | per leaf block if uniform, else per cell | `mapColor` for each codec-numbered value (HEVC 0..34, H.264 0..8/16..19, AV1 0..12) using the 35-entry fixed palette `tools/yuview_palette.json` |
| 3 | `OVT_IntraModeC` | map | `CellGrid.intra_mode_c` | as above | same palette |
| 4 | `OVT_MV0` | vector | `MotionVector` with `list = 0` | per `MotionVector.rect` | `vectorColor 0 255 0 255`, `scaleFactor 16`, `scaleToBlockSize 0` |
| 5 | `OVT_MV1` | vector | `MotionVector` with `list = 1` | per rect | `vectorColor 255 0 255 255`, `scaleFactor 16`, `scaleToBlockSize 0` |
| 6 | `OVT_Ref0` | map | `CellGrid.ref0` | per leaf block if uniform, else per cell | `mapColor` −1 0,0,0; 0..15 fixed palette |
| 7 | `OVT_Ref1` | map | `CellGrid.ref1` | as above | same |
| 8 | `OVT_TxLog2W` | range | `CellGrid.tx_log2_w` | per cell | `defaultRange 2 6 heat` |
| 9 | `OVT_TxLog2H` | range | `CellGrid.tx_log2_h` | per cell | `defaultRange 2 6 heat` |
| 10 | `OVT_TxType` | map | `CellGrid.tx_type` | per cell | `mapColor` per value 0..16 |
| 11 | `OVT_Cbf` | map | `CellGrid.cbf` | per cell | `mapColor` 0..7 (bit pattern colours: Y red, Cb green, Cr blue additive) |
| 12 | `OVT_BitsX8` | range | `Block.bits_x8` | per leaf block | `defaultRange 0 <max over frame> hot` (max computed per file over all frames exported) |
| 13 | `OVT_Skip` | map | `Block.skip` | per leaf block | `mapColor 0 0,0,0,0; 1 0,255,0,128` |
| 14 | `OVT_Level` | range | `Block.level` | per leaf block | `defaultRange 1 6 cool` |
| 15 | `OVT_PartitionId` | range | `CellGrid.partition_id` | per leaf block | `defaultRange 0 <n_partitions-1> lines` |
| 16 | `OVT_DeblockBsV` | range | `CellGrid.deblock_bs_v` | per cell | `defaultRange 0 4 gray` |
| 17 | `OVT_DeblockBsH` | range | `CellGrid.deblock_bs_h` | per cell | `defaultRange 0 4 gray` |
| 18 | `OVT_FilterIdx` | map | `CellGrid.filter_idx` | per leaf block if uniform else per cell | `mapColor` 0..7 |
| 19 | `OVT_PartitionType` | map | `Block.partition_type` | per leaf block | `mapColor` per codec value (H.264 unified mb_type 0..56; HEVC PartMode 0..7; AV1 PARTITION 0..9) |
| 20 | `OVT_MergeOrDirect` | map | `Block.merge_or_direct` | per leaf block | `mapColor 0 0,0,0,0; 1 255,128,0,160` |
| 21 | `OVT_MVD0` | vector | `MotionVector.mvd_*` list 0 (rows omitted when `-32768`) | per rect | `vectorColor 0 128 0 255`, `scaleFactor 16` |
| 22 | `OVT_MVD1` | vector | list 1 | per rect | `vectorColor 128 0 128 255`, `scaleFactor 16` |
| 23 | `OVT_BlockGrid_<Kind>` | map | one type per `BlockKind` present (`CTU`, `CU`, `PU`, `TU`, `MB`, `SubMB`, `SB`, `Block`, `TXB`): value 1 for each block of that kind (draws the partition grid) | per block | `mapColor 1 255,255,255,0` (transparent fill, grid lines drawn by YUView). Ids 23..31 are allocated in `BlockKind` order (23 = MB, 24 = SubMB, 25 = CTU, 26 = CU, 27 = PU, 28 = TU, 29 = SB, 30 = Block, 31 = TXB) |

"Per leaf block if uniform, else per cell" means: if all cells of the leaf block have the same value, one row with the block rect; otherwise one row per 4×4 cell. Values equal to the sentinel (`-1`, `255`, `65535`) are **not** written (no row). Empty `CellGrid` vectors produce no type block at all. `--yuview-types` (chapter 12 §12.2.8) selects a subset by id.

Example (the §6.10 frame, excerpt):

```
%;syntax-version;v1.2
%;seq-specs;tiny16;0;16;16;25
%;type;0;OVT_PredMode;map
%;mapColor;0;128;128;128
%;mapColor;1;255;0;0
%;mapColor;2;0;0;255
%;mapColor;3;0;255;0
%;mapColor;4;255;0;255
%;mapColor;5;255;255;0
%;mapColor;6;0;255;255
%;type;1;OVT_QP;range
%;defaultRange;0;51;jet
%;type;2;OVT_IntraModeY;map
%;mapColor;0;255;255;255
%;mapColor;1;200;200;200
%;mapColor;26;0;128;255
%;type;26;OVT_BlockGrid_CU;map
%;mapColor;1;255;255;255;0
0;0;0;8;8;0;1
0;8;0;8;8;0;1
0;0;8;8;8;0;1
0;8;8;8;8;0;1
0;0;0;8;8;1;32
0;8;0;8;8;1;32
0;0;8;8;8;1;30
0;8;8;8;8;1;33
0;0;0;8;8;2;1
0;8;0;8;8;2;26
0;0;8;8;8;2;0
0;8;8;8;8;2;26
0;0;0;8;8;26;1
0;8;0;8;8;26;1
0;0;8;8;8;26;1
0;8;8;8;8;26;1
```

(`mapColor` lines for `OVT_IntraModeY` are written only for values that occur in the file, to keep headers short; YUView colours unknown values grey.)

### 6.8.3 libaom `inspect`-compatible JSON (AV1 only)

Target consumer: xiph **aomanalyzer** and any script written against the output of libaom `examples/inspect.c`. Keys, nesting, value encodings and framing follow `examples/inspect.c` and `av1/decoder/inspection.c` at the libaom version pinned in `third_party/VERSIONS.md` (v3.12.1, verified against the source; D30). The key names below are the **JSON keys** written by `inspect.c` (`put_block_info(…, "<name>", …)`); the short strings `-mm`, `-cfl`, `-ibc`, `-plt`, `-uvp`, `-c`, … are only the `inspect` **command-line flags** that enable a layer and never appear in the JSON.

**Framing.** Exactly as `inspect` prints it: the file starts with `[` and a newline; one JSON object per decoded frame follows in decode order, each object followed by `,` and a newline; the file ends with `null`, a newline and `]` (the trailing `null` makes the comma after the last object legal JSON). Hidden frames (`show_frame = 0`) are written, as libaom does; `show_existing_frame` entries of the `StreamIndex` are **not** written (`inspect` skips them and OVT has no `FrameAnalysis` for them, §6.2.5). The writer never emits RLE (`inspect -x`).

**Per-MI layers.** Every layer is a 2-D array of `mi_rows` rows × `mi_cols` columns (`= cells_h × cells_w`; the neutral cell grid *is* the MI grid), each element either a scalar or a fixed-length array. A layer that `inspect` writes with an enum map is preceded by the key `"<name>Map"`, an object mapping libaom enum names to their numeric values (e.g. `"blockSizeMap": {"BLOCK_4X4":0, …, "BLOCK_64X16":21}`); the writer emits the map objects with exactly the names and values of `inspect.c`'s `map_entry` tables. Values come from the leaf `Av1Block` that owns the cell (`grid.block_id`), or from the `Av1Txb` that covers the cell where stated. Keys in output order:

| key (map key) | element | from (OVT field → libaom value) |
|---|---|---|
| `"blockSize"` (`"blockSizeMap"`) | int | `Av1Block.bsize` (libaom `BLOCK_SIZE`, 0 `BLOCK_4X4` … 21 `BLOCK_64X16`) |
| `"transformSize"` (`"transformSizeMap"`) | int | inter and IntraBC blocks: `TxSize` of the luma `Av1Txb` (`plane == 0`; chroma TXBs never feed these maps, as libaom's `inter_tx_size[]`/`tx_type_map` are luma) covering the cell; intra blocks: `Av1Block.TxSize`. OVT writes the default `skip_non_transform = 0` encoding, so the value is never −1 |
| `"transformType"` (`"transformTypeMap"`) | int | mirrors `ifd_inspect()` at the pin, including its quirk: when `Av1Block.skip == 1` the `TxType` of the transform block covering the cell (in practice `DCT_DCT` = 0), otherwise **0**; do not "correct" this — the golden test of WI-0.090 compares against `inspect`'s own output |
| `"dualFilterType"` (`"dualFilterTypeMap"`) | int | `interp_filter[0] * 3 + interp_filter[1]` (`inspection.c`: `mi->filter[0] * 3 + mi->filter[1]`; map `REG_REG` 0 … `SHARP_SHARP` 8) |
| `"mode"` (`"modeMap"`) | int | `Av1Block.YMode` (0 `DC_PRED` … 24 `NEW_NEWMV`; 25 `INTRA_INVALID` never written by OVT) |
| `"uv_mode"` (`"uv_modeMap"`) | int | `Av1Block.UVMode` when `YMode < 13` (intra block), else 14 `UV_MODE_INVALID` (13 is `UV_CFL_PRED`) |
| `"motion_mode"` (`"motion_modeMap"`) | int | `Av1Block.motion_mode` (0 `SIMPLE_TRANSLATION`, 1 `OBMC_CAUSAL`, 2 `WARPED_CAUSAL`) |
| `"compound_type"` (`"compound_typeMap"`) | int | libaom `COMPOUND_TYPE` numbering (0 `COMPOUND_AVERAGE`, 1 `COMPOUND_DISTWTD`, 2 `COMPOUND_WEDGE`, 3 `COMPOUND_DIFFWTD`), i.e. the inverse of the plugin's remap (chapter 7 §7.6.1): `Av1CompoundType` WEDGE 0 → 2, DIFFWTD 1 → 3, AVERAGE 2 → 0, DISTANCE 4 → 1; `COMPOUND_INTRA` (3) and `Unknown` (255) → 0. The map object has the three entries `inspect.c` lists (`COMPOUND_AVERAGE` 0, `COMPOUND_WEDGE` 2, `COMPOUND_DIFFWTD` 3) |
| `"skip"` (`"skipMap"`) | int | `Av1Block.skip` (map `{"SKIP":1,"NO_SKIP":0}`) |
| `"filter"` | `[int,int]` | `Av1Block.interp_filter[0..1]` |
| `"cdef_level"` | int | `cdef_y_pri_strength[cdef_idx]` of the block's 64×64 (libaom `cdef_strengths[idx] / CDEF_SEC_STRENGTHS`); 0 when CDEF is off |
| `"cdef_strength"` | int | `cdef_y_sec_strength[cdef_idx]` after the spec's `3 → 4` mapping (libaom `cdef_strengths[idx] % 4`, then `+1` when it equals 3) |
| `"cfl_alpha_idx"` | int | `cfl_alpha_idx` as signalled (`((|CflAlphaU|-1) << 4) + (|CflAlphaV|-1)` for blocks with `UVMode == UV_CFL_PRED`), else 0 |
| `"cfl_alpha_sign"` | int | `cfl_alpha_signs` as signalled for CfL blocks, else 0 |
| `"delta_q"` | int | `Av1Block.CurrentQIndex` (libaom `current_qindex`) |
| `"seg_id"` | int | `Av1Block.segment_id` |
| `"motionVectors"` | `[int,int,int,int]` | `[Mv[0].col, Mv[0].row, Mv[1].col, Mv[1].row]` in **1/8-pel** = neutral value / 2 (exact: neutral values are even); no map |
| `"intrabc"` (`"intrabcMap"`) | int | `Av1Block.use_intrabc` (map `{"INTRABC":1,"NO_INTRABC":0}`) |
| `"palette"` (`"paletteMap"`) | int | `PaletteSizeY` (0 or 2..8; map `ZERO_COLORS` 0, `TWO_COLORS` 2 … `EIGHT_COLORS` 8) |
| `"uv_palette"` (`"uv_paletteMap"`) | int | `PaletteSizeUV` (same map as `palette`) |
| `"referenceFrame"` (`"referenceFrameMap"`) | `[int,int]` | `Av1Block.RefFrame[0..1]` (−1 NONE, 0 `INTRA_FRAME`, 1 `LAST_FRAME` … 7 `ALTREF_FRAME`; the map lists `INTRA_FRAME` … `ALTREF_FRAME`) |

There is **no** superblock-bits layer: libaom v3.12.1 `insp_mi_data` has no `sb_bits` field and `inspect.c` writes no `"sb"` key (D30). Per-block bit counts exist only through accounting (`symbols` below; §6.3.4).

**Accounting (written only when the record has `symbols`, i.e. the plugin was built with `CONFIG_ACCOUNTING=1`).** `"symbolsMap"`: `[string]` = `FrameAnalysis.symbol_names` (index = `symbol_id`). `"symbols"`: a flat array mixing two element shapes exactly as `inspect.c` writes them — a **context marker** `[x, y]` (MI column, MI row) whenever the position changes, followed by one `[symbol_id, bits, samples]` triple per symbol at that position (`bits` in 1/8-bit units). OVT holds aggregated `SymbolCost` records (one per `(x4, y4, symbol_id)`, canonical order of §6.3.7), so it writes, for each position in that order, the marker `[x4, y4]` followed by one triple `[symbol_id, bits_x8, count]` per aggregated record. A consumer that sums triples per `(x, y, symbol_id)` gets identical totals from libaom's and OVT's files; the per-symbol sequence is not reproduced.

**Frame-level keys** (after the layers, in this order):

| key | type | from |
|---|---|---|
| `"frame"` | int | 0-based ordinal of the record within the exported file (decode order, `show_existing_frame` entries excluded); libaom writes `current_frame.frame_number`. When a sub-range is exported the first record is 0 (aomanalyzer expects a contiguous sequence from 0); the original `decode_index` of record 0 is given in `"configString"` |
| `"showFrame"` | 0/1 | `FrameInfo.shown` |
| `"frameType"` | int | `Av1RefState.frame_type` (0 KEY … 3 SWITCH) |
| `"baseQIndex"` | int | `Av1FilterParams.base_q_idx` |
| `"tileCols"`, `"tileRows"` | int | width resp. height **of the first tile in MI units** (libaom `tile_mi_cols`/`tile_mi_rows` of tile (0,0); *not* the number of tiles): `ceil(min(rect.w, coded_size.w) / 4)`, `ceil(min(rect.h, coded_size.h) / 4)` of the `PartitionInfo` of kind Tile with `tile_col = tile_row = 0` (rounded up like `cells_w`) |
| `"deltaQPresentFlag"`, `"deltaQRes"` | int | `Av1FilterParams.delta_q_present`, `delta_q_res` (as signalled, not `1 << delta_q_res`) |
| `"config"` | object | fixed `{"MI_SIZE":4}` (the only entry of libaom's `config_map`) |
| `"configString"` | string | libaom writes `aom_codec_build_config()`; OVT writes `"ovt <ovt_version>; plugin <plugin_id> <plugin_version>; decode_index_offset <n>"`; `<ovt_version>` is `AomJsonOptions::ovt_version` when non-empty (golden tests pass `0.0.0`), else the product version |

Comparison rule (WI-0.090, chapter 14 §14.4.5): an OVT export and an `inspect -A` output of the same stream have the **same key set** per frame; every per-MI layer and every `*Map` object is equal element by element; `symbols` totals per `(x, y, symbol_id)` are equal; `"frame"` and `"configString"` are excluded from the value comparison.

---

## 6.9 Validation rules for `FrameAnalysis` (and `StreamIndex`)

`ovt::model::Validator` (`core/src/model/validator.cpp`) implements every rule below; the CLI exposes it as `ovt analyze --validate` (on emitted records or on a stored `<file.fa.zst|file.fb|dir>`) and `ovt index --validate` (chapter 12 §12.2.8 / §12.2.4; exit 0 = valid, 5 = violations reported, 3 = unreadable input, per chapter 12 §12.1.3). There is no separate validation subcommand (chapter 12 §12.2). Violations are reported as `Diagnostic`s with `code = "MDL-VAL-0nnn"` where `nnn` is the rule number (V-002 → `MDL-VAL-0002`; registry in §6.9.1), severity Error unless the rule says Warning, and `message` = the rule text with the offending values. The test suite runs it on every golden fixture and on every buffer produced by every plugin in CI. Each violation is reported as `{rule, severity, message, path}` where `path` is a FlatBuffers-style locator (`blocks[17].rect`, `grid.qp[1234]`). Severity is **Error** unless stated. If any `Diagnostic` with `severity = Fatal` is present in `FrameAnalysis.diagnostics`, the buffer is a *partial* record: rules V-010 through V-020 and V-024 through V-028 are downgraded to Warning.

| Rule | Statement |
|---|---|
| **V-001** | The buffer carries the `OVTA` identifier (plain or size-prefixed form; both are accepted), passes `flatbuffers::Verifier` for its root type and `schema_version == 1`. `plugin_id` and `plugin_version` are non-empty. |
| **V-002** | `frame.timebase_den > 0`; `frame.coded_size.x == frame.coded_size.y == 0`, `coded_size.w > 0`, `coded_size.h > 0`; `id.decode_index >= 0`; `id.display_index >= -1`; `chroma_format <= 3`; `8 <= bit_depth_luma <= 16` and likewise chroma (4:0:0: `bit_depth_chroma == bit_depth_luma`). **H.264:** `coded_size.w % 16 == 0 && coded_size.h % 16 == 0` and `crop` lies within `coded_size`. **HEVC:** both multiples of 8 (`MinCbSizeY` is a power of two ≥ 8 and is not in `FrameAnalysis`; the exact multiple is checked by the SPS-aware index validator) and `crop` lies within `coded_size`. **AV1:** `coded_size.w = FrameWidth`, `coded_size.h = FrameHeight` are arbitrary (≥ 1, no multiple-of-8 requirement — libaom pads the MI grid internally, §6.2.1 rule 4); `crop.x == crop.y == 0`, `crop.w >= 1`, `crop.h >= 1` and `crop` is **not** required to lie within `coded_size` (render size is a display hint independent of the coded size, AV1 §5.9.6). |
| **V-003** | `grid.cells_w == ceil(coded_size.w / 4)` and `grid.cells_h == ceil(coded_size.h / 4)`. Every non-empty `CellGrid` vector has exactly `cells_w * cells_h` elements. `qp_hist` is empty or has exactly 256 elements. |
| **V-004** | `blocks` is non-empty; `blocks[i].id == i` for all `i`; `blocks[0]` has `parent == -1`, `kind == Other`, `rect == {0, 0, coded_size.w, coded_size.h}`, `level == 0`, `ext_type == ""`, `ext` empty. No other block has `parent == -1`. |
| **V-005** | For every block `b` with `b.id > 0`: `0 <= b.parent < b.id` (pre-order numbering ⇒ acyclic), `blocks[b.parent].children` contains `b.id` exactly once, and every id in any `children` vector names a block whose `parent` is that block. Each block appears in exactly one `children` vector. |
| **V-006** | Every `rect` has `w > 0`, `h > 0`, `x >= 0`, `y >= 0`, and `x, y, w, h` are multiples of 4 — except `blocks[0]`, whose rect is the coded size (V-004), which is arbitrary for AV1 (issue #360). For `level >= 2`, the child's rect lies within its parent's rect. For `level == 1`, `rect.x < coded_size.w` and `rect.y < coded_size.h` (top-level blocks may extend past the coded picture). |
| **V-007** | Children of one parent that have the same `kind` do not overlap (intersection area 0). Exception: `structure == Mbaff` field MB pairs (`AvcMb.mb_field_decoding_flag == 1`) whose two MBs have identical rects. |
| **V-008** | `BlockKind` set is codec-consistent: H264 ⊆ {Other, MB, SubMB}; H265 ⊆ {Other, CTU, CU, PU, TU}; AV1 ⊆ {Other, SB, Block, TXB}. Parent→child kind pairs are limited to: Other→MB, MB→SubMB, Other→CTU, CTU→CU, CU→PU, CU→TU, TU→TU, Other→SB, SB→Block, Block→Block, Block→TXB, SB→TXB (a 128×128 or 64×64 leaf SB). |
| **V-009** | `level` equals the number of parent hops to the root. |
| **V-010** | Every value in `grid.block_id` is `< blocks.size()` and `!= 0`. |
| **V-011** | `blocks[grid.block_id[i]].kind` ∈ {MB, SubMB} (H264), {CU} (H265), {Block, SB} (AV1), and that block has no child of any of the codec's kinds listed here (a *grid leaf*: an MB with SubMB children, or an SB with `Block` children, does not qualify). |
| **V-012** | Cell `i = y4 * cells_w + x4` lies inside `blocks[grid.block_id[i]].rect`: `rect.x <= 4*x4 < rect.x + rect.w` and `rect.y <= 4*y4 < rect.y + rect.h`. Conversely every leaf block of the kinds in V-011 that intersects the coded picture owns at least one cell. |
| **V-013** | `grid.pred[i]` is a valid `PredKind` and, when `blocks[grid.block_id[i]].pred != None`, equals it. |
| **V-014** | `refs[k].index == k` (contiguous); every `grid.ref0[i]`/`ref1[i]` is `-1` or `< refs.size()`; every `RefSlot.frame.decode_index` is `>= -1`; `RefSlot.slot ∈ {-1} ∪ [0,7]`. **AV1:** `refs.size() == 8` whenever `refs` is non-empty, `refs[0].name == "INTRA"`, `refs[0].usage_cells == 0`, `refs[t].slot ∈ [0,7]` for `t ≥ 1` on inter frames and `-1` on intra frames; no cell has `ref0 == 0` or `ref1 == 0`. |
| **V-015** | For every `MotionVector m`: `m.block_id < blocks.size()`; `m.list <= 1`; `0 <= m.ref < refs.size()`; `m.rect` lies within `blocks[m.block_id].rect`; if `grid.mv{list}_x` is non-empty then for every cell inside `m.rect`, `grid.mv{list}_x[i] == m.mv_x`, `grid.mv{list}_y[i] == m.mv_y` and `grid.ref{list}[i] == m.ref`. |
| **V-016** | `refs[k].usage_cells` equals the number of cells `i` with `grid.ref0[i] == k` plus those with `grid.ref1[i] == k` (0 when both vectors are empty). |
| **V-017** | `partitions[p].index == p`; every `grid.partition_id[i] < partitions.size()`; `PartitionInfo.parent` is `-1` or `< p`; `kind` is codec-consistent (H264: Slice only; H265: Slice, SliceSegment, Tile, Substream; AV1: TileGroup, Tile); for every Tile `rect` lies within `coded_size` and tiles do not overlap; `unit_index` is a valid `Unit.index` of the frame (`frame.units` contains it); `bit_count <= file_span.length * 8`. |
| **V-018** | `MotionVector.flags` bit 4 (field) is set **iff** `frame.structure ∈ {TopField, BottomField}` or the owning MB has `AvcMb.mb_field_decoding_flag == 1`. Non-H.264 frames never set bit 4. FFmpeg plugin without the block-params patch: bit 4 follows `AV_FRAME_FLAG_INTERLACED` (chapter 7 §7.6.3, `ANA-H264-0001`); the validator exempts it (issue #303). |
| **V-019** | For every block with children: `bits_x8 >= Σ children.bits_x8` when `bits_x8 != 0` and all children have `bits_x8 != 0` (zero means unknown and exempts the comparison). |
| **V-020** | If `grid.bits_x8` is non-empty: for every leaf block `b` of the V-011 kinds, `Σ_{cells of b} grid.bits_x8[i] == b.bits_x8`, and the per-cell values follow the floor + remainder distribution of §6.3.4 (first `remainder` cells in raster order carry `+1`). |
| **V-021** | `ext_type` matches `kind` per §6.3.3 (`MB→"AvcMb"`, `SubMB→"AvcSubMb"`, `CTU→"HevcCtu"`, `CU→"HevcCu"`, `PU→"HevcPu"`, `TU→"HevcTu"`, `SB|Block→"Av1Block"`, `TXB→"Av1Txb"`) or is `""` with `ext` empty. `filter_params_type` ∈ {"", "AvcFilterParams", "HevcFilterParams", "Av1FilterParams"} and codec-consistent; `frame.ref_state_type` likewise. |
| **V-022** | Every non-empty nested buffer (`ext`, `filter_params`, `ref_state`, `HevcCu.palette`, `Av1Block.palette`) passes the Verifier of the table named by its `*_type`. Inside `Av1Block`: `MiRow == rect.y / 4`, `MiCol == rect.x / 4`, and `bsize` width/height equal `rect.w/h`. Inside `HevcCu`: `1 << log2CbSize == rect.w == rect.h`. Inside `AvcMb`: `mb_addr` equals the raster MB address of `rect` (or the pair-derived address in MBAFF). |
| **V-023** | Every `PlaneRef.blob_id` (as the host stores and returns it; plugins emit the local form of §6.6.2) matches the ABNF of §6.6.2 with `source-hash` equal to the stream's hash (grammar only when the validator is given no expected hash) and `decode-index == frame.id.decode_index`; `(stage, plane)` pairs are unique; the `stage` token matches `PlaneRef.stage`; plane 0 has `width == coded_size.w` and `height == coded_size.h` — **except** AV1 frames with `Av1FilterParams.use_superres = 1`, where `Reconstruction`/`PreLoopRestoration` plane 0 has `width == Av1FilterParams.UpscaledWidth` (the decoder outputs the upscaled picture; §6.2.1 rule 2); planes 1/2 have the §6.2.2 chroma size (of the same reference width) and are absent for 4:0:0; `sample_bytes ∈ {1,2,4}`; `sample_bytes == 1` ⇔ `bit_depth == 8 && !is_signed`; `Residual` stage ⇒ `is_signed && sample_bytes == 2`; `Coefficients` ⇒ `is_signed && sample_bytes == 4`; `stride_bytes >= width * sample_bytes`. |
| **V-024** | Every `SymbolCost.symbol_id < symbol_names.size()`, `x4 < cells_w`, `y4 < cells_h`, `count >= 1`. |
| **V-025** | Canonical orders of §6.3.7 hold: `blocks` is a pre-order traversal with children visited in `children` order; `mvs` sorted by `(block_id, list, rect.y, rect.x)`; `partitions` by `index`; `planes` by `(stage, plane)`; `symbols` by `(y4, x4, symbol_id)`; `refs` by `index`. |
| **V-026** | If `grid.pred` and `grid.ref0` are non-empty: cells with `pred ∈ {Intra, PCM, Palette, None}` have `ref0 == ref1 == -1`; cells with `pred ∈ {Inter, Skip}` have `ref0 >= 0 || ref1 >= 0` (AV1: `>= 1`, index 0 is the INTRA placeholder); `IntraBC` cells have `ref0 == -1` (the current picture is not a `RefSlot`) and, for AV1, `Av1Block.use_intrabc == 1`. |
| **V-027** | Codec ranges: `tx_log2_w/h ∈ {2,3} ∪ {255}` (H264), `{2..5} ∪ {255}` (H265), `{2..6} ∪ {255}` (AV1); `tx_type <= 3 || == 255` (H.26x), `<= 16 || == 255` (AV1); `cbf <= 7 || == 255`; `filter_idx <= 2 || == 255` (H265), `<= 7 || ∈ {254, 255}` (AV1), `∈ {0, 255}` (H264); `deblock_bs_* <= 4 || == 255` (H264), `<= 2 || == 255` (H265), `∈ {0,4,6,8,16,255}` (AV1); `intra_mode_y` per the §6.3.1 table or `-1`; `partition_type` within the codec enum or 255. |
| **V-028** | `grid.qp` values: `-6*(bit_depth_luma-8) <= qp <= 51` (H.26x), `0 <= qp <= 255` (AV1), or `-1`. If `qp_hist` and `grid.qp` are both non-empty, `Σ qp_hist == cells_w * cells_h` and bin `b` counts the cells with `qp + 128 == b` (H.26x) / `qp == b` (AV1). `Block.qp` equals `grid.qp` of its cells for leaf blocks when both are set. |
| **V-029** | Every `Diagnostic.code` matches `^(CNT|SYN|DEC|VQ|MDL|IO|BR|VER|ANA|RND|GUI|CLI)(-[A-Z0-9]{2,8})?-[0-9]{4}$` (chapter 5 §5.7: family, optional qualifier of 2–8 uppercase alphanumerics, four digits; e.g. `SYN-H265-0007`, `CNT-0001`, `MDL-GEN-0003`, `BR-0001`) and `message` is non-empty. |
| **V-030** | (`MetricsResult`) `normalization` equals the canonical JSON (sorted keys, no whitespace, §6.8.1 options with `indent_step = -1`) of `normalization_config`; `frames` sorted by `frame.display_index`; every `map_blob_id` names a `MapBlob` in `maps`; `pooled_*` vectors are empty or have the same length as `frames[0].values`. |
| **V-031** | (`StreamIndex`) `units[i].index == i`; `frames[i].id.decode_index == i`; `display_order` contains each `decode_index` with `display_index >= 0` exactly once, at position `display_index` (field pairs: the first decoded field); every `FrameInfo.units` entry is a valid unit whose `frame.decode_index` equals the frame; `size_bytes == Σ (units[u].file_span.length + units[u].prefix_span.length)` over the frame's `units` (start codes, length prefixes and the preceding AUD/SEI/parameter-set units included — chapter 7 §7.4.2 S7); `parameter_sets` and `sei` are subsets of `units` by index; `declared_refs` and `dpb_after` entries are `< frames.size()` and `!= i`. |
| **V-032** | Re-serialising the buffer with `ovt::model::canonicalize()` yields byte-identical output. The canonical form is what the flatc object API `Pack` writes with `ForceDefaults` off: children (strings, vectors, sub-tables) are created depth first in field-id order before their parent; fields are added in passes of 8/4/2/1 bytes (offsets and structs count as 4), each pass in reverse field-id order; scalars equal to their default are omitted; an empty non-`required` string or vector is absent; struct padding is zero; nested buffers (`X:[ubyte]` + `X_type`) are canonicalised first, without identifier; the `file_identifier` is written only when the input carries it; no shared strings. This catches non-canonical producers (e.g. shared strings, `ForceDefaults`, present-but-empty strings or vectors). |

Implementation note: rules are independent; the validator evaluates all of them and reports every violation (no early exit) so that a plugin author sees the full list in one run, capped at 64 violations per rule followed by one closing violation of that rule (`"... and N more violations of this rule"`) so that hostile buffers stay bounded (fuzzing, WI-0.094). Rules V-002/V-027/V-028 need the codec: it comes from `blocks[1].kind` (MB ⇒ H264, CTU ⇒ H265, SB ⇒ AV1) or, if `blocks.size() == 1`, from the `ext_type` strings; `ovt analyze --validate --codec <h264|h265|av1>` overrides.

### 6.9.1 Diagnostic registry (`MDL`, `VER`)

Format per chapter 5 §5.7 / rule V-029. `MDL` = data model, validator, picture store and cache; `VER` = verify mode (chapter 7 §7.6.4 raises them; registered here because `VerifyReport` is a model table). Families owned by other chapters: `SYN`/`DEC`/`IO`/`BR`/`ANA` chapter 7 §7.9, `CNT` chapter 8 §8.5, `VQ` chapter 9, `RND` chapter 10, `GUI` chapter 11, `CLI` chapter 12.

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `MDL-GEN-0003` | Warning (once per frame) | `"motion vector clamped to ±32767 (%d cells)"` | an analysis plugin's MV conversion overflowed `short` (§6.2.3; HEVC only in practice) | H.265 §8.5.3.2.1 (luma MV range) |
| `MDL-GEN-0004` | Error | `"frame planes (%llu bytes) exceed the picture store cap (%llu)"` | §6.6.3: a single frame's requested stages do not fit the LRU cap; `OVT_E_NOMEM` | — |
| `MDL-CCH-0002` | Warning (once per session) | `"disk cache cap exceeded: all entries are open"` | §6.7.4 eviction cannot free space | — |
| `MDL-CSV-0001` | Warning | `"%d malformed, out-of-range or undeclared line(s) skipped (first at line %d)"` | YUView CSV importer (§6.8.2, WI-0.089) skipped rows | — |
| `MDL-CSV-0002` | Info | `"type '%s' is not an OVT type; not imported"` | YUView CSV importer: foreign `%;type` block | — |
| `MDL-CSV-0003` | Info | `"OVT type '%s' has no CellGrid field; not imported"` | YUView CSV importer: `OVT_Skip`, `OVT_Level`, `OVT_BitsX8`, `OVT_PartitionType`, `OVT_MergeOrDirect`, `OVT_MVD*`, `OVT_BlockGrid_*` | — |
| `MDL-CSV-0004` | Error | `"no usable seq-specs line (%s); no grid"` | YUView CSV importer: missing or unusable `%;seq-specs`, or a side above 16384 or an area above 8192 × 4320 luma samples | — |
| `MDL-VAL-0001` … `MDL-VAL-0032` (one code per rule: `MDL-VAL-0001` for V-001, `MDL-VAL-0002` for V-002, …, `MDL-VAL-0032` for V-032; all 32 codes are registered by this row) | Error (Warning where the rule says so) | the rule text of **V-nnn** with the offending values and a FlatBuffers-style `path` | `ovt::model::Validator`, `ovt index --validate`, `ovt analyze --validate` | §6.9 rule V-nnn |
| `VER-0001` | Error | `"reconstruction mismatch at (%d,%d) plane %d: %d != %d"` | verify mode plane comparison | — |
| `VER-0002` | Error | `"%s differs in %d cells"` | verify mode `CellGrid` comparison | — |
| `VER-0003` | Error | `"block tree differs (%d leaves only in %s)"` | verify mode block-tree comparison | — |
| `VER-0004` | Error | `"decoded picture hash mismatch (plugin %s)"` | `decoded_picture_hash` SEI check | H.265 D.3.19 |

---

## 6.10 Worked example: a 16×16 HEVC frame

Stream: HEVC Main, 16×16 luma, 4:2:0, 8-bit, `CtbSizeY = 16` (`log2_min_luma_coding_block_size_minus3 = 0`, `log2_diff_max_min_luma_coding_block_size = 1`), one I slice, `init_qp_minus26 = 0`, `slice_qp_delta = 6` ⇒ `SliceQpY = 32`, `cu_qp_delta_enabled_flag = 1`, `diff_cu_qp_delta_depth = 1` (quantization group 8×8), SAO off, deblocking on. The single CTU is split (`split_cu_flag = 1`) into four 8×8 CUs, all intra `PART_2Nx2N`, each with one PU and one 8×8 TU (`cbf_luma = 1`, chroma cbf 0). Intra modes: CU0 `IntraPredModeY = 1` (DC), CU1 `26` (vertical), CU2 `0` (planar), CU3 `26`. QPs: CU0 32, CU1 32, CU2 30 (`CuQpDeltaVal = -2`, `qPY_PRED = 32`), CU3 33 (`CuQpDeltaVal = +2`; its quantization group at (8,8) has `qPY_A` = CU2's `QpY` 30 and `qPY_B` = CU1's `QpY` 32, so `qPY_PRED = (30 + 32 + 1) >> 1 = 31` per 8.6.1 and `QpY = 33`). Chroma `intra_chroma_pred_mode = 4` (derived from luma).

Block ids follow the pre-order rule: `0` root, `1` CTU, then per CU `{CU, PU, TU}`: CU0 = 2 (PU 3, TU 4), CU1 = 5 (6, 7), CU2 = 8 (9, 10), CU3 = 11 (12, 13). Cell grid is 4×4 = 16 cells.

```json
{
  "schema_version": 1,
  "plugin_id": "ovt-plugin-de265",
  "plugin_version": "1.0.0+de265-1.0.15",
  "frame": {
    "id": { "decode_index": 0, "display_index": 0 },
    "type": "I", "shown": true, "show_existing_of": -1, "structure": "Frame",
    "temporal_id": 0, "layer_id": 0, "is_rap": true, "poc": 0,
    "pts": 0, "dts": 0, "timebase_num": 1, "timebase_den": 25,
    "size_bytes": 143, "units": [3],
    "coded_size": { "x": 0, "y": 0, "w": 16, "h": 16 }, "crop": { "x": 0, "y": 0, "w": 16, "h": 16 },
    "chroma_format": 1, "bit_depth_luma": 8, "bit_depth_chroma": 8, "qp_base": 32,
    "declared_refs": [], "dpb_after": [0],
    "ref_state_type": "HevcRps",
    "ref_state": { "PicOrderCntVal": 0, "short_term_ref_pic_set_sps_flag": 255, "short_term_ref_pic_set_idx": 255, "st_ref_pic_set": null,
                   "NumPicTotalCurr": 0, "RefPicSetStCurrBefore": [], "RefPicSetStCurrAfter": [], "RefPicSetStFoll": [],
                   "RefPicSetLtCurr": [], "RefPicSetLtFoll": [], "RefPicList0": [], "RefPicList1": [],
                   "collocated_from_l0_flag": 255, "collocated_ref_idx": 255, "NoRaslOutputFlag": 1, "pic_output_flag": 1,
                   "dpb_fullness_after": 1, "sps_max_dec_pic_buffering_minus1": 4, "sps_max_num_reorder_pics": 2, "sps_max_latency_increase_plus1": 0 },
    "frame_num": -1, "slice_count": 1
  },
  "grid": {
    "cells_w": 4, "cells_h": 4,
    "block_id":     [2, 2, 5, 5,   2, 2, 5, 5,   8, 8, 11, 11,   8, 8, 11, 11],
    "partition_id": [0, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0,     0, 0, 0, 0],
    "pred":         ["Intra","Intra","Intra","Intra", "Intra","Intra","Intra","Intra", "Intra","Intra","Intra","Intra", "Intra","Intra","Intra","Intra"],
    "qp":           [32, 32, 32, 32,  32, 32, 32, 32,  30, 30, 33, 33,  30, 30, 33, 33],
    "intra_mode_y": [1, 1, 26, 26,   1, 1, 26, 26,   0, 0, 26, 26,   0, 0, 26, 26],
    "intra_mode_c": [1, 1, 26, 26,   1, 1, 26, 26,   0, 0, 26, 26,   0, 0, 26, 26],
    "mv0_x": [], "mv0_y": [], "mv1_x": [], "mv1_y": [],
    "ref0":  [-1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1],
    "ref1":  [-1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1,  -1, -1, -1, -1],
    "tx_log2_w": [3, 3, 3, 3,  3, 3, 3, 3,  3, 3, 3, 3,  3, 3, 3, 3],
    "tx_log2_h": [3, 3, 3, 3,  3, 3, 3, 3,  3, 3, 3, 3,  3, 3, 3, 3],
    "tx_type":   [0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0],
    "cbf":       [1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1],
    "bits_x8":   [80, 80, 72, 72,  80, 80, 72, 72,  64, 64, 44, 44,  64, 64, 44, 44],
    "deblock_bs_v": [0, 0, 2, 0,  0, 0, 2, 0,  0, 0, 2, 0,  0, 0, 2, 0],
    "deblock_bs_h": [0, 0, 0, 0,  0, 0, 0, 0,  2, 2, 2, 2,  0, 0, 0, 0],
    "filter_idx":   [0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0]
  },
  "blocks": [
    { "id": 0, "parent": -1, "children": [1], "kind": "Other", "rect": { "x": 0, "y": 0, "w": 16, "h": 16 }, "level": 0,
      "partition_type": 255, "pred": "None", "skip": false, "merge_or_direct": false, "qp": 0, "bits_x8": 1048, "ext_type": "", "ext": null },
    { "id": 1, "parent": 0, "children": [2, 5, 8, 11], "kind": "CTU", "rect": { "x": 0, "y": 0, "w": 16, "h": 16 }, "level": 1,
      "partition_type": 255, "pred": "None", "skip": false, "merge_or_direct": false, "qp": 32, "bits_x8": 1048,
      "ext_type": "HevcCtu",
      "ext": { "CtbAddrInRs": 0, "CtbAddrInTs": 0, "slice_segment_idx": 0, "tile_idx": 65535, "end_of_slice_segment_flag": 1,
               "sao_merge_left_flag": 255, "sao_merge_up_flag": 255, "SaoTypeIdx": [], "sao_offset_abs": [], "sao_offset_sign": [],
               "SaoOffsetVal": [], "sao_band_position": [], "sao_eo_class": [], "bit_count_x8": 1048 } },
    { "id": 2, "parent": 1, "children": [3, 4], "kind": "CU", "rect": { "x": 0, "y": 0, "w": 8, "h": 8 }, "level": 2,
      "partition_type": 0, "pred": "Intra", "skip": false, "merge_or_direct": false, "qp": 32, "bits_x8": 320,
      "ext_type": "HevcCu",
      "ext": { "CtDepth": 1, "log2CbSize": 3, "cu_transquant_bypass_flag": 255, "cu_skip_flag": 255, "pred_mode_flag": 255,
               "CuPredMode": "MODE_INTRA", "palette_mode_flag": 255, "part_mode": "PART_2Nx2N", "pcm_flag": 0,
               "pcm_sample_luma": [], "pcm_sample_chroma": [], "rqt_root_cbf": 255,
               "cu_qp_delta_abs": 0, "cu_qp_delta_sign_flag": 255, "CuQpDeltaVal": 0, "IsCuQpDeltaCoded": 1,
               "qPY_PRED": 32, "QpY": 32, "QpY_prime": 32, "QpC_cb_prime": 31, "QpC_cr_prime": 31,
               "cu_chroma_qp_offset_flag": 255, "cu_chroma_qp_offset_idx": 255, "CuQpOffsetCb": 0, "CuQpOffsetCr": 0,
               "quant_group_x": 0, "quant_group_y": 0,
               "intra_chroma_pred_mode": [4], "IntraPredModeC": [1], "palette": null, "bit_count_x8": 320 } },
    { "id": 3, "parent": 2, "children": [], "kind": "PU", "rect": { "x": 0, "y": 0, "w": 8, "h": 8 }, "level": 3,
      "partition_type": 0, "pred": "Intra", "skip": false, "merge_or_direct": false, "qp": 32, "bits_x8": 24,
      "ext_type": "HevcPu",
      "ext": { "partIdx": 0, "prev_intra_luma_pred_flag": 1, "mpm_idx": 1, "rem_intra_luma_pred_mode": 255, "IntraPredModeY": 1,
               "candModeList": [0, 1, 26], "merge_flag": 255, "merge_idx": 255, "inter_pred_idc": "HevcInterPredIdc_Unknown",
               "ref_idx_l0": -1, "ref_idx_l1": -1, "mvd_l0_x": -32768, "mvd_l0_y": -32768, "mvd_l1_x": -32768, "mvd_l1_y": -32768,
               "mvp_l0_flag": 255, "mvp_l1_flag": 255, "mvL0_x": -32768, "mvL0_y": -32768, "mvL1_x": -32768, "mvL1_y": -32768,
               "predFlagL0": 255, "predFlagL1": 255, "refIdxL0": -1, "refIdxL1": -1, "merge_cand_source": "MC_Unknown",
               "ref_poc_l0": -2147483648, "ref_poc_l1": -2147483648, "weighted": 255,
               "LumaWeightL0": -32768, "luma_offset_l0": -32768, "LumaWeightL1": -32768, "luma_offset_l1": -32768,
               "ChromaWeightL0": [], "ChromaOffsetL0": [], "ChromaWeightL1": [], "ChromaOffsetL1": [] } },
    { "id": 4, "parent": 2, "children": [], "kind": "TU", "rect": { "x": 0, "y": 0, "w": 8, "h": 8 }, "level": 3,
      "partition_type": 0, "pred": "Intra", "skip": false, "merge_or_direct": false, "qp": 32, "bits_x8": 272,
      "ext_type": "HevcTu",
      "ext": { "trafoDepth": 0, "log2TrafoSize": 3, "blkIdx": 0, "split_transform_flag": 0, "interSplitFlag": 0,
               "cbf_luma": 1, "cbf_cb": [0], "cbf_cr": [0], "transform_skip_flag": [0, 255, 255],
               "explicit_rdpcm_flag": [], "explicit_rdpcm_dir_flag": [],
               "last_sig_coeff_x": [2, 255, 255], "last_sig_coeff_y": [1, 255, 255], "num_nonzero": [5, 0, 0], "coeff_abs_sum": [37, 0, 0],
               "sign_data_hiding_used": [1, 0, 0], "dst_used": 0, "cross_component_prediction_alpha": [], "scaling_list_used": 0,
               "coeff_blob_id": "", "bit_count_x8": 272 } },
    { "id": 5, "parent": 1, "children": [6, 7], "kind": "CU", "rect": { "x": 8, "y": 0, "w": 8, "h": 8 }, "level": 2,
      "partition_type": 0, "pred": "Intra", "skip": false, "merge_or_direct": false, "qp": 32, "bits_x8": 288, "ext_type": "HevcCu", "ext": { "...": "IntraPredModeC [26], QpY 32, quant_group_x 8" } },
    { "id": 6,  "parent": 5,  "kind": "PU", "rect": { "x": 8, "y": 0, "w": 8, "h": 8 }, "level": 3, "...": "IntraPredModeY 26" },
    { "id": 7,  "parent": 5,  "kind": "TU", "rect": { "x": 8, "y": 0, "w": 8, "h": 8 }, "level": 3, "...": "cbf_luma 1" },
    { "id": 8, "parent": 1, "children": [9, 10], "kind": "CU", "rect": { "x": 0, "y": 8, "w": 8, "h": 8 }, "level": 2,
      "partition_type": 0, "pred": "Intra", "skip": false, "merge_or_direct": false, "qp": 30, "bits_x8": 256, "ext_type": "HevcCu",
      "ext": { "...": "cu_qp_delta_abs 2, cu_qp_delta_sign_flag 1, CuQpDeltaVal -2, qPY_PRED 32, QpY 30, quant_group_y 8" } },
    { "id": 9,  "parent": 8,  "kind": "PU", "rect": { "x": 0, "y": 8, "w": 8, "h": 8 }, "level": 3, "...": "IntraPredModeY 0" },
    { "id": 10, "parent": 8,  "kind": "TU", "rect": { "x": 0, "y": 8, "w": 8, "h": 8 }, "level": 3, "...": "cbf_luma 1" },
    { "id": 11, "parent": 1, "children": [12, 13], "kind": "CU", "rect": { "x": 8, "y": 8, "w": 8, "h": 8 }, "level": 2,
      "partition_type": 0, "pred": "Intra", "skip": false, "merge_or_direct": false, "qp": 33, "bits_x8": 176, "ext_type": "HevcCu",
      "ext": { "...": "cu_qp_delta_abs 2, cu_qp_delta_sign_flag 0, CuQpDeltaVal 2, qPY_PRED 31, QpY 33" } },
    { "id": 12, "parent": 11, "kind": "PU", "rect": { "x": 8, "y": 8, "w": 8, "h": 8 }, "level": 3, "...": "IntraPredModeY 26" },
    { "id": 13, "parent": 11, "kind": "TU", "rect": { "x": 8, "y": 8, "w": 8, "h": 8 }, "level": 3, "...": "cbf_luma 1" }
  ],
  "mvs": [],
  "refs": [],
  "partitions": [
    { "kind": "Slice", "index": 0, "parent": -1, "unit_index": 3,
      "file_span": { "offset": 97, "length": 143 }, "data_bit_offset": 72, "bit_count": 1072,
      "header": { "name": "slice_segment_header", "descriptor": "", "kind": "None", "children": [
          { "name": "first_slice_segment_in_pic_flag", "descriptor": "u(1)", "kind": "UInt", "uvalue": 1, "span": { "bit_offset": 16, "bit_length": 1 }, "spec_ref": "H.265 7.3.6.1" },
          { "name": "no_output_of_prior_pics_flag", "descriptor": "u(1)", "kind": "UInt", "uvalue": 0, "span": { "bit_offset": 17, "bit_length": 1 }, "spec_ref": "H.265 7.3.6.1" },
          { "name": "slice_pic_parameter_set_id", "descriptor": "ue(v)", "kind": "UInt", "uvalue": 0, "span": { "bit_offset": 18, "bit_length": 1 }, "spec_ref": "H.265 7.3.6.1" },
          { "name": "slice_type", "descriptor": "ue(v)", "kind": "Enum", "uvalue": 2, "enum_name": "I", "span": { "bit_offset": 19, "bit_length": 3 }, "spec_ref": "H.265 7.3.6.1" },
          { "name": "slice_qp_delta", "descriptor": "se(v)", "kind": "Int", "ivalue": 6, "span": { "bit_offset": 60, "bit_length": 7 }, "spec_ref": "H.265 7.3.6.1" },
          { "...": "remaining slice header elements" } ] },
      "slice_type": 2, "rect": { "x": 0, "y": 0, "w": 16, "h": 16 }, "first_block_addr": 0, "block_count": 1,
      "qp": 32, "qp_delta": 6, "deblocking_disable": 0, "deblock_alpha_or_beta_offset_div2": 0, "deblock_beta_or_tc_offset_div2": 0,
      "loop_filter_across_slices": 1, "sao_luma": 0, "sao_chroma": 0, "tile_row": 65535, "tile_col": 65535,
      "num_entry_points": 0, "cabac_init": 255, "max_num_merge_cand": 5, "num_ref_idx_active": [], "diagnostics": [] }
  ],
  "filter_params_type": "HevcFilterParams",
  "filter_params": { "pps_deblocking_filter_disabled_flag": 0, "pps_beta_offset_div2": 0, "pps_tc_offset_div2": 0,
                     "deblocking_filter_override_enabled_flag": 0, "slice_deblocking_filter_disabled_flag": [0],
                     "slice_beta_offset_div2": [0], "slice_tc_offset_div2": [0],
                     "pps_loop_filter_across_slices_enabled_flag": 1, "loop_filter_across_tiles_enabled_flag": 1, "pcm_loop_filter_disabled_flag": 0,
                     "sample_adaptive_offset_enabled_flag": 0, "slice_sao_luma_flag": [0], "slice_sao_chroma_flag": [0],
                     "edge_flags_v": [10], "edge_flags_h": [12], "film_grain_characteristics": null },
  "planes": [
    { "stage": "Reconstruction", "plane": 0, "width": 16, "height": 16, "stride_bytes": 64, "bit_depth": 8, "sample_bytes": 1, "is_signed": false,
      "blob_id": "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0/0/recon/y" },
    { "stage": "Reconstruction", "plane": 1, "width": 8, "height": 8, "stride_bytes": 64, "bit_depth": 8, "sample_bytes": 1, "is_signed": false,
      "blob_id": "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0/0/recon/u" },
    { "stage": "Reconstruction", "plane": 2, "width": 8, "height": 8, "stride_bytes": 64, "bit_depth": 8, "sample_bytes": 1, "is_signed": false,
      "blob_id": "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0/0/recon/v" }
  ],
  "symbols": [], "symbol_names": [],
  "qp_hist": [ "0 × 158", 4, 0, 8, 4, "0 × 94" ],
  "diagnostics": []
}
```

The `slice_segment_header` above is abridged: the `bit_offset` values of the shown children and `data_bit_offset = 72` are illustrative (a real 16×16 stream's header is shorter), not re-derived; they are not part of the golden fixture of WI-0.093, which stores the full eight-element header with consistent spans (bits 16..31, `byte_alignment()` included) and therefore `data_bit_offset = 32` (RBSP position counted from the NAL header bit 0, §6.2.7). The chroma QPs follow Table 8-10 (ChromaArrayType 1, offsets 0): `QpC_cb_prime`/`QpC_cr_prime` are 31, 31, 29, 32 for the CUs with `QpY` 32, 32, 30, 33. Reading the example against the rules: `grid.block_id` names only CUs (V-011); `bits_x8` cells sum to 320/288/256/176 (V-020: 320/4 = 80 exactly, etc.); the CTU's 1048 ≥ 320+288+256+176 = 1040 (V-019; the 8 units are the `split_cu_flag` and `end_of_slice_segment_flag`); `deblock_bs_v` is 2 on the left edge of the cells at `x4 = 2` (the CU boundary at x = 8; the picture edge at x = 0 is not filtered), `deblock_bs_h` is 2 on the top edge of row `y4 = 2` (y = 8), both intra ⇒ bS = 2 (§8.7.2.4); `edge_flags_v = [10]` encodes the one vertical 8-aligned interior edge at x = 8 in both 8-rows (bits 1 and 3 of a 2×2 bit grid), `edge_flags_h = [12]` the horizontal one (bits 2, 3); `qp_hist` bins `128+30`, `128+32`, `128+33` hold 4, 8, 4 (the `"0 × n"` tokens are abridgement, the real array has 256 integers). The `"..."` keys are abridgement too; a real export has every field.

---

## 6.11 Work items (Phase 0, this chapter)

All WIs below produce LGPL code under `core/` unless stated. Common dependency: WI-0.0xx "repository skeleton + CMake presets + FlatBuffers via vcpkg" from the Platforms chapter is referred to as **WI-0.001** (repo skeleton) and **WI-0.010** (third-party FlatBuffers/zstd available in the build) here.

### WI-0.080 — Write `ovt_model.fbs` and make it compile
**Goal:** Land the complete neutral + codec-extension schema of §6.3 in the repository and prove it compiles for all target languages.
**Depends on:** WI-0.010.
**Produces:** `core/schema/ovt_model.fbs`; `tools/schema_hash.py` (prints SHA-256 of a schema file's text, used by `meta.json`); `tools/check_schema_in_spec.py`; `core/tests/schema/test_model_schema.cpp`.
**Interface:** §6.3 (schema text is the interface).
**Steps:** 1. Copy the schema from §6.3 verbatim into `core/schema/ovt_model.fbs`. 2. Generate code through the `ovt_schemas` target of WI-0.003 (`flatc --cpp --python --kotlin --reflect-names --gen-name-strings --gen-object-api --gen-compare`, output below the generated-code root `build/<preset>/generated/` of chapter 13 §13.2.3 — the single generated-code layout rule; no `--ts`: TypeScript output is not a deliverable, chapter 16 §16.3.10); fix any syntax issue by editing the spec chapter **and** the file (they must stay identical; add a CI check `tools/check_schema_in_spec.py` that diffs the fenced block against the file). 3. Write `tools/schema_hash.py`. 4. Write a Catch2 test that builds an empty `FrameAnalysis`, finishes with identifier `"OVTA"`, verifies it, and asserts `FrameAnalysisIdentifier() == "OVTA"`, `StreamIndex` finished with `"OVTI"` verifies with `flatbuffers::Verifier(buf, size).VerifyBuffer<ovt::fb::StreamIndex>("OVTI")` (flatc generates `Verify<T>Buffer` only for the schema's `root_type`; secondary roots use the generic API, and `flatbuffers::BufferHasIdentifier` checks the identifier). 5. Add unit tests asserting the spec numbering of every enum against literal tables (`AvcMbType::B_8x8 == 53`, `Av1YMode::NEW_NEWMV == 24`, `Av1TxSize::TX_64X16 == 18`, `HevcPartMode::PART_nRx2N == 7`, …; one assertion per enum value).
**Acceptance tests:** 1. Given the repo with `core/schema/ovt_model.fbs`, When `cmake --build --preset linux-x64-debug --target ovt_schemas` runs, Then `build/linux-x64-debug/generated/ovt_model_generated.h`, `build/linux-x64-debug/generated/python/ovt/fb/FrameAnalysis.py` and the Kotlin sources under `build/linux-x64-debug/generated/kotlin/` exist and the C++ test target `test_model_schema` links. 2. Given `docs/spec/06-data-model.md` and `core/schema/ovt_model.fbs`, When `tools/check_schema_in_spec.py docs/spec/06-data-model.md core/schema/ovt_model.fbs` runs, Then exit code 0 (identical). 3. Given the built test, When `ctest --preset pr -R test_model_schema` runs, Then all enum-numbering assertions and the identifier assertions of step 4 pass.
**Size:** M.
**Notes / pitfalls:** Fields named `type` (`Unit.type`, `FrameInfo.type`, `TrackReference.type`, `ItemReference.type`, `ItemInfo.type`) are deliberate: `type` is not a FlatBuffers keyword, the generated C++ (`type()`), Python (`Type()`) and Kotlin (`type`) accessors clash with no generated member at the pinned FlatBuffers version, and none of these tables is a union; step 2 compiling all three languages is the check. FlatBuffers forbids two enum values with the same name inside one enum but allows them across enums; keep the `Avc`/`Hevc`/`Av1` prefixes on enum *types* and the spec names on *values*. `byte`-typed enums (`Av1RefFrame`) need `: byte`, not `: ubyte`. Do not add `(required)` to any vector; empty vectors are the "not provided" signal (P6). `flatc` warns about PascalCase field names that mirror spec variables (`QpY`, `IntraPredModeY`, …): the schema keeps the spec names (P8), so the build invokes `flatc` **without** `--warnings-as-errors` and the CI log filter ignores only the `field names should be lowercase` warning class.

### WI-0.081 — Write `ovt_container.fbs`
**Goal:** Land the container schema of §6.4.
**Depends on:** WI-0.080.
**Produces:** `core/schema/ovt_container.fbs`; `core/tests/schema/test_container_schema.cpp`.
**Interface:** §6.4.
**Steps:** 1. Copy the schema; compile with `flatc` for all languages (the `include "ovt_model.fbs"` must resolve via `-I core/schema`). 2. Test: build a `ContainerRoot` whose `tree.tracks[0]` is a `TrackInfo` (`track_id 1`) and whose `sample_tables[0]` is a 3-sample `SampleTable` with the same `track_id`, finish with `"OVTC"`, verify, read back. 3. Extend `tools/check_schema_in_spec.py` to cover this file.
**Acceptance tests:** 1. Given `core/schema/ovt_container.fbs`, When the `ovt_schemas` target builds it for C++, Python and Kotlin (no TypeScript, WI-0.080 step 2), Then `flatc` reports no errors. 2. Given `test_container_schema`, When `ctest --preset pr -R test_container_schema` runs, Then `sample_tables[0].dts()->size() == 3` round-trips, `ContainerRootIdentifier() == "OVTC"` and the table list of §6.4 equals the set of tables in the compiled `.bfbs`. 3. Given the extended spec check, When `tools/check_schema_in_spec.py docs/spec/06-data-model.md core/schema/ovt_container.fbs` runs, Then exit 0.
**Size:** S.
**Notes / pitfalls:** floating-point fields (`width`, `height`, `duration_seconds`, `media_rate`) must not be used as map keys or compared for byte identity after JSON round trip without `%.9g` formatting — the JSON exporter test (WI-0.088) covers this. `flatc` applies the `root_type` / `file_identifier` of the **last** parsed declaration, so the including file's `ContainerRoot` / `"OVTC"` win over the included `ovt_model.fbs` declarations; the test asserts `ContainerRootIdentifier() == "OVTC"` to catch a regression if flatc changes this behaviour.

### WI-0.082 — Write `ovt_metrics.fbs`
**Goal:** Land the metrics schema of §6.5.
**Depends on:** WI-0.080.
**Produces:** `core/schema/ovt_metrics.fbs`; `core/tests/schema/test_metrics_schema.cpp`.
**Interface:** §6.5.
**Steps:** 1. Copy, compile, add to the spec check. 2. Test: build a `MetricsReport` with one `MetricsResult` (two `FrameScore`s, one `MapBlob`, one `PoolingResult`), one `BdRateResult` with `Interpolator::Pchip`; finish `"OVTR"`; verify; assert `Interpolator::Akima == 2`.
**Acceptance tests:** 1. Given `core/schema/ovt_metrics.fbs`, When the `ovt_schemas` target builds it for C++, Python and Kotlin, Then `flatc` reports no errors. 2. Given the `MetricsReport` of step 2, When `ctest --preset pr -R test_metrics_schema` runs, Then the buffer finishes with identifier `"OVTR"`, passes `VerifyMetricsReportBuffer`, reads back both `FrameScore` values, the `MapBlob` id and the `BdRateResult.interpolator == Interpolator::Pchip` unchanged, and `Interpolator::Akima == 2`. 3. Given the extended spec check, When `tools/check_schema_in_spec.py docs/spec/06-data-model.md core/schema/ovt_metrics.fbs` runs, Then exit 0.
**Size:** S.
**Notes / pitfalls:** `MetricsResult` has fields appended after the verbatim backbone block — keep them *after* `engine` so field ids stay stable.

### WI-0.083 — CMake integration for generated code and schema versioning
**Goal:** Generate C++/Python/Kotlin code at build time, embed the binary schemas (`.bfbs`) as resources, and expose compile-time schema majors.
**Depends on:** WI-0.080, WI-0.081, WI-0.082.
**Produces:** extensions to `cmake/OvtFlatBuffers.cmake` (created by WI-0.003, chapter 13; one file, one function `ovt_add_flatbuffers_schema(<target> <fbs>...)`; see chapter 16 §16.3.10) — `.bfbs` generation and embedding, Python/Kotlin output per the generated-code layout rule of chapter 13 §13.2.3; `core/include/ovt/cxx/model/schema_id.hpp` (`enum class SchemaId`, below); `core/include/ovt/cxx/model/schema_version.hpp` (a plain committed header in namespace `ovt::model` — `ovt::fb` stays reserved for flatc output: `inline constexpr uint32_t kModelSchemaMajor = 1`, `kContainerSchemaMajor = 1`, `kMetricsSchemaMajor = 1`, and `extern const std::string_view kModelSchemaSha256`, whose value `tools/schema_hash.py` computes at configure time into `build/<preset>/generated/model/schema_hash.inc`); `core/src/model/bfbs_resources.cpp` (a committed source holding `bfbs()`, `schema_major()`, `root_table_name()` and the hash constant; it `#include`s the byte arrays that `tools/embed_bfbs.py` writes to `build/<preset>/generated/ovt_schemas_bfbs.inc`, so nothing generated is committed); `tools/embed_bfbs.py`; `tools/ci/test_regen.sh`. Python modules are generated to `build/<preset>/generated/python/ovt/fb/<Type>.py` plus `__init__.py` (`--python`, namespace layout — `--gen-onefile` would write `<schema>_generated.py` in the output root instead; chapter 13 §13.2.3) and copied into the wheel's `ovt/fb/` package by WI-0.095.
**Interface:**
```cpp
// core/include/ovt/cxx/model/schema_id.hpp
namespace ovt::model {
enum class SchemaId : uint8_t { Model = 1, Container = 2, Metrics = 3 };   // ovt_model.fbs, ovt_container.fbs, ovt_metrics.fbs
std::span<const uint8_t> bfbs(SchemaId);                                     // embedded binary schema
uint32_t                 schema_major(SchemaId);                             // kModelSchemaMajor / kContainerSchemaMajor / kMetricsSchemaMajor
std::string_view         root_table_name(SchemaId);                          // "ovt.fb.FrameAnalysis", "ovt.fb.ContainerRoot", "ovt.fb.MetricsReport"
}
```
`SchemaId` is the schema selector used by `canonicalize` (WI-0.085), `to_json`/`from_json` (WI-0.088) and the validator (WI-0.086).
**Steps:** 1. Extend WI-0.003's `ovt_add_flatbuffers_schema` in `cmake/OvtFlatBuffers.cmake` (custom command per schema, `--reflect-names --gen-name-strings --gen-object-api --gen-compare --scoped-enums --cpp-std c++17` for C++ (`--gen-name-strings` provides the `T::GetFullyQualifiedName()` that `ext_as` compares against `ext_type`); `--python` (namespace layout) and `--kotlin`, all below `build/<preset>/generated/` exactly as chapter 13 §13.2.3 states). 2. `flatc --schema --binary --bfbs-comments` to produce `.bfbs`; embed via `tools/embed_bfbs.py` (generates a `.cpp` with `alignas(8) const uint8_t[]`). 3. `generated/model/schema_hash.inc` written by CMake `configure_file` from the hash `tools/schema_hash.py` computes at configure time (`schema_version.hpp` itself is never configured: `core/include/` is installed unchanged). 4. Test: `bfbs(Model)` loads with `reflection::GetSchema` and `root_table()->name() == "ovt.fb.FrameAnalysis"`.
**Acceptance tests:** 1. Given a clean build, When `core/schema/ovt_model.fbs` is touched and `ovt_core` is rebuilt by `tools/ci/test_regen.sh`, Then `build/<preset>/generated/ovt_model_generated.h` is regenerated (newer mtime) and nothing is written into the source tree. 2. Given the resource test, When `ctest --preset pr -R test_bfbs_resources` runs, Then `root_table_name(SchemaId::Model|Container|Metrics)` and the root tables of the three embedded `.bfbs` equal `ovt.fb.FrameAnalysis`, `ovt.fb.ContainerRoot`, `ovt.fb.MetricsReport`, and `schema_major()` returns 1 for each. 3. Given `PYTHONPATH=build/<preset>/generated/python`, When `python -c "import ovt.fb.FrameAnalysis as m; m.FrameAnalysis"` runs, Then it exits 0.
**Size:** M.
**Notes / pitfalls:** `--scoped-enums` changes C++ enum spelling to `ovt::fb::AvcMbType::I_PCM`; use it consistently (the C ABI uses plain integers). Pin the `flatc` version to the FlatBuffers library version in `vcpkg.json` (`flatbuffers` port) — mismatched generator/library versions produce confusing compile errors. **Overlap:** extends `cmake/OvtFlatBuffers.cmake` created by WI-0.003 (function `ovt_add_flatbuffers_schema`); do not create `core/cmake/OvtFlatBuffers.cmake`. See chapter 16 §16.3.

### WI-0.084 — Model helper library: grid, tree, units, labels
**Goal:** Provide the C++ helpers every consumer uses so that coordinate and unit rules are implemented once.
**Depends on:** WI-0.083.
**Produces:** `core/include/ovt/cxx/model/grid.hpp`, `tree.hpp`, `units.hpp`, `labels.hpp` (C++ headers live under `ovt/cxx/`, D22, chapter 5 §5.2.1); `core/src/model/{grid,tree,units,labels}.cpp`; `core/tests/model/test_grid.cpp`, `test_tree.cpp`, `test_units.cpp`, `test_labels.cpp`, `core/tests/model/labels_golden.txt` (one line per label: `<Enum>.<value>\t<label>` for schema enums and `<field>.<Codec>.<value>\t<label>` for the codec-dependent tables, e.g. `intra_mode_y.H265.26\tINTRA_ANGULAR26`; hand-checked).
**Interface:**
```cpp
namespace ovt::model {
struct CellIndex { uint32_t x4, y4; };
inline uint32_t cell_index(const fb::CellGrid&, CellIndex);                 // y4*cells_w + x4, bounds-checked (throws std::out_of_range)
CellIndex cell_at(const fb::CellGrid&, int luma_x, int luma_y);              // coded-coordinate sample -> cell
fb::Rect chroma_rect(fb::Rect luma, uint8_t chroma_format);                 // §6.2.2
int16_t mv_to_neutral(int32_t spec_mv, fb::Codec);                          // ×4 / ×4 / ×2, clamps to ±32767 (−32768 is the MVD sentinel, §6.2.3)
std::optional<int16_t> mv_to_neutral_checked(int32_t spec_mv, fb::Codec);   // nullopt when the product leaves ±32767
int32_t mv_from_neutral(int16_t neutral, fb::Codec);                        // inverse (exact; throws std::invalid_argument when not divisible)
double  mv_in_luma_samples(int16_t neutral);                                 // / 16.0
struct TreeWalker { const fb::FrameAnalysis& fa; void pre_order(std::function<void(const fb::Block&, int depth)>); const fb::Block* leaf_at(int luma_x, int luma_y, fb::BlockKind preferred) const; std::vector<const fb::Block*> ancestors(uint32_t id) const; };   // leaf_at: the grid leaf's subtree, then its ancestors, else the grid leaf; nullptr outside the grid
template <class T> const T* ext_as(const fb::Block&);                        // §6.3.3, nullptr on mismatch / verify failure
std::string_view label_for(std::string_view field, fb::Codec, int32_t value); // §6.3.1 tables, e.g. ("intra_mode_y", H265, 26) -> "INTRA_ANGULAR26"; fields: pred, intra_mode_y, intra_mode_c, tx_type, filter_idx, and for Block.partition_type the per-kind mb_type / sub_mb_type (H.264), part_mode (HEVC), partition (AV1); any schema enum name returns the enumerator name; sentinels (−1, 0xFF) return ""
std::string blob_id(std::span<const uint8_t,32> hash, int decode_index, fb::PlaneStage, uint8_t plane);   // §6.6.2
struct BlobIdParts { std::array<uint8_t,32> source_hash; int32_t decode_index; BlobKind kind /* Plane | MetricMap | TransformBlock */; fb::PlaneStage stage; std::string metric_id; uint32_t tb_block_id; uint8_t plane /* 0 y, 1 u, 2 v, 3 a */; };
std::string blob_id(const BlobIdParts&);                                    // also the "map-" and "tb-" stages the four-argument form cannot express
bool parse_blob_id(std::string_view, BlobIdParts& out);                      // strict ABNF; `out` unchanged on failure
}
```
**Steps:** 1. Implement `grid.hpp`/`units.hpp` with the tables of §6.2.2/§6.2.3 as `constexpr` arrays. 2. Implement `TreeWalker` (`leaf_at` uses `grid.block_id`, searches the grid leaf's subtree for the preferred kind, then its ancestors, and otherwise returns the grid leaf; `nullptr` outside the grid). 3. Implement `ext_as<T>` with `flatbuffers::Verifier` on the nested buffer and a `T::GetFullyQualifiedName()` comparison against `ext_type` (strip the `ovt.fb.` namespace). 4. Implement `label_for` from the §6.3.1 tables and the enum names of the generated code (`EnumNameAvcMbType`, `EnumNameHevcPredMode`, `EnumNameAv1YMode` and every other `EnumName*` of the schema). 5. Implement blob-id build/parse with a hand-written matcher (no regex library). 6. Tests: conversion tables (H.264 mv 3 → 12; AV1 mv 3 → 6; overflow clamps), chroma rects for all formats, blob ids (valid, each invalid class), labels for every enum value (snapshot test against `core/tests/model/labels_golden.txt`).
**Acceptance tests:** 1. Given the four test files of Produces, When `ctest --preset pr -R "test_(grid|tree|units|labels)"` runs, Then all pass (H.264 mv 3 → 12, AV1 mv 3 → 6, HEVC overflow clamps to 32767 and reports it, every `label_for` value equals `core/tests/model/labels_golden.txt`). 2. Given `parse_blob_id("ab/1/recon/y")` (short hash), When called, Then it returns false. 3. Given the relevant parts of the §6.10 fixture built in `test_tree.cpp` (grid, 14 blocks, `HevcCu`/`HevcPu`/`HevcCtu` ext; WI-0.093's `tiny16_hevc.fa` may replace it), When `leaf_at(9, 9, CU)` is called, Then block id 11 is returned and `ancestors(11) == {1, 0}`.
**Size:** M.
**Notes / pitfalls:** `cell_at` must use floor division for negative inputs (they are invalid → throw). `mv_from_neutral` for AV1 divides by 2 and throws `std::invalid_argument` when the value is odd (a check that also holds in release builds); a stray odd value means a producer stored 1/8-pel values unscaled.

### WI-0.085 — Canonical builder and byte-identity tooling
**Goal:** Implement `canonicalize()` (V-032) and a comparison tool that explains the first differing field between two buffers.
**Depends on:** WI-0.084, WI-0.093 (fixtures the byte-identity tests run on).
**Produces:** `core/include/ovt/cxx/model/canonical.hpp`; `core/src/model/canonical.cpp` (incl. the structural FlatBuffer diff used by `ovt dump --diff A B`, chapter 12 §12.2.6; the CLI wiring is WI-1.507's); tests `core/tests/model/test_canonical.cpp`.
**Interface:** `std::vector<std::string> ovt::model::structural_diff(std::span<const uint8_t> a, std::span<const uint8_t> b, SchemaId, std::string_view root_table)` (one line `<path>: <a value> != <b value>` per difference, all differences in field-id order, FlatBuffers-style paths as in §6.9; an absent field equals its default and an absent string/vector equals an empty one; enums print their names, bools `true`/`false`, strings quoted; a length mismatch prints `<path>.length: na != nb` and then compares the common prefix; nested buffers continue the path (`blocks[2].ext.QpY`); empty when equal; `ovt dump --diff` of WI-1.507 prints these lines and maps non-empty to exit 8); `std::vector<uint8_t> ovt::model::canonicalize(std::span<const uint8_t> buffer, SchemaId, std::string_view root_table)`; rebuilds the buffer in the canonical form of V-032 (the object API `Pack` layout, using the generated `Create*`/`Pack` helpers); sorting the vectors listed in §6.3.7 **is not** done (sorting would hide producer bugs); nested buffers are canonicalised recursively, keeping the §6.3.3 alignment.
**Steps:** 1. Rebuild through the generated object API in the V-032 order (not `flatbuffers::CopyTable`, which adds fields in name order and does not recurse into nested buffers), deterministic. 2. Recurse into nested buffers keyed by `*_type`. 3. The structural diff behind `ovt dump --diff A B` (chapter 12 §12.2.6): walks both trees via reflection and prints every differing path in field-id order (details in the Interface line). 4. Tests: canonicalising the §6.10 fixture twice is idempotent; a buffer built with `ForceDefaults(true)` canonicalises to the same bytes as the fixture (and the validator reports V-032 for the original).
**Acceptance tests:** 1. Given fixture `tiny16_hevc.fa`, When canonicalised, Then the output SHA-256 equals the fixture's recorded hash. 2. Given two edits of `tiny16_hevc.fa` that set `grid.qp[5]` to 30 and to 31 respectively (the fixture's own value is 32), When `structural_diff(a, b, SchemaId::Model, "ovt.fb.FrameAnalysis")` runs, Then it returns exactly `{"grid.qp[5]: 30 != 31"}`, and for two identical buffers it returns an empty vector (the CLI exit code 8 / 0 mapping is tested by WI-1.507).
**Size:** M.
**Notes / pitfalls:** Nested-buffer vectors are plain `[ubyte]` to reflection; recurse explicitly using the `*_type` field. Build nested buffers first, then the outer table, so that the outer builder's alignment matches a native producer's.

### WI-0.086 — `FrameAnalysis` / `StreamIndex` validator (V-001…V-032)
**Goal:** Implement every rule of §6.9 as the `ovt::model::Validator` library, exposed by chapter 12 as `ovt index --validate` (WI-1.505) and `ovt analyze --validate` (WI-2.521).
**Depends on:** WI-0.085, WI-0.093 (golden fixtures).
**Produces:** `core/include/ovt/cxx/model/validator.hpp`; `core/src/model/validator.cpp`; `core/include/ovt/validate.h` (C API `ovt_validate_frame_analysis`, `ovt_validate_stream_index` — declared in chapter 12 §12.3, signatures in the Interface line below — returning violations as `Diagnostic`s with `code = "MDL-VAL-0nnn"`, §6.9); `core/tests/model/test_validator.cpp`; fixture mutators `core/tests/model/mutate.h` (helpers that break one rule at a time).
**Interface:** `struct Violation { std::string rule; fb::Severity severity; std::string message; std::string path; }; std::vector<Violation> ovt::model::validate_frame_analysis(std::span<const uint8_t>, std::span<const uint8_t,32> expected_source_hash, std::optional<fb::Codec> codec_override); std::vector<Violation> validate_frame_analysis(std::span<const uint8_t>, std::optional<fb::Codec> codec_override); /* no expected hash: V-023 checks the grammar only */ std::vector<Violation> validate_stream_index(std::span<const uint8_t>); std::vector<Violation> validate_metrics_result(std::span<const uint8_t>); /* V-030; added in a later part once WI-0.088's canonical JSON exists */ struct RuleInfo { std::string_view id; /* "V-001" */ std::string_view intent; fb::Severity severity; }; std::span<const RuleInfo> ovt::model::Validator::rules();` (the rule table chapter 14 §14.6.1 iterates). C API as declared in chapter 12 §12.3 (`ovt/validate.h`): `ovt_status ovt_validate_frame_analysis(const uint8_t* fb, size_t len, const uint8_t* expected_source_hash /*nullable, 32 bytes*/, int codec_override /*-1 none, else ovt.fb.Codec*/, ovt_Buffer* out /* DiagnosticList */)`, `ovt_status ovt_validate_stream_index(const uint8_t* fb, size_t len, ovt_Buffer* out)`. CLI exposure: `ovt index --validate` and `ovt analyze --validate [--plugin ...] <stream|file.fa|dir>` (chapter 12 §12.2.4 / §12.2.8; exit 0/5/3 per §12.1.3).
**Steps:** 1. Implement V-001–V-009 (structure). 2. V-010–V-020 (grid/tree consistency). 3. V-021–V-025 (ext, planes, symbols, order). 4. V-026–V-029 (semantic ranges). 5. V-030–V-032 (metrics, index, canonical). 6. For each rule write one mutator that produces a minimal violation from the §6.10 fixture and assert exactly that rule fires (table-driven test, 32 rows). 7. C API wrappers that emit violations as `Diagnostic{code:"MDL-VAL-0012", severity, message /* begins with "V-012" */, path}` (a bare `"V-012"` code would fail V-029's regex) so that `ovt index --validate` / `ovt analyze --validate` (chapter 12) print them in `diagnostics[]`.
**Acceptance tests:** 1. Given `tiny16_hevc.fa`, `tiny32_h264.fa` and `tiny64_av1.fa` (WI-0.093), When `validate_frame_analysis` runs in `test_validator.cpp`, Then it returns no violations (the CLI form `ovt analyze --validate` and its exit codes are tested by WI-2.521). 2. Given each of the 32 mutated fixtures (29 `FrameAnalysis` rows, V-031 on a `StreamIndex`, V-030 on a `MetricsResult`, V-032), When validated, Then exactly the intended rule id appears (and no false positive on the unmutated fixture). 3. Given a truncated buffer, When validated, Then `V-001` (`MDL-VAL-0001`) is reported and nothing crashes under ASan/UBSan. 4. Given a 4K synthetic grid (960×540 cells, 130k blocks), When `bench_validator` runs on `m1-air-8gb`, Then it stays within its budget in chapter 14 §14.9.3 / `tests/perf/budgets.json` (planning value ≤ 200 ms).
**Size:** L.
**Notes / pitfalls:** V-012's converse ("every leaf owns a cell") must ignore blocks that lie entirely outside the coded picture (possible for AV1 superblock children on the border). V-015 compares grid MVs only when the grid vectors are non-empty. Use 64-bit accumulators for V-019/V-020 sums. **Overlap:** no `cli/src/cmd/validate.cpp`: chapter 12 §12.2 has no validation subcommand; the index checks are exposed as `ovt index --validate` (WI-1.505) and the analysis checks as `ovt analyze --validate` (WI-2.521); this WI's acceptance tests run through `core/tests/model/test_validator.cpp`. See chapter 16 §16.3.

### WI-0.087 — `PictureStore`: plane blobs, LRU, pinning
**Goal:** Implement the in-memory plane store of §6.6 with the budgets of §6.6.3 and the C API view types.
**Depends on:** WI-0.084.
**Produces:** `core/include/ovt/cxx/model/picture_store.hpp`, `plane.hpp` (incl. `PlaneBlob` and `struct NoMemError : std::runtime_error`); `core/src/model/picture_store.cpp`; `core/include/ovt/decode.h` additions (`ovt_PlaneView`, `ovt_FrameId`, `ovt_Picture` with the chapter 12 §12.3.9 layouts); `core/include/ovt/color.h` holding only the §0.5.6 `ovt_ColorDescription` struct (sizeof 44, layout pinned by static_asserts; the `ovt_hdr_measure`/`ovt_color_*` functions are added by their WIs; `ovt_picture_ref`/`ovt_picture_release` by the first decoder WI that hands out an `ovt_Picture`); tests `core/tests/model/test_picture_store.cpp`.
**Interface:**
```cpp
namespace ovt::model {
class PictureStore {
 public:
  struct Limits { uint32_t analysis_frames = 64; uint32_t plane_frames = 4; uint32_t map_frames = 16; uint64_t hard_cap_bytes = 2ull << 30; };   // every limit non-zero
  explicit PictureStore(Limits);
  std::shared_ptr<const PlaneBlob> put(std::string blob_id, PlaneView src /* copied unless `adopt` */, std::unique_ptr<uint8_t[]> adopt = {});
  std::shared_ptr<const PlaneBlob> get(std::string_view blob_id);     // nullptr if absent (caller regenerates)
  void pin(std::string_view blob_id); void unpin(std::string_view blob_id);
  void put_analysis(int decode_index, std::span<const uint8_t,32> hash, std::vector<uint8_t> fa);   // LRU of FrameAnalysis buffers
  std::shared_ptr<const std::vector<uint8_t>> get_analysis(int decode_index, std::span<const uint8_t,32> hash);
  Stats stats() const;   // resident bytes, counts, evictions
  void set_evict_callback(std::function<void(std::string_view blob_id, PlaneView)>);   // the cache layer hooks here to write planes to disk when enabled
};
}
```
**Steps:** 1. Blob storage with 64-byte-aligned rows (`stride_bytes = round_up(width*sample_bytes, 64)`). 2. LRU per category keyed by `(hash, decode_index)`; frames count as one entry regardless of stage/plane count. 3. Pinning and `OVT_E_NOMEM` behaviour. 4. Thread safety (one `std::mutex`, no locks held during copies larger than 1 MiB — copy outside the lock then insert). 5. Tests: budgets, LRU order, pinned survival, hard cap, concurrency (8 threads put/get), zero-copy `adopt`.
**Acceptance tests:** 1. Given 5 frames of 1080p recon, When they are put in order, Then `get` of frame 0 returns nullptr and frames 1–4 hit. 2. Given frames 0–3 resident and frame 0 pinned, When frames 4–8 are put, Then frame 0 still hits and `stats().evictions == 5` (evictions count frames). 3. Given `hard_cap_bytes = 64 MiB` and an 8K 10-bit 4:2:0 frame (≈ 100 MB), When its planes are put one by one, Then the first chroma plane throws `ovt::model::NoMemError` (the luma plane alone, ≈ 66 MB, fits) (declared in `plane.hpp`: `struct NoMemError : std::runtime_error { using std::runtime_error::runtime_error; };`, mapped to `OVT_E_NOMEM` at the C API) and nothing is evicted. 4. Given the 8-thread put/get concurrency test, When it runs under the `linux-x64-tsan` preset, Then TSan reports no race.
**Size:** M.
**Notes / pitfalls:** Do not evict inside `get()` while handing out a `shared_ptr`; the `owner` semantics keep evicted blobs alive for holders. Use `std::pmr` or plain `new` with explicit alignment (`std::align_val_t{64}`), and free with the matching delete.

### WI-0.088 — JSON exporter/importer (reflection based)
**Goal:** Implement §6.8.1 for all three schemas with nested-buffer expansion and hex byte vectors; and the reverse import.
**Depends on:** WI-0.085.
**Produces:** `core/include/ovt/cxx/model/json.hpp`; `core/src/model/json_export.cpp`, `json_import.cpp` (there is no `export` subcommand; the CLI reaches the exporter through `--format json` of `ovt analyze`/`ovt index`, wired by WI-2.521/WI-1.505); tests `core/tests/model/test_json.cpp` and golden files `core/tests/golden/tiny16_hevc.json`.
**Interface:** `std::string ovt::model::to_json(std::span<const uint8_t> buf, SchemaId, std::string_view root_table, JsonOptions{bool compact=false; bool strict_numbers=false;})`; `std::vector<uint8_t> from_json(std::string_view json, SchemaId, std::string_view root_table)`.
**Steps:** 1. `GenerateText` with the fixed options over the embedded `.bfbs`. 2. Post-process with a streaming rewrite (no JSON DOM library in `core/`): while emitting, the generator is intercepted at the five nested-buffer fields (expanded recursively with the sub-schema) and at the hash-type byte-vector fields (emitted as lowercase hex); the interception is driven by reflection field names, never by string search on the output. 3. Importer: `flatbuffers::Parser` with the `.bfbs`-derived schema text (`flatc --cpp` cannot be used at runtime; keep the `.fbs` text embedded too, `tools/embed_bfbs.py --also-text`); nested objects are parsed with the sub-schema root and re-embedded as bytes; hex strings decoded. 4. Golden tests for the §6.10 fixture and for an index and a metrics report.
**Acceptance tests:** 1. Given `tiny16_hevc.fa`, When exported with `to_json`, Then the output equals `core/tests/golden/tiny16_hevc.json` byte-for-byte. 2. Given that JSON, When imported with `from_json` and canonicalised, Then the bytes equal the fixture (round trip). 3. Given `JsonOptions{compact = true}`, When exported, Then the output has no newline and parses with Python `json.loads`. 4. Given a `MetricsResult` with a NaN score, When exported with `JsonOptions{strict_numbers = true}`, Then the value is written as `"nan"` and the file parses with `json.loads`.
**Size:** L.
**Notes / pitfalls:** FlatBuffers emits `uint64` as numbers (JSON consumers lose precision above 2^53); file offsets stay below that in practice, but hashes must be hex strings (hence the rule). Field order in JSON must follow schema order for determinism — `GenerateText` guarantees it.

### WI-0.089 — YUView statistics CSV exporter and importer
**Goal:** Implement §6.8.2 exactly, verified against YUView.
**Depends on:** WI-0.084.
**Produces:** `core/src/model/yuview_csv.cpp` (+ header `core/include/ovt/cxx/model/yuview_csv.hpp`); `tools/yuview_palette.json` (the CLI options `--format yuview` (alias `yuview-csv`, chapter 12 §12.2.0), `--yuview-types`, `--yuview-poc-column` are wired by WI-2.521 in `cli/src/cmd/analyze.cpp`); tests `core/tests/model/test_yuview_csv.cpp` with golden `core/tests/golden/tiny16_hevc.csv`; `core/tests/data/yuview/*.csv` sample CSVs for the importer (each with a CC0 `.license` file), written by this WI under CC0 (YUView's own sample files are GPL-3.0 and are neither fetched at test time nor redistributed).
**Interface:** `void ovt::model::write_yuview_csv(std::ostream&, const StreamIndex&, std::span<const FrameAnalysis*> frames_in_display_order, YuviewOptions{std::vector<uint16_t> types; bool poc_column_is_poc=false; std::string basename; double fps;})`; `std::vector<uint8_t> read_yuview_csv(std::istream&, int poc) -> FrameAnalysis` (grid-only buffers with `plugin_id = "yuview-csv-import"`).
**Steps:** 1. Header writer with the fixed type table (§6.8.2) and palette. 2. Row emitters: per-block, per-rect, per-cell with the "uniform" rule. 3. Sorting and value filtering (sentinels skipped). 4. Importer: parse `%;type` blocks and rows into `CellGrid` vectors by *name* (`OVT_*` types map back to their fields; foreign type names become extra `symbol_names`-like labelled grids — out of scope, reported as `MDL-CSV-0002`; OVT types without a `CellGrid` field as `MDL-CSV-0003`; skipped rows as `MDL-CSV-0001`; §6.9.1). The importer sizes the grid from `seq-specs` and refuses a side above 16384 or an area above 8192 × 4320 luma samples (`MDL-CSV-0004`); painting is capped at 32 × the cell count so that huge repeated rows stay fast under fuzzing. 5. Manual verification step recorded in the WI log: load `tiny16_hevc.csv` plus its YUV in YUView 2.14 and screenshot; fix keyword spellings if YUView rejects any. This step and acceptance test 3 are recorded as pending in `docs/acceptance/wi/WI-0.089.md` until a YUView binary is available (Mac Mini, or a CI job once the workflow PRs settle).
**Acceptance tests:** 1. Given `tiny16_hevc.fa`, When exported with `write_yuview_csv`, Then the output equals `core/tests/golden/tiny16_hevc.csv`. 2. Given the golden CSV, When imported with `read_yuview_csv`, Then `grid.qp` and `grid.intra_mode_y` equal the fixture's. 3. Given YUView 2.14 on a desktop (manual record `docs/acceptance/wi/WI-0.089.md`; YUView 2.14 has no `--loadfile`/`--quit-after-load` and no exit code — files are plain arguments and the application stays open), When started with `YUView tiny16_16x16.yuv tiny16_hevc.csv` (the YUV copied under a name carrying its size so YUView infers 16×16), Then both items load with no error dialog and the statistics panel lists the OVT types (`OVT_PredMode` … `OVT_BlockGrid_TU`), recorded with a screenshot.
**Size:** M.
**Notes / pitfalls:** YUView parses `fps` as a float and `width;height` as ints; do not write `1e+01`. Semicolons inside names are forbidden — type names are fixed ASCII identifiers. The POC column must be `display_index` unless asked, or vectors land on the wrong frames for AV1 streams with hidden frames. Exporter choices: (a) a type block is written when the type's source exists in at least one exported frame (a non-empty `CellGrid` vector, a leaf block, an MV of that list or a block of that kind), even when every value is a sentinel (`OVT_Ref0` on an intra frame gets a header and no rows); (b) "leaf block" means a block of kind MB/CU/Block with no MB/CU/Block child; (c) the `0..15` palette for Ref0/1 and the TxType, FilterIdx and PartitionType colours all reuse `tools/yuview_palette.json` (value mod 35); (d) `OVT_PartitionId` uses the uniform rule, in practice one row per leaf block; (e) `n_partitions` in `defaultRange 0 <n_partitions-1>` is the largest `partitions.size()` over the exported frames, at least 1.

### WI-0.090 — libaom `inspect`-compatible JSON exporter
**Goal:** Implement §6.8.3 so that aomanalyzer loads OVT output.
**Depends on:** WI-0.084, WI-0.093 (the `tiny64_av1.fa` fixture the tests export).
**Produces:** `core/src/model/aom_inspect_json.cpp` (+ header `core/include/ovt/cxx/model/aom_inspect_json.hpp`); tests `core/tests/model/test_aom_json.cpp`; golden `core/tests/golden/tiny64_av1.inspect.json` (expected export of `tiny64_av1.fa`, written by `gen_fixtures` of WI-0.093 from the §6.8.3 rules, never by hand); `core/tests/golden/aom_inspect_keys.txt` (the key set libaom v3.12.1 `inspect` writes with `-A`, one key per line, copied from §6.8.3); `tools/aomanalyzer_smoke.js`. The CLI option `ovt analyze --format aom-inspect` is wired by WI-2.521; the comparison against libaom's own `inspect` output on a real stream is the golden suite WI-2.581 (chapter 14; phase 2, needs `ovt-plugin-aom`; it supersedes chapter 7 WI-2.161).
**Interface:** `void ovt::model::write_aom_inspect_json(std::ostream&, std::span<const FrameAnalysis*> frames_in_decode_order, AomJsonOptions{int decode_index_offset=0; std::string ovt_version; /* empty = product version; golden tests pass "0.0.0" */})`.
**Steps:** 1. Build the MI arrays from `grid.block_id` → `Av1Block` ext (cache per block). 2. Write with a minimal streaming writer (no DOM), in the key order and framing of §6.8.3 (`[`, objects each followed by `,`, `null`, `]`), with the `*Map` objects copied from the `map_entry` tables of `inspect.c` at the pin. 3. Test the export of `tiny64_av1.fa` against `tiny64_av1.inspect.json` and its key set against `aom_inspect_keys.txt` (`blockSize`, `transformSize`, `transformType`, `dualFilterType`, `mode`, `uv_mode`, `motion_mode`, `compound_type`, `skip`, `filter`, `cdef_level`, `cdef_strength`, `cfl_alpha_idx`, `cfl_alpha_sign`, `delta_q`, `seg_id`, `motionVectors`, `intrabc`, `palette`, `uv_palette`, `referenceFrame`, the `*Map` keys, `symbolsMap`, `symbols`, `frame`, `showFrame`, `frameType`, `baseQIndex`, `tileCols`, `tileRows`, `deltaQPresentFlag`, `deltaQRes`, `config`, `configString`).
**Acceptance tests:** 1. Given `tiny64_av1.fa`, When `write_aom_inspect_json` exports it, Then the output equals `core/tests/golden/tiny64_av1.inspect.json` byte-for-byte and its per-frame key set equals `aom_inspect_keys.txt` without the accounting keys `symbolsMap`/`symbols` when the record has no `symbols` (a second check exports the fixture with three `SymbolCost` records added and compares with the full 46-key set; no `sb`, `mm`, `cfl`, `ibc`, `plt`, `uvp` or single `cdef` key). 2. Given the exported file, When `node tools/aomanalyzer_smoke.js <file>` runs, Then it parses the file as one JSON array (last element `null`), every per-MI layer has `mi_rows × mi_cols` = 16 × 16 elements and the script exits 0 (loading in aomanalyzer itself is a manual step documented with a screenshot in the WI log).
**Size:** M.
**Notes / pitfalls:** libaom writes `motionVectors` as `[col, row, col, row]` (x before y) while the spec's `Mv[]` is `[row, col]`; `Av1Block.Mv` stores spec order. `inspect` writes `transformSize = -1` only with `-snt 1` (`skip_non_transform`); OVT reproduces the default (`-snt 0`) output, so −1 never appears and OVT's 255 sentinel is never written. `tileCols`/`tileRows` are the first tile's size in MI units, not tile counts.

### WI-0.091 — Disk cache: layout, `meta.json`, read/write, zstd
**Goal:** Implement §6.7.1–6.7.2: cache directory per source, atomic metadata, compressed frame/plane/map files, mmap-ed index.
**Depends on:** WI-0.087, WI-0.010 (zstd).
**Produces:** `core/include/ovt/cache.h` (C API as declared in chapter 12 §12.3.14: `ovt_cache_open(const char* root /*nullable = default dir*/, const char* opts_json /*nullable: {"max_bytes":n,"read_only":false}*/, ovt_Cache**)` — options are a JSON string, not a struct (D4 d) —, `ovt_cache_entry(ovt_Cache*, const uint8_t source_hash[32], ovt_CacheEntry**)`, `ovt_cache_put_index`/`ovt_cache_get_index`, `ovt_cache_put_frame`/`ovt_cache_get_frame(…, uint32_t decode_index, …)`, `ovt_cache_put_plane`/`ovt_cache_get_plane(…, uint32_t decode_index, uint8_t stage, uint8_t plane, …)`, `ovt_cache_put_report`/`ovt_cache_get_report(…, const char* kind, …)`, `ovt_cache_entry_release`, `ovt_cache_close`); `core/src/cache/{cache.cpp, meta_json.cpp, zstd_io.cpp, atomic_file.cpp}`; `docs/spec/cache-meta-1.schema.json` (the JSON Schema of §6.7.2); tests `core/tests/cache/test_cache.cpp`.
**Interface:** as above plus C++ `ovt::cache::Cache` wrapper; file names exactly per §6.7.1 (`%08d.fa.zst`, `<stage>-<plane>.pl.zst`).
**Steps:** 1. Paths and directory creation; `cache.json` load/save with `std::filesystem`. 2. Atomic write (`tmp` + `fsync` + `rename`), Android-safe (same filesystem). 3. zstd stream compression (level 3 frames, level 1 planes) with `ZSTD_CCtx` reuse; decompression with size check. 4. `meta.json` writer/reader with validation against the embedded JSON Schema (hand-written checks; no JSON Schema library). 5. `index.fb` written then mmap-ed (`RandomAccessFile`); verified on open. 6. Tests: write/read round trips, crash safety (kill between tmp and rename simulated by a flag), concurrent readers.
**Acceptance tests:** 1. Given a `StreamIndex` and 3 `FrameAnalysis`, When put and the cache reopened, Then all read back byte-identical and `meta.json` validates with `python -m jsonschema -i meta.json docs/spec/cache-meta-1.schema.json`. 2. Given a simulated crash before rename, When `ovt_cache_open` reopens the cache, Then the entry has the previous `meta.json` and no `.tmp` files remain. 3. Given 100 frames of 1080p grids (≈ 5 MB each), When written, Then total size ≤ 100 MB (zstd ≥ 5× on grids) and write throughput meets the `bench_cache_write` budget of chapter 14 §14.9.3 on `m1-air-8gb` (planning value ≥ 200 MB/s uncompressed). 4. Given `OVT_CACHE_DIR` unset and `XDG_CACHE_HOME` unset on Linux, When `ovt_cache_open(NULL, NULL, &c)` runs, Then the root is `~/.cache/ovt/` (macOS: `~/Library/Caches/org.openvideotools.ovt/`; §6.7.1 cache-root rule).
**Size:** L.
**Notes / pitfalls:** `rename` over an existing file is atomic on POSIX; on Android use the app cache dir (same mount). Never store absolute paths in `meta.json`. `mtime` in the source hash means a touched file invalidates — intended.

### WI-0.092 — Cache invalidation and size-cap eviction
**Goal:** Implement §6.7.3 (I1–I7) and §6.7.4.
**Depends on:** WI-0.091, WI-0.086.
**Produces:** `core/src/cache/invalidate.cpp`, `core/src/cache/evict.cpp`; `docs/spec/cache-json-1.schema.json` (JSON Schema of `cache.json`, §6.7.1); tests `core/tests/cache/test_invalidate.cpp`, `test_evict.cpp`. (`cli/src/cmd/cache.cpp` — `ovt cache list|stats|clear|path|import`, chapter 12 §12.2.13 — is produced by WI-1.509, which calls this WI's functions.)
**Interface:** `ovt::cache::Cache::open_entry(hash, codec, std::optional<SelectedPlugin{plugin_id, plugin_version}>) -> Entry` applies I1–I7 as §6.7.3 states (`codec` is the stream's codec: an `index.fb` of another codec fails I4; `std::nullopt` = no plugin check; `Cache::entry()` stays raw); `Cache::enforce_cap()`; `Cache::clear(filter)` and `Cache::list()` used by the CLI grammar of chapter 12 §12.2.13 — kinds: `index` = `index.fb`, `container.fb.zst`; `analysis` = `frames/`, `planes/`, `reports/*` other than `metrics`; `metrics` = `maps/`, `metrics/`, `reports/metrics.fb.zst`; `meta.json` belongs to no kind; `clear --kind k` deletes only those files from matching closed entries and updates `meta.json`, an entry left with only `meta.json` is removed whole, open entries are skipped and reported (`ovt cache list`, `ovt cache stats`, `ovt cache clear [--kind k] [--older-than <days>] [--source <path>] [--yes]`, `ovt cache path`; WI-1.509); the cap is the `cache.max_size` setting (chapter 5 §5.8), not a subcommand.
**Steps:** 1. Implement each I-rule as a function returning an `InvalidationReason` enum; log at info. 2. Eviction loop per §6.7.4 with the open-entry exclusion. 3. `last_used` throttling (≤ 1 write/min). 4. Tests: one test per I-rule (corrupt `meta.json`, bumped major, wrong plugin version, plane size mismatch, …); eviction order test with 5 entries and a small cap; "all entries open" warning path.
**Acceptance tests:** 1. Given an entry written with `schemas.ovt_model.major = 2`, When opened by a major-1 build, Then the directory is deleted and recreated (I3). 2. Given a frame file written by plugin `A 1.0` and current plugin `A 1.1`, When that frame is requested, Then the file is deleted and `get_frame` returns "miss" (I5), other frames untouched. 3. Given cap 100 MB and 5 entries of 40 MB (36 MB `planes/` + 4 MB `frames/`) with distinct `last_used`, When `enforce_cap()` runs, Then the oldest closed entries lose `planes/`, then `maps/`, `reports/`, `frames/` and the whole directory, in that order, until total ≤ 90 MB (entries 1 and 2 removed whole, entry 3 loses `planes/`, total 84 MB), and `cache.json` reflects it. 4. Given the cache after test 3, When `cache.json` is validated with `python -m jsonschema -i cache.json docs/spec/cache-json-1.schema.json`, Then it is valid.
**Size:** M.
**Notes / pitfalls:** Do not recompute directory sizes by walking on every write (slow on Android); maintain `bytes` incrementally and re-walk only on `ovt cache list` or when `cache.json` is missing.

### WI-0.093 — Golden fixtures: hand-built `FrameAnalysis` records and round-trip tests
**Goal:** Produce the §6.10 fixture and two sibling fixtures (one H.264 MB-level, one AV1 SB-level) as checked-in binaries with recorded hashes, used by WI-0.085–0.090.
**Depends on:** WI-0.084.
**Produces:** `core/tests/golden/tiny16_hevc.fa`, `tiny32_h264.fa`, `tiny64_av1.fa` (+ `.sha256`); generator `core/tests/golden/gen_fixtures.cpp` (deterministic builder code — the fixtures are regenerated, never hand-edited; it also writes `core/tests/golden/tiny64_av1.inspect.json` for WI-0.090); the matching `tiny16.yuv` (16×16 4:2:0 synthetic gradient) for YUView checks; the round-trip test `core/tests/model/test_model_roundtrip.cpp`, registered as CTest `test_model_roundtrip` (the test the phase-0 exit demo and gate run, chapter 16 §16.1.1 #10, §16.6.1 #9).
**Interface:** none new; exercises the generated API.
**Steps:** 0. `test_model_roundtrip`: for each fixture, read it with the generated API, verify it (fixtures are stored **not** size-prefixed, the C ABI form, so `VerifyFrameAnalysisBuffer`; the test additionally round-trips the size-prefixed cache/stdout encoding), rebuild it from the object API (`UnPack` → `Pack` with `ForceDefaults(false)`), and compare bytes and SHA-256 with the checked-in file and its `.sha256`. 1. Encode the §6.10 example in builder code (every field, no abridgement). 2. H.264 fixture: 32×32, 4 MBs: I_16x16_2_0_0 (DC), P_L0_16x16 with mv (x 3, y −1) quarter-pel → neutral (12, −4), P_8x8 with four `sub_mb_type`s, P_Skip; two `RefSlot`s (`L0[0]`, `L0[1]`). 3. AV1 fixture: 64×64, one SB with `PARTITION_SPLIT` into four 32×32: DC_PRED intra, NEWMV with `Mv (row −5, col 7)` 1/8-pel → neutral `mv_y = −10`, `mv_x = 14` and `Mvd`, GLOBALMV with `is_global = 1`, NEAREST_NEARESTMV compound with `RefFrame [LAST, ALTREF]`; `cdef_idx` 2; one `Av1LrUnit` per plane. 4. Record SHA-256s; a CI job regenerates and diffs. 5. All three must satisfy the V-rules of §6.9 by construction (the validator itself is WI-0.086, which runs on these fixtures in its acceptance test 1).
**Acceptance tests:** 1. Given `gen_fixtures`, When run twice into two empty directories, Then the outputs are byte-identical (determinism). 2. Given the checked-in fixtures, When `ctest --preset pr -R test_model_roundtrip` runs, Then every fixture verifies, its fields read back with the values of §6.10 (e.g. `grid.qp` of `tiny16_hevc.fa` = `[32,32,32,32, 32,32,32,32, 30,30,33,33, 30,30,33,33]`), the object-API round trip is byte-identical and the SHA-256 equals the `.sha256` file. 3. Given a regenerated fixture set, When compared with the checked-in files by SHA-256 in the CI job of step 4, Then no file differs.
**Size:** M.
**Notes / pitfalls:** For the AV1 fixture, cells of the compound block must have both `ref0` and `ref1` set and `mv1_*` filled, and `usage_cells` of LAST and ALTREF both count those cells (V-016). `tiny64_av1.inspect.json` follows libaom v3.12.1 `inspect.c` except that the file ends with `]` and a newline (the end-of-file hook requires one); intra blocks write `filter = [0,0]`, `dualFilterType = 0`, `motion_mode = 0`; `configString` carries the fixed `ovt 0.0.0`. Delivered in two parts per §16.2.6: part a (helpers, `gen_fixtures`, `tiny16_hevc.fa`, `tiny16.yuv`, the round-trip test) and part b (the H.264 and AV1 fixtures and `tiny64_av1.inspect.json`).

### WI-0.094 — Schema evolution and robustness tests (compatibility, fuzzing)
**Goal:** Prove the additive-evolution rule (P7) and harden the readers against hostile buffers.
**Depends on:** WI-0.086, WI-0.093.
**Produces:** `core/tests/schema/evolution/ovt_model_v1_plus.fbs` (a copy of the schema with one extra field appended to `FrameAnalysis`, `Block`, `CellGrid` and one extra enum value in `PredKind`); test `core/tests/schema/test_evolution.cpp`; fuzz targets `core/tests/fuzz/fuzz_frame_analysis.cpp`, `fuzz_stream_index.cpp`, `fuzz_json_import.cpp`, `fuzz_yuview_csv.cpp`, `fuzz_cache_meta.cpp`; CI step `fuzz-smoke` (60 s per target).
**Interface:** n/a.
**Steps:** 1. Compile the `v1_plus` schema in a separate namespace (`ovt.fb.evo`) and read the WI-0.093 fixtures with it; assert all original fields read identically and new fields return defaults. 2. Write a buffer with `v1_plus`, read it with the real schema; assert the unknown field is ignored and `canonicalize()` drops it (document that canonicalisation by an older reader loses newer fields — expected; the cache stores the writer's bytes, not canonicalised bytes). 3. libFuzzer targets feeding bytes to `Verifier` + `validate_*` + `to_json`; must never crash or exceed 1 s per input. 4. Seed corpus from the fixtures and mutated fixtures.
**Acceptance tests:** 1. Given the `v1_plus` schema and the WI-0.093 fixtures, When `ctest --preset pr -R test_evolution` runs, Then old fields read identically through `v1_plus`, new fields read their defaults, and a `v1_plus` buffer read with the real schema ignores the unknown field. 2. Given the `linux-x64-fuzz` preset (`OVT_ENABLE_FUZZ=ON`), When each fuzz target runs 60 s in the CI step `fuzz-smoke`, Then no crash, timeout or leak is reported under ASan+UBSan. 3. Given a buffer whose `grid.block_id` vector length field claims 2^31 elements, When verified, Then `Verifier` rejects it before any rule runs (no allocation).
**Size:** M.
**Notes / pitfalls:** Verifier limits: set `flatbuffers::Verifier::Options{max_depth = 64, max_tables = 10'000'000}`; the default `max_tables = 1'000'000` is too small for a 4K HEVC block tree (≈ 130k CUs × 3 kinds is fine, but 8K + TUs exceeds it).

### WI-0.095 — Python access to the schemas
**Goal:** Make every FlatBuffer readable from Python without the C API (for research scripts and the test suite's oracles).
**Depends on:** WI-0.083, WI-0.088.
**Produces:** packaging of the generated modules `build/<preset>/generated/python/ovt/fb/` namespace package (`__init__.py` plus one `<Type>.py` per table/enum of the three schemas; chapter 13 §13.2.3) into the wheel's `ovt/fb/` package (never committed under `python/`), `python/ovt/model.py` (`load_frame_analysis(path) -> FrameAnalysis`, `grid_as_numpy(fa) -> dict[str, np.ndarray]` reshaped to `(cells_h, cells_w)`, `blocks_as_dataframe(fa)` optional pandas, `mv_to_luma_samples(v)`), `python/tests/test_model.py`; `pyproject.toml` deps: `flatbuffers`, `numpy`, `zstandard`.
**Interface:** as named; `grid_as_numpy` returns views over the buffer (no copy) using `flatbuffers` `*AsNumpy()` accessors.
**Steps:** 1. Package the generated modules. 2. Implement the helpers, including `.fa.zst` decompression and size-prefix handling. 3. Tests against the WI-0.093 fixtures: `grid_as_numpy(fa)["qp"].reshape(4,4)[2,0] == 30`; JSON export from Python equals the C++ golden (via `flatbuffers` Python `Parser`? not available — instead compare the C++ `ovt analyze --format json` output against values read in Python for 20 fields).
**Acceptance tests:** 1. Given the venv with the built package, When `pytest python/tests/test_model.py` runs, Then all tests pass, including `import ovt.fb.FrameAnalysis` and `grid_as_numpy(load_frame_analysis("tiny16_hevc.fa"))["qp"].reshape(4,4)[2,0] == 30`. 2. Given `tiny64_av1.fa`, When `grid_as_numpy` is called, Then `mv0_x` has dtype `int16` and shape `(16, 16)`.
**Size:** S.
**Notes / pitfalls:** The FlatBuffers Python runtime version must equal the `flatc` version used (WI-0.083 pin). `*AsNumpy()` requires the vector to be non-empty; return an empty array of the right dtype for empty vectors.

(WI-0.096–0.099 are reserved for this chapter's follow-ups: VP9/H.266 extension tables when those codecs are scheduled, and an Arrow IPC exporter if the research persona asks for it.)

---

### Glossary additions (chapter 6)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **MBAFF pair parity** — within a field macroblock pair in an MBAFF picture, 0 = top-field MB, 1 = bottom-field MB (`AvcMb.pair_parity`).
- **CDEF index (`cdef_idx`)** — the per-64×64 selector (0..7) into the frame's CDEF strength table in AV1 (§5.11.56).
- **BD-rate interpolator** — the curve-fitting method used to integrate rate–distortion curves: classic cubic polynomial, PCHIP (piecewise cubic Hermite, monotone; JVET/AOM common test conditions), or Akima.
- **RawVideoFormat** — the `ovt_container.fbs` table describing an uncompressed input (Y4M header values or user-supplied raw YUV geometry: size, chroma format, bit depth, frame rate, colour) (§6.4).
- **TrackReference** — one `tref` entry of an ISOBMFF track (reference type four-cc and the referenced track ids) (§6.4).
- **HdrSignal** — one place where HDR signalling was found or looked for (box, SEI, OBU metadata, VUI) with its value and provenance; `HdrMetadata.signals[]` (§6.4).
- **GroupMembership** — an ISOBMFF sample-group assignment (`sbgp`/`sgpd` grouping type, description index) for a sample range (§6.4).
- **TxSize** — the AV1 transform size enumeration (`Av1TxSize`, AV1 §6.10.16) stored per transform block; the neutral `tx_log2_w/h` is derived from it (§6.3).
- **CliEnvelope** — the `ovt_model.fbs` table (identifier `"OVTE"`) that every `ovt` subcommand emits around its result records: `ovt_version`, `schema_version`, `command`, `input_path`, `input_size`, `source_hash`, `record_count`, `diagnostics`, `status` (chapter 12 §12.1.4 is the field-list owner) (§6.3).
- **PluginList** — the record returned by `ovt plugins`: one entry per loaded analysis or metric plugin with id, version, codec/metric ids, licence and caps (`ovt_model.fbs`, identifier `"OVTP"`, §6.3).
- **`MDL-VAL-0nnn`** — the diagnostic code under which validator rule V-nnn reports a violation (§6.9).
