# 8 — Container inspection and demuxing

> **Status:** normative chapter of the OVT Requirements & Architecture Specification. Conforms to `00-backbone.md` (ADR-001 C++20, ADR-006 own ISOBMFF reader + libavformat demux, ADR-010 FlatBuffers). Background facts: Appendix B.2 and F.6. Where this chapter and the backbone disagree, the backbone wins.
>
> **Audience:** implementers (coding agents) with little ISOBMFF background. Every algorithm is numbered; every interface is code; every box lists its fields in spec order. Spec citations: `14496-12` = ISO/IEC 14496-12:2022 (ISOBMFF), `14496-14` = MP4 file format, `14496-15` = NAL-unit-structured video carriage (AVC/HEVC/VVC), `23001-7` = Common Encryption (CENC), `23000-19` = CMAF, `23008-12` = HEIF, `AV1-ISOBMFF` = AOM "AV1 Codec ISO Media File Format Binding" v1.2.0, `VP-ISOBMFF` = "VP Codec ISO Media File Format Binding", `Opus-ISOBMFF` = "Encapsulation of Opus in ISO Base Media File Format" v0.8.1, `DV-ISOBMFF` = Dolby "Dolby Vision Streams Within the ISO Base Media File Format" v2.4, `QTFF` = Apple QuickTime File Format Specification (developer.apple.com), `ETSI TS 102 366` = AC-3/E-AC-3.

---

## 8.1 Scope and the two roles

### 8.1.1 What the container module does

The `container` and `demux` modules of `libovt-core` (backbone §0.3) answer two different questions and are deliberately **two separate code paths**:

| Role | Question answered | Implementation | Output |
|---|---|---|---|
| **A. Inspection** | "What exactly is in this file, byte by byte?" | OVT's own readers in `core/src/container/` (ISOBMFF, IVF, Annex B, AV1 OBU, Y4M, raw YUV; later EBML, MPEG-TS, DASH/HLS virtual file, MXF) | `ContainerTree` FlatBuffer (boxes → fields → byte/bit spans), `TrackInfo[]`, `SampleTable`, `EditList`, `FragmentTimeline`, `EncryptionInfo`, `HdrMetadata`, `Diagnostic[]` |
| **B. Demux** | "Give me the next compressed packet for the decoder." | `ovt::demux::Demuxer`, a thin C++ wrapper over **libavformat** (`core/src/demux/`) | `Packet` objects (one container sample) split into **units** (NAL units / OBUs) in the form the plugin ABI expects (backbone §0.6.2) |

Rules:

1. Role A MUST never depend on libavformat. Role B MUST never be used to build the inspection tree.
2. Everything the GUI shows in the container panel, hex view, sample table and timeline panels comes from Role A.
3. The **playback path** (ADR-009) always uses Role B.
4. The **analysis path** (ADR-009) uses Role A's `SampleTable` to locate sample bytes for the formats OVT has its own reader for (`isobmff`, `ivf`, `annexb`, `obu`), reading bytes through `RandomAccessFile` and splitting them into units with the unit splitter of §8.7.4. For formats without an own reader in the current phase (Matroska, MPEG-TS, MXF before phase 4) the analysis path uses Role B packets. The choice is made once per `ovt_Stream` by `ovt::container::unit_source_policy(format)` and is reported in `StreamIndex.diagnostics` as `CNT-0001` (Info) so a tester can see which path produced the units.
5. Both roles read the file **only** through `ovt::io::RandomAccessFile` (backbone §0.3). No `mmap`, `fopen`, `std::ifstream` anywhere in `core/src/container/` or `core/src/demux/`.

### 8.1.2 Requirements realised by this chapter

Requirement ids are defined only in chapter 04; this table copies chapter 04's sentences and names what realises them here. Ids in the `FR-<AREA>-1nn` range were added to chapter 04 from this chapter's review.

| ID | Requirement (chapter 04 text) | Realised by | Acceptance test |
|---|---|---|---|
| FR-GEN-004 | OVT MUST open the following inputs by content sniffing, not by file extension: ISOBMFF (`ftyp`/`moov`/`moof`/`styp` at offset 0 or after a `wide`/`free` box), IVF (`DKIF` signature), H.264/HEVC Annex B byte streams (start code `0x000001`), AV1 low-overhead OBU streams (OBU header with `obu_has_size_field = 1`, AV1 §5.3.1) and AV1 Annex B length-delimited streams, Y4M (`YUV4MPEG2` magic) and headerless raw YUV (requires a format description). | §8.6.1 (`format_sniff.cpp`), WI-1.200 | Given `tests/corpora/manifest.json` entries tagged `container-smoke` of every format, When `ovt probe f` runs, Then `ContainerTree.format` equals the manifest's `expected.format` for every file. |
| FR-GEN-011 | OVT MUST accept files of at least 64 GiB and MUST NOT read the entire file into memory to open it. | §8.1.1 rule 5, §8.2.2 (`RandomAccessFile` windows), §8.2.4 (lazy tree), §8.8 (page cache) | Given a 4 GiB MP4 with `moov` first, When the tree is built, Then the `RandomAccessFile` read counter reports < 16 MiB read (test `isobmff_lazy_mdat`). |
| FR-CNT-001 | The ISOBMFF reader MUST parse every box in a file into a `BoxNode` with `fourcc`, `file_span` (whole box including header), `header_length` (8, 16 for `largesize`, +16 for `uuid`) and `children`, including boxes whose type is unknown to the registry (emitted with empty `fields` and a non-empty `payload_span`). | §8.2.3–§8.2.5, WI-1.201 | Given any MP4 tagged `container-smoke`, When `ovt probe --boxes --expand all --format json f.mp4` runs, Then the sum of top-level `file_span.length` equals the file size, and every box reported by `MP4Box -diso` is present with the same size. |
| FR-CNT-002 | The reader MUST decode the payload fields of at least these boxes into `Field` nodes with `BitSpan` relative to the box start and `spec_ref`: `ftyp`, `mvhd`, `tkhd`, `mdhd`, `hdlr`, `elst`, `stsd` and its sample entries, `avcC` (ISO/IEC 14496-15 §5.3.3.1), `hvcC` (§8.3.3.1), `av1C` (AV1 ISOBMFF binding §2.3), `colr`, `pasp`, `clap`, `mdcv`, `clli`, `btrt`, `stts`, `ctts`, `stss`, `stsc`, `stsz`/`stz2`, `stco`/`co64`, `sdtp`, `sgpd`, `sbgp`, `mehd`, `trex`, `mfhd`, `tfhd`, `tfdt`, `trun`, `sidx`, `ssix`, `emsg`, `pssh`, `tenc`, `senc`, `saiz`, `saio`, `schm`, `frma`. | §8.3 (hand decoders), §8.2.8 (`FieldBuilder`), WI-1.205–WI-1.216 | Given `tkhd` of any corpus file, When fields are re-read from the file at `offset + span.bit_offset/8`, Then the values equal `Field.uvalue`/`ivalue` (test `isobmff_span_roundtrip`). |
| FR-CNT-003 | Field names MUST be the exact identifiers used in ISO/IEC 14496-12 syntax (for example `creation_time`, `timescale`, `sample_delta`, `default_sample_flags`), and `Field.spec_ref` MUST name the section (for example `ISO/IEC 14496-12 8.6.1.2`). | §8.2.8 (`FieldBuilder` names), §8.2.10 (registry `spec_ref`), §8.3 notation | Given `tkhd` of any corpus file, When decoded, Then the field names are exactly `creation_time, modification_time, track_ID, reserved, duration, …` and every `Field.spec_ref` begins with `ISO/IEC 14496-12 §8.3.2`. |
| FR-CNT-004 | The reader MUST never read the payload of `mdat` or `idat` boxes while building the tree, and MUST expand the children of `moov`, `trak`, `moof` and `meta` lazily through `ovt_container_expand` so that opening a file reads at most the box headers plus `moov`/`moof` payloads. | §8.2.4, WI-1.204 | Given a 4 GiB MP4 with `moov` first, When the tree is built, Then `mdat`/`idat` payload bytes are never read (`bytes_read` < 16 MiB) and `moov/trak` children are expanded only through `ovt_container_expand` (test `isobmff_lazy_mdat`). |
| FR-CNT-005 | For each video track the reader MUST produce a `TrackInfo` with `track_id`, `handler`/`handler_fourcc`, `codec` (from the sample entry: `avc1`/`avc3` → H264, `hvc1`/`hev1` → H265, `av01` → AV1), `timescale`, `duration`, `language`, `width`/`height` and `coded_width`/`coded_height` (from `tkhd` and the sample entry), `edit_list_present`, `sample_count`, `codec_private` bytes (`avcC`/`hvcC`/`av1C` payload) and the `ovt_ColorDescription` derived from `colr` (`nclx`), `mdcv` and `clli`. | §8.1.3, WI-1.217 | Given `avc1`, `hvc1`, `av01` corpus files, When `ovt probe --tracks --format json` runs, Then `codec`, `timescale`, `duration`, `width/height`, `codec_private[0].fourcc` and `sample_count` equal `MP4Box -info` after the normalisation of §8.9.2. |
| FR-CNT-006 | The reader MUST build a `SampleTable` view for each track giving, for every sample, `offset`, `size`, `dts`, `cts_offset` (CTS = `dts + cts_offset`, after `ctts`), `is_sync` (from `stss` or `trun` sample flags), `chunk_index` and `fragment_index`, for both non-fragmented (`moov`-only) and fragmented (`moof`-based, including `tfdt` and `default_base_is_moof`) files. | §8.4.1, §8.4.3, WI-1.218, WI-1.219 | Given any corpus file, When `ovt probe --samples -t N --format csv` runs, Then every row equals the row in `MP4Box -dts` (DTS, CTS, size, offset, RAP) after the normalisation of §8.9.2. |
| FR-CNT-007 | The reader MUST split each sample of an `avc1`/`hvc1`/`hev1`/`avc3` track into NAL units using the `lengthSizeMinusOne` value from `avcC`/`hvcC` (ISO/IEC 14496-15 §5.3.4.2) and each sample of an `av01` track into OBUs (AV1 ISOBMFF binding §2.4), producing `Unit` records with `file_span`, `prefix_span` and `sample_index`. | §8.7.4 (`unit_split.cpp`), WI-1.231 | Given `avc1`, `hev1`, `av01` corpus files, When `ovt_stream_unit_bytes` is called for unit 0, Then `out_raw.data[0]` is a NAL header (not `0x00`) for H.26x and bit 1 of the AV1 OBU header (`obu_has_size_field`) is set. |
| FR-CNT-008 | When a track is encrypted (`encv` sample entry with `sinf`/`schm`/`tenc`), the reader MUST report `EncryptionInfo` (`scheme_type` = `cenc`/`cbc1`/`cens`/`cbcs`, `default_kid`, `default_is_protected`, `pssh` system IDs) and MUST mark the stream as not decodable with diagnostic `CNT-0040` rather than attempting to parse encrypted slice data. | §8.3.8 (group H), §8.4.4, WI-1.213, WI-1.221 | Given `syn_mp4_cenc_cbcs` (chapter 14 §14.3.3), When `ovt probe --enc --format json` runs, Then `encryption[0].scheme_type == "cbcs"`, `default_kid` is 16 bytes and `diagnostics[]` contains `CNT-0040`. |
| FR-CNT-009 | The reader MUST validate structural constraints and emit `Diagnostic` entries for at least: box extends past parent (`CNT-0108`), `stsz` sample count ≠ `stts` total (`CNT-2005`), `stco` offset beyond EOF (`CNT-2007`), missing `stsd` (`CNT-1011`), `trun` without `tfhd` (`CNT-1011`), `sidx` referenced range mismatch (`CNT-4030`), `ftyp` not first box (`CNT-0002`, Warning) — codes per the registry in chapter 8 §8.5. | §8.5, WI-1.224 | Given every file under `tests/corpora/synthetic/isobmff-broken/` (ids `syn_mp4_broken_*`), When `ovt probe --validate` runs, Then the `expected.diagnostics` codes of the manifest entry are emitted (`ctest -R diag_coverage`). |
| FR-CNT-010 | The IVF reader MUST parse the 32-byte file header (`DKIF`, version, header size, fourcc, width, height, frame rate numerator/denominator, frame count) and the 12-byte frame headers (frame size, 64-bit pts) into a `ContainerTree` with `format = "ivf"`, and MUST hand each frame payload to the OBU splitter. | §8.6.2, WI-1.225 | Given `syn_av1_tiles_cdef_lr` (IVF), When `ovt probe --format json` runs, Then `format == "ivf"`, the root `fields` hold the 32-byte header values and `sample_tables[0].count` equals the walked frame count. |
| FR-CNT-011 | The Annex B reader MUST split H.264 and HEVC byte streams at 3- and 4-byte start codes (H.264 Annex B §B.1.1; H.265 Annex B §B.2), MUST attribute `zero_byte` and `trailing_zero_8bits` to `prefix_span`, and MUST exclude `cabac_zero_words` from the NAL payload. | §8.6.3, WI-1.226 | Given `syn_h264_multislice_cavlc` (`.264`) and `syn_h265_amp_sao_on` (`.265`), When split, Then every `Unit.prefix_span` covers exactly `zero_byte? + 00 00 01` and the unit count equals `ffprobe -show_packets` NAL count (chapter 14 §14.4). |
| FR-CNT-012 | The raw AV1 OBU reader MUST support both the low-overhead format (AV1 §5.2, `obu_has_size_field = 1`) and Annex B (`temporal_unit_size`, `frame_unit_size`, `obu_length` leb128 prefixes), MUST group OBUs into temporal units at each `OBU_TEMPORAL_DELIMITER`, and MUST reject `obu_has_size_field = 0` outside Annex B with `CNT-OBU-0001`. | §8.6.4, WI-1.227 | Given `syn_av1_annexb_obu_split` (both files), When opened, Then `format` is `obu` with `framing == "section5"` / `"annexb"` respectively and one `Unit` per OBU grouped into temporal units at each `OBU_TEMPORAL_DELIMITER`. |
| FR-CNT-013 | The Y4M reader MUST parse the stream header tags `W`, `H`, `F`, `I`, `A`, `C` (including `C420jpeg`, `C420paldv`, `C420mpeg2`, `C420p10`, `C422p10`, `C444p12`, `Cmono`) and `X` comments, MUST map `C` to `chroma_format`, `bit_depth` and `chroma_siting`, and MUST expose each `FRAME` as a raw sample. | §8.6.5, WI-1.228 | Given the Y4M sources of chapter 14 §14.3.3 (`src_720p_8bit.y4m`, `src_1080p_10bit.y4m`), When opened, Then `RawVideoFormat.chroma_format/bit_depth` equal `C420jpeg`/8 and `C420p10`/10 and `frame_count == 60`. |
| FR-CNT-014 | For headerless raw YUV input, OVT MUST require an explicit format description in the grammar `--raw-format WxH:pix_fmt[:fps]` of chapter 12 §12.1.5 (D21; `fps` as `num/den` or a decimal) (`pix_fmt` is an FFmpeg pixel-format name from the accepted set of chapter 8 §8.6.6, for example `yuv420p`, `yuv420p10le`, `nv12`, `p010le`; packed 4:2:2 formats such as `yuyv422`, `uyvy422` and `v210` need their own `frame_bytes` rules and are added by a later WI; frame rate optional; `--raw-offset <bytes>` gives the start offset) and MUST compute `frame_count = floor((file_size − start_offset) / frame_bytes)`. | §8.6.6, WI-1.229 | Given `--raw-format 1280x720:nv12:30000/1001`, When a 1382400 × 30-byte file is opened, Then `frame_bytes == 1382400` and `frame_count == 30`. |
| FR-CNT-015 | OVT MUST parse Matroska/WebM (EBML) element trees with byte spans for every element and MUST produce `TrackInfo` and `SampleTable` views for `V_MPEG4/ISO/AVC`, `V_MPEGH/ISO/HEVC` and `V_AV1` tracks including `CodecPrivate`, `BlockGroup`/`SimpleBlock` timestamps and `Cues`. | §8.6.7 (EBML), WI-4.200–WI-4.203 | Given every Matroska corpus file (§8.9.3), When built, Then `TrackInfo`/`SampleTable` equal `mkvinfo -v -v -z` after normalisation (phase 4). |
| FR-CNT-016 | OVT MUST parse MPEG-2 Transport Streams (ISO/IEC 13818-1) with 188- and 192-byte packets, PAT/PMT, PES headers (PTS/DTS), and MUST produce a `SampleTable` per video PID with access-unit boundaries, plus a bitrate graph source with 10, 100, 250 and 500 ms windows. | §8.6.7 (MPEG-TS), WI-4.204–WI-4.207 | Given tsduck-test `input/*.ts`, When built, Then PID list, PES count and PTS/DTS equal `tsanalyze --json` / ffprobe (phase 4). |
| FR-CNT-017 | OVT MUST open a DASH MPD or HLS master/media playlist as a **virtual file** that concatenates the initialisation segment and media segments of one selected representation/rendition in order, exposing the result through the same `ovt_File` interface so that the ISOBMFF reader and `StreamIndex` work unchanged. | §8.6.7 (DASH/HLS virtual file), WI-4.208–WI-4.210 | Given a local DASH-IF package, When opened through `ovt_container_open_manifest`, Then the tree equals the tree of the concatenated segments (phase 4). |
| FR-CNT-018 | The container tree build for a 1-hour, 1080p, 30 fps progressive MP4 (108 000 samples, `moov` ≈ 2 MiB) MUST complete within the `bench_container_open_moov_1h` budget of chapter 14 §14.9.3 on the M1 reference, excluding lazy children. | §8.2.3 (walk), §8.4.1 (expansion), WI-1.239 | Given `syn_h264_1h_1080p.mp4`, When `bench_container_open_moov_1h` runs (chapter 14 §14.9.3), Then its median and that of `bench_container_sample_table_1h` (track 0 expansion) meet the chapter 14 §14.9.3 budgets (see `tests/perf/budgets.json`; D19). |
| FR-CNT-101 | The ISOBMFF reader MUST survive truncated, size-0, size-1, overlapping and negative-size boxes, emitting diagnostics instead of throwing out of the module (chapter 8 §8.2.3). | §8.2.3, §8.5, WI-1.201, WI-1.224 | Given every file in `tests/corpora/synthetic/isobmff-broken/`, When opened, Then `ovt_container_open` returns `OVT_OK` or `OVT_E_CORRUPT` (never crashes; ASan clean) and `ContainerTree.diagnostics` is non-empty. |
| FR-CNT-102 | The reader MUST compute an `HdrMetadata` summary combining box-level and bitstream-level signalling and MUST flag disagreements with `CNT-5001`…`CNT-5006` (chapter 8 §8.4.6). | §8.4.6, WI-1.223 | Given `syn_h265_main10_pq_hdr` (MP4-wrapped), When probed with `--hdr`, Then `HdrMetadata.kind == HDR10`, `max_cll == 1000`, `max_fall == 400` and `color.transfer == 16`; Given `syn_h265_main10_pq_hdr_mismatch` (chapter 14; `colr` BT.709 over a BT.2020 SPS), Then `CNT-5001`. |
| FR-CNT-103 | `Demuxer` MUST map every libavformat stream to a `TrackInfo` or report `CNT-0901` (chapter 8 §8.1.4). | §8.1.4, WI-1.232 | Given every corpus MP4, When `ovt probe --demux-check` runs, Then every track with `handler_type` in {`vide`,`soun`,`subt`,`text`} has `avstream_index ≥ 0`, or a `CNT-0901` diagnostic names it. |
| FR-CNT-104 | The GUI and the Python package MUST obtain file bytes only through `ovt_container_bytes` / `ovt_stream_unit_bytes` and MUST NOT map or open the file themselves (chapter 8 §8.8). | §8.8, WI-1.234 | Given the `app/` and `python/` source trees, When `grep -rn "mmap|QFile::map" app/ python/` runs, Then it returns no match (CI lint `no_direct_file_access`). |
| FR-CNT-105 | When a raw YUV file is opened without a format description, OVT MUST propose a format when the file size is an exact multiple of a candidate frame size (filename and size heuristics of chapter 8 §8.6.6) and MUST pre-fill the GUI dialog with it. | §8.6.6 steps 3–5, WI-1.229 | Given `foo_1920x1080_yuv420p10le.yuv`, When opened without a sidecar, Then the proposed format is `1920x1080 yuv420p10le` and the dialog is pre-filled. |
| FR-CNT-106 | Every container parser MUST have a libFuzzer target that runs 10 minutes in the nightly fuzz lane without a crash, timeout or leak (chapter 8 §8.9.4). | §8.9.4, WI-1.238 | Given `cmake --preset linux-x64-fuzz`, When `ctest -L fuzz-smoke` runs, Then all `fuzz_container_*` targets exit 0. |

### 8.1.3 `TrackInfo` semantics (schema: chapter 06 §6.4)

The FlatBuffers tables `HandlerType`, `CodecPrivate`, `TrackReference` and `TrackInfo` are defined **once**, in chapter 06 §6.4 (`core/schema/ovt_container.fbs`, landed by WI-0.081); this chapter does not repeat the schema text. This section fixes how each field is derived (WI-1.217):

| Field(s) | Derivation |
|---|---|
| `index` | 0-based position among `trak` boxes in `moov` order; this is the value `ovt_stream_open` takes. |
| `track_id` | `tkhd.track_ID`. Duplicates → `CNT-1012` on the second track. |
| `handler`, `handler_fourcc` | `hdlr.handler_type` as written (`vide`, `soun`, `sbtl`, `subt`, `text`, `meta`, `hint`, `tmcd`, `auxv`) and its enum mapping (`sbtl`/`subt` → `Subtitle`, `text` → `Text`, `tmcd` → `Timecode`, `auxv` → `Auxiliary`, unknown → `Unknown`). |
| `codec`, `sample_entry_fourcc`, `original_format` | first `stsd` entry; mapping `avc1/avc2/avc3/avc4/dvav/dva1 → H264`, `hev1/hvc1/hev2/hvc2/dvhe/dvh1 → H265`, `av01/dav1 → AV1`, `vp09 → VP9`, `vvc1/vvi1 → H266`, `mp4v` with `esds` OTI `0x61`/`0x60` → `MPEG2`; `encv`/`enca`/`resv` → by `sinf/frma.data_format`, which also fills `original_format`. |
| `codec_string` | RFC 6381 `codecs=` value of the first sample entry: AVC `avc1.PPCCLL` from `avcC` (`AVCProfileIndication`, `profile_compatibility`, `AVCLevelIndication`, upper-case hex); HEVC per 14496-15 Annex E (`hvc1.<general_profile_space letter><general_profile_idc>.<profile_compatibility_flags reversed, hex>.<L|H><general_level_idc>.<constraint bytes, trailing zero bytes omitted>`); AV1 per the AV1 ISOBMFF binding §5 (`av01.P.LLT.DD[.M.CCC.cp.tc.mc.F]`, the optional part emitted only when it differs from the defaults); VP9 `vp09.PP.LL.DD`; `mp4a.40.2` style from `esds`; otherwise the bare fourcc. |
| `timescale`, `duration`, `duration_seconds` | `mdhd`; all-ones duration → unknown, `duration_seconds = -1`. |
| `movie_timescale`, `tkhd_duration` | `mvhd.timescale`, `tkhd.duration`. |
| `language` | `mdhd.language` unpacked to ISO 639-2/T, overridden by `elng` when present. |
| `enabled`, `in_movie`, `in_preview` | `tkhd.flags` bits 0, 1, 2. |
| `width`, `height` | `tkhd` 16.16 → double (presentation size). |
| `coded_width`, `coded_height` | `VisualSampleEntry.width/height`. |
| `matrix`, `rotation` | `tkhd.matrix` raw (9 × i32); `rotation` = **clockwise** display rotation in degrees, 0/90/180/270, derived from `(a,b,c,d)` (16.16 units, `1.0 = 0x00010000`): `(1,0,0,1)`→0, `(0,1,−1,0)`→90, `(−1,0,0,−1)`→180, `(0,−1,1,0)`→270; else 0 with `CNT-1024` (Info, "non-rotation matrix"). Sign convention (normative, so the ffprobe parity test of WI-1.230 is well defined): `rotation = (−av_display_rotation_get(matrix)) mod 360` — libavformat reports the same matrix `(0,1,−1,0)` as −90 (counter-clockwise), QuickTime and OVT as 90° clockwise. |
| `pixel_aspect`, `clean_aperture` | `pasp` / `clap` children of the first visual sample entry; empty when absent. |
| `channel_count`, `sample_rate`, `sample_size` | `AudioSampleEntry`; `sample_rate` is the 16.16 value (or `esds`/`dOps` value when the entry holds 0). |
| `sample_count` | `stsz`/`stz2 sample_count` plus Σ `trun.sample_count` over all fragments of the track (the fragment part is filled lazily by WI-1.219). |
| `bitrate_avg_bps`, `bitrate_max_bps` | `btrt.avgBitrate/maxBitrate` when present; else `avg = Σ size × 8 / duration_seconds` and `max` = largest 1-second window, rounded to integer; 0 when unknown. |
| `is_fragmented`, `is_encrypted`, `edit_list_present` | `mvex/trex` for this `track_ID`; `sinf` present; `edts/elst` present. |
| `codec_private` | all configuration children of the first sample entry in `stsd` child order (`avcC`, `hvcC`, `av1C`, `vpcC`, `esds`, `dOps`, `dvcC`, `dvvC`, `dvwC`); `codec_private[0].bytes` is what `ovt_AnalysisDecoderApi.create()` receives. |
| `avstream_index` | filled by §8.1.4; −1 until then. |
| `references` | one `TrackReference` per `tref` child. |
| `trak_offset` | `BoxNode.file_span.offset` of the `trak`. |

The derived views (`SampleTable`, `EditList`, `FragmentTimeline`, `EncryptionInfo`, `HdrMetadata`) are separate tables keyed by `track_id` (chapter 06 §6.4) and are built lazily (§8.4).

### 8.1.4 Matching `TrackInfo` to libavformat streams

libavformat's mov demuxer sets `AVStream.id` to `tkhd.track_ID` (source: `libavformat/mov.c`, `mov_read_tkhd`: `st->id = avio_rb32(pb)`). It skips some tracks (e.g. hint tracks, tracks with unsupported `hdlr` types) and may create streams the ISOBMFF reader does not call tracks (none for MP4; for Matroska, attachments are streams). The matching algorithm run by `ovt::demux::match_tracks(const ContainerTree&, AVFormatContext*)`:

1. For each `AVStream s` (index `i`), look up the unique `TrackInfo t` with `t.track_id == s->id`. If exactly one exists and `t.avstream_index == -1`, set `t.avstream_index = i` and continue.
2. If no `TrackInfo` has that id (libavformat renumbered, or the container is not ISOBMFF): fall back to **order matching within media type**: the k-th `AVStream` with `codecpar->codec_type == AVMEDIA_TYPE_VIDEO` is matched to the k-th `TrackInfo` with `handler == Video`, likewise for audio (`soun`) and subtitles (`subt`/`sbtl`/`text`). Emit `CNT-0902` (Info) naming the fallback.
3. If two `TrackInfo`s share a `track_id` (file bug, `CNT-1012` was already emitted by the reader), match the first unmatched in `trak` order and emit `CNT-0901` (Warning) for the other.
4. **Verification.** For each matched pair compare: (a) `codecpar->codec_type` vs `handler`; (b) `codecpar->extradata` vs `TrackInfo.codec_private[0].bytes` — they MUST be byte-identical for `avcC`/`hvcC`/`av1C` (libavformat copies the box payload verbatim; for `av1C` libavformat `mov.c` exports the full record including the 4-byte header as `extradata` (verified at the FFmpeg 7.1.1 pin of chapter 13; `libavcodec/libdav1d.c` parses that layout); for `hvcC` of an `hev1` entry libavformat may rewrite the arrays, so a difference there is reported with the lower severity Info). `vpcC` is **excluded** from the byte comparison because `mov_read_vpcc` parses it into `codecpar->profile`, `bits_per_raw_sample` and the colour fields and leaves `extradata` empty (§8.7.6): compare those derived fields with the decoded `vpcC` fields instead. For `esds` libavformat stores only the DecoderSpecificInfo, so compare against the `DecoderSpecificInfo` field; for `Opus` libavformat synthesises an `OpusHead`, skip the comparison; (c) `s->nb_frames` (when non-zero) vs `sample_count` for non-fragmented files. Any mismatch → `CNT-0901` (Warning) with the differing item, `avstream_index` is kept but `TrackInfo.diagnostics` carries the warning and the GUI shows a yellow badge on the track.
5. Any `AVStream` left unmatched → `CNT-0903` (Info: "libavformat stream i has no container track; playback only").
6. Any `TrackInfo` with `handler ∈ {Video, Audio, Subtitle, Text}` left unmatched → `CNT-0901` (Warning: "track_ID N not exposed by libavformat"); such a track can be inspected but not played.

For non-ISOBMFF formats the mapping is trivial: IVF/Annex B/OBU/Y4M/raw have exactly one `TrackInfo` (`index 0, track_id 1`) and libavformat exposes exactly one stream (`avstream_index = 0`). Matroska: `AVStream.id == TrackNumber`. MPEG-TS: `AVStream.id == PID`.

---

## 8.2 ISOBMFF reader design

### 8.2.1 Files and namespaces

```
core/include/ovt/container.h                  C API (ovt_container_*; see backbone §0.6.1 and §8.8)
core/src/container/format_sniff.cpp           sniff(): isobmff | ivf | annexb | obu | y4m | raw | ebml | mpegts | unknown
core/src/container/isobmff/movie.h            ParsedMovie (the in-memory result of the walk, below)
core/src/container/isobmff/box_walk.{h,cpp}   header walk, tree build, lazy expansion (§8.2.3–8.2.5)
core/src/container/isobmff/box_context.h      BoxContext, FieldBuilder (§8.2.6–8.2.8)
core/src/container/isobmff/box_registry.h     BoxDecoder, registry lookup API (§8.2.9)
core/src/container/isobmff/box_registry.gen.cpp   GENERATED by tools/gen_box_registry.py (§8.2.10) — never edit by hand
core/src/container/isobmff/decoders/*.cpp     hand-written field decoders, one file per group of §8.3.1–§8.3.10
core/src/container/isobmff/sample_table.cpp   §8.4.1
core/src/container/isobmff/edit_list.cpp      §8.4.2
core/src/container/isobmff/fragments.cpp      §8.4.3
core/src/container/isobmff/cenc.cpp           §8.4.4
core/src/container/isobmff/brands.cpp         §8.4.5
core/src/container/isobmff/hdr_summary.cpp    §8.4.6
core/src/container/isobmff/validate.cpp       §8.5
core/src/container/{ivf,annexb,obu,y4m,rawyuv}.cpp    §8.6
core/src/demux/demuxer.{h,cpp}, avio_adapter.cpp, unit_split.cpp   §8.7
core/src/container/byte_service.cpp           §8.8
core/tests/container/*.cpp, core/tests/fuzz/fuzz_container_*.cpp
tools/gen_box_registry.py, tools/registry_inputs/   §8.2.10
```

Namespace: `ovt::container::isobmff` for the reader, `ovt::container` for shared types and other formats, `ovt::demux` for the libavformat wrapper.

**`ParsedMovie`** (`movie.h`) is the single in-memory object every algorithm of §8.4 and §8.5 takes as input; it is owned by the `ovt_Container` handle and is never exposed through the C API (the API hands out the FlatBuffers built from it):

```cpp
namespace ovt::container::isobmff {
struct ParsedMovie {
  std::shared_ptr<ovt::io::RandomAccessFile> file;   // the byte source (§8.2.2); VirtualFile for DASH/HLS
  uint64_t file_size;
  std::vector<BoxNodeT> top_level;                   // the tree in file order (chapter 06 §6.4 `BoxNode`, object API), children lazily filled (§8.2.4)
  std::vector<TrackInfoT> tracks;                    // §8.1.3, in `trak` order
  MovieHeader mvhd;                                  // timescale, duration, next_track_ID (decoded group A values)
  std::vector<std::string> brands;                   // ftyp major + compatible brands (deduplicated, in file order)
  std::vector<const BoxNodeT*> moofs;                // top-level moof nodes in file order (fragment timeline input, §8.4.3)
  std::vector<DiagnosticT> diagnostics;              // walk-time (Quick level) diagnostics, §8.5
  const BoxNodeT* find(std::string_view path) const; // "moov/trak[0]/mdia/minf/stbl/stsz"; nullptr when absent
};
}
```

Every `build_*` function of §8.4 reads `ParsedMovie` and a `ovt::jobs::Cancel&` token (chapter 07 §7.1.6; checked every ≤ 50 ms of work via `cancel.requested()`), and returns a `flatbuffers::DetachedBuffer` holding the chapter-06 table; the `ovt_Container` stores these buffers and serves them through the C API of chapter 12 §12.3.7.

### 8.2.2 The `BitReader` and `RandomAccessFile` subset this chapter consumes

Both types are defined normatively in chapter 7 (`core/src/bitreader/`, `core/src/io/`). This chapter relies only on the following members; if chapter 7 names them differently, chapter 7 wins and this chapter is edited.

```cpp
namespace ovt::io {
class RandomAccessFile {
public:
  uint64_t size() const noexcept;
  // Reads exactly len bytes at offset into out; throws ovt::IoError if [offset, offset+len) is not inside the file.
  void read_exact(uint64_t offset, std::span<uint8_t> out) const;
  // Returns a view valid until the next call that may evict (page cache, §8.8). len ≤ 1 MiB.
  std::span<const uint8_t> view(uint64_t offset, uint32_t len) const;
  uint64_t bytes_read() const noexcept;   // statistics for the FR-CNT-004 / FR-GEN-011 tests
};
}
namespace ovt {
class ParseError : public std::runtime_error { public: uint64_t bit_position; std::string code; };
class BitReader {                            // big-endian, MSB first; every read bounds-checked, throws ParseError("BR-0001") on overrun
public:
  explicit BitReader(std::span<const uint8_t> data, uint64_t base_bit_offset = 0);
  uint64_t u(unsigned nbits);                // 1..64 bits
  int64_t  s(unsigned nbits);                // two's complement
  uint8_t  u8();  uint16_t u16();  uint32_t u24();  uint32_t u32();  uint64_t u64();
  int16_t  i16(); int32_t  i32();  int64_t  i64();
  uint16_t u16le(); uint32_t u32le(); uint64_t u64le();   // for IVF / QuickTime 'enda' payloads only
  std::array<char,4> fourcc();               // 4 bytes
  std::string str_nul(uint64_t max_bytes);   // UTF-8 up to and consuming NUL; stops at max_bytes without NUL (flag)
  std::span<const uint8_t> bytes(uint64_t n);
  void skip_bits(uint64_t n); void skip_bytes(uint64_t n); void align_byte();
  bool byte_aligned() const noexcept;
  uint64_t bit_pos() const noexcept;         // relative to data.begin(), plus base_bit_offset
  uint64_t bits_left() const noexcept; uint64_t bytes_left() const noexcept;
  uint64_t leb128();                         // AV1 leb128(), ≤ 8 bytes, throws on more
  uint64_t mpeg4_expandable_size();          // ISO 14496-1 §8.3.3 sizeOfInstance (1..4 bytes with 0x80 continuation)
};
}
```

### 8.2.3 Box header walk — algorithm

Definitions: a *box header* is `size (u32) + type (4CC)`, optionally `largesize (u64)` when `size == 1`, optionally `usertype (16 bytes)` when `type == 'uuid'`. `header_length` ∈ {8, 16, 24, 32}. `parent_end` is the exclusive end offset of the enclosing box (the file size for the root).

`walk_children(parent_offset, parent_payload_begin, parent_end, depth, ctx) → vector<BoxNode>`:

1. Set `pos = parent_payload_begin`. If `depth > 32` → emit `CNT-0105` (Error, "box nesting deeper than 32") on the parent and return.
2. **Loop while** `pos < parent_end`:
   1. `remaining = parent_end - pos`. If `remaining < 8`: if `remaining > 0` emit `CNT-0101` (Warning, "N trailing bytes in <parent> that cannot hold a box header") with span `[pos, parent_end)` recorded as a `BoxNode` with `fourcc = ""` and `description = "trailing bytes"`; break.
   2. Read 8 bytes at `pos`: `size32 = u32`, `type = fourcc`. `header_length = 8`.
   3. **QuickTime `wide` quirk:** if `type == 'wide'` and `size32 == 8`, record a `BoxNode{fourcc:"wide", file_span:[pos,pos+8)}` and set `pos += 8`; continue the loop. (`wide` is a placeholder preceding a `mdat` that may need to grow to 64-bit; QTFF "Atoms → wide".)
   4. If `size32 == 1`: require `remaining ≥ 16`, else `CNT-0102` (Error, truncated largesize) and break. `size = u64 at pos+8`, `header_length = 16`. If `size < 16` → `CNT-0103` (Error, "largesize smaller than header"), break.
   5. Else if `size32 == 0`: `size = parent_end - pos` ("box extends to end of file", 14496-12 §4.2.2). Allowed **only** at depth 0 (top level); at depth > 0 emit `CNT-0104` (Warning) and still use `parent_end` (this is what libavformat and GPAC do).
   6. Else `size = size32`. If `size < 8` → `CNT-0106` (Error, "box size N < 8 at offset pos"); **recovery:** if `type` is a printable fourcc that is in the registry, treat `size = 8` and continue; else break (the remaining bytes become a "trailing bytes" node as in 2.1).
   7. If `type == 'uuid'`: require `size ≥ header_length + 16`, else `CNT-0107` (Error) and break. Read 16 bytes `usertype`; `header_length += 16`.
   **Every `break` above** (2.1, 2.4, 2.6, 2.7) turns `[pos, parent_end)` into one node with `fourcc = ""` and `description = "trailing bytes"` that carries the finding, so the child spans always sum to the parent size (WI-1.201 acceptance test 1).
   8. **Overflow / end checks** (all in `uint64_t` with `__builtin_add_overflow`): `end = pos + size`. If the addition overflows, or `end > parent_end`: emit `CNT-0108` (Error, "box <type> at pos claims size S, exceeds parent end by D bytes"); set `end = parent_end` and mark the node `truncated = true` (recorded in `description` suffix " [truncated]" and as a diagnostic). Do not break — decode what is there.
   9. Check `type` bytes: each byte MUST be in `0x20..0x7E` or the box is a QuickTime `©xxx` (first byte `0xA9`) or a numeric iTunes key (`ilst` children named `0x00000001..`). Otherwise emit `CNT-0109` (Warning, "non-printable box type") and set `fourcc` to the four-escape form `"\xNN\xNN\xNN\xNN"` (for example `\x00\x00\x00\x01`) for display; decoding is skipped.
   10. Create `BoxNode n{fourcc = type, uuid = usertype?, file_span = {pos, end-pos}, header_length}`. Look up the registry (§8.2.9) with `(type, ctx)` → `entry`. Fill `spec_ref`, `description` from `entry`; if no entry → `CNT-1001` (Info, "unknown box type"), `description = "unknown"`.
   11. If `entry.is_fullbox`: require `end - pos - header_length ≥ 4`, else `CNT-0110` (Error, "FullBox too short for version/flags") and skip decoding; read `version = u8`, `flags = u24` into `n.version`, `n.flags`; `payload_begin = pos + header_length + 4`. Else `payload_begin = pos + header_length`.
   12. **Payload handling** according to `entry.kind`:
       - `Container`: `n.children = walk_children(pos, payload_begin, end, depth+1, ctx.push(n))`.
       - `ContainerWithPrefix` (e.g. `stsd`, `dref`, `iinf`, `ipma` — a fixed prefix, then child boxes): run the hand decoder to consume the prefix (it returns the offset where children start) then walk children from there.
       - `Leaf`: if a hand decoder exists, run it (§8.2.7); otherwise `n.payload_span = {payload_begin, end - payload_begin}` and `fields` stays empty (the GUI shows the hex).
       - `Opaque` (`mdat`, `idat`, `free`, `skip`, `wide`): never read payload bytes; `payload_span` set.
   13. If `n.fourcc == "mdat"` and `ctx.saw_moov == false`: set `ctx.mdat_before_moov = true` (used for `CNT-0003`, Info "moov after mdat (not fast-start)").
   14. `pos = end`; push `n`.
3. Return the vector.

**Top level:** `walk_children(0, 0, file.size(), 0, root_ctx)`. After it returns: if no `ftyp`/`styp` at index 0 → `CNT-0002` (Warning; QuickTime files without `ftyp` are legal, so Warning not Error, and `ctx.quicktime = true` is set when the first box is `moov`/`mdat`/`wide`/`free` and no `ftyp` exists). If no `moov` and no `moof` → `CNT-0004` (Error, "no movie or fragment"). If `moof` exists without `moov` → `ctx.fragment_only = true` and `CNT-0005` (Info, "fragment-only stream; open with an initialisation segment to get tracks").

**Truncated files:** the root `parent_end` is the real file size; step 2.8 therefore converts a `moov` whose declared size exceeds the file into a truncated node and continues decoding, so a partially downloaded file still shows what it has. Step 2.1 produces the "trailing bytes" node. A truncated `mdat` yields `CNT-0108` on `mdat` and later `CNT-2007` for every chunk offset beyond EOF.

**Reading strategy:** headers are read with `file.read_exact(pos, 32 bytes or fewer)`. A leaf payload that is to be decoded is read with `file.view(payload_begin, min(len, 1 MiB))`; payloads > 1 MiB that have a hand decoder (`stsz`, `stco`, `co64`, `stts`, `ctts`, `trun`, `senc`, `iloc` can be large) are decoded in **windows** — the decoder receives a `BitReader` over a `PagedSpan` (§8.8) that transparently fetches 64 KiB pages; there is no code path that allocates a copy of a payload larger than 1 MiB.

### 8.2.4 Lazy traversal rules

1. `moov` and everything under it is parsed **eagerly** when `ovt_container_open` runs (it is small: ≤ a few MB even for hour-long files).
2. `mdat`, `idat`, `free`, `skip`, `wide` payloads are **never** read by the tree builder (FR-CNT-004). Their `BoxNode.payload_span` is set and `fields` is empty.
3. `moof` boxes: at open time only the **header walk** is done (so the tree has `moof` nodes with correct spans and `mfhd.sequence_number`); their `traf` children are decoded **on demand** by `ovt_container_expand(box_offset)` or when a `FragmentTimeline` is requested for a track (which decodes all `moof`s of that track sequentially, in a job with cancellation).
   - Exception: when the file is fragmented and has ≤ 64 `moof` boxes, decode them eagerly (CMAF single-file VOD typically has thousands; live chunks few).
4. `sidx`, `ssix`, `mfra`/`tfra`/`mfro` are parsed eagerly (they are the seek index). If `mfro` exists, its `size` is used to locate `mfra` from the end of the file **before** walking the top level, so that a huge file with `moov` after `mdat` can still locate fragments quickly; the normal walk then confirms it.
5. `moov` after `mdat`: no special code — the top-level walk skips `mdat` via its header size. For `size == 0` `mdat` (live recording) the `mdat` consumes the rest of the file and `moov` is missing → `CNT-0004`.
6. `ovt_container_expand(box_offset)`: finds the `BoxNode` with `file_span.offset == box_offset`, runs step 2.12 for it if its children/fields were not decoded, and republishes `ovt_container_tree`. Nodes remember `expanded:bool` internally (not in the FlatBuffer).
7. HEIF `iloc` extents and `idat` payloads: `iloc` is decoded eagerly (it is the item index), item payload bytes are read only when the user clicks an item (§8.8).

### 8.2.5 `BoxNode` population rules

| `BoxNode` field | Rule |
|---|---|
| `fourcc` | The 4 bytes as ISO-8859-1 → UTF-8 (so `©nam` displays). Non-printable: 8-hex form (step 2.9). |
| `uuid` | 16 bytes when `fourcc == "uuid"`, else empty. |
| `file_span` | `{box_start, size}` after truncation clamp. |
| `header_length` | 8 / 16 / 24 / 32 (see §8.2.3). FullBox version/flags are **not** part of `header_length`; they are fields with spans. Neither are the fixed 8 bytes of a sample entry (`reserved[6]`, `data_reference_index`) nor the 16-byte `usertype` of a `uuid` box (MediaInfo's reading; mp4box.js counts both and the FullBox bytes in `hdr_size`, see §8.9.2 rule 2). |
| `version`, `flags` | Set for FullBoxes; `-1` otherwise. Also emitted as two `Field`s named `version` (`u(8)`) and `flags` (`u(24)`) as the first two fields so the hex view can highlight them. |
| `fields` | From the hand decoder (`FieldBuilder`). Empty when no decoder or when the decoder threw before producing anything. Partially decoded boxes keep the fields produced before the error plus a diagnostic. |
| `payload_span` | For `Opaque` and undecoded leaves: the whole payload. For decoded leaves: the bytes **after** the last consumed field (`{box_end_of_fields, end - that}`) — non-empty means "unparsed remainder", which the validator reports as `CNT-1005` (Info) unless the registry marks the box `has_trailing_data_ok` (e.g. `avcC` may carry extensions, `stsd` sample entries may carry unknown children already walked). |
| `children` | For containers and for sample entries (which are containers after their fixed fields). |
| `spec_ref` | From the registry, e.g. `"ISO/IEC 14496-12:2022 §8.2.2"`. Hand decoders may override for context (e.g. `colr` in `ipco` → `23008-12 §6.5.x`). |
| `description` | Registry description, e.g. `"Movie header, overall declarations"`. |
| `diagnostics` | Everything the walk/decoder emitted for this box; the `Diagnostic.span` is a `BitSpan` relative to the box offset. Tree-level findings (`ContainerTree.diagnostics`: `CNT-0002`, `CNT-0004`, `CNT-0005`, `CNT-0006` and the elementary-stream families) carry a file-relative span (`byte offset × 8`, length 0 when no extent is known). `CNT-0105` is attached to the parent node; `CNT-0003` (an `mdat` before `moov`) is attached to the `moov` node as Info. |

**`Field` population rules** (`FieldBuilder`, §8.2.8):

- `name`: exact spec syntax name (`creation_time`, `track_ID`, `sample_delta`, `lengthSizeMinusOne`, `general_profile_idc`). Arrays produce one parent `Field` named like the spec array (`entries`) with `children` whose `name` is the element struct name and `array_index` is set; element sub-fields are grandchildren. Arrays with > 1024 elements are **summarised**: the `Field` named `entries` gets `uvalue = entry_count`, `svalue = "see Sample Table view"`, and only the first 64 and last 16 elements are materialised as children; the full data is in `SampleTable`. This keeps `ContainerTree` ≤ 8 MiB for any file.
- `kind`/`uvalue`/`ivalue`/`svalue`/`bytes`: exactly one is meaningful per `kind`. Fixed-point 16.16 and 8.8 → `kind = Fixed`, `ivalue = raw`, `derived = "1.0"` (decimal, 4 places). Fourcc fields → `kind = String`.
- `span`: `bit_offset = (field_byte_start - box_start) * 8 + bit_in_byte`, `bit_length` = bits consumed. For strings: through the terminating NUL. For byte blobs (`configOBUs`, `Data`): the blob extent.
- `spec_ref`: inherited from the box unless the decoder sets a more specific one (e.g. SPS inside `avcC` → `"ISO/IEC 14496-10 §7.3.2.1.1"`).
- `derived`: human form. Mandatory derivations: timestamps (seconds since 1904-01-01 00:00:00 UTC → ISO 8601 `YYYY-MM-DDThh:mm:ssZ`), durations (`→ 12.345 s` using the enclosing timescale from `ctx`), languages (packed 5-bit → `"eng"`), matrix (`"identity"` when `[0x10000,0,0, 0,0x10000,0, 0,0,0x40000000]`, else `"rotation 90°"` for the standard rotations, else `"custom"`), flag bitfields (list of set names), enumerations (H.273 code → name, e.g. `transfer_characteristics = 16 → "PQ (SMPTE ST 2084)"`), profile/level codes (`AVCProfileIndication 100 → "High"`, HEVC `general_profile_idc 2 → "Main 10"`, AV1 `seq_level_idx 13 → "5.1"`).

### 8.2.6 FullBox version and flags handling

1. The registry entry says `is_fullbox` and lists `known_versions` and `known_flag_bits` (from the conformance JSON).
2. If `version ∉ known_versions`: emit `CNT-1003` (Warning, "unknown version V of <box>; decoding as highest known version") and decode with `min(version, max_known)` — except where the spec says a reader MUST ignore unknown versions (`sgpd`, `stsd` entries): then skip decoding and set `payload_span`.
3. If `flags & ~known_flag_mask`: `CNT-1004` (Info, "reserved flag bits set"). Decoders read **only** the flag bits they define.
4. Boxes that look like FullBoxes but are not in some dialects: QuickTime `meta` (no version/flags). Detection rule for `meta`: read the 4 bytes at `payload_begin + 4`; if they spell a registered child fourcc (`hdlr`, `keys`, `ilst`, `pitm`, `iinf`, `iloc`, `ipco`, `iprp`, `iref`, `idat`, `dinf`) and the u32 at `payload_begin` is **not** `0` → QuickTime style, `version = flags = -1`, children begin at `payload_begin`; else ISO style. (ISO `meta` always has `version == 0`, so the u32 is `0x000000XX` with flags normally 0, and the next 4 bytes at `payload_begin+4` are a child's **size**, not its type.) Emit `CNT-1006` (Info, "QuickTime-style meta without version/flags").
5. `version`/`flags` are **not** read for sample entries (they are Boxes, not FullBoxes) even though several vendor files put garbage there.

### 8.2.7 Container-context-sensitive boxes

The same fourcc has different syntax depending on where it sits. `BoxContext` records the ancestor chain; the registry key is `(fourcc, context_pattern)`.

```cpp
namespace ovt::container::isobmff {
struct BoxContext {
  std::vector<std::array<char,4>> ancestors;   // ["moov","trak","mdia","minf","stbl","stsd","hev1"] innermost last
  std::array<char,4> handler_type{};           // from hdlr of the enclosing trak ("vide","soun",...), or "" 
  std::array<char,4> sample_entry{};           // innermost sample entry fourcc, or ""
  uint32_t movie_timescale = 0, track_timescale = 0;   // for derived durations
  uint32_t track_id = 0;
  bool quicktime = false;                      // no ftyp or major_brand 'qt  '
  bool fragment_only = false, saw_moov = false, mdat_before_moov = false;
  uint8_t  stsd_version = 0;                   // for AudioSampleEntry V1 selection
  uint8_t  per_sample_iv_size = 0xFF;          // 0xFF = unknown; from tenc / seig, used by senc
  uint64_t moof_offset = 0;                    // enclosing moof start (trun data_offset base)
  const BoxNode* trex_for_track = nullptr;     // for tfhd defaults
  const BoxNode* parent = nullptr;
  bool in(std::string_view fourcc) const;      // any ancestor equals fourcc
  bool parent_is(std::string_view fourcc) const;
};
}
```

Context-sensitive cases the implementer MUST handle (registry `context_pattern` is a glob over the ancestor chain, innermost last; `*` = any sequence):

| fourcc | Context | Syntax |
|---|---|---|
| `colr` | `*/stsd/<visual sample entry>` | 14496-12 §12.1.5 ColourInformationBox (nclx / rICC / prof; QuickTime also `nclc`) |
| `colr` | `*/ipco` (HEIF item property) | Same syntax, spec_ref `23008-12 §6.5.5`; `nclx` only meaningful |
| `hdlr` | `*/mdia` | handler for a track |
| `hdlr` | `*/meta` | handler for metadata (`mdta`, `mdir`, `pict`) |
| `meta` | `moov`, `trak`, `udta`, root | ISO FullBox vs QuickTime Box (§8.2.6 rule 4) |
| `data` | `*/ilst/*` | iTunes data atom (type indicator + locale + payload) |
| `name` | `*/ilst/----` | freeform key name (iTunes) |
| `name` | `*/tmcd` (QuickTime) | timecode source name |
| `mean` | `*/ilst/----` | freeform namespace |
| `text` | `*/gmhd` | QuickTime text media info (matrix) |
| `text` | `*/stsd` | QuickTime text sample entry |
| `frma` | `*/sinf` | original format of encrypted/restricted sample entry |
| `frma` | `*/wave` (QuickTime audio) | original audio format fourcc (same syntax, different meaning) |
| `mp4a` | `*/stsd` | AudioSampleEntry (ISO or QuickTime v0/v1/v2 layout) |
| `mp4a` | `*/wave` | QuickTime placeholder (4 zero bytes payload) |
| `esds` | `*/mp4a`, `*/mp4v`, `*/mp4s`, `*/wave` | same syntax everywhere |
| `sgpd`/`sbgp` | `*/stbl` vs `*/traf` | same syntax; in `traf`, `group_description_index ≥ 0x10001` refers to a `sgpd` **in the same traf** (14496-12 §8.9.4) |
| `saiz`/`saio` | `*/stbl` vs `*/traf` | same syntax; `saio` offsets in `traf` are relative per `tfhd` base rules (§8.4.4) |
| `senc` | `*/traf` | needs `per_sample_iv_size` from `tenc` (via `trak`→`stsd`→`sinf`) or `seig` |
| `uuid` | any | dispatch on the 16-byte usertype (§8.3 group K) |
| `hvcC`/`avcC`/`av1C` | `*/stsd/<entry>` and `*/ipco` | same syntax; in `ipco` they describe an image item |
| `url `/`urn ` | `*/dref` | data reference; `url ` with flags&1 means self-contained (no string) |
| `btrt` | any sample entry | same syntax |
| `tref` children (`cdsc`, `hint`, …) | `*/tref` | all = `track_IDs[]` |
| `iref` children (`dimg`, `thmb`, `cdsc`, `auxl`, `prem`) | `*/iref` | `SingleItemTypeReferenceBox` (u16 or u32 ids by `iref.version`) |
| `cdsc` | `tref` vs `iref` | different syntax (above) |
| `stsd` child with unknown fourcc | `*/stsd` | decoded as the generic sample entry for the track's `handler_type` (visual/audio/plain), then child boxes walked |

### 8.2.8 `FieldBuilder`

```cpp
namespace ovt::container::isobmff {
class FieldBuilder {
public:
  FieldBuilder(BoxNode& node, uint64_t box_file_offset, BitReader& br);
  // Each reader records span = bits consumed by this call, relative to the box start.
  uint64_t u(std::string_view name, unsigned nbits, std::string_view descriptor = "");   // descriptor default "u(n)"
  int64_t  s(std::string_view name, unsigned nbits);
  uint8_t  u8(std::string_view name);  uint16_t u16(std::string_view name); uint32_t u24(std::string_view);
  uint32_t u32(std::string_view name); uint64_t u64(std::string_view name);
  int16_t  i16(std::string_view name); int32_t i32(std::string_view name); int64_t i64(std::string_view name);
  uint64_t u32or64(std::string_view name, bool wide);     // version-dependent widths
  int64_t  i32or64(std::string_view name, bool wide);
  double   fixed16_16(std::string_view name); double fixed8_8(std::string_view name); double fixed2_30(std::string_view name);
  std::array<char,4> fourcc(std::string_view name);
  std::string str_nul(std::string_view name, uint64_t max_bytes);
  std::string str_fixed(std::string_view name, uint32_t nbytes);          // e.g. compressorname[32]
  std::span<const uint8_t> bytes(std::string_view name, uint64_t n);      // kind Bytes; n ≤ 1 MiB else summarised (first 4096 bytes kept)
  std::string lang_packed(std::string_view name);                         // 1 pad bit + 3x5 bits → "eng"
  void reserved(unsigned nbits, uint64_t expected = 0);                   // records "reserved" field; CNT-1007 Info if != expected
  std::string datetime1904(std::string_view name, bool wide);             // u32/u64 seconds since 1904 → sets derived ISO 8601
  uint64_t duration(std::string_view name, bool wide, uint32_t timescale);// sets derived "x.xxx s"
  void matrix(std::string_view name = "matrix");                          // 9 x i32 (16.16, 2.30 for u,v,w) → derived "identity"/rotation
  // Grouping
  FieldBuilder& group(std::string_view name);                             // push a parent field (struct)
  FieldBuilder& array_element(std::string_view name, uint32_t index);     // push element with array_index
  void end();                                                             // pop
  void derived(std::string_view text);                                    // attach to the most recent field
  void enum_name(std::string_view text);
  void spec_ref(std::string_view text);
  void diag(Severity, std::string_view code, std::string_view msg);       // span = last field's span
  BitReader& reader();
  uint64_t bytes_consumed() const;                                        // from payload begin
  // Large-array policy (§8.2.5): returns true when index should be materialised as a Field
  bool materialise(uint32_t index, uint32_t count) const { return count <= 1024 || index < 64 || index + 16 >= count; }
};
}
```

### 8.2.9 Decoder registry interface

```cpp
namespace ovt::container::isobmff {

enum class BoxKind : uint8_t { Container, ContainerWithPrefix, Leaf, Opaque };

struct BoxDecoder {
  const char* fourcc;                    // "tkhd"; "uuid:8974dbce-7be7-4c51-84f9-7148f9882554" for uuid boxes; "ilst/*" for all ilst children
  const char* context_pattern;           // glob over ancestor chain, e.g. "*/stsd/*" , "*/ipco", "*" (any). Innermost-last.
  // Decodes payload fields. For ContainerWithPrefix the decoder returns the byte offset (from payload begin) where children start; else 0.
  uint64_t (*decode)(BoxContext& ctx, BitReader& br, FieldBuilder& fb);
};

struct RegistryEntry {                   // generated (§8.2.10) + hand-decoder pointer resolved at static init
  const char* fourcc;
  const char* context_pattern;           // "*" when the registry has no container info
  BoxKind kind;
  bool is_fullbox;
  uint8_t known_versions_mask;           // bit v set = version v known (bits 0..7)
  uint32_t known_flag_mask;
  bool has_trailing_data_ok;
  const char* spec_ref;                  // "ISO/IEC 14496-12:2022 §8.3.2"
  const char* description;               // MP4RA description
  const char* allowed_parents;           // comma list from conformance JSON, e.g. "trak" ; "" = unknown
  const BoxDecoder* decoder;             // nullptr = generic shell only
};

// Lookup: exact (fourcc, most specific matching context_pattern) wins; then (fourcc, "*"); then nullptr.
const RegistryEntry* lookup(std::string_view fourcc, const BoxContext& ctx);
const RegistryEntry* lookup_uuid(const std::array<uint8_t,16>& usertype);

// Hand decoders register themselves with this macro (one per translation unit, static init order independent):
#define OVT_BOX_DECODER(FOURCC, PATTERN, FN) \
  static const ::ovt::container::isobmff::BoxDecoder OVT_CONCAT(k_dec_, __LINE__){FOURCC, PATTERN, FN}; \
  static const bool OVT_CONCAT(k_reg_, __LINE__) = ::ovt::container::isobmff::register_decoder(&OVT_CONCAT(k_dec_, __LINE__));
bool register_decoder(const BoxDecoder*);        // appends to a static vector; lookup() resolves lazily on first call
}
```

Decoder contract: a decoder reads **only** through `fb`/`br`; it MUST NOT touch `ctx.parent->children`; it MAY read `ctx` and MAY set `ctx` fields for descendants (`stsd` sets `stsd_version`; `tenc` sets `per_sample_iv_size`; `mdhd` sets `track_timescale`; `mvhd` sets `movie_timescale`). A `ParseError` thrown inside a decoder is caught by the walker, converted to `Diagnostic{Error, "CNT-1002", "truncated <box>: <message>"}` on the node, and the walk continues with the next sibling.

### 8.2.10 Generated registry: `tools/gen_box_registry.py`

**Inputs** (pinned copies committed under `tools/registry_inputs/`, refreshed by the update procedure):

| Input | Source URL | What is taken |
|---|---|---|
| `mp4ra_boxes.csv` | https://github.com/mp4ra/mp4ra.github.io → `CSV/boxes.csv` (ISO family), plus `CSV/boxes-udta.csv` and `CSV/boxes-qt.csv` for the `udta` and QuickTime code lists; the repository root also holds `CSV/brands.csv`, `CSV/handlers.csv`, `CSV/sample-entries.csv`, `CSV/sample-groups.csv`, `CSV/schemes.csv`, `CSV/track-references.csv`, `CSV/item-types.csv`. The path list lives in `tools/mp4ra_sources.json` so a rename upstream is a one-line change | `code`, `description`, `specification`, `type`? (container hints when present) for the ISO family |
| `mp4ra_boxes_udta.csv` | same repo, `data/boxes-udta.csv` | user-data box codes |
| `mp4ra_boxes_qt.csv` | same repo, `data/boxes-qt.csv` | QuickTime codes |
| `mp4ra_brands.csv`, `mp4ra_handlers.csv`, `mp4ra_item_types.csv`, `mp4ra_schemes.csv`, `mp4ra_sample_entries.csv` (`codecs.csv`) | same repo | brand table (§8.4.5), handler names, HEIF item types, protection scheme names, sample entry descriptions |
| `ffc/*.json` | https://github.com/MPEGGroup/FileFormatConformance → `data/standard_features/<spec>/*.json` for `14496-12`, `14496-14`, `14496-15`, `14496-30`, `23008-12` | per box: `fourcc`, `type` (`Box`/`FullBox`/`SampleEntry`/…), `containers`, `versions`, `flags`, `syntax` text, `description`, spec section |
| `ovt_extra_boxes.json` | hand-maintained in `tools/registry_inputs/` | boxes absent from both sources: CENC `23001-7` (`pssh`, `tenc`, `senc`, `schm`, `schi`, `sinf`, `frma`), CMAF `23000-19`, AV1 (`av1C`), VP (`vpcC`), Opus (`dOps`), Dolby (`dvcC`, `dvvC`, `dvwC`, `dac3`, `dec3`, `dac4`), QuickTime (`wave`, `enda`, `chan`, `gmhd`, `gmin`, `tapt`, `clef`, `prof`, `enof`, `fiel`, `wide`, `©xyz`, `tmcd`, `tcmi`), PIFF uuids, Nero `chpl`, iTunes `ilst`/`keys`/`data`/`mean`/`name`, `mpvd` (Google motion photo) |

**Algorithm:**

1. Load all MP4RA CSVs → `names: dict[fourcc → (description, spec)]`. Normalise fourcc: exactly 4 chars; `$20` → space; `(c)` → `©`.
2. Load the conformance JSON → `syntax: dict[fourcc → list of {type, containers, versions, flags, spec_section, syntax_text}]`. Several entries per fourcc = context-sensitive; emit one `RegistryEntry` per distinct `containers` set with `context_pattern = "*/<container>"` (joined with alternatives by emitting multiple rows).
3. Merge `ovt_extra_boxes.json` (same shape as the conformance JSON; it wins on conflict).
4. For every fourcc in the union: `kind` = `Container` if `syntax_text` contains no fields other than child boxes (heuristic: the JSON `type` is `Box` and `syntax` has no `unsigned int(` tokens) or the box is in the hand-maintained `kContainers` list in the script (`moov trak edts mdia minf dinf stbl mvex moof traf mfra udta meta ipco iprp sinf schi tref iref ilst wave gmhd tapt strk strd`), `Opaque` for `mdat idat free skip wide`, `ContainerWithPrefix` for `stsd dref iinf ipma meta(ISO) ` (list in script), else `Leaf`. `is_fullbox` from `type == "FullBox"` or syntax text `extends FullBox`. `known_versions_mask` from `versions`; `known_flag_mask` from `flags` (sum of listed flag values; `0` → `0xFFFFFF` = don't check).
5. Emit `core/src/container/isobmff/box_registry.gen.cpp` containing `static const RegistryEntry kEntries[] = { ... };` sorted by `(fourcc, context_pattern)` plus `kEntryCount`, a `kBrands[]` table (`brand, description, spec`), `kHandlers[]`, `kItemTypes[]`, `kSchemes[]`, and a header comment with input SHA-256s and generation date. The `decoder` member is `nullptr` in the generated file; `lookup()` joins by `(fourcc, context_pattern)` against `register_decoder()` registrations at first use.
6. Emit `tools/registry_inputs/REGISTRY_DIFF.md`: boxes in MP4RA but not in any JSON (= shell only), boxes in `ovt_extra_boxes.json` that MP4RA now registers (= remove our copy), and hand decoders registered in `decoders/*.cpp` (found by grepping `OVT_BOX_DECODER(`) with no registry entry (= error, CI fails).
7. The generated file is **committed** (so the build needs no Python); CI runs the generator and fails if the committed file differs (`tools/gen_box_registry.py --check`).

**Update procedure** (quarterly, or when a new box appears in a corpus file): `python3 -I tools/gen_box_registry.py --fetch` downloads the inputs into `tools/registry_inputs/` (new, empty dir per fetch), prints the diff of fourcc sets, regenerates, runs `ctest -L registry`. Commit inputs + generated file together.

Snippet of the generated output:

```cpp
// GENERATED by tools/gen_box_registry.py on 2026-11-03 from mp4ra@<sha> ffc@<sha> ovt_extra@<sha256>. DO NOT EDIT.
#include "box_registry.h"
namespace ovt::container::isobmff {
const RegistryEntry kEntries[] = {
  {"avcC","*/stsd/*",BoxKind::Leaf,false,0x01,0xFFFFFF,true,"ISO/IEC 14496-15:2022 §5.3.3.1","AVC configuration","avc1,avc2,avc3,avc4,encv,resv",nullptr},
  {"colr","*/ipco",BoxKind::Leaf,false,0x01,0xFFFFFF,false,"ISO/IEC 23008-12:2022 §6.5.5","Colour information (item property)","ipco",nullptr},
  {"colr","*/stsd/*",BoxKind::Leaf,false,0x01,0xFFFFFF,false,"ISO/IEC 14496-12:2022 §12.1.5","Colour information","avc1,hev1,...",nullptr},
  {"mvhd","moov",BoxKind::Leaf,true,0x03,0x000000,false,"ISO/IEC 14496-12:2022 §8.2.2","Movie header, overall declarations","moov",nullptr},
  /* ... ~340 entries ... */
};
const size_t kEntryCount = sizeof(kEntries)/sizeof(kEntries[0]);
}
```

---

## 8.3 Boxes with hand-written field decoders

Notation: `u8/u16/u24/u32/u64` big-endian unsigned; `i16/i32/i64` signed; `u(n)` n bits; `16.16`/`8.8`/`2.30` fixed point; `4cc` four-character code; `str` NUL-terminated UTF-8; `v0:/v1:` version-dependent; `[...]` repeated; `{...}` struct; `?cond` present only if cond. "Phase" is the OVT phase in which the decoder MUST exist (1 or 4). Derived values (seconds from a timescale, dB from a gain, enum names, decoded dates) are emitted as an additional child `Field` directly after the source field, named `<field>.derived` with `kind = String` and `svalue` holding the derived text and an empty `span` (the schema `Field` of chapter 06 §6.4 has no dedicated derived member). Each row is one `OVT_BOX_DECODER` registration in the file named in the group heading. Every decoder MUST read fields in exactly the listed order so spans are contiguous.

### 8.3.1 Group A — File and movie level (`core/src/container/isobmff/decoders/movie.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
|  `ftyp`, `styp` | 14496-12 §4.3 / 14496-12 §8.16.2 | `major_brand 4cc`, `minor_version u32`, `compatible_brands [4cc]` to end | brand descriptions from `kBrands`; profile checks §8.4.5 | 1  |
| `mvhd` | 14496-12 §8.2.2 | FullBox v0/v1; `creation_time u32/u64`, `modification_time u32/u64`, `timescale u32`, `duration u32/u64`, `rate 16.16`, `volume 8.8`, `reserved u16`, `reserved u32[2]`, `matrix i32[9]`, `pre_defined u32[6]`, `next_track_ID u32` | ISO 8601 dates; duration seconds; sets `ctx.movie_timescale` | 1 |
|  `trak` | 14496-12 §8.3.1 | container | — | 1  |
|  `tkhd` | 14496-12 §8.3.2 | FullBox v0/v1; flags `0x1 track_enabled`, `0x2 track_in_movie`, `0x4 track_in_preview`, `0x8 track_size_is_aspect_ratio`; `creation_time`, `modification_time` (u32/u64), `track_ID u32`, `reserved u32`, `duration u32/u64`, `reserved u32[2]`, `layer i16`, `alternate_group i16`, `volume 8.8`, `reserved u16`, `matrix i32[9]`, `width 16.16`, `height 16.16` | flag names; dates; duration in movie timescale → s; matrix → rotation; sets `ctx.track_id` | 1  |
|  `tref` + children | 14496-12 §8.3.3 | container; each child `<reference_type>`: `track_IDs [u32]` to end | reference type description (`cdsc` "content describes", `hint`, `vdep`, `vplx`, `subt`, `tmcd` (QT), `chap` (QT), `auxl`, `sbas`, `scal`, `font`, `hind`) | 1  |
|  `trgr` + children (`msrc`, `ster`) | 14496-12 §8.3.4 | container; child FullBox `track_group_id u32` | — | 4  |
|  `edts` | 14496-12 §8.6.5 | container | — | 1  |
|  `elst` | 14496-12 §8.6.6 | FullBox v0/v1; `entry_count u32`; `[ {segment_duration u32/u64, media_time i32/i64, media_rate_integer i16, media_rate_fraction i16} ]` | `media_time == -1` → "empty edit"; `media_rate 0` → "dwell"; durations → s (movie timescale); media_time → s (track timescale) | 1  |
|  `mdia` | 14496-12 §8.4.1 | container | — | 1  |
|  `mdhd` | 14496-12 §8.4.2 | FullBox v0/v1; `creation_time`, `modification_time`, `timescale u32`, `duration u32/u64`, `pad u(1)`, `language u(5)[3]`, `pre_defined u16` | language "eng"; `0x7FFF`/`und`; duration s; sets `ctx.track_timescale` | 1  |
|  `hdlr` | 14496-12 §8.4.3 | FullBox; `pre_defined u32`, `handler_type 4cc`, `reserved u32[3]`, `name str` (QuickTime quirk: if `ctx.quicktime` and first byte == remaining length − 1, decode as counted Pascal string) | handler description (`kHandlers`); sets `ctx.handler_type` | 1  |
|  `elng` | 14496-12 §8.4.6 | FullBox; `extended_language str` (RFC 5646) | — | 4  |
|  `minf` | 14496-12 §8.4.4 | container | — | 1  |
|  `vmhd` | 14496-12 §12.1.2 | FullBox (flags = 1); `graphicsmode u16`, `opcolor u16[3]` | graphicsmode name (0 copy, 0x24 transparent, 0x40 dither copy, 0x100 straight alpha…) | 1  |
|  `smhd` | 14496-12 §12.2.2 | FullBox; `balance 8.8`, `reserved u16` | — | 1  |
|  `hmhd` | 14496-12 §12.4.2 | FullBox; `maxPDUsize u16`, `avgPDUsize u16`, `maxbitrate u32`, `avgbitrate u32`, `reserved u32` | — | 4  |
|  `nmhd`, `sthd` | 14496-12 §8.4.5.2 / 14496-12 §12.6.2 | FullBox, no fields | — | 1  |
|  `dinf` | 14496-12 §8.7.1 | container | — | 1  |
|  `dref` | 14496-12 §8.7.2 | FullBox, ContainerWithPrefix; `entry_count u32`; then child boxes | — | 1  |
|  `url ` | 14496-12 §8.7.2 | FullBox; flags `0x1 self-contained`; `?!(flags&1) location str` | "self-contained" | 1  |
|  `urn ` | 14496-12 §8.7.2 | FullBox; `name str`, `?more location str` | — | 1  |
|  `udta` | 14496-12 §8.10.1 | container (children: group L) | — | 1  |
|  `free`, `skip`, `wide` | 14496-12 §8.1.2 / QTFF | Opaque | size only | 1  |
|  `pdin` | 14496-12 §8.1.3 | FullBox; `[ {rate u32, initial_delay u32} ]` to end | — | 4  |
|  `mdat` | 14496-12 §8.1.1 | Opaque | — | 1  |

### 8.3.2 Group B — Sample description and sample entries (`core/src/container/isobmff/decoders/sample_entries.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
|  `stbl` | 14496-12 §8.5.1 | container | — | 1  |
|  `stsd` | 14496-12 §8.5.2 | FullBox v0/v1, ContainerWithPrefix; `entry_count u32`; children = sample entries decoded by `ctx.handler_type` | sets `ctx.stsd_version`; `entry_count != children.size()` → `CNT-1010` | 1  |
| *SampleEntry* (common prefix of every entry) | §8.5.2.2 | `reserved u8[6]`, `data_reference_index u16` | — | 1 |
| *VisualSampleEntry* (`avc1 avc2 avc3 avc4 hev1 hvc1 hvc2 hvc3 hev2 dvh1 dvhe dva1 dvav av01 vp08 vp09 mp4v s263 encv resv apch apcn …` and any entry under `vide`) | §12.1.3 | after prefix: `pre_defined u16`, `reserved u16`, `pre_defined u32[3]`, `width u16`, `height u16`, `horizresolution 16.16`, `vertresolution 16.16`, `reserved u32`, `frame_count u16`, `compressorname` (`u8 length` + 31 bytes, display as string), `depth u16`, `pre_defined i16 (= -1)`; then child boxes (`avcC hvcC av1C vpcC esds btrt pasp clap colr mdcv clli cclv dvcC dvvC dvwC fiel sinf st3d sv3d ccst`) | codec from fourcc; `depth` → "24 colour", "32 colour+alpha", "40 greyscale"; sets `ctx.sample_entry` | 1 |
| *AudioSampleEntry* (`mp4a Opus fLaC ac-3 ec-3 ac-4 dtsc samr sawb alac twos sowt lpcm in24 in32 fl32 fl64 ulaw alaw enca …` under `soun`) | §12.2.3; QTFF "Sound Sample Description" | after prefix, **layout selected by rule R-AUDIO**: ISO v0: `reserved u32[2]`, `channelcount u16`, `samplesize u16`, `pre_defined u16`, `reserved u16`, `samplerate 16.16`. ISO V1 (`ctx.stsd_version == 1`): `entry_version u16 (=1)`, `reserved u16[3]`, `channelcount`, `samplesize`, `pre_defined`, `reserved`, `samplerate 16.16`, then `srat` child allowed. QT v1 (`version u16 == 1` and `ctx.quicktime`): `version u16`, `revision_level u16`, `vendor 4cc`, `channels u16`, `sample_size u16`, `compression_id i16`, `packet_size u16`, `sample_rate 16.16`, `samples_per_packet u32`, `bytes_per_packet u32`, `bytes_per_frame u32`, `bytes_per_sample u32`. QT v2 (`version == 2`): `version u16`, `revision_level u16`, `vendor 4cc`, `always3 u16`, `always16 u16`, `alwaysMinus2 i16`, `always0 u16`, `always65536 u32`, `sizeOfStructOnly u32`, `audioSampleRate f64`, `numAudioChannels u32`, `always7F000000 u32`, `constBitsPerChannel u32`, `formatSpecificFlags u32`, `constBytesPerAudioPacket u32`, `constLPCMFramesPerAudioPacket u32`. Then child boxes (`esds dOps dfLa dac3 dec3 dac4 btrt chan wave sinf srat`) | **R-AUDIO:** peek `u16` at prefix+8: if `ctx.stsd_version == 1` → ISO V1; else if value ∈ {1,2} and (`ctx.quicktime` or a `wave` child follows) → QT v1/v2; else ISO v0. Derived: sample rate Hz, channel layout | 1 |
|  `tx3g` (3GPP timed text sample entry) | 3GPP TS 26.245 §5.16 | after prefix: `displayFlags u32`, `horizontal_justification i8`, `vertical_justification i8`, `background_color_rgba u8[4]`, `BoxRecord {top i16, left i16, bottom i16, right i16}`, `StyleRecord {startChar u16, endChar u16, font_ID u16, face_style_flags u8, font_size u8, text_color_rgba u8[4]}`; children `ftab` (`entry_count u16`, `[ {font_ID u16, font_name_length u8, font str[len]} ]`) | flag names | 1  |
| `wvtt` (WVTTSampleEntry) | 14496-30 §7.5 | after prefix: children `vttC` (`config str` to end), `vlab` (`source_label str`), `btrt` | — | 1 |
| `stpp` (XMLSubtitleSampleEntry) | 14496-30 §6.3 | after prefix: `namespace str`, `schema_location str`, `auxiliary_mime_types str`; children `btrt`, `mime` | — | 1 |
| `mett` | 14496-12 §12.3.3 | after prefix: `content_encoding str`, `mime_format str`; children `btrt`, `txtC` | — | 1 |
|  `metx` | 14496-12 §12.3.3 | after prefix: `content_encoding str`, `namespace str`, `schema_location str` | — | 1  |
|  `urim` | 14496-12 §12.3.3 | after prefix: child `uri ` (FullBox, `theURI str`), child `uriI` (FullBox, `uri_initialization_data` bytes to end) | — | 1  |
| `tmcd` (QuickTime timecode) | QTFF "Timecode Sample Description" | after prefix: `reserved u32`, `flags u32` (bit0 drop frame, bit1 24 hour max, bit2 negative ok, bit3 counter), `time_scale u32`, `frame_duration u32`, `number_of_frames u8`, `reserved u8`; child `name` (`text_size u16`, `language u16`, `text str[text_size]`) | frame rate = time_scale / frame_duration; flag names | 1 |
| `text` (QuickTime text sample entry) | QTFF "Text Sample Description" | after prefix: `display_flags u32`, `text_justification i32`, `background_color u16[3]`, `default_text_box i16[4]`, `reserved u64`, `font_number u16`, `font_face u16`, `reserved u8`, `reserved u16`, `foreground_color u16[3]`, `text_name` Pascal string to end | — | 4 |
|  `mp4s`, `rtp `, `enc*` generic | 14496-12 §12.x | prefix only, then children | `enc*`: derived "encrypted, original = frma" | 1  |

### 8.3.3 Group C — Codec configuration records and sample-entry children (`core/src/container/isobmff/decoders/codec_config.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
| `avcC` | 14496-15 §5.3.3.1.2 (AVCDecoderConfigurationRecord) | `configurationVersion u8 (=1)`, `AVCProfileIndication u8`, `profile_compatibility u8`, `AVCLevelIndication u8`, `reserved u(6) (=0b111111)`, `lengthSizeMinusOne u(2)`, `reserved u(3) (=0b111)`, `numOfSequenceParameterSets u(5)`, `[ {sequenceParameterSetLength u16, sequenceParameterSetNALUnit bytes} ]`, `numOfPictureParameterSets u8`, `[ {pictureParameterSetLength u16, pictureParameterSetNALUnit bytes} ]`; `?profile_idc ∈ {100,110,122,144}` (and bytes remain): `reserved u(6)`, `chroma_format u(2)`, `reserved u(5)`, `bit_depth_luma_minus8 u(3)`, `reserved u(5)`, `bit_depth_chroma_minus8 u(3)`, `numOfSequenceParameterSetExt u8`, `[ {sequenceParameterSetExtLength u16, sequenceParameterSetExtNALUnit bytes} ]` | profile name (66 Baseline, 77 Main, 88 Extended, 100 High, 110 High 10, 122 High 4:2:2, 244 High 4:4:4 Predictive); level "4.1"; `NAL length size = lengthSizeMinusOne+1`; each SPS/PPS child gets `derived = "→ syntax tree"` and is handed to `ovt::syntax` (chapter 7) which attaches its parsed `SyntaxNode` under the field; `avc3`/`avc4` with PS inside → `CNT-1020` Info | 1 |
| `hvcC` | 14496-15 §8.3.3.1.2 (HEVCDecoderConfigurationRecord) | `configurationVersion u8 (=1)`, `general_profile_space u(2)`, `general_tier_flag u(1)`, `general_profile_idc u(5)`, `general_profile_compatibility_flags u32`, `general_constraint_indicator_flags u(48)`, `general_level_idc u8`, `reserved u(4) (=1111)`, `min_spatial_segmentation_idc u(12)`, `reserved u(6)`, `parallelismType u(2)`, `reserved u(6)`, `chromaFormat u(2)`, `reserved u(5)`, `bitDepthLumaMinus8 u(3)`, `reserved u(5)`, `bitDepthChromaMinus8 u(3)`, `avgFrameRate u16`, `constantFrameRate u(2)`, `numTemporalLayers u(3)`, `temporalIdNested u(1)`, `lengthSizeMinusOne u(2)`, `numOfArrays u8`, `[ {array_completeness u(1), reserved u(1), NAL_unit_type u(6), numNalus u16, [ {nalUnitLength u16, nalUnit bytes} ]} ]` | profile (1 Main, 2 Main 10, 3 Main Still, 4 RExt, 9 SCC), tier, level `/30`; `avgFrameRate/256 fps`; `chromaFormat` name; `general_constraint_indicator_flags` decoded bit names (progressive, interlaced, non_packed, frame_only, + RExt constraint bits); each VPS/SPS/PPS/SEI NAL → syntax module; `hev1` with no PS arrays → Info "parameter sets in-band" | 1 |
| `av1C` | AV1-ISOBMFF §2.3 (AV1CodecConfigurationRecord) | `marker u(1) (=1)`, `version u(7) (=1)`, `seq_profile u(3)`, `seq_level_idx_0 u(5)`, `seq_tier_0 u(1)`, `high_bitdepth u(1)`, `twelve_bit u(1)`, `monochrome u(1)`, `chroma_subsampling_x u(1)`, `chroma_subsampling_y u(1)`, `chroma_sample_position u(2)`, `reserved u(3)`, `initial_presentation_delay_present u(1)`, `?present initial_presentation_delay_minus_one u(4)` / `?!present reserved u(4)`, `configOBUs bytes` to end | profile name (Main/High/Professional), level "5.1" (`2 + (idx>>2)` `.` `idx&3`), bit depth (8/10/12 from high_bitdepth+twelve_bit), chroma format (mono/420/422/444); `configOBUs` split into OBUs (must be `OBU_SEQUENCE_HEADER` and metadata OBUs only; else `CNT-1021`) → syntax module | 1 |
| `vpcC` | VP-ISOBMFF §2.3 | FullBox (v1); `profile u8`, `level u8`, `bitDepth u(4)`, `chromaSubsampling u(3)`, `videoFullRangeFlag u(1)`, `colourPrimaries u8`, `transferCharacteristics u8`, `matrixCoefficients u8`, `codecIntializationDataSize u16`, `codecIntializationData bytes` | H.273 names; `codecIntializationDataSize != 0` for VP9 → `CNT-1022` | 1 |
| `esds` | 14496-14 §6.7; 14496-1 §7.2.6.5–7.2.6.7 | FullBox; `ES_Descriptor`: `tag u8 (=0x03)`, `size expandable`, `ES_ID u16`, `streamDependenceFlag u(1)`, `URL_Flag u(1)`, `OCRstreamFlag u(1)`, `streamPriority u(5)`, `?dep dependsOn_ES_ID u16`, `?url URLlength u8, URLstring str[len]`, `?ocr OCR_ES_Id u16`; `DecoderConfigDescriptor`: `tag (=0x04)`, `size`, `objectTypeIndication u8`, `streamType u(6)`, `upStream u(1)`, `reserved u(1)`, `bufferSizeDB u24`, `maxBitrate u32`, `avgBitrate u32`; `?` `DecoderSpecificInfo`: `tag (=0x05)`, `size`, `bytes`; `SLConfigDescriptor`: `tag (=0x06)`, `size`, `predefined u8`; unknown tags → `{tag, size, bytes}` | OTI names (0x40 AAC, 0x20 MPEG-4 Visual, 0x6B MP3, 0x69 MP3-13818-3, 0x21 AVC); streamType names; for OTI 0x40 decode `AudioSpecificConfig` (14496-3 §1.6.2.1): `audioObjectType u(5)` (`?==31 → 32 + u(6)`), `samplingFrequencyIndex u(4)` (`?==15 samplingFrequency u24`), `channelConfiguration u(4)`, + GASpecificConfig `frameLengthFlag u(1)`, `dependsOnCoreCoder u(1)`, `extensionFlag u(1)`; SBR/PS explicit signalling (`syncExtensionType 0x2B7`) → "HE-AAC" | 1 |
| `dOps` | Opus-ISOBMFF §4.3.2 | `Version u8 (=0)`, `OutputChannelCount u8`, `PreSkip u16`, `InputSampleRate u32`, `OutputGain i16`, `ChannelMappingFamily u8`, `?family != 0`: `StreamCount u8`, `CoupledCount u8`, `ChannelMapping u8[OutputChannelCount]` | gain dB = `OutputGain/256`; pre-skip ms at 48 kHz | 1 |
| `dfLa` | FLAC-ISOBMFF §3.3.2 | FullBox; `[ METADATA_BLOCK {last u(1), type u(7), length u24, data} ]`; STREAMINFO (type 0) decoded: `min/max_block_size u16`, `min/max_frame_size u24`, `sample_rate u(20)`, `channels_minus1 u(3)`, `bps_minus1 u(5)`, `total_samples u(36)`, `md5 u8[16]` | — | 4 |
| `dac3` / `dec3` | ETSI TS 102 366 Annex F | `dac3`: `fscod u(2)`, `bsid u(5)`, `bsmod u(3)`, `acmod u(3)`, `lfeon u(1)`, `bit_rate_code u(5)`, `reserved u(5)`. `dec3`: `data_rate u(13)`, `num_ind_sub u(3)`, `[ {fscod u(2), bsid u(5), reserved u(1), asvc u(1), bsmod u(3), acmod u(3), lfeon u(1), reserved u(3), num_dep_sub u(4), ?num_dep_sub>0 chan_loc u(9) / else reserved u(1)} ]` | sample rate, channel layout, bitrate | 4 |
| `btrt` | 14496-12 §8.5.2.2 | `bufferSizeDB u32`, `maxBitrate u32`, `avgBitrate u32` | kbit/s | 1 |
|  `pasp` | 14496-12 §12.1.4 | `hSpacing u32`, `vSpacing u32` | PAR "16:11", DAR with width/height | 1  |
|  `clap` | 14496-12 §12.1.4 | `cleanApertureWidthN u32`, `cleanApertureWidthD u32`, `cleanApertureHeightN u32`, `cleanApertureHeightD u32`, `horizOffN u32`, `horizOffD u32`, `vertOffN u32`, `vertOffD u32` | clean aperture rect in pixels (`Rect`), `CNT-1023` if non-integral | 1  |
|  `colr` | 14496-12 §12.1.5; QTFF "Color Parameter Atoms" | `colour_type 4cc`; `nclx`: `colour_primaries u16`, `transfer_characteristics u16`, `matrix_coefficients u16`, `full_range_flag u(1)`, `reserved u(7)`; `nclc` (QuickTime): three `u16`, no range byte; `rICC`/`prof`: `ICC_profile bytes` to end | H.273 names (1 BT.709, 9 BT.2020, 12 P3-D65; 16 PQ, 18 HLG, 1/6/14/15 SDR; 1 BT.709, 9 BT.2020 NCL, 10 CL, 14 ICtCp); ICC: `profile size u32`, description tag text; `nclc` in non-QT file → `CNT-5002` | 1  |
|  `mdcv` | 14496-12 §12.1.6 (MasteringDisplayColourVolumeBox) / ST 2086 | `display_primaries_x u16, display_primaries_y u16` ×3 (order as written: G, B, R — the HEVC SEI order of H.265 D.2.28, which ISOBMFF `mdcv` and Matroska follow; ST 2086 itself does not fix an order), `white_point_x u16`, `white_point_y u16`, `max_display_mastering_luminance u32`, `min_display_mastering_luminance u32` | chromaticities ×0.00002; luminance ×0.0001 cd/m²; recognise "P3-D65 1000/0.0001", "BT.2020 4000/0.005" | 1  |
|  `clli` | 14496-12 §12.1.6 (ContentLightLevelBox) | `max_content_light_level u16`, `max_pic_average_light_level u16` | cd/m² | 1  |
|  `cclv` | 14496-12 §12.1.6 (ContentColourVolumeBox, 14496-12:2022); the field names and order are those of the H.265 `content_colour_volume` SEI (H.265 D.2.40, semantics D.3.40) with a leading 2-bit reserved field: primaries are `i(32)` in 0.00002 units, luminance values `u(32)` in 0.0000001 (10⁻⁷) cd/m² — not the 0.0001 cd/m² of `mdcv` | `reserved u(2)`, `ccv_cancel_flag u(1)`, `ccv_persistence_flag u(1)`, `ccv_primaries_present_flag u(1)`, `ccv_min_luminance_value_present_flag u(1)`, `ccv_max_luminance_value_present_flag u(1)`, `ccv_avg_luminance_value_present_flag u(1)`; `?primaries [ {ccv_primaries_x i32, ccv_primaries_y i32} ]×3`; `?min ccv_min_luminance_value u32`; `?max ccv_max_luminance_value u32`; `?avg ccv_avg_luminance_value u32` | luminance ×0.0000001 cd/m² | 1  |
| `dvcC`, `dvvC`, `dvwC` | DV-ISOBMFF §2.2 (DOVIDecoderConfigurationRecord) | `dv_version_major u8`, `dv_version_minor u8`, `dv_profile u(7)`, `dv_level u(6)`, `rpu_present_flag u(1)`, `el_present_flag u(1)`, `bl_present_flag u(1)`, `dv_bl_signal_compatibility_id u(4)`, `reserved u(28)`, `reserved u32[4]` | "Profile 8.1 (HDR10 compatible)" from profile + compat id (0 none, 1 HDR10, 2 SDR, 4 HLG, 6 Blu-ray HDR10); `dvcC` MUST be used for profiles ≤ 7 and `dvvC` for ≥ 8 else `CNT-5020` | 1 |
| `fiel` | QTFF "Video Sample Description Extensions" | `fields u8` (1 progressive, 2 interlaced), `detail u8` (0 unknown, 1 top first, 6 bottom first, 9 top first/bottom displayed first, 14 bottom first/top displayed first) | names | 1 |
| `st3d`, `sv3d`/`svhd`/`proj`/`prhd`/`equi`/`cbmp` | Google Spatial Media spec | `st3d`: FullBox, `stereo_mode u8`; `proj` children: `prhd` (yaw/pitch/roll `16.16`), `equi` (`projection_bounds u32[4]`), `cbmp` (`layout u32`, `padding u32`) | names | 4 |
| `ccst` | 23008-12 §6.5.x (CodingConstraintsBox) | FullBox; `all_ref_pics_intra u(1)`, `intra_pred_used u(1)`, `max_ref_per_pic u(4)`, `reserved u(26)` | — | 4 |
| `srat` | 14496-12 §12.2.4 | FullBox; `sampling_rate u32` | — | 1 |
|  `chnl` | 14496-12 §12.2.4 | FullBox; `stream_structure u8`, `?&1 definedLayout u8, …`, `?&2 object_count u8` (full syntax per spec) | layout names | 4  |

### 8.3.4 Group D — Sample tables in `stbl` (`core/src/container/isobmff/decoders/sample_tables.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
|  `stts` | 14496-12 §8.6.1.2 | FullBox; `entry_count u32`; `[ {sample_count u32, sample_delta u32} ]` | total duration; average frame rate = timescale / mean delta; `sample_delta == 0` → `CNT-2001` | 1  |
|  `ctts` | 14496-12 §8.6.1.3 | FullBox v0/v1; `entry_count u32`; `[ {sample_count u32, sample_offset u32 (v0) / i32 (v1)} ]` | min/max offset; negative offsets in v0 (value ≥ 2^31) → `CNT-2002` | 1  |
|  `cslg` | 14496-12 §8.6.1.4 | FullBox v0/v1; `compositionToDTSShift i32/i64`, `leastDecodeToDisplayDelta i32/i64`, `greatestDecodeToDisplayDelta i32/i64`, `compositionStartTime i32/i64`, `compositionEndTime i32/i64` | seconds | 1  |
|  `stsc` | 14496-12 §8.7.4 | FullBox; `entry_count u32`; `[ {first_chunk u32, samples_per_chunk u32, sample_description_index u32} ]` | `first_chunk` not strictly increasing → `CNT-2003`; `sample_description_index == 0` or `> stsd.entry_count` → `CNT-2004` | 1  |
|  `stsz` | 14496-12 §8.7.3.2 | FullBox; `sample_size u32`, `sample_count u32`, `?sample_size==0 [entry_size u32]` | total bytes; max/min/mean size; `sample_count` vs `stts` total → `CNT-2005` | 1  |
|  `stz2` | 14496-12 §8.7.3.3 | FullBox; `reserved u24`, `field_size u8 (4,8,16)`, `sample_count u32`, `[entry_size u(field_size)]` (4-bit entries packed two per byte, high nibble first) | as `stsz`; bad `field_size` → `CNT-2006` | 1  |
|  `stco` | 14496-12 §8.7.5 | FullBox; `entry_count u32`; `[chunk_offset u32]` | chunk count; offset beyond EOF → `CNT-2007` | 1  |
|  `co64` | 14496-12 §8.7.5 | FullBox; `entry_count u32`; `[chunk_offset u64]` | as `stco`; both `stco` and `co64` present → `CNT-2008` | 1  |
|  `stss` | 14496-12 §8.6.2 | FullBox; `entry_count u32`; `[sample_number u32]` (1-based) | sync count, mean GOP length; `sample_number` 0 or > count → `CNT-2009`; absent box → derived "all samples sync" on `stbl` | 1  |
| `stps` | QTFF "Partial Sync Sample Atom" | FullBox; `entry_count u32`; `[sample_number u32]` | partial-sync (open GOP) count | 1 |
|  `sdtp` | 14496-12 §8.6.4 | FullBox; `[ {is_leading u(2), sample_depends_on u(2), sample_is_depended_on u(2), sample_has_redundancy u(2)} ]` × `sample_count` from `stsz`/`stz2` (payload length must equal `sample_count`, else `CNT-2010`) | enum names (`sample_depends_on`: 0 unknown, 1 depends (not I), 2 does not depend (I), 3 reserved) | 1  |
|  `padb` | 14496-12 §8.7.6 | FullBox; `sample_count u32`; `[ {reserved u(1), pad1 u(3), reserved u(1), pad2 u(3)} ]` × ceil(count/2) | — | 4  |
|  `stdp` | 14496-12 §8.5.3 (DegradationPriorityBox) | FullBox; `[priority u16]` × sample_count | — | 4  |
|  `stsh` | 14496-12 §8.6.3 | FullBox; `entry_count u32`; `[ {shadowed_sample_number u32, sync_sample_number u32} ]` | `CNT-1030` Info "rarely used shadow sync table" | 4  |
|  `subs` | 14496-12 §8.7.7 | FullBox v0/v1; flags; `entry_count u32`; `[ {sample_delta u32, subsample_count u16, [ {subsample_size u16 (v0) / u32 (v1), subsample_priority u8, discardable u8, codec_specific_parameters u32} ]} ]` | for AVC/HEVC (`flags == 0`): subsample = NAL unit (14496-15 §4.9); `codec_specific_parameters` decoded per 14496-15 (`SubsampleInformation` flags: `subsample_type`…); AV1 (AV1-ISOBMFF §2.4.5): `flags == 0` = temporal unit/OBU level | 1  |
|  `saiz` | 14496-12 §8.7.8 | FullBox; `?flags&1 aux_info_type 4cc, aux_info_type_parameter u32`; `default_sample_info_size u8`; `sample_count u32`; `?default==0 [sample_info_size u8]` | `aux_info_type` name (`cenc`, `cbcs`, absent = CENC per 23001-7 §7.1) | 1  |
|  `saio` | 14496-12 §8.7.9 | FullBox v0/v1; `?flags&1 aux_info_type 4cc, aux_info_type_parameter u32`; `entry_count u32`; `[offset u32/u64]` | in `traf`: resolved absolute offset per §8.4.4 | 1  |
|  `sgpd` | 14496-12 §8.9.3 | FullBox v0/v1/v2; `grouping_type 4cc`; `?v==1 default_length u32`; `?v≥2 default_sample_description_index u32`; `entry_count u32`; `[ ?(v==1 && default_length==0) description_length u32; SampleGroupDescriptionEntry(grouping_type) ]` with entry syntax: `roll`/`prol`: `roll_distance i16`; `rap `: `num_leading_samples_known u(1)`, `num_leading_samples u(7)`; `tele`: `level_independently_decodable u(1)`, `reserved u(7)`; `sync`: `reserved u(2)`, `NAL_unit_type u(6)`; `seig` (23001-7 §6): `reserved u8`, `crypt_byte_block u(4)`, `skip_byte_block u(4)`, `isProtected u8`, `Per_Sample_IV_Size u8`, `KID u8[16]`, `?isProtected==1 && Per_Sample_IV_Size==0 constant_IV_size u8, constant_IV u8[constant_IV_size]`; `tscl`: `temporalLayerId u8`, `tlprofile_space u(2)`, `tltier_flag u(1)`, `tlprofile_idc u(5)`, `tlprofile_compatibility_flags u32`, `tlconstraint_indicator_flags u(48)`, `tllevel_idc u8`, `tlMaxBitRate u16`, `tlAvgBitRate u16`, `tlConstantFrameRate u8`, `tlAvgFrameRate u16`; `tsas`, `stsa`: no fields; `alst`: `roll_count u16`, `first_output_sample u16`, `[sample_offset u32]`, `[ {num_output_samples u16, num_total_samples u16} ]`; `av1M`/`av1s` (AV1 metadata/switch) : per AV1-ISOBMFF; unknown grouping types: `description_length` bytes raw (v1) or `CNT-2020` (v0: length unknown; stop) | KID hex; `roll_distance` → "pre-roll N samples" | 1  |
|  `sbgp` | 14496-12 §8.9.2 | FullBox v0/v1; `grouping_type 4cc`; `?v==1 grouping_type_parameter u32`; `entry_count u32`; `[ {sample_count u32, group_description_index u32} ]` | index 0 = "no group"; index > entries of matching `sgpd` (local 0x10000+ in `traf`) → `CNT-2021`; sum of `sample_count` ≠ track/fragment sample count → `CNT-2022` | 1  |

### 8.3.5 Group E — Fragmentation (`core/src/container/isobmff/decoders/fragment_boxes.cpp`; the derived `FragmentTimeline` view lives in `core/src/container/isobmff/fragments.cpp`, §8.4.3)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
|  `mvex` | 14496-12 §8.8.1 | container | sets `TrackInfo.is_fragmented` for every `trex` | 1  |
|  `mehd` | 14496-12 §8.8.2 | FullBox v0/v1; `fragment_duration u32/u64` | seconds (movie timescale) | 1  |
|  `trex` | 14496-12 §8.8.3 | FullBox; `track_ID u32`, `default_sample_description_index u32`, `default_sample_duration u32`, `default_sample_size u32`, `default_sample_flags u32` | sample_flags bitfield (see `trun`); stored in `ctx.trex_for_track` | 1  |
|  `leva` | 14496-12 §8.8.13 | FullBox; `level_count u8`; `[ {track_id u32, padding_flag u(1), assignment_type u(7), ?type∈{0,1} grouping_type 4cc, ?type==1 grouping_type_parameter u32, ?type==4 sub_track_id u32} ]` | assignment_type names | 4  |
|  `moof` | 14496-12 §8.8.4 | container (lazy, §8.2.4); sets `ctx.moof_offset` | sequence number from `mfhd` shown in node description | 1  |
|  `mfhd` | 14496-12 §8.8.5 | FullBox; `sequence_number u32` | non-monotonic → `CNT-4001` | 1  |
|  `traf` | 14496-12 §8.8.6 | container | — | 1  |
|  `tfhd` | 14496-12 §8.8.7 | FullBox; flags: `0x000001 base-data-offset-present`, `0x000002 sample-description-index-present`, `0x000008 default-sample-duration-present`, `0x000010 default-sample-size-present`, `0x000020 default-sample-flags-present`, `0x010000 duration-is-empty`, `0x020000 default-base-is-moof`; `track_ID u32`, `?0x1 base_data_offset u64`, `?0x2 sample_description_index u32`, `?0x8 default_sample_duration u32`, `?0x10 default_sample_size u32`, `?0x20 default_sample_flags u32` | flag names; effective defaults after `trex` fallback shown as derived on the box; both `0x1` and `0x020000` set → `CNT-4002` | 1  |
|  `tfdt` | 14496-12 §8.8.12 | FullBox v0/v1; `baseMediaDecodeTime u32/u64` | seconds (track timescale); continuity check §8.4.3 | 1  |
|  `trun` | 14496-12 §8.8.8 | FullBox v0/v1; flags: `0x000001 data-offset-present`, `0x000004 first-sample-flags-present`, `0x000100 sample-duration-present`, `0x000200 sample-size-present`, `0x000400 sample-flags-present`, `0x000800 sample-composition-time-offsets-present`; `sample_count u32`, `?0x1 data_offset i32`, `?0x4 first_sample_flags u32`, `[ {?0x100 sample_duration u32, ?0x200 sample_size u32, ?0x400 sample_flags u32, ?0x800 sample_composition_time_offset u32 (v0) / i32 (v1)} ]`. **sample_flags bitfield** (§8.8.3.1): `reserved u(4)`, `is_leading u(2)`, `sample_depends_on u(2)`, `sample_is_depended_on u(2)`, `sample_has_redundancy u(2)`, `sample_padding_value u(3)`, `sample_is_non_sync_sample u(1)`, `sample_degradation_priority u16` | sum of durations, sum of sizes, first sample absolute offset (§8.4.3), count of sync samples; `0x4` and `0x400` both set → `CNT-4003`; large arrays summarised (§8.2.5) | 1  |
| `senc` | 23001-7 §7.2 | FullBox; flags `0x2 UseSubSampleEncryption`; `sample_count u32`; `[ {InitializationVector u8[Per_Sample_IV_Size], ?flags&2 subsample_count u16, [ {BytesOfClearData u16, BytesOfProtectedData u32} ]} ]` where `Per_Sample_IV_Size` = `ctx.per_sample_iv_size` (from `tenc`, overridden per sample by `seig`). **If unknown** (`0xFF`): try IV size 8 then 16 and accept the one for which the parse ends exactly at the box end; if neither → `CNT-6001` and leave payload undecoded | IV hex; clear/protected totals; sum per sample vs sample size → `CNT-6002` | 1 |
|  `mfra` | 14496-12 §8.8.9 | container; located via `mfro` (§8.2.4 rule 4) | — | 1  |
|  `tfra` | 14496-12 §8.8.10 | FullBox v0/v1; `track_ID u32`, `reserved u(26)`, `length_size_of_traf_num u(2)`, `length_size_of_trun_num u(2)`, `length_size_of_sample_num u(2)`, `number_of_entry u32`; `[ {time u32/u64, moof_offset u32/u64, traf_number u((len+1)*8), trun_number u((len+1)*8), sample_number u((len+1)*8)} ]` | time → s; `moof_offset` not pointing at a `moof` header → `CNT-4010` | 1  |
|  `mfro` | 14496-12 §8.8.11 | FullBox; `size u32` | equals `mfra.size` else `CNT-4011` | 1  |
|  `sidx` | 14496-12 §8.16.3 | FullBox v0/v1; `reference_ID u32`, `timescale u32`, `earliest_presentation_time u32/u64`, `first_offset u32/u64`, `reserved u16`, `reference_count u16`; `[ {reference_type u(1), referenced_size u(31), subsegment_duration u32, starts_with_SAP u(1), SAP_type u(3), SAP_delta_time u(28)} ]` | each reference's absolute byte range (`sidx end + first_offset + Σ previous sizes`) and time range; `reference_type 1` = points to another `sidx` (hierarchical); SAP type names (14496-12 Annex I) | 1  |
|  `ssix` | 14496-12 §8.16.4 | FullBox; `subsegment_count u32`; `[ {range_count u32, [ {level u8, range_size u24} ]} ]` | per-level byte ranges | 1  |
|  `prft` | 14496-12 §8.16.5 | FullBox v0/v1; `reference_track_ID u32`, `ntp_timestamp u64`, `media_time u32/u64`; flags (14496-12:2022 §8.16.5): `0x1` = media_time refers to the first sample of the following `moof`, `0x2` = media_time is an arbitrary time inside the following segment, `0x4` = `ntp_timestamp` is the production time; `0x8`, `0x10` = encoder/producer reference positions (decode flag names from the 2022 text and keep unknown bits as `CNT-1004`) | NTP → ISO 8601 UTC (seconds since 1900 + 32-bit fraction); media_time → s | 1  |
| `emsg` | 23009-1 §5.10.3.3 (EventMessageBox, v0 and v1; 14496-12 only references it) | v0: `scheme_id_uri str`, `value str`, `timescale u32`, `presentation_time_delta u32`, `event_duration u32`, `id u32`, `message_data bytes` to end. v1: `timescale u32`, `presentation_time u64`, `event_duration u32`, `id u32`, `scheme_id_uri str`, `value str`, `message_data bytes` | known schemes: `urn:mpeg:dash:event:2012` (MPD validity), `urn:scte:scte35:2013:bin` (SCTE-35 → show `splice_command_type`), `https://aomedia.org/emsg/ID3` (ID3v2 tag → frames); `0xFFFFFFFF` duration = unknown | 1 |
|  `styp` | 14496-12 §8.16.2 | as `ftyp` | — | 1  |

### 8.3.6 Group F — Metadata / HEIF item structures (`core/src/container/isobmff/decoders/meta_items.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
| `meta` | 14496-12 §8.11.1 / QTFF | FullBox (ISO) or Box (QuickTime, §8.2.6 rule 4); container | — | 1 |
|  `hdlr` (in `meta`) | 14496-12 §8.11.1 | as group A; `handler_type` ∈ `mdta` (QuickTime keys), `mdir` (iTunes), `pict` (HEIF), `null` | — | 1  |
|  `pitm` | 14496-12 §8.11.4 | FullBox v0/v1; `item_ID u16 (v0) / u32 (v1)` | — | 1  |
|  `iinf` | 14496-12 §8.11.6 | FullBox v0/v1, ContainerWithPrefix; `entry_count u16 (v0) / u32 (v1)`; children `infe` | count check `CNT-8001` | 1  |
|  `infe` | 14496-12 §8.11.6 | FullBox v0–v3; flags `0x1 hidden`; v0/v1: `item_ID u16`, `item_protection_index u16`, `item_name str`, `content_type str`, `?more content_encoding str`; v1 adds `extension_type 4cc` + `ItemInfoExtension` (`fdel`: `content_location str`, `content_MD5 str`, `content_length u64`, `transfer_length u64`, `entry_count u8`, `[group_id u32]`); v2/v3: `item_ID u16 (v2) / u32 (v3)`, `item_protection_index u16`, `item_type 4cc`, `item_name str`, `?item_type=='mime' content_type str, ?more content_encoding str`, `?item_type=='uri ' item_uri_type str` | item type description (`kItemTypes`: `hvc1`, `av01`, `grid`, `iden`, `iovl`, `Exif`, `mime`, `jpeg`, `tmap`) | 1  |
|  `iloc` | 14496-12 §8.11.3 | FullBox v0/v1/v2; `offset_size u(4)`, `length_size u(4)`, `base_offset_size u(4)`, `index_size u(4) (v1/v2) / reserved u(4) (v0)`, `item_count u16 (v<2) / u32 (v2)`; `[ {item_ID u16/u32, ?v≥1 reserved u(12), construction_method u(4), data_reference_index u16, base_offset u(base_offset_size*8), extent_count u16, [ {?v≥1&&index_size>0 extent_index u(index_size*8), extent_offset u(offset_size*8), extent_length u(length_size*8)} ]} ]` | construction_method names (0 file offset, 1 `idat` offset, 2 item offset); absolute file span per extent; sizes ∉ {0,4,8} → `CNT-8002`; extent beyond EOF/`idat` → `CNT-8003` | 1  |
| `iprp` | 14496-12 §8.11.14.1 (ItemPropertiesBox; HEIF usage 23008-12 §9.3) | container (`ipco`, `ipma`) | — | 1 |
|  `ipco` | 14496-12 §8.11.14.2 (ItemPropertyContainerBox; HEIF usage 23008-12 §9.3) | container of properties: `ispe` (FullBox; `image_width u32`, `image_height u32`), `pixi` (FullBox; `num_channels u8`, `[bits_per_channel u8]`), `irot` (`reserved u(6)`, `angle u(2)` → ×90°), `imir` (`reserved u(7)`, `axis u(1)`), `clap`, `pasp`, `colr`, `hvcC`, `av1C`, `auxC` (FullBox; `aux_type str`, `aux_subtype bytes`), `rloc` (FullBox; `horizontal_offset u32`, `vertical_offset u32`), `lsel` (`layer_id u16`), `a1op` (`op_index u8`), `a1lx` (`reserved u(7)`, `large_size u(1)`, `layer_size u16/u32 ×3`), `altr` (in `grpl`) | property index (1-based) shown in node description for `ipma` linkage | 1  |
|  `ipma` | 14496-12 §8.11.14.3 (ItemPropertyAssociationBox) | FullBox v0/v1; flags `0x1` → 15-bit indices; `entry_count u32`; `[ {item_ID u16 (v0) / u32 (v1), association_count u8, [ {essential u(1), property_index u(7) (flags&1 ? u(15))} ]} ]` | per item: resolved property fourcc list; index 0 = none; index > `ipco` children → `CNT-8004` | 1  |
| `iref` + children | 14496-12 §8.11.12 | FullBox v0/v1; children `SingleItemTypeReferenceBox` (`dimg`, `thmb`, `cdsc`, `auxl`, `prem`, `base`, `iovl`…): `from_item_ID u16 (v0) / u32 (v1)`, `reference_count u16`, `[to_item_ID u16/u32]` | reference type names; dangling ids → `CNT-8005` | 1 |
|  `idat` | 14496-12 §8.11.11 | Opaque (read on demand for `construction_method 1`) | — | 1  |
|  `dinf`/`dref` (in `meta`) | 14496-12 §8.11.1 | as group A | — | 1  |
| `grpl` + `altr`/`ster`/`eqiv` | 14496-12 §8.18.3 (EntityToGroupBox) / 23008-12 | container; child FullBox: `group_id u32`, `num_entities_in_group u32`, `[entity_id u32]` | — | 4 |
|  `xml `, `bxml` | 14496-12 §8.11.2 | FullBox; `xml str` to end / `data bytes` | first 256 chars preview | 4  |
| `ipro` + children `sinf` (item protection; unrelated to `iprp`/`ipco`) | 14496-12 §8.11.5 | FullBox; `protection_count u16`; children `sinf` | — | 4 |

### 8.3.7 Group G — User data, iTunes and QuickTime metadata (`core/src/container/isobmff/decoders/udta.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
|  `udta` | 14496-12 §8.10.1 | container (in `moov`, `trak`, `moof`) | — | 1  |
|  `cprt` | 14496-12 §8.10.2 | FullBox; `pad u(1)`, `language u(5)[3]`, `notice str` (UTF-8 or UTF-16 with BOM) | — | 1  |
|  `titl`, `dscp`, `perf`, `auth`, `gnre`, `albm`, `kywd`, `loci`, `rtng`, `clsf`, `yrrc` | 3GPP TS 26.244 §8 | FullBox; `pad u(1)`, `language u(5)[3]`, then string(s); `albm` adds `trackNumber u8`; `yrrc`: `recordingYear u16`; `loci`: `name str`, `role u8`, `longitude 16.16`, `latitude 16.16`, `altitude 16.16`, `astronomical_body str`, `additional_notes str` | — | 4  |
| `©xyz` (QuickTime/3GPP GPS) | QTFF; Apple/Android camera convention | `size u16`, `language u16` (packed), `location str[size]` (ISO 6709, e.g. `+37.3349-122.0090/`) | parsed latitude/longitude/altitude as decimal degrees | 1 |
| `©nam`, `©ART`, `©alb`, `©day`, `©too`, `©cmt`, `©gen`, `©wrt`, `©des`, `©swr`, `©mak`, `©mod` (in `udta`, QuickTime) | QTFF "User Data Atoms" | `[ {size u16, language u16, text str[size]} ]` (international text list) | — | 1 |
| `name` (in `udta`) | QTFF | `text str` to end | — | 1 |
| `kind` | 14496-12 §8.10.4 | FullBox; `schemeURI str`, `value str` | known: `urn:mpeg:dash:role:2011` values | 1 |
| `chpl` (Nero chapters) | Nero Digital (de facto; no public spec — layout from FFmpeg `mov.c` `mov_read_chpl`) | FullBox v0/v1; `?v==1 reserved u32`; `chapter_count u8`; `[ {start_time u64 (100 ns units), title_len u8, title str[title_len]} ]` | start time → s | 1 |
| `meta` (iTunes, `hdlr == 'mdir'`) → `ilst` | QTFF "Metadata" / de facto iTunes | `ilst` container; children `©nam ©ART ©alb ©day ©too ©gen ©cmt ©wrt ©grp ©lyr aART trkn disk tmpo cpil pgap covr desc ldes tvsh tven tvsn tves stik hdvd rtng purd sonm soar soaa ----` each a container of `data` (and for `----`: `mean`, `name`, `data`) | atom name → human label | 1 |
| `data` (in `ilst/*`) | QTFF "Data Atom" | `type_indicator u32` (byte 0 `type_set_identifier`, bytes 1–3 `well_known_type`), `locale_indicator u32` (country u16 + language u16), `value bytes` to end | well-known types: 0 reserved/implicit, 1 UTF-8, 2 UTF-16BE, 3 S/JIS, 13 JPEG, 14 PNG, 21 BE signed int (1/2/3/4/8 bytes), 22 BE unsigned int, 23 BE float32, 24 BE float64, 27 BMP, 28 QuickTime metadata atom, 65 8-bit int, 66 16-bit, 67 32-bit, 74 64-bit, 75 u8, 76 u16, 77 u32, 78 u64; `trkn`/`disk` implicit = `u16 pad, u16 number, u16 total, u16 pad` | 1 |
| `mean`, `name` (in `----`) | iTunes | FullBox (version/flags = 0); `str` to end | — | 1 |
| `meta` (QuickTime `hdlr == 'mdta'`) → `keys` | QTFF "Metadata Item Keys Atom" | FullBox; `entry_count u32`; `[ {key_size u32, key_namespace 4cc (`mdta`/`udta`), key_value str[key_size-8]} ]` | `ilst` children in this `meta` are named by 1-based key index (fourcc = big-endian u32); decoder renames node description to the key string (`com.apple.quicktime.location.ISO6709`, `com.android.version`, `com.android.capture.fps`…) | 1 |
| `uuid` (see group K) | — | — | — | — |

### 8.3.8 Group H — Encryption (`core/src/container/isobmff/decoders/encryption.cpp`)

| fourcc | Spec | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
| `sinf` | 14496-12 §8.12.1 | container (`frma`, `schm`, `schi`); inside `encv`/`enca`/`encs`/`enct`/`resv` | — | 1 |
|  `frma` | 14496-12 §8.12.2 | `data_format 4cc` | sets `TrackInfo.original_format` | 1  |
|  `schm` | 14496-12 §8.12.5 | FullBox; flags `0x1` → `scheme_uri` present; `scheme_type 4cc`, `scheme_version u32`, `?flags&1 scheme_uri str` | scheme names (`cenc` AES-CTR, `cbc1`, `cens` pattern CTR, `cbcs` pattern CBC, `piff`, `odkm` OMA, `iAEC` ISMA) from `kSchemes`; version `0x00010000` = 1.0 | 1  |
|  `schi` | 14496-12 §8.12.6 | container (`tenc`, or `uuid` PIFF tenc, or `odkm`/`iKMS`/`iSFM`) | — | 1  |
| `tenc` | 23001-7 §8.2 | FullBox v0/v1; `reserved u8`; `?v==0 reserved u8` / `?v==1 default_crypt_byte_block u(4), default_skip_byte_block u(4)`; `default_isProtected u8`; `default_Per_Sample_IV_Size u8`; `default_KID u8[16]`; `?default_isProtected==1 && default_Per_Sample_IV_Size==0 default_constant_IV_size u8, default_constant_IV u8[size]` | KID as UUID hex; pattern "1:9"; sets `ctx.per_sample_iv_size`; IV size ∉ {0,8,16} → `CNT-6003` | 1 |
| `pssh` | 23001-7 §8.1 | FullBox v0/v1; `SystemID u8[16]`; `?v==1 KID_count u32, [KID u8[16]]`; `DataSize u32`; `Data bytes[DataSize]` | SystemID name from table below; expected box size `= 12 (FullBox header) + 16 (SystemID) + (version == 1 ? 4 + 16·KID_count : 0) + 4 + DataSize`; actual size ≠ expected → `CNT-6004`; Widevine `Data` decoded as protobuf (`WidevinePsshData`: field 1 `algorithm` varint, 2 `key_id` bytes (repeated), 3 `provider` string, 4 `content_id` bytes, 7 `policy` string, 8 `crypto_period_index`); PlayReady `Data` decoded as PlayReady Object: `length u32le`, `record_count u16le`, `[ {record_type u16le, record_length u16le, record_value}]` with type 1 = UTF-16LE WRMHEADER XML (KID base64 shown decoded) | 1 |
| *pssh system IDs* | DASH-IF content protection registry https://dashif.org/identifiers/content_protection/ | `edef8ba9-79d6-4ace-a3c8-27dcd51d21ed` Widevine · `9a04f079-9840-4286-ab92-e65be0885f95` PlayReady · `94ce86fb-07ff-4f43-adb8-93d2fa968ca2` FairPlay (Apple, rare in pssh) · `1077efec-c0b2-4d02-ace3-3c1e52e2fb4b` W3C Common PSSH / ClearKey · `5e629af5-38da-4063-8977-97ffbd9902d4` Marlin · `adb41c24-2dbf-4a6d-958b-4457c0d27b95` Nagra · `80a6be7e-1448-4c37-9e70-d5aebe04c8d2` Irdeto · `f239e769-efa3-4850-9c16-a903c6932efb` Adobe Primetime · `9a27dd82-fde2-4725-8cbc-4234aa06ec09` Verimatrix · `e2719d58-a985-b3c9-781a-b030af78d30e` ClearKey (DASH-IF) · `3d5e6d35-9b9a-41e8-b843-dd3c6e72c42c` ChinaDRM · `a68129d3-575b-4f1a-9cba-3223846cf7c3` Arris | table in `kPsshSystems[]` | 1 |
| `senc`, `saiz`, `saio`, `sgpd(seig)`, `sbgp(seig)` | see groups D/E | — | — | 1 |

### 8.3.9 Group K — `uuid` boxes with known user types (`core/src/container/isobmff/decoders/uuid_boxes.cpp`)

Dispatch key: 16-byte usertype. Unknown uuids: `description = "unknown uuid"`, payload as hex, plus heuristics: payload beginning with `<?xpacket` or `<x:xmpmeta` → "XMP (non-standard uuid)".

| usertype (UUID) | Owner / meaning | Fields | Derived | Phase |
|---|---|---|---|---|
| `8974dbce-7be7-4c51-84f9-7148f9882554` | PIFF 1.1 `TrackEncryptionBox` (in `schi`) | FullBox; `default_AlgorithmID u24` (0 none, 1 AES-CTR, 2 AES-CBC), `default_IV_size u8`, `default_KID u8[16]` | as `tenc`; `CNT-6010` Info "PIFF (pre-CENC) encryption boxes" | 1 |
| `a2394f52-5a9b-4f14-a244-6c427c648df4` | PIFF `SampleEncryptionBox` (in `traf`) | FullBox; `?flags&1 AlgorithmID u24, IV_size u8, KID u8[16]`; `sample_count u32`; `[ {InitializationVector u8[IV_size], ?flags&2 NumberOfEntries u16, [ {BytesOfClearData u16, BytesOfEncryptedData u32} ]} ]` | as `senc` | 1 |
| `6d1d9b05-42d5-44e6-80e2-141daff757b2` | PIFF `tfxd` TfxdBox (Smooth Streaming) | FullBox v0/v1; `fragment_absolute_time u32/u64`, `fragment_duration u32/u64` | seconds (timescale 10 MHz typical → from `mdhd`) | 1 |
| `d4807ef2-ca39-4695-8e54-26cb9e46a79f` | PIFF `tfrf` TfrfBox | FullBox v0/v1; `fragment_count u8`; `[ {fragment_absolute_time u32/u64, fragment_duration u32/u64} ]` | — | 1 |
|  `be7acfcb-97a9-42e8-9c71-999491e3afac` | Adobe XMP packet (XMP Specification Part 3 §1.2.5) | `xmp str` to end (UTF-8 XML) | detects Google Motion Photo (`GCamera:MicroVideoOffset`, `GCamera:MotionPhoto`, `Container:Directory` with `Item:Semantic="MotionPhoto"`) and Samsung Motion Photo (`Camera:MotionPhoto`); derived: embedded video offset/length; also detects 360° (`GSpherical`) | 1  |
| `d08a4f18-10f3-4a82-b6c8-32d8aba183d3` | PIFF `ProtectionSystemSpecificHeaderBox` (pre-CENC `pssh`) | as `pssh` v0 | `CNT-6010` | 1 |
| `55534d54-21d2-4fce-bb88-695cfac9c740` | Sony/Canon `USMT` metadata container (contains `MTDT`) | `MTDT`: `entry_count u16`; `[ {size u16, language u16, encoding u16, type u16, value} ]` | — | 4 |
| `50524f46-21d2-4fce-bb88-695cfac9c740` | Sony `PROF` profile | opaque | — | 4 |
|  `ffcc8263-f855-4a93-8814-587a02521fdd` | Samsung `SEFT` trailer pointer (motion photo, found at file end, not in box structure) — detect via trailer scan, see the `mfro`/trailer tail read of §8.2.4 rule 4 | — | — | 4  |
| `mpvd` (regular fourcc, not uuid) | Google Motion Photo "motion photo video data" box in HEIC/JPEG-in-ISOBMFF | Opaque | links to XMP offsets | 1 |

### 8.3.10 Group M — QuickTime-specific track/media boxes (`core/src/container/isobmff/decoders/quicktime.cpp`)

| fourcc | Spec (QTFF section) | Fields in spec order | Derived | Phase |
|---|---|---|---|---|
| `tapt` | "Track Aperture Mode Dimensions" | container (`clef`, `prof`, `enof`) | — | 1 |
| `clef`, `prof`, `enof` | same | FullBox; `width 16.16`, `height 16.16` | clean/production/encoded aperture | 1 |
| `gmhd` | "Base Media Information Header" | container (`gmin`, `text`, `tmcd`, `tcmi`) | — | 1 |
| `gmin` | "Base Media Info Atom" | FullBox; `graphics_mode u16`, `opcolor u16[3]`, `balance i16`, `reserved u16` | — | 1 |
| `text` (in `gmhd`) | "Text Media Information" | `matrix i32[9]` | identity check | 1 |
| `tcmi` (in `gmhd/tmcd`) | "Timecode Media Information" | FullBox; `text_font u16`, `text_face u16`, `text_size u16`, `text_color u16[3]`, `background_color u16[3]`, `font_name` Pascal str | — | 4 |
| `wave` (in audio sample entry) | "siDecompressionParam" | container: `frma` (`data_format 4cc`), `mp4a` placeholder (4 zero bytes), `esds`, `enda` (`littleEndian i16`), terminator (box with `size 8`, `type 0x00000000` — the walker treats a zero type inside `wave` as the terminator, not `CNT-0109`) | — | 1 |
| `chan` | CoreAudio `AudioChannelLayout` | FullBox; `mChannelLayoutTag u32`, `mChannelBitmap u32`, `mNumberChannelDescriptions u32`; `[ {mChannelLabel u32, mChannelFlags u32, mCoordinates f32[3]} ]` | layout tag name (`(100<<16)|1` Mono, `(101<<16)|2` Stereo, `(121<<16)|6` 5.1 MPEG…), label names | 1 |
| `load` | "Track Load Settings" | `preload_start_time i32`, `preload_duration i32`, `preload_flags u32`, `default_hints u32` | — | 4 |
| `clip`/`crgn`, `matt`/`kmat` | "Clipping"/"Matte" | `crgn`: `region_size u16`, `region_boundary_box i16[4]`, `clip_region_data bytes` | — | 4 |
| `glbl` | FFmpeg/QuickTime global codec header (used by some muxers for MPEG-4/VC-1 extradata) | `bytes` to end | — | 4 |
| `©xyz`, `keys`, `mdta` handling | see group G | — | — | 1 |

**Count:** groups A–M register approximately 118 hand decoders in phase 1 and 24 more in phase 4 (the exact number is reported by `tools/gen_box_registry.py --check`); every other registered fourcc (≈ 200) gets the generated shell (type, FullBox header, spec link, hex payload).

---

## 8.4 Derived views

All derived views are FlatBuffers tables in `core/schema/ovt_container.fbs` (schema text: chapter 06 §6.4) and are produced by free functions in `ovt::container::isobmff`. They are computed **once per track** on first request, cached in the `ovt_Container` object, and are deterministic (backbone §0.7.5).

### 8.4.1 Sample table expansion

Schema: `SampleTable`, `GroupMembership` — chapter 06 §6.4 (normative; not repeated here).

C++:

```cpp
namespace ovt::container::isobmff {
// Builds the SampleTable for one track from the moov tables (stbl) and, when the file is fragmented, appends every fragment
// of that track in file order (uses FragmentTimeline, §8.4.3). Throws nothing; all problems become diagnostics.
flatbuffers::DetachedBuffer build_sample_table(const ParsedMovie& movie, uint32_t track_id, const FragmentTimeline* fragments /* nullable */,
                                               ovt::jobs::Cancel& cancel /* chapter 07 §7.1.6 */);
}
```

**Algorithm (moov part):** all arithmetic in `uint64_t`/`int64_t`; every addition uses `ovt::checked_add` (wrapping `__builtin_add_overflow`), and an overflow emits `CNT-2030` (Error, "64-bit overflow in sample table at sample i") and stops the expansion at that sample (`count` is truncated).

1. Read `sample_count_z` from `stsz.sample_count` (or `stz2`). If > `kMaxSamples = 50'000'000` → `CNT-2031` (Error) and clamp to `kMaxSamples` (memory: ≈ 40 bytes per sample → 2 GB cap; a 24-hour 120 fps track is 10.4 M samples).
2. **Durations / DTS.** `dts[0] = 0`. For each `stts` entry `(n, delta)`: for `n` samples `duration[i] = delta`, `dts[i+1] = dts[i] + delta`. Let `sample_count_t = Σ n`. If `sample_count_t ≠ sample_count_z` → `CNT-2005` (Error, "stts covers N samples, stsz M"); `count = min(...)`. A `delta == 0` is allowed only for the last sample (→ `CNT-2001` Warning otherwise).
3. **Composition offsets.** If `ctts` present: expand runs `(n, offset)`; for v0 treat `offset` as `uint32_t` → if `≥ 2^31`, `CNT-2002` (Warning, "v0 ctts with value ≥ 2^31 interpreted as negative", matching libavformat/GPAC behaviour) and store `int32_t(offset)`; v1: signed. Run total ≠ `count` → `CNT-2011` (Warning); missing entries are 0. `cts[i] = dts[i] + cts_offset[i]`. **`cslg` is informative** (14496-12 §8.6.1.4: hints that let a player compute a DTS→CTS shift without scanning `ctts`); it never changes `cts[i]` and is never added to presentation times. Store `composition_to_dts_shift = cslg.compositionToDTSShift` when `cslg` is present, else the derived `max(0, −min(cts_offset))` (what libavformat computes); when `cslg` is present and `cslg.compositionToDTSShift < max_i(dts[i] − cts[i])` (i.e. `cts[i] + shift < dts[i]` for some i) → `CNT-2012` (Warning, "cslg compositionToDTSShift S smaller than the ctts-derived shift D"); `cslg.leastDecodeToDisplayDelta ≠ min(cts_offset)` or `greatestDecodeToDisplayDelta ≠ max(cts_offset)` → `CNT-2012` as well (same message family).
4. **Sizes.** `stsz.sample_size ≠ 0` → every `size[i] = sample_size`; else `size[i] = entry_size[i]`. `stz2`: unpack `field_size` bits. `size[i] == 0` → `CNT-2013` (Info, "zero-size sample").
5. **Chunk map.** `chunk_count = stco.entry_count` (or `co64`). For `stsc` entries `e_k = (first_chunk_k, spc_k, sdi_k)`: entry `k` applies to chunks `first_chunk_k .. first_chunk_{k+1} − 1` (last entry: to `chunk_count`). Require `first_chunk_1 == 1`, strictly increasing, `≤ chunk_count` → else `CNT-2003` (Error) and stop at the offending entry. For chunk `c` (1-based) with `spc` samples: `sample s` (0-based running counter) gets `chunk_index[s] = c`, `sample_description_index[s] = sdi`, and `offset[s] = chunk_offset[c] + Σ_{j in chunk before s} size[j]` (checked adds). If the running counter reaches `count` before chunks are exhausted → `CNT-2014` (Warning, "stsc/stco describe more samples than stsz"); if chunks are exhausted first → `CNT-2015` (Error, "N samples have no chunk"); their `offset = 0` and they are marked in `diagnostics`.
6. **Offsets vs file.** For every sample: `offset + size > file_size` → `CNT-2007` (Error, first 16 occurrences listed, then a count). Overlap check: sort sample spans across **all tracks** by offset (done once in `validate.cpp`, §8.5): two samples overlapping → `CNT-2016` (Error). Samples outside any `mdat` payload → `CNT-2017` (Warning; QuickTime reference movies and `dref` externals excepted when `url` flag ≠ 1).
7. **Sync.** `stss` present: `is_sync[i] = (i+1) ∈ stss`; entries out of range → `CNT-2009`. `stss` absent: all `true` (14496-12 §8.6.2: "If the sync sample box is not present, every sample is a sync sample"). `stps`: same into `is_partial_sync`. `sdtp` present: copy byte per sample (length check `CNT-2010`); if `sdtp.sample_depends_on == 2` (I-picture) but `!is_sync[i]` → Info `CNT-2018` (likely open-GOP / CRA).
8. **Sample groups.** For each `sbgp` in `stbl`: expand `(sample_count, group_description_index)` runs into `description_index[]`; look up the `sgpd` with the same `grouping_type` (and, when `sbgp.version == 1`, the same `grouping_type_parameter`); missing `sgpd` → `CNT-2021`. Store `GroupMembership`.
9. **Descriptor index check.** `sample_description_index[i] > stsd.entry_count` → `CNT-2004`.

**Fragment part:** after `moov` samples, for each fragment `f` of this track in `FragmentTimeline` (file order): append its samples with `dts` from the fragment timeline (`tfdt` or continuity), `offset` from the trun base rules, `is_sync = !sample_is_non_sync_sample`, `sdtp` packed from `sample_flags` bits `is_leading..sample_has_redundancy`, `fragment_index = f`, `chunk_index = 0`. Sample groups from the `traf`'s `sbgp`/`sgpd` (local indices `≥ 0x10001`).

**Formulas summary (normative):**

```
DTS[i]      = Σ_{j<i} duration[j]                    (moov)      | tfdt.baseMediaDecodeTime + Σ_{j<i within traf} duration[j]   (fragment)
CTS[i]      = DTS[i] + cts_offset[i]                 (track timescale; cslg is NOT added — it is an informative hint, 14496-12 §8.6.1.4)
PT[i]       = presentation_start(seg) + (CTS[i] − media_time(seg)) × movie_timescale / track_timescale    (14496-12 §8.6.6; seg = the elst
              segment containing CTS[i], §8.4.2; samples in no segment are not presented)
            = CTS[i]                                 when the track has no elst (implicit single segment with media_time = 0, §8.4.2 step 1)
offset[i]   = chunk_offset[chunk(i)] + Σ_{j in same chunk, j<i} size[j]          (moov)
            = base(traf) + trun.data_offset + Σ_{j<i within trun} size[j]       (fragment, §8.4.3)
seconds(t)  = t / timescale   (double; never integer division)
```

### 8.4.2 Edit list presentation timeline

Schema: `EditList`, `EditSegment` — chapter 06 §6.4.

```cpp
flatbuffers::DetachedBuffer build_edit_list(const ParsedMovie&, uint32_t track_id, const SampleTable&);
// Maps a sample's CTS (track timescale) to presentation time in movie timescale; returns false if the sample is not presented.
bool sample_presentation_time(const EditList&, const SampleTable&, uint64_t sample_index, uint32_t movie_timescale, int64_t* out_pts_movie);
```

**Algorithm:**

1. No `elst` → one implicit `normal` segment: `media_time = 0`, `segment_duration = tkhd.duration` (if 0 or all-ones, use `mdhd.duration` rescaled), `media_rate = 1`.
2. For each entry `k` in order: `presentation_start_k = Σ previous segment_duration`. Classify: `media_time == −1` → `empty` (nothing shown; adds delay); `media_rate_integer == 0` → `dwell` (freeze the frame at `media_time`); `media_rate ≠ 1` and `≠ 0` → `rate` (unsupported by most players; `CNT-3003` Info); else `normal`.
3. For `normal`/`dwell`: `first_sample` = index of the sample with the largest `CTS ≤ media_time` (plain CTS, no `cslg` shift; binary search over CTS sorted — note CTS is not sorted in decode order, so build `display_order` by sorting `(CTS, index)` once); `last_sample` = sample whose `CTS` is the largest `< media_time + segment_duration × track_timescale / movie_timescale`. If `media_time ≥ track media duration` → `CNT-3001` (Error, "edit starts beyond media duration"). If the segment extends beyond media end → `CNT-3002` (Warning, "edit exceeds media by D"). Sum of all `segment_duration` ≠ `tkhd.duration` → `CNT-3004` (Warning).
4. `implied_start_offset` = `media_time` of the first `normal` segment. `negative_cts_compensation = (implied_start_offset == CTS[0] − DTS[0] == cts_offset[0])`, i.e. the common pattern (MP4Box, ffmpeg) where `elst` removes the B-frame delay so that the first presented frame has `PT == 0`.
5. `sample_presentation_time`: find the segment containing the sample (`first_sample ≤ i ≤ last_sample`), `pts_movie = presentation_start + (CTS[i] − media_time) × movie_timescale / track_timescale` (use 128-bit intermediate via `__int128` or `ovt::muldiv64`); samples in no segment return `false`.

**`elst ↔ tfdt` sanity check** (fragmented tracks only, run in `validate.cpp`):

1. Let `M` = `implied_start_offset` (track timescale). Let `T0` = `tfdt.baseMediaDecodeTime` of the first fragment of the track, `C0` = `cts_offset` of its first sample.
2. CMAF (23000-19 §7.5.13) allows an `elst` **only** to express the presentation-time offset; the expected `M` is `T0 + C0` (first presented composition time) **or** `C0` alone when `T0 == 0`.
3. If `M ∉ {T0 + C0, C0}` and `|M − (T0 + C0)| > duration[0]` → `CNT-3010` (Warning, "elst media_time M does not match first fragment presentation time T0+C0"). If `T0 > 0` and no `elst` and `C0 > 0` → `CNT-3011` (Info, "presentation starts at T0+C0 ≠ 0; players will apply an offset").
4. If `elst` has more than one `normal` segment in a file with `moof` → `CNT-3012` (Warning, "multi-segment edit list in fragmented file; CMAF forbids").

### 8.4.3 Fragment timeline

Schema: `FragmentTimeline`, `FragmentInfo`, `TrafInfo`, `TrunInfo`, `SidxInfo`, `SidxRef` — chapter 06 §6.4.

```cpp
flatbuffers::DetachedBuffer build_fragment_timeline(const ParsedMovie&, uint32_t track_id, ovt::jobs::Cancel&);
```

**Algorithm:**

1. Collect top-level `moof` nodes in file order; expand each lazily (§8.2.4). For each `moof` find its `mdat`: the next top-level `mdat` after it (CMAF guarantees exactly one; if the next top-level box is not `mdat` → `CNT-4004` Warning).
2. For each `traf` with `tfhd.track_ID == track_id`:
   1. **Defaults:** `default_sample_duration/size/flags` = `tfhd` value if its flag is set, else the `trex` value for this track (missing `trex` and missing `tfhd` value → `CNT-4005` Error, use 0). `sample_description_index` likewise (default 1 when neither).
   2. **Base offset rule (14496-12 §8.8.7):** `base = tfhd.base_data_offset` if flag `0x1`; else if flag `0x020000` (default-base-is-moof) `base = moof_offset`; else (neither): for the **first** `traf` in the `moof`, `base = moof_offset`; for later `traf`s, `base = end of the previous traf's last sample data` (i.e. `prev_trun.first_sample_offset + prev_trun.size_sum` of the previous traf's last trun). Record `TrafInfo.base_data_offset = base`. `CNT-4006` (Info) when neither flag is set and more than one `traf` exists ("implicit base offsets; fragile").
   3. **Each `trun` in order:** `first_sample_offset = base + (data_offset present ? data_offset : (previous trun in this traf ? prev.first_sample_offset + prev.size_sum : 0))`. The spec requires `data_offset` for every trun except where the implicit "follows the previous" rule applies; if absent on the first trun of a traf without `base_data_offset` → `CNT-4007` (Warning). Per sample `s`: `duration = trun.sample_duration or default`, `size = trun.sample_size or default`, `flags = (s == 0 && first_sample_flags present) ? first_sample_flags : (trun.sample_flags or default)`, `cts_offset = trun value or 0`. `offset[s] = first_sample_offset + Σ sizes before s`.
   4. **DTS:** if `tfdt` present → `dts_start = baseMediaDecodeTime`; else `dts_start = expected_dts` (computed continuity; `CNT-4008` Info "no tfdt; DTS inferred"). Then `expected_dts_next = dts_start + Σ durations` across all truns of this traf.
   5. **Continuity check:** `dts_gap = tfdt − expected_dts` (expected from the previous fragment of this track; for the first fragment, expected = `moov` duration of the track if the file also has `stbl` samples, else no check). `dts_gap ≠ 0` → `CNT-4020` (Warning, "tfdt discontinuity: gap of G ticks (±G/ts s) before fragment k"); negative gap → "overlap".
   6. **Data bounds:** every sample offset must lie inside the paired `mdat` payload → `CNT-4021` (Error) otherwise; the last sample must end exactly at `mdat` end → else `CNT-4022` (Info, "N bytes of mdat not referenced by trun").
   7. **`senc`/`saio`:** resolve `saio` offsets: in `traf`, an offset is relative to the **same base** as trun data (`base_data_offset` / `moof_offset` rule) — 14496-12 §8.7.9 says relative to the `BaseDataOffset` of the traf, and when `default-base-is-moof` is set, to the `moof` start. Record `saio_resolved`. If `senc` exists, `saio_resolved[0]` should equal `senc` payload start + 4 (+ nothing: the `sample_count` is part of senc; aux data begins at the first IV) → else `CNT-6005` (Warning).
3. **`sidx` cross-check:** for each `sidx` whose `reference_ID == track_id`: compute `abs_offset` of each reference = `sidx.file_span.end + first_offset + Σ previous referenced_size`; find the `moof` starting at `abs_offset` (→ `sidx_reference_index` on the fragment). `referenced_size ≠ (moof.size + mdat.size)` (+ any `styp`/`prft`/`emsg` between them that the reference covers — i.e. the span to the next reference) → `CNT-4030` (Warning). `subsegment_duration ≠ Σ trun durations of the referenced moof(s)` (same timescale required; `sidx.timescale ≠ mdhd.timescale` → `CNT-4031` Info) → `CNT-4032` (Warning, with the delta). `earliest_presentation_time ≠ min CTS of the first referenced fragment` → `CNT-4033` (Info). `starts_with_SAP == 1` but first sample `sample_is_non_sync_sample == 1` → `CNT-4034` (Warning).
4. **`mfra` cross-check:** every `tfra` entry's `moof_offset` must equal a `moof` start, and `time` must equal the DTS (or CTS per spec: "presentation time") of sample `(traf_number, trun_number, sample_number)` → `CNT-4010`.
5. **`mfhd.sequence_number`** strictly increasing across fragments → else `CNT-4001`.

### 8.4.4 CENC per-sample IV / subsample map

Schema: `EncryptionInfo`, `SampleEncryption`, `SubsampleRange`, `PsshInfo` — chapter 06 §6.4 (`PsshInfo.kids` is `kid_count` × 16 bytes concatenated).

```cpp
// Builds per-sample encryption records for samples [first, first+count). Reads senc payloads (or saio/saiz-addressed aux data) on demand.
flatbuffers::DetachedBuffer build_encryption_map(const ParsedMovie&, const SampleTable&, const FragmentTimeline*, uint32_t track_id,
                                                 uint64_t first_sample, uint64_t count);
// For the hex view: returns [clear, protected, clear, ...] absolute byte ranges of a sample.
std::vector<ByteRange> sample_clear_protected_ranges(const SampleEncryption&, uint64_t sample_offset, uint32_t sample_size);
```

**Algorithm (per sample `i`):**

1. Determine the `seig` group: if a `GroupMembership{grouping_type:"seig"}` exists and `description_index[i] != 0`, take that `sgpd` entry (fragment-local indices resolve into the traf's `sgpd`); else take `tenc` defaults. From it: `is_protected`, `kid`, `per_sample_iv_size`, `constant_iv`, `crypt/skip_byte_block`.
2. If `!is_protected` → record with empty `iv`, no subsamples (`CNT-6006` Info when the whole track is unprotected but scheme boxes exist).
3. Locate the sample's auxiliary data: `moov` samples: `saio`/`saiz` in `stbl` (`saio` offsets absolute; `saiz` size per sample); fragment samples: `senc` in the traf (preferred, when present) or `saio_resolved`/`saiz` in the traf. The aux data for sample `i` starts at `saio[chunk or traf] + Σ saiz sizes of earlier samples in that run`.
4. Parse: `iv = per_sample_iv_size bytes` (if `per_sample_iv_size == 0` → `iv = constant_iv`); if subsample encryption (senc flag `0x2`, or `saiz` size > IV size): `subsample_count u16`, then pairs. Check `Σ(clear + protected) == size[i]` → else `CNT-6002` (Error).
5. For `cbcs`/`cens` pattern schemes: `crypt_byte_block:skip_byte_block` (e.g. `1:9`) applied inside each protected range in 16-byte blocks; `sample_clear_protected_ranges` expands the pattern so the hex view shades exactly the encrypted bytes. For NAL-structured video, 23001-7 §9 requires the NAL length field and NAL header (and the slice header bytes) to be in the clear — but only in the **first subsample of each NAL unit**; a NAL unit may span several subsamples and the continuation subsamples may have `BytesOfClearData == 0` (Shaka Packager / Widevine content does this). Check: walk the sample's length prefixes (`length_size` from `avcC`/`hvcC`) and, for every subsample whose start coincides with a NAL unit start, require `clear_bytes ≥ length_size + 1` → else `CNT-6007` (Warning, "subsample protects NAL length/header bytes of NAL n"); a subsample that starts inside a NAL unit is exempt. For `cbcs`/`cens` the pattern (`crypt_byte_block:skip_byte_block`) applies inside the protected range only, so clear 0 with a pattern is legal and not reported.
6. IV sizes (23001-7 §8.2, §9/§10): `cenc`/`cens` (CTR) use per-sample IVs of 8 or 16 bytes (`Per_Sample_IV_Size ∈ {8, 16}`); `cbc1` uses 16-byte per-sample IVs; `cbcs` uses a **16-byte constant IV** (`Per_Sample_IV_Size == 0`, `default_constant_IV_size == 16`, the IV copied into every record's `iv`). `cbcs` with `per_sample_iv_size ≠ 0` → `CNT-6008` (Info, "cbcs with per-sample IVs; constant IV expected by CMAF"); any other size → `CNT-6003`.

### 8.4.5 Brand and profile checks

Brand table `kBrands[]` is generated (MP4RA brands CSV) with a hand-maintained `kBrandChecks[]` in `brands.cpp` mapping a brand to a list of check functions. `ftyp.major_brand` and `compatible_brands` of the file (and of every `styp` for segments) are evaluated; each failing check emits a `CNT-7xxx` diagnostic on the `ftyp`/`styp` node.

| Brand(s) | Spec | Checks performed (each → diagnostic code) |
|---|---|---|
| `isom`, `iso2`…`iso9`, `isob` | 14496-12 Annex E | `iso2`+: `moov` before any `moof`; `iso5`+ (14496-12:2008 Amd. 1): `tfdt` and `default-base-is-moof` allowed, so `CNT-7001` Warning if `default-base-is-moof` or `tfdt` is used without `iso5`/`iso6`+ or `cmfc`/`dash`/`msdh` in the compatible brands; `iso6`+: `sidx`/`ssix`/`styp`/`prft` allowed |
| `mp41`, `mp42` | 14496-14 | `mp42`: `esds` for `mp4a`/`mp4v` present → `CNT-7002` when missing |
| `qt  ` | QTFF | `nclc` colr allowed; `wave` allowed; else emit `CNT-5002` for `nclc` in non-`qt` files |
| `avc1` | 14496-15 §4.3 | at least one `avc1`/`avc2`/`avc3`/`avc4` sample entry; `avc1`/`avc2` MUST have `numOfSequenceParameterSets ≥ 1` (`CNT-7003`); `avc3`/`avc4` MAY carry in-band PS; samples of `avc1` MUST NOT contain SPS/PPS NALs (checked on first 8 sync samples by the unit splitter → `CNT-7004` Warning) |
| `hvc1`/`hev1` entries (no brand) | 14496-15 §8.4 | `hvc1`: arrays MUST contain VPS/SPS/PPS (`CNT-7005`); `hev1`: PS MAY be in-band; `array_completeness == 1` required for `hvc1` (`CNT-7006` Info) |
| `av01` entries | AV1-ISOBMFF §2.2 | `av1C.configOBUs` MUST contain exactly one `OBU_SEQUENCE_HEADER` (`CNT-7007`); a sample SHOULD NOT contain an `OBU_TEMPORAL_DELIMITER` (AV1-ISOBMFF §2.4 "SHOULD be omitted") → `CNT-7008` Warning; `obu_has_size_field == 1` for every OBU except possibly the last (`CNT-7009` Error); sync samples MUST start with a key frame (checked via syntax module) |
| `cmfc` | 23000-19 §7 (CMAF track) | exactly one `trak` (`CNT-7010`); `mvex` with `trex` present (`CNT-7011`); `mvhd.duration == 0` or `mehd` (Info `CNT-7012`); `stbl` tables empty (`stts/stsc/stsz/stco entry_count == 0`, `CNT-7013`); every `moof` has exactly one `traf` (`CNT-7014`); `tfhd` has `default-base-is-moof` (`CNT-7015`); `tfdt` present in every traf (`CNT-7016`); `trun` sample durations/flags present or defaulted; `elst` only of the single-normal-segment form (§8.4.2 step 4, `CNT-3012`); `sdtp`/`stss` absent (`CNT-7017` Info); video sample entry has `colr` (`CNT-7018` Warning — CMAF §7.5.3 requires colour signalling) ; `pasp` present when PAR ≠ 1:1; `btrt` recommended (`CNT-7019` Info) |
| `cmf2` | 23000-19 §7.7 (CMAF 2nd structural brand) | all `cmfc` checks plus: negative composition offsets (`trun` version 1) used instead of `elst` (`elst` with `media_time > 0` → `CNT-7020`); `cslg` absent (CMAF tracks signal composition offsets per fragment; `CNT-7021` Info when present); `sample_description_index` constant; AVC/HEVC sample entries `avc1`/`hvc1` with PS in sample entry (`CNT-7022` Info when `avc3`/`hev1`) |
| `cmfs`, `cmff`, `cmfl` | 23000-19 §7.3.x (segment, fragment, chunk) | `styp` MUST be present at the start of a CMAF segment with `cmfs` (`CNT-7023`); `cmfl` chunks: ≤ 1 `moof`; `sidx` only at segment start (`CNT-7024`); `emsg`/`prft` only before `moof` (`CNT-7025`) |
| `dash` | 23009-1 §6 ("self-initialising") | `moov` contains `mvex` (`CNT-7030` when missing) and the `stbl` tables are empty (`CNT-7030` also when `stsz.sample_count > 0`, because `dash` means a self-initialising segment whose samples all live in `moof`s) |
| `msdh`, `msix` | 23009-1 §6.3.4 | `msdh`: each media segment has ≥ 1 `moof`; `msix`: `sidx` MUST be present and index every subsegment (`CNT-7031` when no `sidx`, `CNT-7032` when a `moof` is unreferenced by `sidx`) |
| `sims`, `lmsg`, `risx`, `ssss`, `dsms`, `dims` | 23009-1 | informational description only; `lmsg` on a non-last segment → `CNT-7033` Info |
| `iso6`/`iso5` with HLS use | Apple HLS Authoring Specification §... (fMP4) | `tfdt` MUST be present (`CNT-7040`); `mdat` single per `moof`; init segment without samples; `sidx` not required; `emsg` ID3 scheme recognised; `hvc1`/`avc1` (PS in `stsd`) required by Apple (`CNT-7041` Warning when `hev1`/`avc3` with `cmfc` and HLS context detected via playlist open) |
| `mif1`, `miaf`, `heic`, `heix`, `hevc`, `avif`, `avis`, `MA1B`, `MA1A` | 23008-12 §10 / MIAF 23000-22 §7 / AVIF | `mif1`: `meta` at root with `hdlr == 'pict'`, `pitm`, `iinf`, `iloc`, `iprp` (`CNT-8010` each); `miaf`: every image item has `ispe` (`CNT-8011`), the primary item is a coded image or derived image with `pixi` (`CNT-8012`), `clap`/`irot`/`imir` order (`clap` before `irot` before `imir`, `CNT-8013`), `colr` present (`CNT-8014` Info); `heic`: primary item `hvc1`; `avif`: primary item `av01` with `av1C`; `avis`: `trak` with `av01` and `pict` handler |
| `M4A `, `M4V `, `M4P `, `M4B ` | Apple iTunes | iTunes `ilst` expected (`CNT-7050` Info when missing) |
| `3gp4`…`3gp9`, `3g2a` | 3GPP TS 26.244 | `udta` 3GPP asset boxes allowed; `amr`/`sawb` sample entries |
| `piff` | PIFF 1.1 | uuid encryption boxes expected; CENC `senc`/`tenc` also present → `CNT-6011` Info "both PIFF and CENC signalling" |
| `mp71` | MPEG-7 | none |

Implementation: `struct BrandCheck { const char* brand; void (*check)(const ParsedMovie&, DiagnosticSink&); }`, iterated for the major brand and every compatible brand (deduplicated). Checks that need sample bytes (first N samples) run in a job and are skipped on files > 1 GiB unless the user requests deep validation (`ovt probe --validate --validate-level deep`, chapter 12 §12.2.2).

### 8.4.6 `HdrMetadata` summary

Schema: `HdrMetadata`, `HdrSignal`, `HdrKind` — chapter 06 §6.4. `HdrMetadata.color` is the 44-byte (§0.5.6 layout with the leading `uint32_t struct_size`, D4) little-endian image of `ovt_ColorDescription` (§0.5.6).

```cpp
namespace ovt::container::isobmff {
struct HdrProbeOptions { uint32_t max_sync_samples = 8; uint32_t max_bytes = 16u << 20; bool allow_slow_scan = false; };
// Box-level pass is synchronous; the bitstream pass reads up to max_sync_samples sync samples through the unit splitter and the syntax module.
flatbuffers::DetachedBuffer build_hdr_metadata(const ParsedMovie&, uint32_t track_id, const SampleTable&, ovt::io::RandomAccessFile&,
                                               const HdrProbeOptions&, ovt::jobs::Cancel&);
}
```

**Algorithm:**

1. **Box level.** From the first visual sample entry of the track: `colr` (nclx → `primaries/transfer/matrix/full_range`; `nclc` → same without range; ICC → `kind` stays Unknown unless nclx also exists), `mdcv` → `mdcv_*` fields, `clli` → `max_cll/max_fall`, `dvcC`/`dvvC`/`dvwC` → `dv_*`, sample entry fourcc `dvh1`/`dvhe`/`dva1`/`dvav`/`dav1` → `has_dolby_vision`, `av1C` (`high_bitdepth`, `twelve_bit`, `monochrome`) → `bit_depth`, `chroma_format`, `vpcC` → full colour triple + `bitDepth`. `hvcC`/`avcC` SPS VUI (parsed by the syntax module) → `sps_vui` signal with `colour_primaries/transfer_characteristics/matrix_coeffs/video_full_range_flag/chroma_loc`.
2. **Bitstream level** (skipped when `probed_samples` would exceed `max_bytes`): iterate sync samples in decode order up to `max_sync_samples`; split into units (§8.7.4); for each unit ask `ovt::syntax` (chapter 7) for: HEVC/AVC SEI payload types `137` (mastering_display_colour_volume, H.265 D.2.28), `144` (content_light_level_info, D.2.35), `4` (user_data_registered_itu_t_t35) with `itu_t_t35_country_code == 0xB5`, `terminal_provider_code == 0x003C`, `terminal_provider_oriented_code == 0x0001`, `application_identifier == 4` → HDR10+ (ST 2094-40; `application_version`, `num_windows` extracted), or provider `0x003B` (Dolby) / Dolby ST 2094-10 T.35; HEVC NAL `nal_unit_type == 62` (UNSPEC62 = Dolby Vision RPU) or `63` (EL); for AVC-based Dolby Vision (profiles 9/`dvav`) the RPU is carried in NAL `nal_unit_type == 28` (per the Dolby AVC carriage spec; treat NAL 28 in a `dvav`/`dva1` track as `nal_28_rpu`); AV1 `OBU_METADATA` with `metadata_type` `1` (HDR_CLL), `2` (HDR_MDCV), `4` (ITUT_T35 → HDR10+ check as above), `5` (timecode) ; AV1 sequence header `color_config` → primaries/transfer/matrix/range. Record an `HdrSignal` per finding with the first `sample_index`/`unit_index`.
3. **Classification** (first rule that matches):
   1. `dv_*` from box present **or** `nal_62_rpu` found → `DolbyVision`; `dv_profile` from box; if RPU found but no `dvcC/dvvC` → `CNT-5010` (Warning, "Dolby Vision RPU in bitstream but no DV configuration box"); if box present but no RPU in probed samples → `CNT-5011` (Warning, "dvcC/dvvC present but no RPU NAL found in N sync samples") — note profile 8.x RPUs are in every frame, so this check is reliable.
   2. `transfer == 16 (PQ)` and HDR10+ T.35 found → `HDR10Plus`.
   3. `transfer == 16` and (`mdcv` or `sei_137`) present → `HDR10`.
   4. `transfer == 16` → `PQ_NoStatic` (+ `CNT-5012` Info "PQ without static metadata").
   5. `transfer == 18 (HLG)` → `HLG`.
   6. `transfer ∈ {1, 6, 13, 14, 15}` or primaries `1/5/6` → `SDR`.
   7. else `Unknown`.
4. **Mismatch rules** (`box_vs_bitstream_mismatch = true` and one diagnostic each): box `colr` triple ≠ `sps_vui`/`color_config` triple (`CNT-5001` Warning, lists both; the **bitstream** value is what decoders use — libavformat prefers `colr` though, so the GUI shows both); `mdcv` values ≠ `sei_137` values (`CNT-5003` Info); `clli` ≠ `sei_144` (`CNT-5004` Info); `av1C.high_bitdepth` ≠ sequence header `high_bitdepth` (`CNT-5005` Error); `hvcC.general_profile_idc`/`bitDepthLumaMinus8` ≠ SPS (`CNT-5006` Error); `dvcC.dv_bl_signal_compatibility_id == 1` (HDR10) but `transfer ≠ 16` (`CNT-5021` Warning).
5. `color` is the `ovt_ColorDescription` consumed by render and metrics (backbone §0.5.6): box values win for `primaries/transfer/matrix/full_range` when `colr` exists (matches libavformat/Apple behaviour), bitstream values otherwise; `has_hdr10plus`/`has_dolby_vision`/`dv_profile`/`dv_level` from the classification.

---

## 8.5 Validation rules (diagnostic codes `CNT-xxxx`)

All codes are stable identifiers (backbone §0.7.6). Format of the ISOBMFF family: `CNT-` + 4 digits; the first digit groups: `0` structure/walk, `1` box-level, `2` sample tables, `3` edit lists, `4` fragments/index, `5` colour/HDR, `6` encryption, `7` brands/profiles, `8` HEIF/items, `9` demux matching. Severity per backbone `Severity {Info, Warning, Error, Fatal}`; `Fatal` is used only when no tree can be produced at all (`ovt_container_open` returns `OVT_E_CORRUPT`). Codes referenced in §8.2–§8.4, §8.6 and §8.7 are listed here with severity, message template, the algorithm that raises them and the spec reference (registry row format of chapter 05 §5.7; code format `CNT(-<QUALIFIER>)?-<nnnn>` — bare `CNT-nnnn` is ISOBMFF/QuickTime, qualified families `CNT-IVF-`, `CNT-ESB-` (Annex B elementary stream), `CNT-OBU-`, `CNT-Y4M-`, `CNT-RAW-`, `CNT-EBML-`, `CNT-TS-`, `CNT-DASH-`). This table is the **only** registry of `CNT-*` codes in the specification; a code appears exactly once in `core/src/container/diag_codes.inc` (five columns: `X(CNT_0101, "CNT-0101", Warning, "trailing bytes ...", "§8.2.3")`, the last being the spec reference of this table, `""` when none), `docs/spec/diagnostic-codes.md` is generated from it, and `ovt probe --list-diagnostics` (chapter 12 §12.2.2) prints the table.

| Code | Severity | Message template (parameters in backticks are substituted; the text is the `Diagnostic.message`) | When raised | Spec reference |
|---|---|---|---|---|
| CNT-0001 | Info | unit source policy for this stream (own reader vs libavformat) | §8.2.3 box walk / §8.2.4 lazy expansion | — |
| CNT-0002 | Warning | first box is not `ftyp`/`styp` (QuickTime without `ftyp` is legal) | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.3 |
| CNT-0003 | Info | `moov` after `mdat` (not progressive/fast-start) | §8.2.3 box walk / §8.2.4 lazy expansion | — |
| CNT-0004 | Error | no `moov` and no `moof` | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §8.2.1 |
| CNT-0005 | Info | fragment-only stream without `moov` (needs init segment) | §8.2.3 box walk / §8.2.4 lazy expansion | 23000-19 |
| CNT-0006 | Fatal | file shorter than 8 bytes or first 8 bytes not a plausible box header after sniffing as ISOBMFF | §8.2.3 box walk / §8.2.4 lazy expansion | — |
| CNT-0007 | Info | box at offset `o` not yet expanded (lazy; call `ovt_container_expand`) — carried on the unexpanded `BoxNode.diagnostics` only, never counted as a validation finding | §8.2.4 lazy traversal, `ovt_container_tree` | — |
| CNT-0040 | Warning | track `track_ID` is encrypted (`scheme_type`); slice/OBU payloads are not parsed and the stream is reported as not decodable | §8.1.3 `TrackInfo`, §8.4.4 | 23001-7 §4 |
| CNT-0101 | Warning | trailing bytes in a container that cannot hold a box header | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-0102 | Error | truncated `largesize` | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-0103 | Error | `largesize < 16` | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-0104 | Warning | `size == 0` box not at top level | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2.2 |
| CNT-0105 | Error | nesting deeper than 32 | §8.2.3 box walk / §8.2.4 lazy expansion | — |
| CNT-0106 | Error | `size < 8` (negative / tiny size) | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-0107 | Error | `uuid` box too short for usertype | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-0108 | Error | box exceeds parent/file end (truncated) | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-0109 | Warning | non-printable box type | §8.2.3 box walk / §8.2.4 lazy expansion | — |
| CNT-0110 | Error | FullBox shorter than 4 bytes | §8.2.3 box walk / §8.2.4 lazy expansion | 14496-12 §4.2 |
| CNT-1001 | Info | unknown fourcc (not in MP4RA/conformance/extra registry) | §8.3 hand decoders, §8.5 validation driver (Standard level) | — |
| CNT-1002 | Error | decoder hit end of box while reading field (truncated payload) | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1003 | Warning | unknown FullBox version | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1004 | Info | reserved flag bits set | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1005 | Info | unparsed trailing bytes after the last field | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1006 | Info | QuickTime-style `meta` without version/flags | §8.3 hand decoders, §8.5 validation driver (Standard level) | QTFF |
| CNT-1007 | Info | reserved field has unexpected value | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1010 | Error | `stsd.entry_count` ≠ number of sample entries found | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.5.2 |
| CNT-1011 | Error | required box missing: `mvhd` in `moov`; `tkhd`, `mdia` in `trak`; `mdhd`, `hdlr`, `minf` in `mdia`; `stbl`, `dinf` in `minf`; `stsd`, `stts`, `stsc`, `stsz|stz2`, `stco|co64` in `stbl` (unless `cmfc`/fragment-only with empty stbl, where `stts/stsc/stsz/stco` must still exist with 0 entries); `mfhd` in `moof`; `tfhd` in `traf`; message names the box and parent | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.x "Mandatory: Yes" |
| CNT-1012 | Error | duplicate box where exactly one is allowed (`mvhd`, `tkhd`, `mdhd`, `hdlr`, `stsd`, `stts`, `stsz`, `stco`/`co64`, `tfhd`, `tfdt`, `mehd`, `ftyp`) or duplicate `track_ID` | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.x "Quantity: Exactly one" |
| CNT-1013 | Warning | box in a parent where the registry does not allow it (`allowed_parents` non-empty and mismatch); e.g. `colr` in `stbl`, `elst` outside `edts`, `stss` in `traf` | §8.3 hand decoders, §8.5 validation driver (Standard level) | conformance JSON |
| CNT-1014 | Error | box size does not match its fields: payload ends before the fields' declared count (`entry_count` × entry size > payload) | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1015 | Warning | box size larger than fields + known children by more than 0 bytes **and** box is one of the fixed-size boxes (`tkhd`, `mdhd`, `mvhd`, `vmhd`, `smhd`, `btrt`, `pasp`, `clli`, `mdcv`, `tfdt`, `mfhd`, `mehd`, `trex`) | §8.3 hand decoders, §8.5 validation driver (Standard level) | per box |
| CNT-1020 | Info | `avc3`/`avc4` sample entry carries SPS/PPS in `avcC` (allowed, but in-band is the point) | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-15 §5.4 |
| CNT-1021 | Error | `av1C.configOBUs` contains an OBU other than sequence header / metadata | §8.3 hand decoders, §8.5 validation driver (Standard level) | AV1-ISOBMFF §2.3.4 |
| CNT-1022 | Warning | `vpcC.codecIntializationDataSize ≠ 0` for VP8/VP9 | §8.3 hand decoders, §8.5 validation driver (Standard level) | VP-ISOBMFF §2.3 |
| CNT-1023 | Warning | `clap` yields non-integral or out-of-range aperture | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §12.1.4 |
| CNT-1024 | Info | `tkhd.matrix` is neither identity nor a 90°/180°/270° rotation; `TrackInfo.rotation = 0` | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.3.2 |
| CNT-1030 | Info | deprecated / legacy box present: `stsh`; PIFF uuid boxes (also `CNT-6010`); QuickTime `nclc` in ISO-branded file (`CNT-5002`); `mp4v`+`esds` with AVC OTI (`0x21`) instead of `avc1`; `url ` data references to external files in a self-contained file; `hint` tracks; `tref/hint` | §8.3 hand decoders, §8.5 validation driver (Standard level) | various |
| CNT-1031 | Warning | `hdlr.handler_type` unknown (not in `kHandlers`) | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.4.3 |
| CNT-1032 | Warning | `mdhd.timescale == 0` or `mvhd.timescale == 0` | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.4.2 |
| CNT-1033 | Warning | `tkhd.duration` (rescaled) differs from `mdhd.duration` by more than one sample duration and no `elst` explains it | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.3.2 |
| CNT-1034 | Info | `tkhd.width/height` ≠ `VisualSampleEntry.width/height` after `pasp`/`clap` (anamorphic or scaled presentation) | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §12.1.3 |
| CNT-1035 | Warning | `tkhd` not `track_in_movie` for a video/audio track | §8.3 hand decoders, §8.5 validation driver (Standard level) | 14496-12 §8.3.2 |
| CNT-2001 | Warning | `stts.sample_delta == 0` for a non-last sample | §8.4.1 sample table expansion | 14496-12 §8.6.1.2 |
| CNT-2002 | Warning | `ctts` v0 offset ≥ 2^31 treated as negative | §8.4.1 sample table expansion | 14496-12 §8.6.1.3 |
| CNT-2003 | Error | `stsc.first_chunk` not starting at 1 / not increasing / > chunk count | §8.4.1 sample table expansion | 14496-12 §8.7.4 |
| CNT-2004 | Error | `sample_description_index` 0 or > `stsd.entry_count` | §8.4.1 sample table expansion | 14496-12 §8.7.4 |
| CNT-2005 | Error | sample count mismatch `stts` vs `stsz`/`stz2` | §8.4.1 sample table expansion | 14496-12 §8.6.1.2 |
| CNT-2006 | Error | `stz2.field_size ∉ {4,8,16}` | §8.4.1 sample table expansion | 14496-12 §8.7.3.3 |
| CNT-2007 | Error | chunk/sample byte range beyond end of file | §8.4.1 sample table expansion | 14496-12 §8.7.5 |
| CNT-2008 | Error | both `stco` and `co64` present | §8.4.1 sample table expansion | 14496-12 §8.7.5 |
| CNT-2009 | Error | `stss` sample number out of range or not increasing | §8.4.1 sample table expansion | 14496-12 §8.6.2 |
| CNT-2010 | Error | `sdtp` length ≠ sample count | §8.4.1 sample table expansion | 14496-12 §8.6.4 |
| CNT-2011 | Warning | `ctts` run total ≠ sample count | §8.4.1 sample table expansion | 14496-12 §8.6.1.3 |
| CNT-2012 | Warning | `cslg` hints inconsistent with `ctts`: compositionToDTSShift `S` < derived shift `D`, or least/greatest delta ≠ min/max `cts_offset` (informative box; CTS unchanged) | §8.4.1 sample table expansion | 14496-12 §8.6.1.4 |
| CNT-2013 | Info | zero-size sample | §8.4.1 sample table expansion | — |
| CNT-2014 | Warning | `stsc`/`stco` describe more samples than `stsz` | §8.4.1 sample table expansion | 14496-12 §8.7.4 |
| CNT-2015 | Error | samples without a chunk | §8.4.1 sample table expansion | 14496-12 §8.7.4 |
| CNT-2016 | Error | sample byte ranges overlap (within or across tracks) | §8.4.1 sample table expansion | — |
| CNT-2017 | Warning | sample outside any `mdat` payload | §8.4.1 sample table expansion | 14496-12 §8.1.1 |
| CNT-2018 | Info | `sdtp` says I-picture but not in `stss` (open GOP) | §8.4.1 sample table expansion | 14496-12 §8.6.4 |
| CNT-2019 | Warning | `stss` present but lists every sample (should be omitted) | §8.4.1 sample table expansion | 14496-12 §8.6.2 |
| CNT-2020 | Error | `sgpd` v0 with unknown grouping type (entry length unknown) | §8.4.1 sample table expansion | 14496-12 §8.9.3 |
| CNT-2021 | Error | `sbgp` index without matching `sgpd` entry | §8.4.1 sample table expansion | 14496-12 §8.9.2 |
| CNT-2022 | Warning | `sbgp` sample total ≠ sample count | §8.4.1 sample table expansion | 14496-12 §8.9.2 |
| CNT-2030 | Error | 64-bit overflow while summing durations/offsets | §8.4.1 sample table expansion | — |
| CNT-2031 | Error | sample count > 50 000 000, table truncated | §8.4.1 sample table expansion | — |
| CNT-2032 | Warning | DTS not strictly increasing between consecutive samples (`delta 0`) in a video track | §8.4.1 sample table expansion | 14496-12 §8.6.1.2 |
| CNT-2033 | Warning | first presented CTS (`min CTS`) ≠ 0 and no `elst` (presentation starts late; players apply an implicit offset) | §8.4.1 sample table expansion | 14496-12 §8.6.6 |
| CNT-3001 | Error | edit starts beyond media duration | §8.4.2 edit list | 14496-12 §8.6.6 |
| CNT-3002 | Warning | edit exceeds media duration | §8.4.2 edit list | 14496-12 §8.6.6 |
| CNT-3003 | Info | `media_rate ∉ {0,1}` (unsupported by most players) | §8.4.2 edit list | 14496-12 §8.6.6 |
| CNT-3004 | Warning | Σ `segment_duration` ≠ `tkhd.duration` | §8.4.2 edit list | 14496-12 §8.6.6 |
| CNT-3010 | Warning | `elst.media_time` inconsistent with first fragment `tfdt + cts_offset` | §8.4.2 edit list | 23000-19 §7.5.13 |
| CNT-3011 | Info | presentation offset implied without `elst` | §8.4.2 edit list | — |
| CNT-3012 | Warning | multi-segment `elst` in fragmented/CMAF file | §8.4.2 edit list | 23000-19 §7.5.13 |
| CNT-4001 | Warning | `mfhd.sequence_number` not strictly increasing | §8.4.3 fragment timeline | 14496-12 §8.8.5 |
| CNT-4002 | Warning | `tfhd` has both `base-data-offset-present` and `default-base-is-moof` | §8.4.3 fragment timeline | 14496-12 §8.8.7 |
| CNT-4003 | Warning | `trun` has both `first-sample-flags-present` and `sample-flags-present` | §8.4.3 fragment timeline | 14496-12 §8.8.8 |
| CNT-4004 | Warning | `moof` not immediately followed by `mdat` | §8.4.3 fragment timeline | 23000-19 §7.3.2.3 |
| CNT-4005 | Error | no default sample duration/size/flags (neither `tfhd` nor `trex`) while `trun` omits them | §8.4.3 fragment timeline | 14496-12 §8.8.7 |
| CNT-4006 | Info | implicit base data offsets with several `traf`s | §8.4.3 fragment timeline | 14496-12 §8.8.7 |
| CNT-4007 | Warning | first `trun` of a `traf` without `data_offset` and no `base_data_offset` | §8.4.3 fragment timeline | 14496-12 §8.8.8 |
| CNT-4008 | Info | `tfdt` absent; DTS inferred by continuity | §8.4.3 fragment timeline | 14496-12 §8.8.12 |
| CNT-4010 | Error | `tfra` entry does not point at a `moof` / wrong time | §8.4.3 fragment timeline | 14496-12 §8.8.10 |
| CNT-4011 | Error | `mfro.size` ≠ `mfra` size | §8.4.3 fragment timeline | 14496-12 §8.8.11 |
| CNT-4020 | Warning | `tfdt` discontinuity (gap or overlap vs previous fragment) | §8.4.3 fragment timeline | 14496-12 §8.8.12 |
| CNT-4021 | Error | fragment sample outside its `mdat` | §8.4.3 fragment timeline | 14496-12 §8.8.8 |
| CNT-4022 | Info | bytes in `mdat` not referenced by any `trun` | §8.4.3 fragment timeline | — |
| CNT-4030 | Warning | `sidx.referenced_size` ≠ actual bytes | §8.4.3 fragment timeline | 14496-12 §8.16.3 |
| CNT-4031 | Info | `sidx.timescale` ≠ track timescale | §8.4.3 fragment timeline | 14496-12 §8.16.3 |
| CNT-4032 | Warning | `sidx.subsegment_duration` ≠ Σ `trun` durations | §8.4.3 fragment timeline | 14496-12 §8.16.3 |
| CNT-4033 | Info | `sidx.earliest_presentation_time` ≠ first CTS | §8.4.3 fragment timeline | 14496-12 §8.16.3 |
| CNT-4034 | Warning | `starts_with_SAP` but first sample is non-sync | §8.4.3 fragment timeline | 14496-12 §8.16.3 |
| CNT-4035 | Warning | fragment `trun` sample count 0 and not `duration-is-empty` | §8.4.3 fragment timeline | 14496-12 §8.8.8 |
| CNT-4036 | Info | `styp` brands differ between segments of one file | §8.4.3 fragment timeline | 14496-12 §8.16.2 |
| CNT-5001 | Warning | `colr` nclx differs from SPS VUI / AV1 `color_config` | §8.4.6 HdrMetadata | 14496-12 §12.1.5 |
| CNT-5002 | Warning | QuickTime `nclc` colr in non-`qt` file | §8.4.6 HdrMetadata | QTFF |
| CNT-5003 | Info | `mdcv` differs from SEI 137 | §8.4.6 HdrMetadata | ST 2086 |
| CNT-5004 | Info | `clli` differs from SEI 144 | §8.4.6 HdrMetadata | CTA-861.3 |
| CNT-5005 | Error | `av1C` bit depth/chroma ≠ sequence header | §8.4.6 HdrMetadata | AV1-ISOBMFF §2.3 |
| CNT-5006 | Error | `hvcC`/`avcC` profile/bit depth ≠ SPS | §8.4.6 HdrMetadata | 14496-15 |
| CNT-5007 | Warning | `colr` absent on a video track (SDR assumed) | §8.4.6 HdrMetadata | 23000-19 §7.5.3 |
| CNT-5008 | Warning | `transfer_characteristics == 2` (unspecified) with 10-bit BT.2020 content | §8.4.6 HdrMetadata | — |
| CNT-5010 | Warning | DV RPU in bitstream, no `dvcC`/`dvvC` | §8.4.6 HdrMetadata | DV-ISOBMFF |
| CNT-5011 | Warning | `dvcC`/`dvvC` present, no RPU found in probed samples | §8.4.6 HdrMetadata | DV-ISOBMFF |
| CNT-5012 | Info | PQ without static metadata | §8.4.6 HdrMetadata | — |
| CNT-5020 | Warning | `dvcC` used for profile ≥ 8 or `dvvC` for ≤ 7 | §8.4.6 HdrMetadata | DV-ISOBMFF §2.2 |
| CNT-5021 | Warning | `dv_bl_signal_compatibility_id` contradicts `colr` | §8.4.6 HdrMetadata | DV-ISOBMFF §2.2 |
| CNT-5022 | Info | sample entry `dvh1`/`dvhe` (non-cross-compatible) vs `hvc1`+`dvvC` | §8.4.6 HdrMetadata | DV-ISOBMFF §3 |
| CNT-6001 | Error | `senc` IV size unknown and not inferable | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §7.2 |
| CNT-6002 | Error | subsample clear+protected ≠ sample size | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §7.2 |
| CNT-6003 | Error | `tenc` IV size ∉ {0,8,16} | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §8.2 |
| CNT-6004 | Error | `pssh.DataSize` inconsistent with box size | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §8.1 |
| CNT-6005 | Warning | `saio` does not point at `senc` sample data | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §7.1 |
| CNT-6006 | Info | encrypted track with all samples unprotected | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 |
| CNT-6007 | Warning | subsample protects NAL length/header bytes | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §9 |
| CNT-6008 | Info | `cbcs` with per-sample IV | §8.4.4 CENC map, §8.3.8 decoders | 23000-19 §8.2.2 |
| CNT-6009 | Warning | `pssh` with unknown SystemID | §8.4.4 CENC map, §8.3.8 decoders | — |
| CNT-6010 | Info | PIFF (pre-CENC) encryption boxes | §8.4.4 CENC map, §8.3.8 decoders | PIFF 1.1 |
| CNT-6011 | Info | both PIFF and CENC signalling | §8.4.4 CENC map, §8.3.8 decoders | — |
| CNT-6012 | Warning | encrypted sample entry (`encv`/`enca`) without `sinf`/`frma`/`schm` | §8.4.4 CENC map, §8.3.8 decoders | 14496-12 §8.12 |
| CNT-6013 | Warning | `tenc.default_KID` all zero while `default_isProtected == 1` | §8.4.4 CENC map, §8.3.8 decoders | 23001-7 §8.2 |
| CNT-8001 | Error | `iinf.entry_count` ≠ number of `infe` | §8.3.6 item decoders, §8.4.5 MIAF checks, WI-4.213 | 14496-12 §8.11.6 |
| CNT-8002 | Error | `iloc` size field ∉ {0,4,8} | §8.3.6 item decoders, §8.4.5 MIAF checks, WI-4.213 | 14496-12 §8.11.3 |
| CNT-8003 | Error | `iloc` extent outside file/`idat` | §8.3.6 item decoders, §8.4.5 MIAF checks, WI-4.213 | 14496-12 §8.11.3 |
| CNT-8004 | Error | `ipma` property index out of range | §8.3.6 item decoders, §8.4.5 MIAF checks, WI-4.213 | 14496-12 §8.11.14.3 |
| CNT-8005 | Error | `iref`/`pitm` references non-existent item | §8.3.6 item decoders, §8.4.5 MIAF checks, WI-4.213 | 14496-12 §8.11.12 |
| CNT-9001 | Error | sample bytes do not split into units cleanly (NAL length exceeds sample, OBU size overrun) | §8.7.4 unit splitting | 14496-15 §5.3.4.2 / AV1-ISOBMFF §2.4 |
| CNT-9002 | Warning | Annex B start codes found inside an `avc1`/`hvc1` sample (muxer bug) | §8.7.4 unit splitting | 14496-15 §4.3 |
| CNT-9003 | Warning | AV1 `OBU_TEMPORAL_DELIMITER` inside a sample | §8.7.4 unit splitting | AV1-ISOBMFF §2.4 |
| CNT-0901 | Warning | libavformat stream ↔ track mismatch (§8.1.4): `item` differs (codec type / extradata / frame count); never raised for `vpcC` | §8.1.4 track ↔ stream matching, §8.7.3 | — |
| CNT-0902 | Info | track matched by order fallback | §8.1.4 track ↔ stream matching, §8.7.3 | — |
| CNT-0903 | Info | libavformat stream with no container track | §8.1.4 track ↔ stream matching, §8.7.3 | — |
| CNT-0904 | Info | packets of a stream could not be mapped to container samples by position or DTS (`sample_index = -1`) | §8.1.4 track ↔ stream matching, §8.7.3 | — |
| CNT-7001 | Warning | `default-base-is-moof`/`tfdt` used but no `iso5`+/`cmfc`/`dash`/`msdh` brand declares them | §8.4.5 brand checks | 14496-12 Annex E |
| CNT-7002 | Warning | `mp42` file: `mp4a`/`mp4v` sample entry without `esds` | §8.4.5 brand checks | 14496-14 §5 |
| CNT-7003 | Error | `avc1`/`avc2` sample entry with `numOfSequenceParameterSets == 0` | §8.4.5 brand checks | 14496-15 §5.3.4 |
| CNT-7004 | Warning | SPS/PPS NAL unit inside an `avc1` sample (parameter sets must be out of band) | §8.4.5 brand checks (Deep) | 14496-15 §5.3.4 |
| CNT-7005 | Error | `hvc1` sample entry whose `hvcC` arrays lack VPS, SPS or PPS | §8.4.5 brand checks | 14496-15 §8.4.1 |
| CNT-7006 | Info | `hvc1` sample entry with `array_completeness == 0` | §8.4.5 brand checks | 14496-15 §8.3.3 |
| CNT-7007 | Error | `av1C.configOBUs` does not contain exactly one `OBU_SEQUENCE_HEADER` | §8.4.5 brand checks | AV1-ISOBMFF §2.3.4 |
| CNT-7008 | Warning | sample `i` contains an `OBU_TEMPORAL_DELIMITER` (SHOULD be omitted) | §8.4.5 brand checks (Deep) | AV1-ISOBMFF §2.4 |
| CNT-7009 | Error | OBU `k` of sample `i` has `obu_has_size_field == 0` and is not the last OBU | §8.4.5 brand checks (Deep) | AV1-ISOBMFF §2.4 |
| CNT-7010 | Error | `cmfc` file with `N` `trak` boxes (exactly one required) | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7011 | Error | `cmfc` file without `mvex`/`trex` | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7012 | Info | `cmfc` file with `mvhd.duration ≠ 0` and no `mehd` | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7013 | Error | `cmfc` file whose `stbl` tables are not empty (`box.entry_count == N`) | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7014 | Error | `cmfc` `moof` `k` with `N` `traf` boxes (exactly one required) | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7015 | Warning | `cmfc` `tfhd` of fragment `k` without `default-base-is-moof` | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7016 | Error | `cmfc` `traf` of fragment `k` without `tfdt` | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7017 | Info | `cmfc` file with `sdtp`/`stss` (not used by CMAF) | §8.4.5 brand checks | 23000-19 §7.3.2 |
| CNT-7018 | Warning | `cmfc` video sample entry without `colr` | §8.4.5 brand checks | 23000-19 §7.5.3 |
| CNT-7019 | Info | `cmfc` video sample entry without `btrt` | §8.4.5 brand checks | 23000-19 §7.5 |
| CNT-7020 | Warning | `cmf2` file with `elst.media_time > 0` (negative composition offsets expected instead) | §8.4.5 brand checks | 23000-19 §7.7 |
| CNT-7021 | Info | `cmf2` file with `cslg` | §8.4.5 brand checks | 23000-19 §7.7 |
| CNT-7022 | Info | `cmf2` file with in-band parameter-set entries (`avc3`/`hev1`) | §8.4.5 brand checks | 23000-19 §7.7 |
| CNT-7023 | Error | `cmfs` segment without a leading `styp` | §8.4.5 brand checks | 23000-19 §7.3.3 |
| CNT-7024 | Warning | `sidx` not at the segment start, or `cmfl` chunk with more than one `moof` | §8.4.5 brand checks | 23000-19 §7.3.3 |
| CNT-7025 | Warning | `emsg`/`prft` after the `moof` of a CMAF chunk | §8.4.5 brand checks | 23000-19 §7.3.3 |
| CNT-7030 | Error | `dash` brand but no `mvex`, or `stsz.sample_count > 0` (self-initialising segment expected) | §8.4.5 brand checks | 23009-1 §6 |
| CNT-7031 | Error | `msix` brand without `sidx` | §8.4.5 brand checks | 23009-1 §6.3.4 |
| CNT-7032 | Warning | `msix` file: `moof` at offset `o` not referenced by any `sidx` | §8.4.5 brand checks | 23009-1 §6.3.4 |
| CNT-7033 | Info | `lmsg` brand on a segment that is not the last | §8.4.5 brand checks | 23009-1 §6.3.4 |
| CNT-7040 | Error | HLS fMP4 segment without `tfdt` | §8.4.5 brand checks | HLS Authoring Specification for Apple Devices |
| CNT-7041 | Warning | HLS context with `hev1`/`avc3` sample entry (Apple requires `hvc1`/`avc1`) | §8.4.5 brand checks | HLS Authoring Specification for Apple Devices |
| CNT-7050 | Info | iTunes brand (`M4A `/`M4V `/`M4P `/`M4B `) without `ilst` | §8.4.5 brand checks | — |
| CNT-8010 | Error | `mif1` file: `meta` lacks required child `box` (`hdlr == pict`, `pitm`, `iinf`, `iloc`, `iprp`) | §8.4.5 MIAF/HEIF checks | 23008-12 §10 |
| CNT-8011 | Error | MIAF image item `item_ID` without `ispe` | §8.4.5 MIAF/HEIF checks, WI-4.213 | 23000-22 §7.3.6 |
| CNT-8012 | Error | MIAF primary item `item_ID` without `pixi` | §8.4.5 MIAF/HEIF checks, WI-4.213 | 23000-22 §7.3.6 |
| CNT-8013 | Warning | transformative properties not in the order `clap`, `irot`, `imir` for item `item_ID` | §8.4.5 MIAF/HEIF checks | 23000-22 §7.3.6 |
| CNT-8014 | Info | MIAF image item `item_ID` without `colr` | §8.4.5 MIAF/HEIF checks | 23000-22 §7.3.6 |
| CNT-IVF-0001 | Info | IVF `header_size` is `N` (≠ 32); first frame taken at offset `max(N, 32)` | §8.6.2 IVF reader | libvpx `ivfdec.c` |
| CNT-IVF-0002 | Info | IVF header `frame_count` `N` differs from the walked count `M` | §8.6.2 IVF reader | libvpx `ivfdec.c` |
| CNT-IVF-0003 | Warning | last IVF frame truncated: `frame_size` `N` exceeds EOF by `M` bytes (clamped); or only `K` of the 12 frame-header bytes remain (no frame kept, walk ends) | §8.6.2 IVF reader | — |
| CNT-ESB-0001 | Warning | `N` bytes of garbage before the first start code / after the last NAL unit | §8.6.3 Annex B splitter | H.264 clause B.1.1 |
| CNT-ESB-0002 | Info | more than 8 `trailing_zero_8bits` after NAL unit `n` | §8.6.3 Annex B splitter | H.264 clause B.1.2 |
| CNT-ESB-0003 | Warning | empty NAL unit after start code at offset `o` | §8.6.3 Annex B splitter | H.264 clause B.1.2 |
| CNT-ESB-0004 | Info | 3-byte start code before SPS/PPS/VPS/AUD/first NAL of an access unit (`zero_byte` required) | §8.6.3 Annex B splitter | H.264 clause B.1.2, H.265 clause B.1.2 |
| CNT-ESB-0005 | Error | `forbidden_zero_bit` set in NAL header at offset `o` (false start code or corruption) | §8.6.3 Annex B splitter | H.264 §7.4.1 |
| CNT-OBU-0001 | Error | OBU at offset `o` has `obu_has_size_field == 0` outside Annex B framing (undecodable); open fails with `OVT_E_UNSUPPORTED` | §8.6.4 OBU detection | AV1 §5.3.1 |
| CNT-OBU-0002 | Info | both Section 5 and Annex B framings parse; `framing` chosen by the first-byte rule | §8.6.4 OBU detection | AV1 §5.2, Annex B |
| CNT-OBU-0003 | Warning | length at offset `o` exceeds the bytes left in the file or enclosing unit; unit clamped to `N` bytes | §8.6.4 OBU walk, WI-1.227 | AV1 §5.3, Annex B |
| CNT-OBU-0004 | Error | OBU header, `leb128` or length at offset `o` is malformed (incl. `obu_length` 0); the OBU or the rest of its enclosing unit is skipped | §8.6.4 OBU walk, WI-1.227 | AV1 §4.10.5, §5.3.2 |
| CNT-Y4M-0001 | Info | Y4M header without `F` tag; 25:1 assumed | §8.6.5 Y4M reader | yuv4mpeg(5) |
| CNT-Y4M-0002 | Error | Y4M `C` tag `value` unknown; open fails with `OVT_E_UNSUPPORTED` | §8.6.5 Y4M reader | yuv4mpeg(5), FFmpeg `yuv4mpegdec.c` |
| CNT-Y4M-0003 | Error | `FRAME` magic missing at offset `o` (frame `i`); walk stopped | §8.6.5 Y4M reader | yuv4mpeg(5) |
| CNT-RAW-0001 | Warning | `N` trailing bytes do not form a whole frame (ignored) | §8.6.6 raw YUV | — |
| CNT-RAW-0002 | Error | raw format undetermined; `N` candidates (`list`) — supply `--raw-format` or a sidecar (CLI exit 2) | §8.6.6 raw YUV step 5 | — |
| CNT-DASH-0001 | Info | dynamic MPD snapshotted at parse time | WI-4.208 MPD parser | 23009-1 §5.3.1.2 |
| CNT-DASH-0002 | Info | `xlink` reference on `element` ignored | WI-4.208 MPD parser | 23009-1 §5.5 |
| CNT-DASH-0003 | Info | `SegmentBase` of representation `id` not resolved (no `sidx` resolver); one whole-resource segment assumed | WI-4.208 MPD parser | 23009-1 §5.3.9.2 |
| CNT-DASH-0004 | Warning | open-ended `SegmentTimeline` (`r = −1`) without a period duration; timeline truncated | WI-4.208 MPD parser | 23009-1 §5.3.9.6 |
| CNT-DASH-0005 | Error | unknown template identifier `$name$` in `SegmentTemplate` of representation `id` | WI-4.208 MPD parser | 23009-1 §5.3.9.4.4 |
| CNT-DASH-0006 | Error | representation `id` has no segment information; skipped | WI-4.208 MPD parser | 23009-1 §5.3.9 |
| CNT-DASH-0007 | Warning | invalid value `v` for attribute `name` on `element`; default used | WI-4.208 MPD parser | 23009-1 §5.3 |
| CNT-DASH-0008 | Warning | `SegmentURL` count `N` differs from the `SegmentTimeline` count `M` for representation `id` | WI-4.208 MPD parser | 23009-1 §5.3.9.3 |
| CNT-DASH-0009 | Fatal | not a well-formed MPD (`detail`); open fails with `OVT_E_CORRUPT` | WI-4.208 MPD parser | 23009-1 §5.3.1 |
| CNT-EBML-0001 | Error | invalid EBML variable-size integer at offset `o` (length marker > 8 bytes or reserved all-ones id) | §8.6.7 EBML reader, WI-4.201 | RFC 8794 §4 |
| CNT-EBML-0002 | Warning | unknown-size element `id` outside `Segment`/`Cluster` | §8.6.7 EBML reader, WI-4.201 | RFC 8794 §6.2, RFC 9559 |
| CNT-EBML-0003 | Error | element `id` extends past its parent / EOF (truncated) | §8.6.7 EBML reader, WI-4.201 | RFC 8794 §6 |
| CNT-EBML-0004 | Warning | `CRC-32` element mismatch in master `id` | §8.6.7 EBML reader, WI-4.201 | RFC 8794 §11.3.1 |
| CNT-EBML-0005 | Info | element id `0x…` not in the Matroska schema (shown as hex) | §8.6.7 EBML reader, WI-4.200 | RFC 9559 |
| CNT-TS-1001 | Error | TS_sync_loss: `N` consecutive packets without `0x47` at offset `o` | §8.6.7 TS packet sync, WI-4.204 | TR 101 290 §5.2.1 (1.1) |
| CNT-TS-1002 | Error | Sync_byte_error at packet `k` | §8.6.7 TS packet sync, WI-4.204 | TR 101 290 §5.2.1 (1.2) |
| CNT-TS-1003 | Error | PAT_error_2: PID 0 absent for > 0.5 s / `table_id ≠ 0x00` / scrambled | §8.6.7 PSI parsing, WI-4.205 | TR 101 290 §5.2.1 (1.3) |
| CNT-TS-1005 | Error | PMT_error_2: PMT PID absent for > 0.5 s / scrambled | §8.6.7 PSI parsing, WI-4.205 | TR 101 290 §5.2.1 (1.5) |
| CNT-TS-1006 | Warning | Continuity_count_error on PID `pid` at packet `k` (`expected`/`got`) | §8.6.7 TS packets, WI-4.204 | TR 101 290 §5.2.1 (1.4) |
| CNT-TS-1007 | Warning | PID_error: PID `pid` referenced by PMT but no packets for `T` s | §8.6.7 PSI parsing, WI-4.205 | TR 101 290 §5.2.1 (1.6) |
| CNT-TS-2001 | Warning | Transport_error: `transport_error_indicator` set at packet `k` | §8.6.7 TS packets, WI-4.204 | TR 101 290 §5.2.2 (2.1) |
| CNT-TS-2002 | Error | CRC_error in `table` section on PID `pid` | §8.6.7 PSI parsing, WI-4.205 | TR 101 290 §5.2.2 (2.2) |
| CNT-TS-2003 | Warning | PCR_repetition_error: `D` ms between PCRs on PID `pid` (> 40 ms) | §8.6.7 timing table, WI-4.206 | TR 101 290 §5.2.2 (2.3a) |
| CNT-TS-2004 | Warning | PCR_discontinuity_indicator_error: PCR jump `D` ms without `discontinuity_indicator` on PID `pid` | §8.6.7 timing table, WI-4.206 | TR 101 290 §5.2.2 (2.3b) |
| CNT-TS-2005 | Info | PCR_accuracy_error: `D` ns deviation on PID `pid` (> ±500 ns) | §8.6.7 timing table, WI-4.206 | TR 101 290 §5.2.2 (2.4) |
| CNT-TS-2006 | Warning | PTS_error: `D` ms between PTS on PID `pid` (> 700 ms) | §8.6.7 PES reassembly, WI-4.206 | TR 101 290 §5.2.2 (2.5) |
| CNT-TS-2007 | Warning | CAT_error: scrambled packets present but no CAT (PID 1) | §8.6.7 PSI parsing, WI-4.205 | TR 101 290 §5.2.2 (2.6) |
**Out of scope for phase 1:** ETSI TR 101 290 (MPEG-TS priority 1–2 checks, `CNT-TS-*`) and EBML (`CNT-EBML-*`) are phase-4 codes (§8.6.7, WI-4.200–WI-4.207); priority 3 checks are not implemented.

**Validation driver:** `ovt::container::isobmff::validate(ParsedMovie&, ValidationLevel {Quick, Standard, Deep}, ovt::jobs::Cancel&)` (`ovt probe --validate --validate-level <quick|standard|deep>`, chapter 12 §12.2.2): `Quick` = walk-time diagnostics only (always on); `Standard` = + sample table, edit list, fragment, brand checks without reading sample bytes (default for `ovt_container_open`); `Deep` = + sample-byte checks (unit split of first 8 sync samples per track, HDR probe, CMAF sample constraints) — run as a job.

---

## 8.6 Other formats

### 8.6.1 Format sniffing (`format_sniff.cpp`)

`ovt::container::Format sniff(RandomAccessFile&)` reads the first 64 KiB and decides in this order (first match wins):

1. bytes 0..3 == `"DKIF"` → `ivf`.
2. bytes 0..9 == `"YUV4MPEG2 "` → `y4m`.
3. bytes 4..7 ∈ {`ftyp`, `styp`, `moov`, `moof`, `mdat`, `free`, `skip`, `wide`, `uuid`, `sidx`, `pdin`, `emsg`, `prft`} and (u32 at 0 ≥ 8 or == 0 or == 1) → `isobmff`.
4. bytes 0..3 == `1A 45 DF A3` (EBML header) → `ebml`.
5. byte 0 == `0x47` and byte 188 == `0x47` and byte 376 == `0x47` (or stride 192/204) → `mpegts`.
6. bytes 0..3 == `06 0E 2B 34` (MXF KLV key start; run-in up to 64 KiB allowed) → `mxf`.
7. starts with `#EXTM3U` → `hls`; starts with `<?xml` or `<MPD` containing `<MPD` within 4 KiB → `dash`.
8. Annex B test: `00 00 01` or `00 00 00 01` at offset 0 (after ≤ 64 leading zero bytes) → `annexb`; codec guessed from the first NAL header: H.264 if `(b0 & 0x80) == 0` and `nal_unit_type (b0 & 0x1F) ∈ {7, 9, 6, 5, 1}` and the HEVC interpretation fails; HEVC if `(b0 & 0x80) == 0`, `((b0 >> 1) & 0x3F) ∈ {32, 33, 34, 35, 39, 19, 20, 21}` and the second header byte satisfies `(b1 & 0x07) != 0` (`nuh_temporal_id_plus1 ≥ 1`; `nuh_layer_id` = `((b0 & 1) << 5) | (b1 >> 3)` is normally 0); when both interpretations are plausible the sniffer reports the codec guess `Ambiguous` next to `annexb` (the result carries a heuristic guess `H264` / `H265` / `Ambiguous`); the stream reader resolves `Ambiguous` by parsing the first parameter set with both syntax parsers (chapter 7) and picking the one without `ParseError`, once those parsers are available.
9. AV1 OBU test (§8.6.4 heuristic) → `obu`.
10. File extension hint (`.yuv`, `.raw`, `.rgb`) → `raw`.
11. else `unknown` → `ovt_container_open` returns `OVT_E_UNSUPPORTED`.

Extension hints never override content sniffing except for `raw`.

### 8.6.2 IVF

Source of truth: libvpx `ivfdec.c`/`ivfenc.c` (https://chromium.googlesource.com/webm/libvpx/+/refs/heads/main/ivfenc.c) and libaom `common/ivfdec.c`. All integers little-endian.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `signature` | `"DKIF"` |
| 4 | 2 | `version` | 0 |
| 6 | 2 | `header_size` | 32; if > 32 use it as the first frame offset; if < 32 the field is ignored and the first frame is taken at offset 32 as libvpx does (both cases `CNT-IVF-0001` Info) |
| 8 | 4 | `fourcc` | `VP80`, `VP90`, `AV01`, (`H264` seen in the wild) |
| 12 | 2 | `width` | |
| 14 | 2 | `height` | |
| 16 | 4 | `timebase_denominator` | frame rate numerator in libvpx naming ("rate") |
| 20 | 4 | `timebase_numerator` | ("scale") |
| 24 | 4 | `frame_count` | often 0 or wrong; **never trusted** — frames are counted by walking (`CNT-IVF-0002` Info when it differs) |
| 28 | 4 | `unused` | |

Frame header (12 bytes) at each frame: `frame_size u32le`, `timestamp u64le` (in `timebase_numerator/timebase_denominator` units), then `frame_size` bytes of payload = one AV1 **temporal unit** (OBUs in Section 5 low-overhead format, each with `obu_has_size_field` normally 1) or one VP9 superframe.

Tree: root `BoxNode{fourcc:"IVF "}` with fields above; one child per frame `BoxNode{fourcc:"FRM ", file_span, fields: frame_size, timestamp}` (lazy: the children list is materialised in windows of 4096 frames via `ovt_container_expand`). `TrackInfo`: `track_id 1`, `timescale = timebase_denominator / g` and frame timestamps are multiplied by `timebase_numerator / g` with `g = gcd(numerator, denominator)` (a frame timestamp `t` is `t × scale / rate` seconds; libvpx writes `scale ≠ 1` for e.g. 30000/1001), `duration = (last timestamp + 1) × scale/g`, `codec` from fourcc. `SampleTable`: `dts = timestamp × scale/g` (IVF timestamps are PTS of the temporal unit; for AV1 with hidden frames the temporal unit's shown frame), `offset = frame_payload_offset`, `size = frame_size`, `is_sync` = temporal unit begins with a key frame (determined by the syntax module on the first frame header; lazily for the rest). Truncated last frame (`frame_size` exceeds EOF) → `CNT-IVF-0003` Warning, frame kept with clamped size.

### 8.6.3 Annex B byte streams (H.264 Annex B, H.265 Annex B)

Spec: ITU-T H.264 Annex B (clauses B.1–B.2), H.265 Annex B (clauses B.1–B.2). Terminology: `leading_zero_8bits`, `zero_byte`, `start_code_prefix_one_3bytes` (`00 00 01`), `trailing_zero_8bits`.

**Scan algorithm** `annexb_split(RandomAccessFile&, callback(UnitSpan))`, streaming over 1 MiB windows with a 4-byte carry:

1. `pos = 0`. Skip `leading_zero_8bits`: advance while byte == `0x00` — but remember `zero_run_start`.
2. Find the first `00 00 01` at or after `pos` (SIMD-friendly: search for `0x01` whose two predecessors are `0x00`). If none → if any non-zero bytes remain after `pos` emit `CNT-ESB-0001` (Warning, "N bytes of garbage before first start code / at end") and stop.
3. The start code occupies `[sc, sc+3)`. If `sc ≥ 1` and byte `sc−1 == 0x00`, the `zero_byte` is at `sc−1` and the 4-byte form `00 00 00 01` is recorded (`prefix_span = [sc−1, sc+3)`); further preceding zeros are `leading_zero_8bits` of this NAL / `trailing_zero_8bits` of the previous one (both legal; recorded as `prefix_span` extension only when they immediately precede a 4-byte code? No: **rule**: `prefix_span` covers exactly `zero_byte? + 3-byte start code`; extra zero bytes are attributed to the previous NAL's `trailing_zero_8bits` and recorded as Info `CNT-ESB-0002` in `ContainerTree.diagnostics` (with the unit index in the message; `Unit` has no diagnostics member in chapter 06) only when more than 8 of them occur — common in padded streams).
4. `nal_start = sc + 3`. Find the next occurrence of `00 00 00` or `00 00 01` at or after `nal_start` (H.264 clause B.1.2: the NAL ends before the next `00 00 00`, `00 00 01`, or EOF; `00 00 02` cannot occur in a NAL (EPB guarantees), `00 00 03` is an EPB and MUST NOT be treated as a boundary). `nal_end` = that position (or EOF).
5. Strip `trailing_zero_8bits`: while `nal_end > nal_start` and byte `nal_end−1 == 0x00`, decrement `nal_end`. (The last byte of a NAL is the RBSP stop bit byte or a `cabac_zero_word` `0x0000` which is EPB-escaped to `00 00 03`; so a NAL never legitimately ends in `0x00` — H.264 §7.4.1.) If the NAL becomes empty → `CNT-ESB-0003` (Warning, "empty NAL unit after start code at offset"); the empty NAL is **not** emitted as a unit (it has no header byte), and unit indices count emitted units only.
6. Emit `UnitSpan{prefix_span, file_span = [nal_start, nal_end), start_code_bytes = 3 or 4}`. Check the `zero_byte` rule: H.264 clause B.1.2 / H.265 clause B.1.2 — a `zero_byte` (4-byte code) MUST precede SPS, PPS (and VPS), AUD, and the first NAL of an access unit; the splitter raises `CNT-ESB-0004` (Info; decoders accept it) for a 3-byte code before SPS/PPS/VPS/AUD only — the first-NAL-of-an-access-unit case needs slice-header parsing and is left to the index module (chapter 7). `CNT-ESB-0002` counts the zero bytes between a NAL's last non-zero byte and the following `zero_byte`/start code (or EOF) and is raised once per NAL when they exceed 8, including after the last NAL; non-zero bytes between a `00 00 00` boundary and the next start code are `CNT-ESB-0001`.
7. `pos = nal_end`; go to 2.

The forbidden_zero_bit of each NAL header MUST be 0, else `CNT-ESB-0005` (Error, "forbidden_zero_bit set — likely false start code or corruption"). Access-unit grouping (which NALs form one frame) is done by the `index` module (chapter 7), not here; the container layer gives `Unit.file_span`/`prefix_span` and `sample_index = unit index` (one "sample" = one NAL in Annex B). `TrackInfo`: one track, `timescale 90000`, `duration` unknown (−1) until the index is built; `codec` from sniffing.

### 8.6.4 Raw AV1 OBU streams: Section 5 low-overhead vs Annex B

Spec: AV1 Bitstream & Decoding Process v1.0.0 Errata 1, §5.2 (OBU syntax), §5.3.1 (`leb128`), Annex B (length-delimited bitstream format: `temporal_unit_size`, `frame_unit_size`, `obu_length`).

**Detection heuristic** `obu_detect(span first_64k) → {Section5, AnnexB, NotAv1}`:

1. **Section 5 attempt.** `pos = 0`; for up to 3 OBUs: read `obu_header` byte `b`: require `forbidden_bit = (b >> 7) == 0`, `obu_type = (b >> 3) & 0xF ∈ {1 SEQUENCE_HEADER, 2 TEMPORAL_DELIMITER, 3 FRAME_HEADER, 4 TILE_GROUP, 5 METADATA, 6 FRAME, 7 REDUNDANT_FRAME_HEADER, 8 TILE_LIST, 15 PADDING}`, `obu_has_size_field = (b >> 1) & 1 == 1` (a raw Section 5 stream without size fields is undecodable without framing, so has_size_field MUST be 1; when the header is otherwise valid but has_size_field is 0 and the Annex B attempt below also fails, the file is rejected with `CNT-OBU-0001` (Error) and `OVT_E_UNSUPPORTED`, FR-CNT-012), `reserved_1bit == 0`; if `obu_extension_flag = (b >> 2) & 1` read the extension byte; read `obu_size = leb128()` (≤ 8 bytes, value < 2^32); require `pos + header + obu_size ≤ 64 KiB window or ≤ file size`. The **first** OBU MUST be `TEMPORAL_DELIMITER` (size 0) or `SEQUENCE_HEADER`. If all 3 OBUs (or fewer when the file ends) pass → `Section5`.
2. **Annex B attempt.** `pos = 0`; `temporal_unit_size = leb128()` with `1 ≤ tus ≤ file_size − pos`; inside: `frame_unit_size = leb128()` with `1 ≤ fus ≤ tus`; inside: `obu_length = leb128()` with `1 ≤ ol ≤ fus` (an `obu_length` of 0 is `CNT-OBU-0004`); the OBU header at that point MUST pass the header checks of step 1 **except** that `obu_has_size_field` MAY be 0 (typical) or 1 (then `obu_size` must equal `obu_length − header_bytes − leb128 bytes`); first OBU MUST be `TEMPORAL_DELIMITER` or `SEQUENCE_HEADER`. Repeat for the second temporal unit if present. Pass → `AnnexB`.
3. Neither → `NotAv1`. If **both** pass (possible for tiny crafted files), prefer `Section5` when byte 0 == `0x12` and byte 1 == `0x00` (`OBU_TEMPORAL_DELIMITER` with `has_size_field`), else `AnnexB`; emit `CNT-OBU-0002` Info.

**Tree:** `obu` root with `format` field = `"section5"` / `"annexb"`; children per temporal unit: `BoxNode{fourcc:"TU  ", fields: temporal_unit_size (Annex B) or derived size}`, under it `FU  ` (Annex B only) and `OBU ` nodes with fields `obu_forbidden_bit`, `obu_type` (+ enum name), `obu_extension_flag`, `obu_has_size_field`, `obu_reserved_1bit`, `[temporal_id, spatial_id, extension_header_reserved_3bits]`, `obu_size` / `obu_length`. Payload parsing belongs to the syntax module. Units handed to plugins: for Annex B files the splitter **re-frames** each OBU into Section 5 form (sets `obu_has_size_field = 1` and inserts the `leb128` size) into a scratch buffer — `Unit.file_span` still points at the original bytes and `Unit.prefix_span` covers the `obu_length` leb128; `ovt_stream_unit_bytes.out_raw` returns the re-framed bytes (documented in §8.8).

### 8.6.5 Y4M (YUV4MPEG2)

Spec: the `yuv4mpeg` man page (mjpegtools) and FFmpeg `libavformat/yuv4mpegdec.c` for the extended `C` tags. Header: `"YUV4MPEG2"` followed by space-separated tags, terminated by `0x0A`. Each frame: `"FRAME"` + optional space-separated frame tags + `0x0A` + planes (Y then U then V, no padding, rows top-down).

| Tag | Meaning | Values |
|---|---|---|
| `W<n>` | width | required |
| `H<n>` | height | required |
| `F<num>:<den>` | frame rate | e.g. `F30000:1001`; absent → 25:1 with `CNT-Y4M-0001` Info |
| `I<c>` | interlacing | `p` progressive, `t` top-field-first, `b` bottom-field-first, `m` mixed, `?` unknown |
| `A<num>:<den>` | pixel aspect ratio | `0:0` unknown |
| `C<cs>` | colour space (table below) | absent → `420jpeg` |
| `X<key>=<value>` | comment/extension | `XYSCSS=`, `XCOLORRANGE=FULL|LIMITED` (FFmpeg) |

`C` tag → chroma format / bit depth / siting:

| `C` value | chroma_format | bit depth | chroma siting (`ovt_ColorDescription.chroma_siting`) |
|---|---|---|---|
| `420jpeg`, `420` | 4:2:0 | 8 | 2 (center) |
| `420mpeg2` | 4:2:0 | 8 | 1 (left) |
| `420paldv` | 4:2:0 | 8 | 3 (top-left, co-sited) |
| `422` | 4:2:2 | 8 | 1 |
| `444` | 4:4:4 | 8 | 0 |
| `444alpha` | 4:4:4 + alpha plane | 8 | 0 |
| `mono` | 4:0:0 | 8 | 0 |
| `420p10`, `422p10`, `444p10` | as named | 10 (little-endian 16-bit samples) | 0 |
| `420p12`, `422p12`, `444p12` | as named | 12 | 0 |
| `420p14`, `422p14`, `444p14` | as named | 14 | 0 |
| `420p16`, `422p16`, `444p16` | as named | 16 | 0 |
| `mono10`, `mono12`, `mono16` (`mono14` OVT-native: accepted by the reader, not written by FFmpeg 7.x `yuv4mpegenc`) | 4:0:0 | as named | 0 |
| `411` | 4:1:1 | 8 | — (rare; supported) |
| anything else | — | — | `CNT-Y4M-0002` Error, open fails with `OVT_E_UNSUPPORTED` |

Frame size in bytes = `Σ planes (plane_w × plane_h × bytes_per_sample)`; the reader walks `FRAME` markers (verifying the 5-byte magic each time → `CNT-Y4M-0003` Error on mismatch, stop) and builds a `SampleTable` with `dts = i`, `timescale = F.num`, `duration = F.den`, `offset` at the first plane byte, `size` = frame size, all sync. `TrackInfo.codec = Unknown` and a `RawVideoFormat` table (below) is attached. Tree: root `Y4M ` with one field per tag, lazy `FRAME` children (fields: frame tags).

### 8.6.6 Raw YUV with sidecar or explicit format

Schema: `RawVideoFormat` — chapter 06 §6.4 (`color` is the `ovt_ColorDescription` struct image).

**Pixel-format string grammar** (FFmpeg `pix_fmt` names; EBNF):

```
pix_fmt     := planar_yuv | semi_planar | gray | rgb ;
planar_yuv  := "yuv" ("400"|"420"|"422"|"444"|"411"|"440") "p" [bits] [endian] ;
bits        := "9" | "10" | "12" | "14" | "16" ;        (* absent = 8 *)
endian      := "le" | "be" ;                            (* required when bits present *)
semi_planar := "nv12" | "nv21" | "nv16" | "nv24" | "p010" [endian] | "p012" [endian] | "p016" [endian] | "p210" [endian] | "p410" [endian] ;
gray        := "gray" [bits] [endian] ;                 (* gray, gray10le, gray12le, gray16le *)
rgb         := "rgb24" | "bgr24" | "rgb48" endian | "gbrp" [bits] [endian] ;
```

Examples: `yuv420p`, `yuv420p10le`, `yuv422p10le`, `yuv444p12le`, `gray10le`, `nv12`, `p010le`. `frame_bytes` for `yuvXYZp` with `bits`: `bps = bits ≤ 8 ? 1 : 2`; `cw = ceil(w / sx)`, `ch = ceil(h / sy)` with `(sx,sy)` = 420 → (2,2), 422 → (2,1), 444 → (1,1), 411 → (4,1), 440 → (1,2), 400 → no chroma; `frame_bytes = bps × (w·h + 2·cw·ch)`; `p010`: `2 × (w·h + 2·cw·ch)` with interleaved UV.

**Sources of the format, in priority order:**

1. **Sidecar** `<file>.ovt-raw.json` next to the file (JSON Schema `core/schema/raw_sidecar.schema.json`: `{ "width": int, "height": int, "pix_fmt": string, "fps": "num/den" | number, "header_bytes": int = 0, "color": { "primaries": int, "transfer": int, "matrix": int, "full_range": bool } }`). Written by the GUI dialog on confirmation so the next open is silent.
2. **Explicit** CLI flag `--raw-format WxH:pix_fmt[:fps_num/fps_den]` (chapter 12 §12.1.5, D21 — `--raw` alone is `ovt dump`'s raw-plane output flag; e.g. `--raw-format 1920x1080:yuv420p10le:30000/1001`; the same string is the `raw_format` argument of the C API `ovt_container_open_format(ovt_File*, "raw", raw_format, NULL, ovt_Container**)` of chapter 12 §12.3.7, and `--raw-fps <num/den>` may replace the third component). The C entry point parses the explicit string only (`header_bytes = 0`); the sidecar, the filename/size heuristics and the `--raw-offset`/`--raw-fps` overrides are applied by the C++ `ovt::container::raw::resolve()` that the CLI and GUI call. `raw_format == NULL` makes the C entry point run the same `resolve()` (sidecar, then heuristics; `OVT_E_INVALID_ARG` with `CNT-RAW-0002` when other than exactly one candidate remains), so a sidecar's `header_bytes` reaches the C API that way; the string grammar gets no `@<header_bytes>` suffix (ruling of 2026-11-13 on issue #366 item 3).
3. **Filename heuristics:** regex `(\d{2,5})[xX](\d{2,5})` for `WxH`; token match against the grammar (`yuv420p10le`, `420p10`, `p010`, `nv12`, `gray`); `(\d{1,3}(?:\.\d+)?)fps` or `_(\d{2,3})(?:p|fps)\b`; tokens `8bit|10bit|12bit`, `420|422|444`. Compose candidates.
4. **Size heuristic:** for each candidate `(w,h,pix_fmt)` from step 3, and when no `WxH` is present for the common sizes `{3840×2160, 2560×1440, 1920×1080, 1920×1088, 1280×720, 1024×768, 960×540, 832×480, 720×576, 720×480, 640×480, 640×360, 416×240, 352×288, 320×240, 176×144}` × `{yuv420p, yuv420p10le, yuv422p, yuv422p10le, yuv444p, yuv444p10le, nv12, p010le, gray}`: accept when `file_size % frame_bytes == 0` and `frame_count ≥ 1`. Rank: candidates with a `WxH` filename match first; then, among candidates with `frame_count ≥ 2`, the larger frame size (i.e. the smaller `frame_count`) first; then 4:2:0 before 4:2:2 before 4:4:4, 8-bit before 10-bit; candidates with `frame_count > 100 000` come last. Defaults when nothing says otherwise: `chroma_format` is 2 for 4:4:0, 3 for RGB and 4 for 4:1:1 (as the Y4M reader); a bare `p010`/`p012`/`p016`/`p210`/`p410` is little-endian; without a `color` object primaries, transfer and matrix are H.273 value 2 (unspecified) and the range is limited; `SampleTable.dts = i × fps_den` with `timescale = fps_num` (as the Y4M reader); sidecar keys the schema does not name are ignored.
5. **Dialog** (GUI, chapter 11) pre-filled with the top candidate; CLI without `--raw-format` and with other than exactly one candidate emits `CNT-RAW-0002`, returns `OVT_E_INVALID_ARG` (exit 2) and prints the ranked candidates (the GUI dialog pre-fills `candidates[0]`) (`ovt probe --raw-candidates`, chapter 12 §12.2.2, lists them without opening).

Frames: `dts = i`, `timescale = fps_num`, `duration = fps_den`, `offset = header_bytes + i × frame_bytes`. A trailing partial frame → `CNT-RAW-0001` Warning (ignored bytes).

### 8.6.7 Later-phase formats (phase 4; designs fixed now so the model does not change)

**EBML / Matroska / WebM** (`core/src/container/ebml/`): schema-generated from `ebml_matroska.xml` (IETF CELLAR, https://github.com/ietf-wg-cellar/matroska-specification, RFC 9559) by `tools/gen_ebml_schema.py` → `ebml_schema.gen.cpp` with `{id, name, type (master/uint/int/float/string/utf8/date/binary), path, min/max occurrences, default, range}`. The reader: variable-size integer parsing (RFC 8794 §4: length marker bit; `0xFF`-all-ones = unknown size, only legal on `Segment`/`Cluster` → scan to the next sibling-level ID), element tree with `BoxNode.fourcc` = hex id string (`"0x1A45DFA3"`), `description` = schema name (`EBML`), `fields` = single value field with span. Lazy: `Cluster` children parsed on demand; `Cues` eagerly; `SimpleBlock`/`BlockGroup` headers decoded (track number vint, timestamp i16, flags: keyframe `0x80`, invisible `0x08`, lacing `0x06`, discardable `0x01`), lacing (Xiph / fixed / EBML) expanded to frame spans. Elements of interest beyond the schema: `Video/Colour` master (`MatrixCoefficients`, `BitsPerChannel`, `ChromaSubsamplingHorz/Vert`, `CbSubsamplingHorz/Vert`, `ChromaSitingHorz/Vert`, `Range`, `TransferCharacteristics`, `Primaries`, `MaxCLL`, `MaxFALL`, `MasteringMetadata{PrimaryRChromaticityX…, WhitePointChromaticityX/Y, LuminanceMax/Min}`) → `HdrMetadata`; `BlockAdditionMapping` (`BlockAddIDValue`, `BlockAddIDType` = `"dvcC"`/`"dvvC"`/`"hvcE"`/`"avcE"`, `BlockAddIDExtraData` = the DV configuration record) and `BlockAdditions/BlockMore/BlockAddID == 4` carrying the DV RPU per frame (as read by dovi_tool) → DV detection; `CodecPrivate` (= `avcC`/`hvcC`/`av1C`/`vpcC`... re-used by the ISOBMFF decoders of §8.3 group C through a `BoxContext` with `ancestors = ["mkv","CodecPrivate"]`); `ContentEncryption` (`ContentEncAlgo`, `ContentEncKeyID`, `ContentEncAESSettings`) → `EncryptionInfo`; `Tags`, `Chapters`, `Attachments` (offset/size only). `TrackInfo.track_id = TrackNumber`, `timescale = 1e9 / TimestampScale`. `SampleTable` from Clusters (`dts = ClusterTimestamp + BlockTimestamp`, CTS = same (Matroska stores PTS; DTS is unknown → `cts_offset = 0`, `is_sync` = keyframe flag or presence in `Cues`), lazily per Cluster. Oracle: `mkvinfo -v -v -z` and `mediainfo --Details=1`.

**MPEG-TS** (`core/src/container/mpegts/`): packet sync (188/192/204-byte packets, tried in that order with every phase within the first 64 KiB; for a stride of 192 a phase of 4 means the 4-byte M2TS `TP_extra_header` precedes the sync byte and the packet node carries a `timestamp` field, a phase of 0 means 4 trailing bytes; the sniffer of §8.6.1 requires `0x47` at byte 0, so accepting a sync byte at offset 4 is a follow-up; resync rule: a sync-byte mismatch at the expected position with `0x47` again at the next expected position is one isolated `CNT-TS-1002` and the walk stays on the grid, otherwise the walk searches forward from the last good sync byte + 1 for a position with up to 5 aligned sync bytes (at least 2 near EOF) and emits `CNT-TS-1001` once per loss episode with `N` = packet slots skipped (≥ 1); bytes before the first sync byte are one episode starting at offset 0; a partial packet at EOF is ignored silently; tree: root `MPTS`, one `TSPK` child per packet in windows of 65 536 packets expanded through `ovt_container_expand`, an adaptation field as child node `ADPT`, field names in ISO/IEC 13818-1 §2.4.3 snake_case; `TrackInfo` stays empty until WI-4.205/4.206; continuity rule: the counter advances only on packets with payload, an adaptation-only packet repeats the last value, one duplicate is accepted, packets with `discontinuity_indicator`, null packets (PID `0x1FFF`) and `transport_error_indicator` packets are not checked and the counter resynchronises after each `CNT-TS-1006`; per-PID statistics live in the internal `mpegts::File` struct until the `TsReport` of WI-4.207; the container diagnostics list is capped at 10 000 entries per file while counts stay exact), per-packet header fields (`transport_error_indicator`, `payload_unit_start_indicator`, `transport_priority`, `PID`, `transport_scrambling_control`, `adaptation_field_control`, `continuity_counter`), adaptation field (`discontinuity_indicator`, `random_access_indicator`, `PCR`/`OPCR` 33+6+9 bits, `splice_countdown`, private data), PSI section parsing (PAT `0x00`, CAT `0x01`, PMT with ES descriptors incl. registration `0x05`, AVC `0x28`, HEVC `0x38`, AC-3 `0x6A`, HDR/`HEVC_video_descriptor` fields, DVB SDT/EIT/TDT/TOT as read-only tables), PES header (`stream_id`, `PES_packet_length`, `PTS_DTS_flags`, 33-bit PTS/DTS with marker bits, `ESCR`, `data_alignment_indicator`), reassembly of PES payloads per PID → `SampleTable` (one sample per PES packet; `offset` = first payload byte; `units` from Annex B splitting), PCR/PTS/DTS timing table (as Elecard shows), bitrate per PID. `TrackInfo.track_id = PID`. TR 101 290 subset (phase 4, codes `CNT-TS-1xxx` priority 1: `TS_sync_loss`, `Sync_byte_error`, `PAT_error(_2)`, `Continuity_count_error`, `PMT_error(_2)`, `PID_error`; `CNT-TS-2xxx` priority 2: `Transport_error`, `CRC_error`, `PCR_error`/`PCR_repetition_error` (> 40 ms), `PCR_discontinuity_indicator_error`, `PCR_accuracy_error` (> ±500 ns), `PTS_error` (> 700 ms), `CAT_error`; priority 3 deferred). Oracle: TSDuck `tsanalyze`, `tstables`, `tsp -P continuity`; DVB Inspector for PSI trees.

**MXF** (deferred; `core/src/container/mxf/` not before phase 4 and only if an owner decision reopens it): KLV triplet walk (16-byte UL key, BER length, value), partition packs, header metadata sets (preface, content storage, material/source packages, descriptors) with local tags via primer pack, index tables, essence containers → `SampleTable`. Oracle: bmx `mxf2raw --info --all` (Apache-2.0) and FFmpeg `mxf` demuxer. Until then MXF files are opened through libavformat only (Role B) with `ContainerTree.format = "mxf"` and a root node `description = "MXF structure view not implemented (phase 4+)"`.

**DASH MPD / HLS M3U8 virtual file** (`core/src/container/virtual_file/`, phase 4): the playlist is parsed (`MPD`: `Period/AdaptationSet/Representation/SegmentTemplate|SegmentList|SegmentBase` with `$Number$`/`$Time$` expansion and `SegmentTimeline`; `M3U8`: `#EXT-X-MAP` init, `#EXTINF` media segments, `#EXT-X-BYTERANGE`, `#EXT-X-KEY`, `#EXT-X-DISCONTINUITY`, `#EXT-X-STREAM-INF` variants). One **Representation / variant** is selected (GUI list; CLI `--variant`). A `VirtualFile : RandomAccessFile` concatenates `init segment ‖ media segment 1 ‖ … ‖ media segment N` into one logical byte space (segments fetched on demand over HTTP or from local paths, cached under `cache/segments/<sha256(url)>`), and keeps a `SegmentMap { logical_offset, length, url, segment_index, byte_range, period_id, discontinuity_sequence }`. The ISOBMFF reader runs unchanged on the `VirtualFile`; every `BoxNode.file_span` is a logical span and `ovt_container_segment_for_offset(offset)` returns the provenance (shown in the tree as a segment column and in the hex view status bar). `styp` at each media segment start is kept as a top-level node. HLS with TS segments → the MPEG-TS reader over the same `VirtualFile`. Encrypted HLS (`#EXT-X-KEY METHOD=AES-128`) segments are shown as encrypted spans; no decryption in OVT. Live playlists are snapshotted once (no refresh in phase 4).

---

## 8.7 libavformat demux wrapper (`ovt::demux`)

### 8.7.1 Interface

```cpp
// core/src/demux/demuxer.h  (C++20; the C API in core/include/ovt/decode.h wraps it)
namespace ovt::demux {

struct StreamInfo {
  int avstream_index;            // AVStream::index
  int container_track_id;        // AVStream::id (track_ID / TrackNumber / PID), -1 if none
  ovt::fb::Codec codec;          // mapped from AVCodecID (AV_CODEC_ID_H264 → H264, HEVC → H265, AV1 → AV1, VP9 → VP9, H266 → H266, MPEG2VIDEO → MPEG2, else Unknown)
  AVMediaType media_type;
  AVRational time_base;
  int64_t nb_frames;             // 0 if unknown
  int64_t duration;              // in time_base; AV_NOPTS_VALUE if unknown
  std::string codec_private_fourcc;   // "avcC","hvcC","av1C","vpcC","annexb","none"
  std::vector<uint8_t> codec_private; // see §8.7.5
  uint8_t nal_length_size;       // 1,2,4 for length-prefixed streams; 0 for Annex B; 0 for AV1
  bool length_prefixed;          // true for ISOBMFF/Matroska AVC/HEVC; false for Annex B (TS, .264)
};

struct UnitSpan { uint64_t offset_in_packet; uint32_t length; uint32_t prefix_length; };   // defined in demux/unit_split.h (WI-1.231) and included here; length prefix / start code bytes precede offset_in_packet

struct Packet {
  int avstream_index;
  int64_t sample_index;          // container sample index in the owning TrackInfo's SampleTable; -1 if unknown (§8.7.3)
  int64_t pts, dts, duration;    // AVPacket values, stream time_base
  int64_t file_pos;              // AVPacket::pos, -1 unknown
  bool keyframe;                 // AV_PKT_FLAG_KEY
  std::span<const uint8_t> data; // valid until the next read_packet()/seek()/destructor
  std::vector<UnitSpan> units;   // §8.7.4 split; empty for non-video
  // Returns the i-th unit in plugin-ABI form (no start code, no length prefix; AV1 OBU with obu_has_size_field=1). May point into a scratch buffer.
  std::span<const uint8_t> unit_bytes(size_t i) const;
};

class Demuxer {
public:
  // Opens through a custom AVIOContext over the RandomAccessFile (§8.7.2). hint_format: ContainerTree.format or "" (lets libavformat probe).
  static ovt::Result<std::unique_ptr<Demuxer>> open(std::shared_ptr<ovt::io::RandomAccessFile> file, std::string_view hint_format,
                                                     const ovt::fb::ContainerTree* tree /* nullable; enables sample_index mapping */);
  ~Demuxer();
  const std::vector<StreamInfo>& streams() const noexcept;
  // Selects the streams whose packets read_packet() returns (others are discarded with AVDISCARD_ALL).
  void select_streams(std::span<const int> avstream_indices);
  // Returns the next packet of a selected stream; an ok Result holding std::nullopt means end of stream. Never throws; libavformat errors → OVT_E_IO / OVT_E_CORRUPT / OVT_E_NOMEM.
  ovt::Result<std::optional<Packet>> read_packet();
  // Seeks so that the next packet of avstream_index is the sample with the given container index (requires tree) ...
  ovt::Status seek_to_sample(int avstream_index, int64_t sample_index);
  // ... or the last sync sample at or before time (stream time_base), backwards flag like AVSEEK_FLAG_BACKWARD.
  ovt::Status seek_to_time(int avstream_index, int64_t ts, bool backward = true);
  AVFormatContext* raw() noexcept;   // for the playback decoder (chapter 7); never exposed through the C API
};

// Matching per §8.1.4; writes TrackInfo.avstream_index into a mutable copy of the tree's tracks and returns diagnostics.
std::vector<ovt::fb::DiagnosticT> match_tracks(std::vector<ovt::fb::TrackInfoT>& tracks, const Demuxer&);
}
```

Thread-safety: one `Demuxer` per thread (backbone §0.7.2). `ovt::Result<T>` is the shared type of chapter 07 §7.1.6; `ovt::Status` is `namespace ovt::demux { using Status = ovt::Result<void>; }` (no end-of-stream `ovt_status` value exists; end of stream is the `std::nullopt` result of `read_packet()`).

Oracle parity: `ffprobe -show_packets` packet counts are an exact target for ISOBMFF and IVF (one packet per sample / temporal unit); for MPEG-TS and Annex B elementary streams ffprobe's count depends on parser access-unit splitting (`-fflags +nofillin`, AUD presence), so the parity tests of WI-1.230 accept ±1 packet there, and exact equality only when AUDs are present. The playback path owns one; the analysis path (when it uses Role B) owns another over the same `RandomAccessFile` (which is itself read-only and thread-safe for `read_exact`).

### 8.7.2 Custom `AVIOContext` adapter (`avio_adapter.cpp`)

1. `avio_alloc_context(buffer = av_malloc(256 KiB), 256 KiB, write_flag = 0, opaque = this, read_packet = &Adapter::read, write_packet = nullptr, seek = &Adapter::seek)`.
2. `read(opaque, buf, size)`: `n = min(size, file.size() − pos)`; `n == 0` → return `AVERROR_EOF`; `file.read_exact(pos, {buf, n})`; `pos += n`; return `n`. Exceptions → `AVERROR(EIO)`.
3. `seek(opaque, offset, whence)`: `whence & AVSEEK_SIZE` → return `file.size()`; `SEEK_SET/CUR/END` → compute, clamp `0 ≤ pos ≤ size`, return `pos`. Set `ctx->seekable = AVIO_SEEKABLE_NORMAL`.
4. `fmt = avformat_alloc_context(); fmt->pb = avio; fmt->flags |= AVFMT_FLAG_CUSTOM_IO;` then `avformat_open_input(&fmt, nullptr, hint ? av_find_input_format(hint) : nullptr, &opts)` (the custom `AVIOContext` must be attached to a pre-allocated context before the call) with `opts`: `"fflags" = "+genpts"` only for `mpegts`; for `mov,mp4,m4a,3gp,3g2,mj2` set `"advanced_editlist" = "0"` and `"ignore_editlist" = "0"` so libavformat applies the edit list the simple way (one offset), matching OVT's own `EditList` semantics; set `"export_all" = "1"` so that all metadata keys (`udta`/`meta` items that libavformat would otherwise drop) are exported for matching (`export_all` affects metadata, not stream creation; timecode `tmcd` streams are always exposed). Then `avformat_find_stream_info` with `"probesize" = 5 MiB` (the knob that bounds the probe for ISOBMFF, whose headers are enough) and defaults for TS.
5. `hint_format` mapping: `isobmff → "mov,mp4,m4a,3gp,3g2,mj2"`, `ivf → "ivf"`, `annexb → "h264"` or `"hevc"` by sniffed codec, `obu → "obu"` for low-overhead (Section 5) OBU streams and `annexb(av1) → "av1"` for AV1 Annex B streams — `libavformat/av1dec.c` registers two demuxers: `ff_obu_demuxer` (`.p.name = "obu"`, long name "AV1 low overhead OBU") and `ff_av1_demuxer` (`.p.name = "av1"`, long name "AV1 Annex B"); both exist unchanged in FFmpeg 7.1, `y4m → "yuv4mpegpipe"`, `raw → "rawvideo"` with `-video_size`, `-pixel_format`, `-framerate` options from `RawVideoFormat`, `ebml → "matroska,webm"`, `mpegts → "mpegts"`, `mxf → "mxf"`.
6. Android: the `RandomAccessFile` wraps a `pread` on a `ParcelFileDescriptor` (chapter 7/13); nothing else changes.

### 8.7.3 Stream enumeration and `sample_index` mapping

After open, `streams()` is filled from `fmt->streams[i]` (`id`, `codecpar`, `time_base`, `nb_frames`, `duration`). `select_streams` sets `st->discard = AVDISCARD_ALL` for others. OVT **never** inserts bitstream filters itself (`h264_mp4toannexb`, `hevc_mp4toannexb`, `av1_frame_split`, `extract_extradata` are forbidden — they would change bytes and offsets). Exception that cannot be avoided: FFmpeg's `obu` and `av1` (Annex B) demuxers internally run the `av1_frame_merge` BSF to form temporal units, so for raw AV1 streams `pkt->pos` is BSF-derived (usually −1) and packet boundaries are temporal units; byte offsets for those formats always come from OVT's own OBU walker (§8.6.4), never from `pkt->pos`.

`Packet.sample_index` (needed so `Unit.sample_index` and the hex view agree with the container tree):

1. If a `ContainerTree` with a `SampleTable` for the matched track exists: find the sample with `offset == pkt->pos` (binary search over `SampleTable.offset`, which is monotonic within a track for all real muxers; keep a per-stream cursor and try `cursor+1` first). Exact match required; libavformat's `mov` demuxer sets `pkt->pos` to the sample's file offset (`mov_read_packet`: `pkt->pos = sample->pos`).
2. If no match (edit-list-trimmed packets, `pos == -1` for TS/Matroska lacing): match by `dts` (`SampleTable.dts` rescaled to the stream time_base, tolerance ±1 tick); still none → `-1` and `CNT-0904` Info once per stream.
3. Matroska: `pkt->pos` is the position of the `SimpleBlock`/`Block` element, not the frame; use the EBML `SampleTable` whose `offset` is defined identically (phase 4).

### 8.7.4 Unit splitting (`unit_split.cpp`)

Shared by the demuxer (Role B) and by the own-reader analysis path (Role A, operating on sample bytes from `SampleTable`). Produces `UnitSpan`s and plugin-ABI unit bytes.

```cpp
namespace ovt::demux {
enum class Framing : uint8_t { LengthPrefixed, AnnexB, Av1Obu, Raw };
struct SplitResult { std::vector<UnitSpan> units; std::vector<ovt::fb::DiagnosticT> diagnostics; bool rewritten; /* AV1 size field inserted */ };
SplitResult split_units(std::span<const uint8_t> sample, Framing framing, uint8_t nal_length_size /* 1,2,4 */,
                        std::vector<uint8_t>& scratch /* receives only the rewritten last AV1 OBU when `rewritten` is set */,
                        bool temporal_delimiters_expected = false /* true for IVF / OBU-ES callers: a leading TD is still emitted but raises no CNT-9003 */);
// Returns unit i in plugin-ABI form; the last unit comes from `scratch` when `rewritten` is set. Packet::unit_bytes wraps it.
std::span<const uint8_t> unit_bytes(std::span<const uint8_t> sample, const SplitResult&, const std::vector<uint8_t>& scratch, size_t i);
// LengthPrefixed edge cases: nal_length_size ∉ {1,2,4} → CNT-9001 Error, the whole sample is one unit; a tail shorter than the length prefix → CNT-9001 Error, no unit; an unparsable AV1 header/leb128 → CNT-9001 Error, the rest of the sample is one unit.
}
```

Rules:

1. **LengthPrefixed (AVC/HEVC/VVC in ISOBMFF, Matroska):** `pos = 0`; while `pos + L ≤ size` (L = `nal_length_size` = `lengthSizeMinusOne + 1` from `avcC`/`hvcC`): `n = big-endian L-byte integer`; if `n == 0` → `CNT-9001` Warning ("zero-length NAL"), `pos += L`, continue; if `pos + L + n > size` → `CNT-9001` Error ("NAL length n exceeds sample"), the remainder becomes one unit with `length = size − pos − L` and splitting stops; else emit `UnitSpan{pos + L, n, L}`; `pos += L + n`. The plugin receives `sample[pos+L .. pos+L+n)` — **the length prefix is stripped and no start code is added** (the plugins' decoders are configured with `avcC`/`hvcC` and expect raw NAL units; the ABI contract in backbone §0.6.2 says "Annex B NAL without start code"). If the first 4 bytes of a sample are `00 00 00 01` and `L == 4` (a muxer bug: Annex B inside `avc1`) → `CNT-9002` Warning and fall back to `AnnexB` splitting for that sample.
2. **AnnexB (TS, `.264`/`.265`, Matroska with Annex B — rare):** the §8.6.3 algorithm over the sample bytes; units are NALs without start code.
3. **Av1Obu (ISOBMFF `av01`, Matroska `V_AV1`, IVF):** `pos = 0`; while `pos < size`: parse `obu_header` (1–2 bytes); if `obu_has_size_field`: `obu_size = leb128`, `unit_len = header + leb + obu_size`; else: the OBU extends to the end of the sample (AV1-ISOBMFF §2.4 allows this only for the **last** OBU; if it is not the last → cannot happen by construction), `unit_len = size − pos`, and the OBU MUST be **rewritten** into the `scratch` buffer with `obu_has_size_field = 1` and an inserted `leb128(obu_size)` so the plugin (`push()` takes "one OBU with size field") gets a self-delimiting OBU; `rewritten = true`; `Unit.file_span` still references the original bytes, `prefix_span.length = 0`. `OBU_TEMPORAL_DELIMITER` inside an ISOBMFF sample → `CNT-9003` Warning, unit still emitted (plugins ignore TDs). IVF/Annex B-OBU inputs: the TU already starts with a TD; keep it. For libaom's inspection decoder the whole temporal unit is pushed OBU by OBU in order (chapter 7 decides whether to batch).
4. **Raw (Y4M/raw YUV):** one unit = the whole frame; `prefix_length = 0`.
5. `OBU`/NAL `obu_size`/length overrun past the sample end → `CNT-9001` Error, unit clamped.

### 8.7.5 Seeking

- `seek_to_sample(stream, i)`: requires `tree`; `ts = SampleTable.dts[i]` rescaled from track timescale to `AVStream.time_base` (`av_rescale_q`); `avformat_seek_file(fmt, stream, INT64_MIN, ts, ts, AVSEEK_FLAG_BACKWARD)`; `avcodec_flush_buffers` is the decoder's job (chapter 7); then read packets, discarding until `Packet.sample_index == i` (or `dts ≥ ts` when `sample_index == -1`). libavformat seeks to a keyframe at or before `ts`; the analysis path relies on that to start at a RAP. Returns `OVT_E_INVALID_ARG` if `i ≥ count`.
- `seek_to_time(stream, ts, backward)`: `avformat_seek_file` with `AVSEEK_FLAG_BACKWARD` when `backward`; else flags 0.
- For fragmented MP4 libavformat uses `sidx`/`mfra` or scans `moof`s; OVT does not help it. For Annex B/OBU elementary streams libavformat cannot seek by time without an index; `seek_to_sample` therefore uses the OVT `StreamIndex` (chapter 7) to find the file offset of the unit and performs `avio_seek(fmt->pb, offset, SEEK_SET)` followed by `avformat_flush` — allowed because OVT owns the `AVIOContext`.

### 8.7.6 `codec_private` extraction for plugin `create()`

| `codecpar->codec_id` | Source | `codec_private` passed to `create(codec_private, len)` | `codec_private_fourcc` |
|---|---|---|---|
| H264, ISOBMFF/Matroska | `codecpar->extradata` begins with `0x01` (configurationVersion) | the bytes verbatim (= `avcC` payload) | `avcC` |
| H264, Annex B (TS, `.264`) | `extradata` begins with `00 00 00 01` or is empty | `nullptr, 0`; the SPS/PPS NALs are pushed as units before the first slice (they are in the stream anyway); if `extradata` holds Annex B PS, split them with §8.6.3 and push them first | `annexb` |
| HEVC, ISOBMFF/Matroska | `extradata[0] == 0x01` and length ≥ 23 | verbatim (`hvcC` payload) | `hvcC` |
| HEVC, Annex B | as H264 | `nullptr` | `annexb` |
| AV1, ISOBMFF/Matroska | `extradata` begins with `0x81` (`marker=1, version=1`) and length ≥ 4 | verbatim (`av1C` payload incl. the 4-byte record header and `configOBUs`, as libavformat `mov.c` exports it at the FFmpeg 7.1.1 pin — chapter 13 §13.1; the host passes it unchanged) | `av1C` |
| AV1, IVF/OBU | empty | `nullptr`; the sequence header OBU arrives in the first TU | `none` |
| VP9 | libavformat's `mov_read_vpcc` (`libavformat/mov.c`) parses `vpcC` into `codecpar->profile`, `bits_per_raw_sample` and the colour fields and leaves `extradata` empty (VP9 has no codec-private in FFmpeg); use `TrackInfo.codec_private` from Role A for the raw `vpcC` payload | verbatim | `vpcC` |
| others | — | `nullptr` | `none` |

Rule: when a `ContainerTree` is available, **prefer** `TrackInfo.codec_private[0].bytes` from Role A (it has the box offset for hex linkage) and use the libavformat copy only for verification (§8.1.4 step 4). The plugin host (chapter 7) calls `create(bytes, len, threads, &dec)` with these bytes.

---

## 8.8 Hex view data service

The GUI's hex panel, the "click a field → highlight bytes" feature and the Python API all read file bytes **through the core**, never by mapping the file themselves (FR-CNT-104, §8.1.2). Reasons: Android content URIs cannot be mmapped reliably; `VirtualFile` (DASH/HLS) has no backing file; one page cache per `ovt_Container` keeps memory bounded.

```c
/* core/include/ovt/container.h */
/* Copies up to len bytes starting at file offset into *out. Ownership (chapter 12 §12.3.1 rule 3a, D4): the caller owns *out and releases it with
   ovt_buffer_free(); the copy is independent of the page cache. len ≤ 1 MiB (OVT_E_INVALID_ARG otherwise). Reads past EOF are clipped:
   out->size < len and no error. offset ≥ file size → out->size == 0. Thread-safe (see below). */
ovt_status ovt_container_bytes(ovt_Container*, uint64_t offset, uint32_t len, ovt_Buffer* out);
/* Unit bytes (backbone §0.6.1): out_raw = bytes as stored (length prefix / start code excluded; AV1 OBU re-framed with size field when the container stored it without one);
   out_rbsp = emulation-prevention-free payload for H.26x (empty for AV1 / raw). Either pointer may be NULL. Both buffers are caller-owned (rule 3a, ovt_buffer_free). */
ovt_status ovt_stream_unit_bytes(ovt_Stream*, uint32_t unit_index, ovt_Buffer* out_raw, ovt_Buffer* out_rbsp);
/* Maps a file offset to the innermost BoxNode containing it (for hex → tree navigation). Returns the box's file offset and depth; OVT_E_INVALID_ARG if outside any box. */
ovt_status ovt_container_box_at(ovt_Container*, uint64_t offset, uint64_t* out_box_offset, int* out_depth);
/* For VirtualFile containers: which segment a logical offset belongs to (url, segment index, byte range). JSON string in *out_json (caller-owned, rule 3a). */
ovt_status ovt_container_segment_for_offset(ovt_Container*, uint64_t offset, ovt_Buffer* out_json);
/* Total logical size (file size or VirtualFile size). Never fails. */
uint64_t   ovt_container_size(const ovt_Container*);
/* Page-cache statistics {"hits":..,"misses":..,"bytes_read":..} as a JSON string (caller-owned, rule 3a). */
ovt_status ovt_container_stats_json(ovt_Container*, ovt_Buffer* out_json);
```

**Page cache (`byte_service.cpp`):** `PageCache { page_size = 64 KiB; capacity = 64 pages (4 MiB); LRU list; std::unordered_map<page_no, Page> }` per `ovt_Container`, guarded by a mutex. **Threading contract (the documented exception to chapter 12 §12.3.1 rule 4):** `ovt_container_bytes`, `ovt_container_box_at`, `ovt_container_segment_for_offset`, `ovt_container_size` and `ovt_container_stats_json` are thread-safe and may be called concurrently with any other call on the same `ovt_Container` (including a running `ovt_container_fragments` job), because they only take the page-cache mutex and read the immutable parts of the tree; all other `ovt_container_*` functions keep rule 4. The GUI therefore calls `ovt_container_bytes` from the UI thread for visible rows — a "cheap getter" in the sense of backbone §0.7.2 as long as the page is cached; a miss costs one `pread`; the GUI pre-fetches the pages of the visible range plus one page each side on a job thread. `RandomAccessFile::view()` (§8.2.2) is implemented on top of the same cache for the reader's windowed decoding (`PagedSpan`: a `BitReader` source that fetches pages on demand and asserts a ≤ 1 MiB window). Statistics (`hits`, `misses`, `bytes_read`) are exposed via `ovt_container_stats_json` for the FR-CNT-004 / FR-GEN-011 tests.

**GUI rules (normative for chapter 11):** the hex view requests `[row_start, row_start + rows × 16)` through `ovt_container_bytes`; selection of a `Field` highlights `box.file_span.offset + span.bit_offset / 8` for `ceil((bit_offset % 8 + bit_length) / 8)` bytes, with the bit range drawn as a sub-highlight in the binary column; `ovt_container_box_at` resolves a click in the hex view to the tree node; for units (syntax view) `ovt_stream_unit_bytes(out_raw)` is the byte source and `Unit.epb_positions` (backbone §0.5.3) maps RBSP bit spans to raw bytes.

---

## 8.9 Test oracles, golden tests and corpora

### 8.9.1 Oracles and their commands

| Oracle | Install (CI) | Command | What is compared |
|---|---|---|---|
| GPAC MP4Box ≥ 2.4 (LGPL; test-time only) | `apt install gpac` / `brew install gpac` | `MP4Box -diso -out tree.xml f.mp4` (full box XML, no offsets); `MP4Box -dts -out dts.txt f.mp4` (per-sample DTS/CTS/size/offset/RAP); `MP4Box -info f.mp4`; `MP4Box -boxes` (registry cross-check) | box set and sizes per path (depth-first), FullBox version/flags, decoded field values by name map (`tools/oracle_maps/mp4box_fields.json`); sample table rows. **Scope:** only boxes MP4Box decodes are compared field-by-field — the oracle subset is the key set of `mp4box_fields.json`; boxes MP4Box prints as `UnknownBox`/hex (`cclv`, `dvvC` details, `tapt` children, vendor `uuid`s) are compared by `(path, size)` only and their fields by mp4box.js/MediaInfo/mp4ff where those print them, else by the span round-trip test |
| mp4ff `mp4ff-info` ≥ 0.50 (MIT) | `go install github.com/Eyevinn/mp4ff/cmd/mp4ff-info@<pinned>` | `mp4ff-info -l all f.mp4` | box tree with field dumps, `StartPos` where printed; CENC/`senc`/`sidx`/`emsg` fields |
| MediaInfo CLI ≥ 24 (BSD-2) | `apt install mediainfo` | `mediainfo --Details=1 f.mp4` (trace with offsets), `mediainfo --Output=JSON f.mp4` (interpreted HDR/colour) | element offsets and sizes; HDR format strings (`HDR10`, `HDR10+`, `Dolby Vision, Version 1.0, dvhe.08.06, BL+RPU, HDR10 compatible`) vs `HdrMetadata.kind`/`dv_profile`/`dv_bl_compat_id` |
| mp4box.js (BSD-3) | `npm ci` in `tools/oracles/mp4boxjs/` (pinned `package-lock.json`) | `node tools/oracles/mp4boxjs/dump.js f.mp4` → JSON of `box.start`, `size`, `hdr_size`, `type`, decoded fields | offsets and header lengths of every box; field values for `colr/mdcv/clli/cclv`, `avcC/hvcC/av1C`, `sidx`, `emsg`, `tfhd/trun` |
| ffprobe (LGPL) | FFmpeg from `third_party/` | `ffprobe -v trace -i f.mp4 2>&1 | grep "type:'"` (box offsets/sizes), `ffprobe -show_packets -show_entries packet=pos,dts,pts,size,flags`, `ffprobe -show_streams -show_entries stream_side_data` | top-level and `moov` box offsets; packet pos/dts/pts vs `SampleTable`; Mastering Display / Content Light / DOVI configuration record side data |
| Bento4 `mp4dump` (GPLv2; test-time only, optional) | `apt install bento4` or prebuilt | `mp4dump --format json f.mp4` | fields for CENC boxes (`tenc`, `senc`, `pssh`), `tx3g` |
| dovi_tool / hdr10plus_tool (MIT) | prebuilt binaries | `dovi_tool info -i es.hevc -f 0`, `hdr10plus_tool extract -i es.hevc -o out.json` | RPU presence/profile, HDR10+ presence/`num_windows` per frame vs `HdrMetadata.signals` |
| mkvinfo (GPLv2, phase 4) | `apt install mkvtoolnix` | `mkvinfo -v -v -z f.mkv` | EBML element offsets, sizes, values |
| TSDuck (BSD-2, phase 4) | `apt install tsduck` | `tsanalyze --json f.ts`, `tstables --all-sections --xml` | PID list, PCR/PTS stats, PSI tables |

### 8.9.2 Golden-test procedure (chapter 14 tooling; container-specific comparison rules)

Oracle programs, normalisers, golden files and the CTest glue are owned by chapter 14 (backbone §0.10): oracle recipes §14.4.6 (MP4Box `-diso`/`-dts`/`-dnal`), §14.4.7 (`mp4ff-info -l all`), §14.4.8 (`mediainfo --Details=1`), §14.4.9 (mp4box.js), ffprobe (§14.4.1 family); normalisers `tests/oracles/normalize/{mp4box_diso,mp4ff_info,mediainfo_trace}.py` and `tests/oracles/mp4boxjs_dump.mjs` with the field maps `tests/oracles/normalize/{gpac,mp4ff}_field_map.json` (chapter 14) plus the container maps `tools/oracle_maps/*.json` contributed by WI-1.236; canonical JSON families `box-tree` and `sample-table` (§14.4 intro); goldens under `tests/golden/isobmff/` produced in the oracle image (WI-0.554/WI-0.555/WI-0.564) and compared by `ovt_add_golden_test(...)` of WI-0.556 (`OVT_UPDATE_GOLDEN=1` regenerates; mismatch prints a unified diff); the ISOBMFF suite is WI-1.571 (`golden_isobmff_<oracle>_<id>`). Known oracle disagreements live in `tests/golden/KNOWN_DIFFS.md` (WI-0.564) and are suppressed per `(oracle, path, field)`. There is no `tools/golden.py generate|compare` and no `tools/golden/normalize_*.py` (chapter 16 §16.3.9, §16.3.13); `tools/golden.py coverage` (WI-1.236) is the only chapter-8 tool and reports hand-decoder coverage against FileFormatConformance `file_features`.

What the container goldens compare (these rules are the chapter-8 contribution; the suite of WI-1.571 implements them with the chapter-14 comparer):

1. **Tree set equality:** the set of `(path, size)` from `ovt probe --boxes --expand all -f json` (`box-tree` family) equals every oracle's set (after suppressions). Missing or extra box → test fails.
2. **Offsets:** every box offset equals mp4box.js `start` and the MediaInfo trace offset; `header_length` equals mp4box.js `hdr_size` minus 4 for FullBoxes, minus 8 for sample entries and minus 16 for `uuid` boxes (normalised by `tests/oracles/normalize/mp4boxjs.py`; the documented differences live in `tests/golden/isobmff/KNOWN_DIFFS.md`).
3. **Fields:** for every field name present in an oracle's normalised output and in OVT's `fields`, values are equal (field names mapped to ISO syntax names through the field maps, e.g. MP4Box `TrackID` → `track_ID`, `SampleCount` → `sample_count`; values: integers as decimal, fixed point as the raw integer, fourccs as strings, byte blobs as lowercase hex, booleans `0/1`). Fields OVT decodes that no oracle prints are checked by the span round-trip test (FR-CNT-002) instead.
4. **Sample table:** every row of `MP4Box -dts` (sample number, DTS, CTS, size, offset, RAP flag) equals OVT's `SampleTable` row (`sample-table` family; media times, not presentation). Additionally ffprobe `-show_packets` `pos`/`dts` match for the first 1000 packets of each track.
5. **HDR:** `HdrMetadata.kind`, `dv_profile`, `dv_bl_compat_id` and the `colr` triple equal MediaInfo JSON (`HDR_Format`, `HDR_Format_Profile`, `HDR_Format_Compatibility`, `colour_primaries`, `transfer_characteristics`, `matrix_coefficients`) via `tools/oracle_maps/mediainfo_hdr.json`.
6. **Diagnostics:** entries tagged `isobmff-broken` carry `expected.diagnostics: ["CNT-0108", ...]` in the manifest; the test asserts each expected code appears at least once and that no `Error`-severity code outside the expected list appears.
7. **Performance** (`ctest -L perf`, nightly): FR-CNT-018 timings run through the WI-0.560 harness as `bench_container_open_moov_1h` and siblings (§14.9.3), budgets in `tests/perf/budgets.json`.

### 8.9.3 Corpora (entries in the single `tests/corpora/manifest.json` of chapter 14 §14.2.4, fetched by `tools/fetch_corpora.py`, never committed)

The manifest shape is chapter 14 §14.2.4 (`entries[]` with `id`, `source`, `url`, `sha256`, `size_bytes`, `licence`, `licence_spdx`, `redistributable`, `codecs`, `containers`, `extract`, `features`, `tests`, `tags`); there are no per-directory manifests and no `--lane` flag. Container entries (WI-1.237) use `tags: ["container-smoke"]` (≤ 300 MiB, one file per feature, part of `fast`) or `["container-full"]`, `tests: ["box-tree", "sample-table", "hdr"]`, and an additional per-entry object `expected: { "format": "isobmff", "codec": "H265", "hdr_kind": "DolbyVision", "diagnostics": ["CNT-…"] }` that the container tests read (the schema of §14.2.4 allows this extra object). Fetching: `tools/fetch_corpora.py --tags container-smoke` (chapter 14 grammar; exit codes 0/2/3/4).

| Corpus | URL | Use |
|---|---|---|
| MPEG FileFormatConformance (Git LFS, BSD-3-Clear metadata; media under ISO conformance terms) | https://github.com/MPEGGroup/FileFormatConformance (`data/file_features/**` + LFS media) | per-spec feature coverage: every box/version/flag combination; the JSON `file_features` lists which boxes each file exercises — `tools/golden.py coverage` (WI-1.236) reports hand-decoder coverage against it |
| GPAC test media | https://github.com/gpac/testsuite (`media/` tree, fetched selectively: `auxiliary_files`, `isomedia`, `dash`, `cmaf`, `hevc`, `av1`, `encryption`, `hdr`) | broad quirks: QuickTime, 3GPP, HEIF, CENC, DASH |
| Bento4 test files | https://github.com/axiomatic-systems/Bento4/tree/master/Test (and https://www.bento4.com/downloads/ sample content) | CENC/PIFF/PlayReady/Widevine `pssh`, `senc`, `saio`, Smooth Streaming uuids |
| DASH-IF test vectors | https://testassets.dashif.org/ , https://dash.akamaized.net/ (e.g. `/akamai/bbb_30fps/`, `/dash264/TestCases/`) | `sidx`, `styp`, `emsg`, multi-period MPD, CMAF |
| CTA WAVE / CMAF test content | https://github.com/cta-wave/Test-Content , https://dash.akamaized.net/WAVE/ | `cmfc`/`cmf2` structural brand conformance; `tfdt`/`sidx` consistency |
| Apple HLS examples (fMP4) | https://developer.apple.com/streaming/examples/ (`bipbop_adv_example_hevc`, `bipbop_16x9`) | HLS fMP4 (`hvc1`, `tfdt`, `emsg` ID3), `#EXT-X-MAP` |
| Dolby Vision samples | Dolby developer test clips (https://professionalsupport.dolby.com/ — login; mirror pins in manifest) and Dolby Vision "Profile 5/8.1" clips from https://github.com/quietvoid/dovi_tool/tree/main/assets (tests) | `dvcC`/`dvvC`, `dvh1`/`hvc1+dvvC`, RPU NAL 62 |
| HDR10+ samples | https://github.com/quietvoid/hdr10plus_tool/tree/main/assets ; Samsung HDR10+ demo clips (https://hdr10plus.org/) | T.35 SEI detection, `mdcv`/`clli` vs SEI mismatch |
| AOM AV1 conformance / `av1-isobmff` samples | https://aomedia.org/ ; https://github.com/AOMediaCodec/av1-isobmff (test content list), https://storage.googleapis.com/aom-test-data/ (IVF, OBU Annex B / Section 5) | `av01`, `av1C.configOBUs`, IVF, raw OBU detection |
| WebM/Matroska (phase 4) | https://github.com/webmproject/libwebm/tree/main/testing/testdata ; https://github.com/Matroska-Org/matroska-test-files | EBML reader |
| MPEG-TS (phase 4) | https://github.com/tsduck/tsduck-test (`input/*.ts`) ; https://streams.videolan.org/ | PSI/PES/PCR, TR 101 290 |
| HEIF/AVIF | https://github.com/nokiatech/heif_conformance ; https://github.com/AOMediaCodec/av1-avif/tree/master/testFiles | `meta`/`iinf`/`iloc`/`ipco`/`ipma`/`iref` |
| Broken/fuzzed files (own, committed, tiny) | `tests/corpora/synthetic/isobmff-broken/` generated by `tools/make_broken.py` (invoked from `tests/corpora/synthetic/generate.py`, WI-0.553) from a 20 KiB base file: truncated at 37 offsets, size 0/1/7/0xFFFFFFFF injected, `moov` after `mdat`, overlapping boxes, `stsc` descending, `stco` beyond EOF, `ctts` v0 negative, duplicate `tkhd`, 33-deep nesting, `uuid` short, non-printable types | FR-CNT-101 (§8.1.2) and `CNT-*` code coverage; every code in §8.5 MUST be triggered by at least one file (`ctest -R diag_coverage`) |

### 8.9.4 Fuzzing targets (`core/tests/fuzz/`, libFuzzer, `OVT_ENABLE_FUZZ`)

| Target | Input | Seed corpus |
|---|---|---|
| `fuzz_isobmff_tree` | bytes → `RandomAccessFile` (memory) → `ovt_container_open` + `ovt_container_tree` + `ovt_container_expand` on every box | `tests/corpora/synthetic/isobmff-broken/` (ids `syn_mp4_broken_*`) + first 64 KiB of every corpus file |
| `fuzz_isobmff_sample_table` | same + `build_sample_table` for every track | same |
| `fuzz_isobmff_fragments` | same + `build_fragment_timeline` | CMAF seeds |
| `fuzz_box_decoder_<fourcc>` (generated for every hand decoder by `tools/gen_fuzz_targets.py`) | payload bytes → that decoder with a synthetic `BoxContext` | payloads extracted from corpora by `tools/extract_box_payloads.py` |
| `fuzz_annexb_split`, `fuzz_obu_split`, `fuzz_ivf`, `fuzz_y4m`, `fuzz_raw_format_string` | bytes | ES corpora, format strings |
| `fuzz_unit_split` | `(framing byte, nal_length_size byte, sample bytes)` | AVC/HEVC/AV1 samples |
| `fuzz_demuxer_open` | bytes → libavformat through the adapter (sanitizer build of FFmpeg) | small MP4/TS/MKV |
| `fuzz_ebml_tree`, `fuzz_mpegts` (phase 4) | bytes | MKV/TS seeds |

Every target MUST reach 10 minutes without crash/timeout/leak under ASan+UBSan in the nightly `fuzz-smoke` lane; `-max_len=1048576`, `-timeout=10`, `-rss_limit_mb=2048`.

---

## 8.10 Work items

Common conventions for every WI below: all code is C++20 under `core/src/container/` or `core/src/demux/`, licensed LGPL-2.1-or-later; unit tests are Catch2 under `core/tests/container/` (or `core/tests/demux/`); every parser reads through `ovt::BitReader`; every WI adds its diagnostic codes to `diag_codes.inc`; acceptance tests name the Catch2 tag (`[container][isobmff]`) or the exact CLI command. "Corpus file X" means the entry with `id` X in the single `tests/corpora/manifest.json` (chapter 14 §14.2.4). Ids of the form `syn_*` are chapter 14 §14.3.3 recipes; every other id named in an acceptance test below (public-corpus files such as `cta_cmaf_video_cmfv`, `dashif_sidx_m4s`, `bento4_cenc_subsample`, `heif_heic_grid`) is added to the manifest by WI-1.237 from the sources of §8.9.3 — an acceptance test MUST NOT name a file that is in neither place.

### Phase 1 (WI-1.200 … WI-1.239)

### WI-1.200 — Create the container module skeleton and format sniffer
**Goal:** Put the `container` module, its C API surface and the format sniffer in place so later WIs only add decoders.
**Depends on:** WI-1.100 (`io::RandomAccessFile`), WI-1.102 (`BitReader`), WI-0.083 (FlatBuffers build), WI-1.512 (logging).
**Produces:** `core/include/ovt/container.h` (declarations only, copied from chapter 12 §12.3.7/§8.8 — the header text is owned and finalised by chapter 12 WI-1.511); `core/src/container/{format_sniff.cpp, container.cpp, diag_codes.inc, diagnostics.h}`; generated headers from `core/schema/ovt_container.fbs` (schema text is chapter 06 §6.4, landed by WI-0.081 — this WI does not edit it); `core/tests/container/sniff_test.cpp`.
**Interface:** backbone §0.6.1 `ovt_container_*`; §8.6.1 `sniff()`; §8.8 C functions (stubs returning `OVT_E_UNSUPPORTED` until WI-1.234). `ovt_File` is forward-declared in `container.h` and defined internally by this WI (wrapping `io::RandomAccessFile`); WI-1.511 adds `ovt/file.h` and the `ovt_file_open*` functions on that struct. `ovt_container_list_diagnostics` returns `OVT_E_UNSUPPORTED` until the `DiagnosticRegistry` table is added to `ovt_container.fbs` by a spec amendment before WI-1.503; the data source is the C++ `ovt::container::all_diagnostics()`.
**Steps:** 1. Wire `ovt_container.fbs` (WI-0.081) into the module build and generate headers. 2. Implement `sniff()` exactly as §8.6.1 steps 1–11. 3. Implement `ovt_container_open/close/tree/expand` dispatching on `Format` to per-format readers (stubs). 4. Add `diag_codes.inc` with the X-macro and `ovt::container::diag(code)` helper; add `ovt probe --list-diagnostics` data source. 5. Tests for sniffing every manifest entry tagged `container-smoke` (chapter 14 §14.2.4; elementary streams and ISOBMFF alike) and the ISOBMFF first-box variants (`ftyp`, `moov`-first QuickTime, `styp` segment, size-0 `mdat`).
**Acceptance tests:** 1. Given every `tests/corpora/manifest.json` entry tagged `container-smoke` (chapter 14 §14.2.4; there are no per-format corpus directories), When `sniff()` runs, Then the result equals the entry's `expected.format` (`[container][sniff][needs-corpus]`). 2. Given a 7-byte file, When `ovt_container_open` runs, Then it returns `OVT_E_UNSUPPORTED` without crashing.
**Size:** M.
**Notes / pitfalls:** QuickTime files may start with `wide` or `free`; HEVC Annex B first NAL is often `VPS (32)`; do not read beyond 64 KiB in the sniffer.

### WI-1.201 — Implement the ISOBMFF box header walk with spans and truncation handling
**Goal:** Produce a complete `BoxNode` tree (no field decoding yet) for any ISOBMFF file, with exact `file_span`/`header_length` and all `CNT-01xx` diagnostics.
**Depends on:** WI-1.200, WI-0.553 (`tools/make_broken.py`).
**Produces:** `core/src/container/isobmff/box_walk.{h,cpp}`, `box_context.h` (`BoxContext`), `core/tests/container/box_walk_test.cpp`, `tests/corpora/synthetic/isobmff-broken/*` (the 13 named files of §8.9.3, about 240 KiB, committed under CC0 via `REUSE.toml` and marked `binary` in `.gitattributes`, generated with `tools/make_broken.py` of chapter 14 WI-0.553; the 37-offset truncation sweep runs in memory and is not committed).
**Interface:** §8.2.3 algorithm; §8.2.5 `BoxNode` rules; `walk_children` signature.
**Steps:** 1. Implement steps 1–3 of §8.2.3 with a hard-coded container list (`moov trak mdia minf stbl edts dinf udta mvex moof traf mfra meta ipco iprp sinf schi tref wave gmhd tapt`) until the registry (WI-1.203) replaces it. 2. Implement truncation clamp, size 0/1, `uuid`, `wide`, non-printable types, depth limit. 3. Implement `mdat`/`idat`/`free`/`skip` as Opaque. 4. Write `tools/make_broken.py` producing the §8.9.3 broken corpus from a 20 KiB base MP4. 5. Tests: sum-of-spans equals file size; every broken file yields its expected code; ASan clean.
**Acceptance tests:** 1. Given every corpus MP4, When `walk` runs, Then Σ top-level `file_span.length == file_size` and the `(path,size)` set equals `MP4Box -diso` (golden, WI-1.236) (`[container][isobmff][walk]`). 2. Given `tests/corpora/synthetic/isobmff-broken/size7_in_moov.mp4`, When walked, Then `CNT-0106` is emitted and the siblings after it are still listed. 3. Given a `moov` whose size exceeds EOF by 1000, When walked, Then `CNT-0108` is emitted and `trak` children are present.
**Size:** M.
**Notes / pitfalls:** `size == 0` only at top level; `wide` is exactly 8 bytes; never trust `largesize` without checking `≥ 16`; `__builtin_add_overflow` on every offset addition. Until WI-1.203 the walker uses a hard-coded kind table (the container list of step 1, the Opaque boxes and a short list of well-known leaves for the step 2.6 recovery); `CNT-1001` and FullBox version/flags for non-`meta` boxes wait for the registry, `meta` follows §8.2.6 rule 4 in the walker.

### WI-1.202 — Implement `FieldBuilder`, `BoxContext` propagation and the decoder registration macro
**Goal:** Give hand decoders their toolkit: span-recording field readers, grouping, derived text, context flags, and `OVT_BOX_DECODER` registration with pattern lookup.
**Depends on:** WI-1.201.
**Produces:** `core/src/container/isobmff/{field_builder.h, field_builder.cpp, box_registry.h, box_registry.cpp}`; tests `core/tests/container/field_builder_test.cpp`, `core/tests/container/registry_lookup_test.cpp`.
**Interface:** §8.2.8 `FieldBuilder`; §8.2.9 `BoxDecoder`, `RegistryEntry`, `lookup()`, `register_decoder()`, `OVT_BOX_DECODER`.
**Steps:** 1. Implement every `FieldBuilder` method; spans = `(bit_pos_before − box_start_bits, bits consumed)`. 2. Implement `materialise()` large-array policy. 3. Implement `datetime1904`, `lang_packed`, `matrix`, `fixed16_16` derivations. 4. Implement context-pattern glob matching (`*`, `*/x`, `*/x/*`, exact) with specificity ordering (more literal segments win). 5. Implement `register_decoder` and lazy join with the (hand-coded for now) entry table. 6. Unit tests with synthetic payloads, including a decoder that throws mid-way (fields before the throw kept, `CNT-1002` added).
**Acceptance tests:** 1. Given payload `00 00 00 01 00 00 00 02`, When `fb.u32("a"); fb.u32("b")`, Then spans are `{0,32}` and `{32,32}` relative to the box start plus header bits (`[container][fieldbuilder]`). 2. Given patterns `*/stsd/*` and `*/ipco` registered for `colr`, When looked up with ancestors `[meta,iprp,ipco]`, Then the `ipco` decoder is returned. 3. Given a 2000-element array, When built, Then exactly 80 element children exist and the parent `uvalue == 2000`.
**Size:** M.
**Notes / pitfalls:** `FullBox` version/flags fields must be emitted by the walker (not decoders) so every FullBox has them even with no decoder; bit spans for `u(n)` fields that cross byte boundaries are legal.

### WI-1.203 — Write `tools/gen_box_registry.py` and generate `box_registry.gen.cpp`
**Goal:** Replace the hard-coded container list with the generated registry from MP4RA CSV + FileFormatConformance JSON + `ovt_extra_boxes.json`.
**Depends on:** WI-1.202.
**Produces:** `tools/gen_box_registry.py`, `tools/registry_inputs/{mp4ra_*.csv, ffc/*.json, ovt_extra_boxes.json, REGISTRY_DIFF.md, INPUTS.sha256}`, `core/src/container/isobmff/box_registry.gen.cpp`, CI step `registry-check`, `core/tests/container/registry_gen_test.cpp`.
**Interface:** §8.2.10 inputs/outputs/algorithm; `RegistryEntry` layout.
**Steps:** 1. Write the fetch step (`--fetch`, each download into a fresh empty directory; run with `python3 -I`). 2. Parse CSVs (handle `$20`, `(c)`), parse JSON, merge extras. 3. Classify `kind`/`is_fullbox`/versions/flags per §8.2.10 step 4. 4. Emit the C++ table + brands/handlers/item types/schemes tables + header comment with input hashes. 5. Emit `REGISTRY_DIFF.md`; `--check` mode diffing the committed output. 6. Test: every fourcc used by hand decoders (grep) exists; `lookup("mvhd")` is FullBox with versions {0,1}.
**Acceptance tests:** 1. Given the committed inputs, When `python3 -I tools/gen_box_registry.py --check` runs, Then exit code 0 and no diff. 2. Given `lookup("stsd", ctx{stbl})`, When it is called, Then `kind == ContainerWithPrefix && is_fullbox`. 3. Given an unknown fourcc `zzzz`, When it is parsed, Then `lookup` returns `nullptr` and walking a box of that type yields `CNT-1001`.
**Size:** M.
**Notes / pitfalls:** The conformance JSON lists several entries per fourcc with different containers — emit one row each; a fourcc present only in MP4RA has `allowed_parents == ""` (do not emit `CNT-1013` for those); keep the generator dependency-free (stdlib only).

### WI-1.204 — Implement lazy traversal, `ovt_container_expand` and `mfro`/`mfra` location
**Goal:** Make opening a multi-GB or fragmented file fast: eager `moov`, header-only `moof`, on-demand expansion, index boxes first.
**Depends on:** WI-1.203.
**Produces:** changes in `box_walk.cpp`; `core/src/container/isobmff/lazy.cpp`; tests `core/tests/container/lazy_test.cpp`. (Reduced per chapter 16 §16.3.5: the 1-hour perf file is the synthetic recipe `syn_h264_1h_1080p` of chapter 14 §14.3.3, generated by `tests/corpora/synthetic/generate.py` of WI-0.553 — no `tools/make_perf_files.py`.)
**Interface:** §8.2.4 rules 1–7; `ovt_container_expand(ovt_Container*, uint64_t box_offset)`.
**Steps:** 1. Add `expanded` flag per node and `expand(node)` that performs step 2.12 of §8.2.3. 2. Implement the `mfro` tail read (last 16 bytes) and `mfra` pre-parse. 3. `moof` policy: header walk only unless ≤ 64 `moof`s. 4. Windowed payload reads via `PagedSpan` for large leaves (`stsz`, `stco`, `co64`, `stts`, `ctts`, `trun`, `senc`, `iloc`). 5. Tests with `RandomAccessFile::bytes_read()` assertions.
**Acceptance tests:** 1. Given `syn_h264_1h_1080p.mp4` (chapter 14 §14.3.3 recipe 27; `moov` first, multi-GiB `mdat`), When opened, Then `bytes_read() < 16 MiB`, and `bench_container_open_moov_1h` (`ctest -L perf -R container_open`) meets its budget of chapter 14 §14.9.3 (see `tests/perf/budgets.json`; D19). 2. Given a CMAF file with 3600 `moof`s, When opened, Then no `traf` child exists until `ovt_container_expand(moof_offset)` is called, after which `tfhd/tfdt/trun` appear. 3. Given a file with `moov` after a 2 GiB `mdat`, When opened, Then `CNT-0003` is emitted and tracks are present.
**Size:** M.
**Notes / pitfalls:** `ovt_container_tree` must return a **new** FlatBuffer after each expand (buffers are immutable); keep the internal tree as mutable C++ objects (`BoxNodeT`) and serialise on demand.

### WI-1.205 — Hand decoders, group A (file and movie level)
**Goal:** Decode every group A box of §8.3 with spans and derived values.
**Depends on:** WI-1.202.
**Produces:** `core/src/container/isobmff/decoders/movie.cpp`; tests `core/tests/container/decoders_movie_test.cpp`.
**Interface:** §8.3 group A rows; `OVT_BOX_DECODER`.
**Steps:** 1. `ftyp/styp`, `mvhd`, `tkhd` (flags, matrix derivation). 2. `tref` children, `elst`, `mdhd` (language), `hdlr` (Pascal-string quirk). 3. `vmhd/smhd/hmhd/nmhd/sthd`, `dref/url /urn `. 4. Context side-effects (`movie_timescale`, `track_timescale`, `track_id`, `handler_type`). 5. Golden field comparison for these boxes.
**Acceptance tests:** 1. Given corpus `isom_basic.mp4`, When probed, Then `mvhd.timescale`, `tkhd.track_ID`, `mdhd.language`, `elst.entries[0].media_time` equal the MP4Box golden (`[container][decoders][movie]`). 2. Given a QuickTime file with Pascal-string `hdlr.name`, When it is parsed, Then the decoded name has no leading length byte. 3. Given `tkhd.matrix` of a 90°-rotated phone video, When it is parsed, Then `derived == "rotation 90°"`.
**Size:** M.
**Notes / pitfalls:** `tkhd.duration` all-ones (0xFFFFFFFF or 2^64−1) means unknown; `elst.media_time` is in the **track** timescale while `segment_duration` is in the **movie** timescale.

### WI-1.206 — Hand decoders, group B (`stsd` and sample entries)
**Goal:** Decode `stsd` and all visual/audio/text/metadata sample entries, including the audio layout selection rule R-AUDIO, then walk their child boxes.
**Depends on:** WI-1.205.
**Produces:** `core/src/container/isobmff/decoders/sample_entries.cpp`; tests `core/tests/container/decoders_sample_entries_test.cpp`.
**Interface:** §8.3 group B rows; `ContainerWithPrefix` return offset contract (§8.2.9).
**Steps:** 1. `stsd` prefix + per-handler dispatch for unknown entry fourccs. 2. VisualSampleEntry (incl. `compressorname` counted string). 3. AudioSampleEntry ISO v0/V1, QT v1/v2 with R-AUDIO. 4. `tx3g`+`ftab`, `wvtt`+`vttC/vlab`, `stpp`, `mett`, `metx`, `urim`, `tmcd`+`name`. 5. Set `ctx.sample_entry`; set `TrackInfo.sample_entry_fourcc` later (WI-1.217). 6. Tests across ISO, QuickTime (`.mov` from iPhone), and 3GPP corpus files.
**Acceptance tests:** 1. Given an iPhone `.mov` with `mp4a` QT v1, When decoded, Then `samples_per_packet` etc. are present and `esds` is found under `wave` (`[container][decoders][stsd]`). 2. Given CMAF `hvc1` entry, When it is parsed, Then `width/height/depth` equal golden and children include `hvcC`, `colr`, `pasp`, `btrt`. 3. Given `tx3g`, When it is parsed, Then `ftab.entries[0].font` equals the mp4ff golden.
**Size:** M.
**Notes / pitfalls:** Sample entries are Boxes, not FullBoxes; some encoders write garbage in `pre_defined` — record with `CNT-1007` Info, never fail; `compressorname` is 32 bytes always.

### WI-1.207 — Hand decoders, group C part 1 (codec configuration records)
**Goal:** Decode `avcC`, `hvcC`, `av1C`, `vpcC`, `esds` (+AudioSpecificConfig), `dOps` and hand embedded parameter sets to the syntax module.
**Depends on:** WI-1.206, WI-1.108 (H.264 SPS/PPS), WI-1.112 (H.265 VPS/SPS/PPS), WI-1.116 (AV1 sequence header) — together they provide `ovt::syntax::parse_parameter_set(codec, bytes) → SyntaxNode`.
**Produces:** `core/src/container/isobmff/decoders/codec_config.cpp`; tests `core/tests/container/decoders_codec_config_test.cpp`.
**Interface:** §8.3 group C rows 1–6; `CodecPrivate` population.
**Steps:** 1. `avcC` with the profile-dependent extension (guard on remaining bytes). 2. `hvcC` general_* fields and arrays. 3. `av1C` + `configOBUs` split (OBU header walk; `CNT-1021`). 4. `vpcC`, `dOps`, `esds` descriptor loop with `mpeg4_expandable_size`, OTI/stream-type names, AudioSpecificConfig. 5. Call the syntax module for each SPS/PPS/VPS/sequence header and attach the returned tree under the field (`Field.children`). 6. Record `CodecPrivate{fourcc, bytes, box_offset}`.
**Acceptance tests:** 1. Given `syn_h264_multislice_cavlc_mp4` (chapter 14 §14.3.3 recipe 29; `avc1`, `profile_idc` of its x264 encode), When decoded, Then `AVCProfileIndication` equals the SPS `profile_idc`, `lengthSizeMinusOne == 3`, `numOfSequenceParameterSets == 1`, `chroma_format == 1` and the SPS child has `profile_idc == 100` (`[container][decoders][codec_config]`). 2. Given `hev1_main10.mp4`, When it is parsed, Then `general_profile_idc == 2`, `bitDepthLumaMinus8 == 2`, arrays contain NAL types 32,33,34. 3. Given `av01_10bit.mp4`, When it is parsed, Then `high_bitdepth == 1`, `seq_level_idx_0` derived `"5.1"` matches mp4box.js, `configOBUs` has exactly one sequence header. 4. Given `aac_lc.m4a`, When it is parsed, Then `audioObjectType == 2`, `samplingFrequencyIndex == 3 (48000)`, `channelConfiguration == 2`.
**Size:** L.
**Notes / pitfalls:** `avcC` extension bytes are absent in many files even for High profile — check `bytes_left() ≥ 4`; `hvcC.general_constraint_indicator_flags` is 48 bits — use `u(48)`; `esds` expandable sizes can be 4 bytes with `0x80` continuation; `av1C` `reserved` bits are not zero in some early encoders (Info only).

### WI-1.208 — Hand decoders, group C part 2 (colour, HDR, Dolby Vision, misc. sample-entry children)
**Goal:** Decode `btrt`, `pasp`, `clap`, `colr` (nclx/nclc/rICC/prof), `mdcv`, `clli`, `cclv`, `dvcC/dvvC/dvwC`, `fiel`, `srat`.
**Depends on:** WI-1.206.
**Produces:** `core/src/container/isobmff/decoders/colour_hdr.cpp`; tests `core/tests/container/decoders_colour_test.cpp`; `core/src/color/h273_names.cpp` (shared table of H.273 names, if not already in chapter 7's `color` module).
**Interface:** §8.3 group C rows 9–17; `colr` context patterns (`*/stsd/*` and `*/ipco`).
**Steps:** 1. `colr` with all four types and ICC header preview. 2. `mdcv`/`clli`/`cclv` with unit conversions. 3. DV record with profile/compat derivation and `CNT-5020`. 4. `pasp`/`clap`/`btrt`/`fiel`/`srat`. 5. Golden comparison vs mp4box.js for HDR boxes.
**Acceptance tests:** 1. Given `syn_h265_main10_pq_hdr.mp4` (chapter 14 §14.3.3 recipe 10, muxed with `colr`/`mdcv`/`clli` boxes), When decoded, Then `colr.transfer_characteristics == 16` derived `"PQ (SMPTE ST 2084)"`, `mdcv.max_display_mastering_luminance == 10000000` derived `"1000.0 cd/m²"`, `clli.max_content_light_level == 1000` (`[container][decoders][colour]`). 2. Given `dv_p81.mp4`, When it is parsed, Then `dvvC.dv_profile == 8`, `dv_bl_signal_compatibility_id == 1`, derived contains `"HDR10 compatible"`. 3. Given a `.mov` with `nclc`, When it is parsed, Then three fields are decoded and no `full_range_flag` field exists.
**Size:** M.
**Notes / pitfalls:** `mdcv` primaries order in the box is as written (ST 2086 says G,B,R; some muxers write R,G,B) — record raw and let the derived text name the colour by matching known chromaticities, not by position.

### WI-1.209 — Hand decoders, group D part 1 (time-to-sample, chunk and size tables)
**Goal:** Decode `stts`, `ctts`, `cslg`, `stsc`, `stsz`, `stz2`, `stco`, `co64`, `stss`, `stps`, `sdtp` with the large-array summarisation and the `CNT-20xx` box-level checks.
**Depends on:** WI-1.204 (windowed reads).
**Produces:** `core/src/container/isobmff/decoders/sample_tables.cpp`; tests `core/tests/container/decoders_sample_tables_test.cpp`.
**Interface:** §8.3 group D rows 1–11.
**Steps:** 1. `stts/ctts` with run statistics in `derived`. 2. `stsc` monotonicity, `stsz/stz2` (nibble packing). 3. `stco/co64`, `stss/stps`, `sdtp` with `sample_count` from `ctx` (set by `stsz` decoder — order: `stsz` is read before `sdtp` because decoders run in box order; if `sdtp` precedes `stsz`, defer via a second pass flag). 4. `cslg` v0/v1. 5. Tests with a 1-hour file: arrays summarised, tree ≤ 8 MiB.
**Acceptance tests:** 1. Given `syn_h264_1h_1080p.mp4` (chapter 14 §14.3.3 recipe 27), When the tree is serialised, Then its size ≤ 8 MiB and the `stsz` entry count equals the recipe-27 frame count (108 000) with 80 materialised children (`[container][decoders][stbl]`). 2. Given `tests/corpora/synthetic/isobmff-broken/stsc_descending.mp4` (`tools/make_broken.py`, §8.9.3), When it is parsed, Then `CNT-2003` Error. 3. Given `tests/corpora/synthetic/isobmff-broken/ctts_v0_negative.mp4`, When it is parsed, Then `CNT-2002` Warning and the offset field `ivalue < 0`.
**Size:** M.
**Notes / pitfalls:** `stz2` 4-bit entries: high nibble first; `stss` is absent when all samples are sync — do not invent one.

### WI-1.210 — Hand decoders, group D part 2 (sample groups, subsamples, auxiliary info)
**Goal:** Decode `sgpd` (all listed grouping types incl. `seig`), `sbgp`, `subs`, `saiz`, `saio`, `padb`, `stdp`, `stsh` with context awareness (`stbl` vs `traf`).
**Depends on:** WI-1.209.
**Produces:** `core/src/container/isobmff/decoders/sample_groups.cpp`; tests `core/tests/container/decoders_sample_groups_test.cpp`.
**Interface:** §8.3 group D rows 12–20.
**Steps:** 1. `sgpd` versions 0/1/2 with per-type entry decoders and `default_length` logic. 2. `sbgp` v0/v1. 3. `subs` v0/v1 with 14496-15 `codec_specific_parameters`. 4. `saiz/saio` with `aux_info_type` names. 5. `padb/stdp/stsh`.
**Acceptance tests:** 1. Given `cenc_cbcs_keyrotation.mp4`, When decoded, Then `sgpd(seig)` entries show `crypt_byte_block == 1`, `skip_byte_block == 9`, KIDs equal mp4ff golden (`[container][decoders][sgpd]`). 2. Given an HEVC file with `sgpd(roll)` and `sgpd(rap )`, When it is parsed, Then `roll_distance` and `num_leading_samples` equal MP4Box golden. 3. Given `sgpd` v0 with grouping type `zzzz`, When it is parsed, Then `CNT-2020`.
**Size:** M.
**Notes / pitfalls:** In `traf`, `sbgp.group_description_index ≥ 0x10001` indexes the traf-local `sgpd` (index − 0x10000); `saiz.default_sample_info_size == 0` means per-sample sizes follow.

### WI-1.211 — Hand decoders, group E part 1 (movie extends and movie fragments)
**Goal:** Decode `mvex`, `mehd`, `trex`, `leva`, `moof`, `mfhd`, `traf`, `tfhd`, `tfdt`, `trun` with flag-driven fields and the `sample_flags` bitfield.
**Depends on:** WI-1.204.
**Produces:** `core/src/container/isobmff/decoders/fragment_boxes.cpp` (box decoders of group E; distinct from the `FragmentTimeline` builder `core/src/container/isobmff/fragments.cpp` of WI-1.219); tests `core/tests/container/decoders_fragment_boxes_test.cpp`.
**Interface:** §8.3 group E rows 1–10.
**Steps:** 1. `trex` + `ctx.trex_for_track` map. 2. `tfhd` flags and effective defaults in derived text. 3. `trun` v0/v1 with the flag combinations; `sample_flags` bitfield decode into sub-fields with bit spans. 4. `tfdt`, `mfhd`, `mehd`, `leva`. 5. Tests over CMAF and Smooth Streaming corpora.
**Acceptance tests:** 1. Given `cta_cmaf_video_cmfv` (CTA WAVE, §8.9.3), When the first `moof` is expanded, Then `tfhd.flags & 0x020000`, `trun.sample_count` and `trun.data_offset` equal mp4box.js golden and the first sample's `sample_is_non_sync_sample == 0` (`[container][decoders][moof]`). 2. Given `trun` v1 with negative offsets, When it is parsed, Then `sample_composition_time_offset.ivalue < 0`. 3. Given a `traf` without `tfhd`, When it is parsed, Then `CNT-1011` Error names `tfhd`.
**Size:** M.
**Notes / pitfalls:** `first_sample_flags` applies only to sample 0 and only when flag `0x4`; the per-sample loop reads fields in the fixed order duration, size, flags, cts — regardless of which flags are set.

### WI-1.212 — Hand decoders, group E part 2 (random access and segment index boxes)
**Goal:** Decode `mfra`, `tfra`, `mfro`, `sidx`, `ssix`, `prft`, `emsg` (v0/v1 with known schemes), `styp`.
**Depends on:** WI-1.211.
**Produces:** `core/src/container/isobmff/decoders/index_boxes.cpp`; tests `core/tests/container/decoders_index_test.cpp`.
**Interface:** §8.3 group E rows 11–17.
**Steps:** 1. `tfra` with variable-width `traf/trun/sample_number`. 2. `sidx` with absolute byte/time ranges derived. 3. `ssix`, `prft` (NTP → ISO 8601), `mfro`. 4. `emsg` v0/v1 and scheme-specific payload previews (ID3 frame ids; SCTE-35 `splice_command_type`). 5. Tests over DASH-IF segments and Apple HLS fMP4.
**Acceptance tests:** 1. Given `dashif_sidx_m4s` (DASH-IF, §8.9.3), When decoded, Then `sidx.references[k].referenced_size` and derived absolute offsets equal the `moof` offsets found by the walker for all k (`[container][decoders][sidx]`). 2. Given an HLS segment with `emsg` v1 ID3, When it is parsed, Then `scheme_id_uri == "https://aomedia.org/emsg/ID3"` and the derived preview lists `TXXX`/`PRIV` frames. 3. Given `mfra` at file end, When it is parsed, Then `mfro.size == mfra.file_span.length`.
**Size:** M.
**Notes / pitfalls:** `tfra` numbers are 1-based; `prft.ntp_timestamp` upper 32 bits are seconds since 1900 (subtract 2208988800 for Unix).

### WI-1.213 — Hand decoders, group H (encryption) and `senc`
**Goal:** Decode `sinf/frma/schm/schi/tenc/pssh` (with the system-ID table and Widevine/PlayReady payload decoders) and `senc` with IV-size context and inference.
**Depends on:** WI-1.210, WI-1.211.
**Produces:** `core/src/container/isobmff/decoders/encryption.cpp`, `core/src/container/isobmff/decoders/pssh_systems.cpp` (`kPsshSystems[]`), minimal protobuf varint reader `core/src/container/isobmff/decoders/pb_lite.h`; tests `core/tests/container/decoders_encryption_test.cpp`.
**Interface:** §8.3 group H rows; `BoxContext.per_sample_iv_size`.
**Steps:** 1. `schm/frma/tenc` (+ context side effect). 2. `pssh` v0/v1, system table, Widevine protobuf fields 1–4,7,8; PlayReady Object + WRMHEADER UTF-16LE (show `KID` and `LA_URL`). 3. `senc` using context, with inference fallback (`CNT-6001`). 4. Tests over Bento4 and GPAC encrypted samples (`cenc`, `cbcs`, PlayReady, Widevine).
**Acceptance tests:** 1. Given `bento4_widevine.mp4`, When decoded, Then `pssh.SystemID` derived `"Widevine"` and the protobuf `key_id` equals `tenc.default_KID` (`[container][decoders][cenc]`). 2. Given `gpac_cbcs.mp4` first `moof` (constant IV, `Per_Sample_IV_Size == 0`), When decoded, Then `senc` entries contain only subsample pairs and Σ(clear+protected) == sample size for all samples. 3. Given `senc` in a fragment-only file without init segment, When it is parsed, Then `CNT-6001` or successful inference with Info noted.
**Size:** M.
**Notes / pitfalls:** Protobuf wire types: field 2 `key_id` is `bytes` (wire type 2) and repeated; do not assume only one KID. PlayReady XML is UTF-16LE with BOM.

### WI-1.214 — Hand decoders, group F (metadata and HEIF item boxes)
**Goal:** Decode `meta` (ISO/QuickTime), `pitm`, `iinf/infe` (v0–v3), `iloc` (v0–v2), `iprp/ipco` properties, `ipma`, `iref`, `idat`.
**Depends on:** WI-1.208 (shared `colr`, `hvcC`, `av1C` decoders via `*/ipco` patterns).
**Produces:** `core/src/container/isobmff/decoders/meta_items.cpp`; tests `core/tests/container/decoders_heif_test.cpp`.
**Interface:** §8.3 group F rows; `meta` detection rule §8.2.6(4).
**Steps:** 1. `meta` dialect detection. 2. `iinf/infe` all versions and `fdel` extension. 3. `iloc` with variable field sizes and absolute extent spans. 4. `ipco` property set (`ispe pixi irot imir auxC rloc lsel a1op a1lx clap pasp colr hvcC av1C`), `ipma` with 7/15-bit indices, `iref` typed references, `pitm`. 5. Tests over Nokia HEIF conformance and AVIF test files.
**Acceptance tests:** 1. Given `heif_heic_grid.heic`, When decoded, Then `pitm.item_ID`, `iloc` extents and `ipma` associations match `MP4Box -diso` golden and every `dimg` reference resolves (`[container][decoders][heif]`). 2. Given `avif_10bit.avif`, When it is parsed, Then `ipco/av1C.high_bitdepth == 1` and `ispe` dimensions match `ffprobe`. 3. Given an iPhone `.mov` with QuickTime `meta/keys/ilst`, When it is parsed, Then `CNT-1006` Info and the `ilst` children are renamed by key string.
**Size:** M.
**Notes / pitfalls:** `iloc` v0 has no `construction_method` (file offsets only); `ipma` indices are 1-based with 0 = none.

### WI-1.215 — Hand decoders, group G (user data, iTunes `ilst`, QuickTime `keys`, `©xyz`, `chpl`, `kind`)
**Goal:** Decode `udta` children incl. iTunes and QuickTime metadata and location.
**Depends on:** WI-1.214.
**Produces:** `core/src/container/isobmff/decoders/udta.cpp`; tests `core/tests/container/decoders_udta_test.cpp`.
**Interface:** §8.3 group G rows.
**Steps:** 1. `cprt`, `kind`, `chpl`, `name`. 2. `©xyz` ISO 6709 parsing to decimal degrees. 3. QuickTime international text list atoms. 4. `ilst/*` + `data` type table + `----/mean/name`. 5. `keys` + numeric-fourcc renaming. 6. Tests on iTunes `.m4a`, Android camera `.mp4` (`com.android.version`), iPhone `.mov`.
**Acceptance tests:** 1. Given `itunes_tagged.m4a`, When decoded, Then `ilst/©nam/data.value == "Title"` with `well_known_type == 1` and `trkn` derived `"3/12"` (`[container][decoders][udta]`). 2. Given `pixel_camera.mp4`, When it is parsed, Then `©xyz` derived latitude/longitude equal `ffprobe` `location` tag to 4 decimals. 3. Given `nero_chapters.mp4`, When it is parsed, Then `chpl` start times derived seconds match `MP4Box -info` chapters.
**Size:** M.
**Notes / pitfalls:** `data.type_indicator` first byte is a version byte (0); the type is the low 24 bits; `©xyz` strings end with `/`.

### WI-1.216 — Hand decoders, groups K and M (`uuid` boxes and QuickTime media boxes)
**Goal:** Dispatch `uuid` by usertype (PIFF, XMP motion photo, Sony/Canon) and decode QuickTime `tapt/clef/prof/enof`, `gmhd/gmin/text`, `wave/frma/enda/chan`.
**Depends on:** WI-1.213 (PIFF reuses CENC semantics), WI-1.206 (`wave` under audio entries).
**Produces:** `core/src/container/isobmff/decoders/uuid_boxes.cpp`, `core/src/container/isobmff/decoders/quicktime.cpp`; tests `core/tests/container/decoders_uuid_qt_test.cpp`.
**Interface:** §8.3 groups K and M; `lookup_uuid()`.
**Steps:** 1. `uuid` table + decoders for PIFF `tenc/senc/tfxd/tfrf/pssh`. 2. XMP decoder with Motion Photo / GSpherical detection and derived video offset. 3. Unknown-uuid heuristics. 4. QuickTime boxes incl. `wave` terminator handling and `chan` layout tag names. 5. Tests on Smooth Streaming ISMV, Pixel Motion Photo (`MVIMG_*.jpg`-embedded MP4 and `.mp4` with XMP), iPhone `.mov`.
**Acceptance tests:** 1. Given `smooth_piff.ismv`, When decoded, Then the `uuid` nodes have descriptions `PIFF TrackEncryptionBox`/`SampleEncryptionBox` and `CNT-6010` Info (`[container][decoders][uuid]`). 2. Given `motion_photo.mp4`, When it is parsed, Then the XMP uuid derived text contains `MicroVideoOffset=<n>` and `n` equals the `mdat`-relative offset verified by the test. 3. Given an iPhone `.mov`, When it is parsed, Then `tapt/clef.width` equals the display width and `wave` children include `frma`, `enda`.
**Size:** M.
**Notes / pitfalls:** The `wave` terminator is a box with type `0x00000000` — only legal inside `wave`; `chan.mChannelLayoutTag` high 16 bits are the tag, low 16 bits the channel count.

### WI-1.217 — Build `TrackInfo` from the parsed `moov`
**Goal:** Populate `TrackInfo[]` (schema chapter 06 §6.4, derivation table §8.1.3) for every `trak`, including codec mapping, `codec_string`, `codec_private` list, matrix/rotation, bitrates, encryption/fragmentation flags, references and durations.
**Depends on:** WI-1.205–1.216.
**Produces:** `core/src/container/isobmff/tracks.cpp`; tests `tracks_test.cpp`.
**Interface:** `std::vector<TrackInfoT> build_tracks(const ParsedMovie&)`; `TrackInfo` schema.
**Steps:** 1. Walk `trak`s in order; read `tkhd/mdhd/hdlr/stsd/edts/tref`. 2. Map sample entry fourcc → `Codec` (`avc1/avc2/avc3/avc4/dvav/dva1 → H264`; `hev1/hvc1/hev2/hvc2/dvhe/dvh1 → H265`; `av01/dav1 → AV1`; `vp09 → VP9`; `vvc1/vvi1 → H266`; `mp4v` with `esds` OTI 0x61/0x60 → MPEG2`; `encv/resv → by frma`). 3. `codec_private` list from children in order. 4. `is_fragmented` from `trex`, `sample_count` from `stsz` + fragments (lazy: fragments part filled by WI-1.219). 5. Tests across all corpora: every `trak` yields a `TrackInfo`.
**Acceptance tests:** 1. Given `dv_p81.mp4`, When tracks are built, Then `tracks[0].codec == H265`, `sample_entry_fourcc == "hvc1"`, `codec_private` fourccs are `["hvcC","dvvC"]` in that order (`[container][tracks]`). 2. Given `encv_cenc.mp4`, When it is parsed, Then `is_encrypted && original_format == "avc1"`. 3. Given a file with two tracks sharing `track_ID`, When it is parsed, Then `CNT-1012` on the second.
**Size:** S.
**Notes / pitfalls:** `handler_fourcc` `sbtl` (Apple) and `subt`/`text` all map to `Subtitle`/`Text`; `tmcd` → `Timecode`.

### WI-1.218 — Implement `SampleTable` expansion for `moov` tables
**Goal:** Produce per-sample DTS/CTS/offset/size/sync/groups from `stbl` with the exact formulas and overflow rules of §8.4.1.
**Depends on:** WI-1.217.
**Produces:** `core/src/container/isobmff/sample_table.cpp` (fills `SampleTable`/`GroupMembership` of chapter 06 §6.4); tests `core/tests/container/sample_table_test.cpp`; implementation of the C API `ovt_container_sample_table(ovt_Container*, int track_index, uint32_t first, uint32_t count, ovt_Buffer* out)` (declared in chapter 12 §12.3.7; `count == 0` means "to the end").
**Interface:** §8.4.1 `build_sample_table`; formulas block.
**Steps:** 1. Steps 1–4 (durations, DTS, CTS, sizes) with checked arithmetic. 2. Step 5 chunk map. 3. Steps 6–9 (file bounds, sync, groups, descriptor index). 4. Cross-track overlap check hook for `validate.cpp`. 5. Golden comparison with `MP4Box -dts` on every non-fragmented corpus file.
**Acceptance tests:** 1. Given every non-fragmented corpus MP4, When `ovt probe --samples -t 0 --format json` runs, Then every row equals the `MP4Box -dts` golden row (`[container][sample_table][golden]`). 2. Given `syn_h264_1h_1080p`, When the sample table is expanded, Then `bench_container_sample_table_1h` meets its chapter 14 §14.9.3 budget (see `tests/perf/budgets.json`). 3. Given `syn_mp4_broken_stco_beyond_eof`, When the sample table is expanded, Then `CNT-2007` with the first offending sample index. 4. Given synthetic `stts` with Σ deltas > 2^63, When the sample table is expanded, Then `CNT-2030` and `count` truncated, no UB (UBSan clean).
**Size:** M.
**Notes / pitfalls:** `ctts` applies in decode order; CTS is not monotonic; keep SoA vectors (`std::vector<int64_t>` etc.) and serialise once.

### WI-1.219 — Implement `FragmentTimeline` and fragment sample appending
**Goal:** Decode all fragments of a track into `FragmentInfo/TrafInfo/TrunInfo`, apply base-offset and continuity rules, cross-check `sidx`/`mfra`, and append fragment samples to the `SampleTable`.
**Depends on:** WI-1.218, WI-1.211, WI-1.212.
**Produces:** `core/src/container/isobmff/fragments.cpp` (fills the §8.4.3 tables of chapter 06 §6.4); tests `core/tests/container/fragments_test.cpp`; implementation of the C API `ovt_container_fragments(ovt_Container*, int track_index, ovt_Job* job, ovt_Buffer* out)` (chapter 12 §12.3.7).
**Interface:** §8.4.3 algorithm steps 1–5.
**Steps:** 1. `moof`/`mdat` pairing and lazy expansion in a job with cancellation every 50 ms. 2. Defaults and base-offset rules. 3. `trun` sample loop, DTS/continuity, bounds. 4. `saio` resolution. 5. `sidx`/`mfra`/`mfhd` cross-checks. 6. Append to `SampleTable`; golden vs `MP4Box -dts` (which prints fragment samples too) and ffprobe `-show_packets pos`.
**Acceptance tests:** 1. Given `syn_mp4_frag` (init + segments of chapter 14 §14.3.3 recipe 20b) and `cta_cmaf_video_cmfv` (CTA WAVE), When the timeline is built, Then every sample `offset`/`size`/`dts` equals ffprobe `pos/dts` for the first 1000 packets and `dts_gap == 0` for all fragments (`[container][fragments]`). 2. Given `dashif_sidx_m4s`, When its fragments are parsed, Then `CNT-4030`/`CNT-4032` are **not** emitted and every `SidxRef.actual_size == referenced_size`. 3. Given a Smooth Streaming fragment with two `traf`s and no base flags, When its fragments are parsed, Then `CNT-4006` Info and offsets equal `MP4Box -dts`. 4. Given a file where fragment 3's `tfdt` jumps by 9000, When its fragments are parsed, Then `CNT-4020` names fragment 3 with gap 9000.
**Size:** L.
**Notes / pitfalls:** `data_offset` is signed; `moof_offset` is the offset of the `moof` **header** (not payload); an empty `trun` (`sample_count 0`) is legal with `duration-is-empty`.

### WI-1.220 — Implement `EditList` and the `elst ↔ tfdt` sanity check
**Goal:** Build the presentation timeline from `elst`, map samples to presentation times, and run the CMAF edit-list sanity checks.
**Depends on:** WI-1.219.
**Produces:** `core/src/container/isobmff/edit_list.cpp` (fills `EditList`/`EditSegment`); tests `edit_list_test.cpp`.
**Interface:** §8.4.2 `build_edit_list`, `sample_presentation_time`; checks `CNT-30xx`.
**Steps:** 1. Segment classification and sample range resolution via a CTS-sorted index. 2. `sample_presentation_time` with 128-bit intermediate. 3. `negative_cts_compensation` detection. 4. `elst ↔ tfdt` rules 1–4. 5. Tests: x264 default (`elst` removing 2-frame delay), QuickTime empty edit, CMAF with `elst` and without.
**Acceptance tests:** 1. Given `syn_h264_weightedpred_ltr` wrapped in MP4 by `generate.py` (x264 B-frames → `elst` removing the delay), When built, Then one `normal` segment with `media_time == CTS[0] − DTS[0]` (the B-frame delay) and `negative_cts_compensation == true` (`[container][editlist]`). 2. Given `syn_mp4_elst` (recipe 20a, 500 ms empty edit), When it is parsed, Then segments `[empty, normal]` and `sample_presentation_time(0)` equals the empty edit duration rescaled. 3. Given `syn_cmaf_bad_elst_cmfv` (hand-broken variant of `cta_cmaf_video_cmfv`, media_time ≠ tfdt+cts), When it is parsed, Then `CNT-3010`.
**Size:** S.
**Notes / pitfalls:** `elst.media_time` uses the track timescale and `segment_duration` the movie timescale — rescale with `muldiv64`; rate ≠ 1 segments are informational only.

### WI-1.221 — Implement the CENC per-sample encryption map and hex shading ranges
**Goal:** Resolve per-sample KID/IV/subsamples from `tenc`/`seig`/`senc`/`saio`/`saiz` and expose clear/protected byte ranges.
**Depends on:** WI-1.213, WI-1.219.
**Produces:** `core/src/container/isobmff/cenc.cpp` (fills `EncryptionInfo`); tests `core/tests/container/cenc_test.cpp`; implementation of the C API `ovt_container_encryption(ovt_Container*, int track_index, uint64_t first, uint64_t count, ovt_Buffer* out)` (chapter 12 §12.3.7).
**Interface:** §8.4.4 `build_encryption_map`, `sample_clear_protected_ranges`.
**Steps:** 1. Defaults + `seig` override. 2. Aux-data location for `moov` and fragment samples. 3. IV/subsample parse and the sum check. 4. Pattern expansion for `cbcs`/`cens`. 5. Golden vs `mp4ff-info -l senc:1` IVs and Bento4 `mp4dump`.
**Acceptance tests:** 1. Given `bento4_cenc_subsample`, When built for samples 0..99, Then every IV equals the mp4ff golden and Σ(clear+protected) == size (`[container][cenc]`). 2. Given `syn_mp4_cenc_cbcs` (recipe 20c), When it is parsed, Then `iv == tenc.default_constant_IV` for every sample and the expanded ranges alternate 16 encrypted / 144 clear bytes within protected spans. 3. Given a sample whose first subsample of a NAL unit protects byte 0 of that NAL, When it is parsed, Then `CNT-6007`; Given a continuation subsample with `BytesOfClearData == 0`, Then no diagnostic.
**Size:** M.
**Notes / pitfalls:** For `cbcs`, the pattern restarts at each subsample's protected range; partial trailing blocks (< 16 bytes) are clear.

### WI-1.222 — Implement brand and profile checks
**Goal:** Evaluate `ftyp`/`styp` brands against the §8.4.5 check table and emit `CNT-7xxx`/`CNT-8010+` diagnostics.
**Depends on:** WI-1.220, WI-1.221.
**Produces:** `core/src/container/isobmff/brands.cpp` (`kBrandChecks[]`); tests `brands_test.cpp`.
**Interface:** `struct BrandCheck`; `ValidationLevel` gating for sample-byte checks.
**Steps:** 1. ISO/MP4/QT brand rows. 2. CMAF `cmfc/cmf2/cmfs/cmff/cmfl`. 3. DASH `dash/msdh/msix` + HLS heuristics. 4. AVC/HEVC/AV1 sample-entry rows (deep checks via unit splitter from WI-1.231). 5. MIAF/HEIF rows. 6. Tests on CTA WAVE content (must be clean) and on hand-broken variants.
**Acceptance tests:** 1. Given every CTA WAVE `cmfc` file, When validated at `Standard`, Then no `CNT-70xx` Error/Warning is emitted (`[container][brands]`). 2. Given `syn_mp4_broken_cmfc_two_tracks`, When it is parsed, Then `CNT-7010`. 3. Given `syn_mp4_broken_msix_without_sidx`, When it is parsed, Then `CNT-7031`. 4. Given `syn_mp4_broken_hvc1_no_ps`, When it is parsed, Then `CNT-7005`.
**Size:** M.
**Notes / pitfalls:** Brand checks are advisory; never fail `ovt_container_open` because of them; `compatible_brands` is often padded with duplicates — deduplicate.

### WI-1.223 — Implement `HdrMetadata` (box-level + bitstream probe)
**Goal:** Produce the HDR summary and the box-vs-bitstream mismatch diagnostics.
**Depends on:** WI-1.208, WI-1.218, WI-1.231 (unit splitter), WI-1.110 (H.264 SEI), WI-1.114 (H.265 SEI), WI-1.116 (AV1 metadata OBU).
**Produces:** `core/src/container/isobmff/hdr_summary.cpp` (fills `HdrMetadata`/`HdrSignal`); tests `core/tests/container/hdr_summary_test.cpp`; implementation of the C API `ovt_container_hdr_metadata(ovt_Container*, int track_index, ovt_Job* job, ovt_Buffer* out, ovt_ColorDescription* out_color)` (chapter 12 §12.3.7).
**Interface:** §8.4.6 algorithm; `HdrProbeOptions`.
**Steps:** 1. Box-level collection into `HdrSignal`s and the `color` struct image (`ovt_ColorDescription`). 2. Sync-sample probe through unit splitter + syntax module (SEI 137/144/T.35 HDR10+, NAL 62, AV1 metadata OBUs, VUI/color_config). 3. Classification rules 3.1–3.7. 4. Mismatch rules. 5. Golden vs MediaInfo JSON on the HDR corpus and dovi_tool/hdr10plus_tool on extracted ES.
**Acceptance tests:** 1. Given `hdr_hdr10plus_dv81` (Dolby/HDR10+ sample, §8.9.3), When built, Then `kind == DolbyVision`, `dv_profile == 8`, `has_hdr10plus == true`, signals include `nal_62_rpu` and `sei_t35_hdr10plus` with `sample_index == 0` (`[container][hdr]`). 2. Given `hdr_hlg_bt2020` (public HLG clip, §8.9.3), When it is probed, Then `kind == HLG`. 3. Given `syn_h265_main10_pq_hdr_mismatch` (chapter 14; `colr` BT.709 over a BT.2020 SPS), When it is probed, Then `CNT-5001` and `box_vs_bitstream_mismatch == true`. 4. Given a 2 GB file, When it is probed, Then the probe reads ≤ 16 MiB (`bytes_read` delta).
**Size:** M.
**Notes / pitfalls:** HDR10+ T.35: country `0xB5`, provider `0x003C`, provider-oriented code `0x0001`, application identifier `4`; DV RPU in HEVC is `nal_unit_type 62` with `nuh_layer_id 0`; AV1 HDR10+ uses `OBU_METADATA` type 4 with the same T.35 bytes.

### WI-1.224 — Implement the validation driver, diagnostics table and broken-corpus coverage test
**Goal:** Wire all `CNT-xxxx` checks into `validate(ParsedMovie&, ValidationLevel)` and prove every code is reachable.
**Depends on:** WI-1.218–1.223.
**Produces:** `core/src/container/isobmff/validate.cpp`; complete `diag_codes.inc`; `core/tests/container/diag_coverage_test.cpp`; `isobmff-broken` entries in `tests/corpora/manifest.json` with `expected.diagnostics` (§8.9.3).
**Interface:** §8.5 table; `ValidationLevel {Quick, Standard, Deep}`.
**Steps:** 1. Required/duplicate box checks (`CNT-1011/1012`), allowed-parent check (`CNT-1013`), size checks (`CNT-1014/1015`). 2. Cross-track overlap (`CNT-2016`), `mdat` containment (`CNT-2017`). 3. Level gating and job integration for `Deep`. 4. Extend `make_broken.py` until every code in §8.5 has a triggering file. 5. `ovt probe --list-diagnostics` output test (chapter 12 §12.2.2).
**Acceptance tests:** 1. Given the broken corpus, When `ctest -R diag_coverage` runs, Then every code listed in `diag_codes.inc` (except the phase-4 `CNT-TS-*`/`CNT-EBML-*` families) is emitted by at least one file. 2. Given every clean corpus file, When validated at `Standard`, Then no `Error` is emitted (allow-list in `KNOWN_DIFFS.md`). 3. Given `ovt probe --list-diagnostics`, When it runs, Then output has one line per code with severity and spec reference.
**Size:** M.
**Notes / pitfalls:** `CNT-1011` must respect CMAF/fragment-only exceptions; `Deep` must be cancellable.

### WI-1.225 — Implement the IVF reader
**Goal:** Open IVF files into a `ContainerTree` + `TrackInfo` + `SampleTable`.
**Depends on:** WI-1.200.
**Produces:** `core/src/container/ivf.cpp`; tests `ivf_test.cpp`; fuzz target `fuzz_ivf`.
**Interface:** §8.6.2 header/frame layout; lazy frame children.
**Steps:** 1. Header parse with LE readers, `header_size` handling. 2. Frame walk with truncation handling. 3. `SampleTable` with timestamps and lazily-computed `is_sync`. 4. Tree nodes with fields and spans. 5. Tests on libaom test vectors.
**Acceptance tests:** 1. Given `syn_av1_gop60_1080p.ivf` (chapter 14 §14.3.3 recipe 26), When probed, Then `frame_count_actual` equals `ffprobe -count_packets` and every `offset == pos + 12` and `size` equals ffprobe `size` (ffprobe's `pos` is the 12-byte frame header, `SampleTable.offset` the payload) (`[container][ivf]`). 2. Given an IVF truncated mid-frame, When it is parsed, Then `CNT-IVF-0003` and the last sample size is clamped.
**Size:** S.
**Notes / pitfalls:** All IVF integers are little-endian; `frame_count` in the header is unreliable. Zero `timebase_numerator` or `timebase_denominator` → `TrackInfo.timescale = 0`, `duration_seconds = −1`, timestamps raw (scale 1). The last sample repeats the previous sample's duration (one tick for a one-frame stream). `is_sync` without the syntax module: a minimal OBU walk (first `OBU_FRAME`/`OBU_FRAME_HEADER`: `frame_type == KEY_FRAME` and `show_existing_frame == 0`, or `reduced_still_picture_header`) for `AV01`, the VP9 and VP8 frame-tag checks for `VP90`/`VP80`, non-sync for any other fourcc. The C entry points `ovt_container_track_info`/`ovt_container_sample_table` stay `OVT_E_UNSUPPORTED` until WI-1.217 wires them with the `ovt_Buffer` allocator; the reader exposes `TrackInfo` and `SampleTable` through the C++ `Reader`.

### WI-1.226 — Implement the Annex B start-code splitter
**Goal:** Split H.264/H.265 Annex B streams into NAL units with exact `prefix_span`/`file_span`, trailing-zero handling and `zero_byte` checks.
**Depends on:** WI-1.200.
**Produces:** `core/src/container/annexb.cpp` (also used by `unit_split.cpp`); tests `annexb_test.cpp`; fuzz `fuzz_annexb_split`; `tools/ref_annexb.py` (reference Annex B splitter in pure Python: prints one `offset,length,nal_type` line per NAL using the §8.6.3 boundary rule; the oracle of acceptance test 1 and of chapter 14 WI-1.570; D27).
**Interface:** §8.6.3 algorithm; `annexb_split(RandomAccessFile&, callback)` and an in-memory span overload.
**Steps:** 1. Windowed scanner with 4-byte carry. 2. Boundary rule (`00 00 00` / `00 00 01`) and trailing-zero strip. 3. `zero_byte` rule and `forbidden_zero_bit` check. 4. Codec sniff helper. 5. Golden vs GPAC (`MP4Box -add x.264 -new tmp.mp4` then `MP4Box -dnal 1 tmp.mp4`, which lists NAL sizes per sample) and vs a reference splitter in Python (`tools/ref_annexb.py`) on the ES corpus (ffprobe is unsuitable here: its packets are access units, not NAL units).
**Acceptance tests:** 1. Given every `.264/.265` corpus file, When split, Then NAL count and `(offset,length)` list equal `tools/ref_annexb.py` output (`[container][annexb]`). 2. Given a stream with 1000 trailing zero bytes after a NAL, When it is split into NAL units, Then the NAL length excludes them and `CNT-ESB-0002` Info appears once. 3. Given bytes `00 00 01 00 00 01 65 ...`, When it is split into NAL units, Then `CNT-ESB-0003` for the empty NAL.
**Size:** S.
**Notes / pitfalls:** `00 00 03` is an EPB, never a boundary; `00 00 02` cannot occur inside a NAL; `cabac_zero_word`s (`0x0000`) are escaped to `00 00 03`, so a NAL that ends with them ends in `0x03` (non-zero) and the trailing-zero strip rule holds.

### WI-1.227 — Implement raw AV1 OBU stream detection and reader (Section 5 and Annex B)
**Goal:** Detect and walk `.obu` files in both framings, producing TU/FU/OBU nodes and a `SampleTable` with one sample per temporal unit; re-frame Annex B OBUs for the plugin ABI.
**Depends on:** WI-1.200.
**Produces:** `core/src/container/obu.cpp`; tests `obu_test.cpp`; fuzz `fuzz_obu_split`.
**Interface:** §8.6.4 heuristic and tree; re-framing contract used by `ovt_stream_unit_bytes`.
**Steps:** 1. `leb128` and OBU header parsing (shared with `unit_split.cpp`). 2. Detection steps 1–3. 3. Section 5 walk grouping OBUs into TUs at each `OBU_TEMPORAL_DELIMITER`. 4. Annex B walk with `temporal_unit_size/frame_unit_size/obu_length`. 5. Re-framing into scratch with `obu_has_size_field = 1`. 6. Tests on aom test data in both framings (`av1-1-b8-01-size-16x16.obu` etc. and their Annex B conversions produced by `aomdec --annexb`).
**Acceptance tests:** 1. Given every `.obu` corpus file, When sniffed, Then `format` field equals the manifest (`section5`/`annexb`) (`[container][obu]`). 2. Given an Annex B file, When unit 0 bytes are requested, Then byte 0 has bit 1 set (`obu_has_size_field`) and a `leb128` size follows whose value equals `obu_length − header bytes`. 3. Given a Section 5 file, When it is parsed, Then the TU count equals the number of `OBU_TEMPORAL_DELIMITER` OBUs in the file.
**Size:** S.
**Notes / pitfalls:** `obu_extension_flag` adds a second header byte; `leb128` max 8 bytes; Annex B `obu_length` includes the OBU header.

### WI-1.228 — Implement the Y4M reader
**Goal:** Parse the YUV4MPEG2 header and `FRAME` markers into a `SampleTable` and `RawVideoFormat`.
**Depends on:** WI-1.200.
**Produces:** `core/src/container/y4m.cpp` (fills `RawVideoFormat`, chapter 06 §6.4); tests `y4m_test.cpp`; fuzz `fuzz_y4m`.
**Interface:** §8.6.5 tags and `C` table.
**Steps:** 1. Header tag parser (unknown tags kept as fields). 2. `C` → format mapping incl. `XCOLORRANGE`. 3. Frame walk with magic verification. 4. Tree and `TrackInfo`. 5. Tests on `ffmpeg -f lavfi -i testsrc` generated files for every `C` value.
**Acceptance tests:** 1. Given the first 30 frames of `src_1080p_10bit.y4m` (chapter 14 §14.3.3 source recipe; `C420p10`), When probed, Then `frame_count == 30`, `bit_depth == 10`, `chroma_format == 1`, `frame_bytes == 1920·1080·2·1.5` (`[container][y4m]`). 2. Given a corrupted 5th `FRAME` magic, When it is parsed, Then `CNT-Y4M-0003` and `frame_count == 4`.
**Size:** S.
**Notes / pitfalls:** Frame-level tags (`FRAME Ixx`) exist; the header line may exceed 1 KiB with `X` comments — cap at 64 KiB.

### WI-1.229 — Implement raw YUV opening: format grammar, heuristics, sidecar and C API
**Goal:** Open `.yuv` files with a parsed `pix_fmt` string from sidecar/CLI/filename/size heuristics.
**Depends on:** WI-1.228 (shares `RawVideoFormat`).
**Produces:** `core/src/container/rawyuv.cpp`, `pix_fmt_grammar.cpp`; `core/schema/raw_sidecar.schema.json`; implementation of `ovt_container_open_format(…, "raw", raw_format, …)` (chapter 12 §12.3.7); tests `core/tests/container/rawyuv_test.cpp`; fuzz `core/tests/fuzz/fuzz_raw_format_string.cpp`.
**Interface:** §8.6.6 grammar, priority list, `frame_bytes` formula.
**Steps:** 1. Grammar parser → `RawVideoFormat` (exhaustive tests). 2. Sidecar read/write (write path for the GUI). 3. Filename regexes + size heuristic + ranking. 4. `ovt_container_open_format("raw")` and candidate list output (`ovt probe --raw-candidates`, chapter 12 §12.2.2). 5. Tests with files named per the heuristics.
**Acceptance tests:** 1. Given `foo_1920x1080_yuv420p10le.yuv` of exactly 30 frames, When opened without sidecar, Then candidates[0] is `1920x1080 yuv420p10le` with `frame_count 30` (`[container][raw]`). 2. Given `--raw-format 1280x720:nv12:30000/1001` (chapter 12 §12.1.5), When the raw input is opened with it, Then `frame_bytes == 1382400`. 3. Given a sidecar with `header_bytes: 64`, When the raw input is opened with it, Then sample 0 `offset == 64`.
**Size:** S.
**Notes / pitfalls:** 10-bit planar formats store 16-bit little-endian samples (2 bytes per sample); `p010` is semi-planar with 16-bit samples; odd widths round chroma up.

### WI-1.230 — Implement the libavformat `Demuxer` with the custom `AVIOContext`
**Goal:** Open any supported file through `RandomAccessFile`, enumerate streams, read packets, and seek.
**Depends on:** WI-1.200, WI-0.012 (FFmpeg `core` flavour from `third_party/`).
**Produces:** `core/src/demux/{demuxer.h, demuxer.cpp, avio_adapter.cpp}`; tests `core/tests/demux/demuxer_test.cpp`; fuzz `fuzz_demuxer_open`.
**Interface:** §8.7.1 `Demuxer`, `StreamInfo`, `Packet` (without `units` until WI-1.231); §8.7.2 adapter; §8.7.5 `seek_to_time`.
**Steps:** 1. Adapter read/seek callbacks and option dictionary per format. 2. `open`, `streams()`, `select_streams`, `read_packet` with `AVPacket` lifetime management (`av_packet_unref` on next call). 3. `seek_to_time`. 4. Codec id mapping. 5. Tests: packet count equals `ffprobe -count_packets` for every corpus file; Android-style `pread` file wrapper exercised.
**Acceptance tests:** 1. Given every corpus MP4/IVF/ES/Y4M, When all packets are read, Then the count equals `ffprobe -count_packets` and every `Packet.file_pos` equals ffprobe `pos` (`[demux]`). 2. Given `seek_to_time(ts=30 s, backward)`, When it is called, Then the next packet is a keyframe with `pts ≤ 30 s`. 3. Given a 0-byte file, When it is opened, Then `open` returns `OVT_E_CORRUPT` or `OVT_E_UNSUPPORTED` without leaking (`valgrind`/LSan clean).
**Size:** M.
**Notes / pitfalls:** Never insert bitstream filters; set `AVFMT_FLAG_GENPTS` only for TS; `avformat_close_input` before freeing the AVIO buffer (`av_freep(&ctx->buffer)` then `avio_context_free`).

### WI-1.231 — Implement the unit splitter (length-prefixed, Annex B, AV1 OBU)
**Goal:** Convert a container sample into plugin-ABI units per §8.7.4, shared by Role A and Role B.
**Depends on:** WI-1.226, WI-1.227.
**Produces:** `core/src/demux/unit_split.{h,cpp}`; tests `unit_split_test.cpp`; fuzz `fuzz_unit_split`; `tools/extract_samples.py` (`--track n --samples <sel> f.mp4 -o dir/`: writes each container sample's raw bytes as `sample_<n>.bin` plus `samples.json` with `{index, offset, size, nal_length_size, nal_count}` computed by the script itself from `stsz`/`stz2`, `stsc`, `stco`/`co64` and `avcC`/`hvcC` in plain Python with its own length walk (no MP4Box on the CI runners; fragmented files are rejected for now), used to build the unit-splitter test inputs; D27).
**Interface:** §8.7.4 `split_units`, `Framing`, `SplitResult`; `Packet::unit_bytes`.
**Steps:** 1. LengthPrefixed with `nal_length_size` 1/2/4 and the Annex B-inside-avc1 fallback. 2. AnnexB via WI-1.226 span overload. 3. Av1Obu with last-OBU re-framing. 4. Raw. 5. Diagnostics `CNT-9001..9003`. 6. Tests with real samples extracted from corpora (`tools/extract_samples.py`).
**Acceptance tests:** 1. Given an `avc1` sample from `syn_h264_multislice_cavlc_mp4` (chapter 14 §14.3.3 recipe 29) extracted with `tools/extract_samples.py`, When split, Then units == the `nal_count` of `samples.json` (the script's independent length walk) for that sample and `unit_bytes(0)[0] & 0x1F` is a valid NAL type (no `0x00` first byte) (`[demux][unit_split]`). 2. Given an `av01` sample whose last OBU lacks a size field, When it is split, Then `rewritten == true` and re-parsing `unit_bytes(last)` yields `obu_has_size_field == 1` with the correct size. 3. Given a NAL length exceeding the sample, When it is split, Then `CNT-9001` Error and the unit is clamped.
**Size:** S.
**Notes / pitfalls:** Do **not** add start codes; AV1 TDs inside samples are a muxer bug but harmless.

### WI-1.232 — Implement track ↔ stream matching, `sample_index` mapping and `seek_to_sample`
**Goal:** Tie Role B packets to Role A samples and tracks.
**Depends on:** WI-1.230, WI-1.231, WI-1.218, WI-1.219.
**Produces:** `core/src/demux/match_tracks.cpp`, additions to `demuxer.cpp`; tests `core/tests/demux/match_tracks_test.cpp`; core function behind `ovt probe --demux-check` (chapter 12 §12.2.2).
**Interface:** §8.1.4 algorithm; §8.7.3 mapping; §8.7.5 `seek_to_sample`.
**Steps:** 1. `match_tracks` steps 1–6 with diagnostics. 2. `sample_index` by `pos` then `dts`. 3. `seek_to_sample` incl. ES path through `StreamIndex` offsets. 4. `--demux-check` printing matches and mismatches. 5. Tests over all corpora.
**Acceptance tests:** 1. Given every corpus MP4, When `ovt probe --demux-check` runs, Then all `vide/soun/subt` tracks have `avstream_index ≥ 0` or a `CNT-0901` names them, and `codec_private` byte comparison passes for `avcC/hvcC/av1C/vpcC` (`[demux][match]`). 2. Given `cmaf_video.cmfv`, When reading all packets, Then `sample_index` is `0,1,2,…` without gaps. 3. Given `seek_to_sample(stream, 500)`, When it is called, Then the first returned packet has `sample_index ≤ 500`, `keyframe == true`, and subsequent packets reach 500 without skipping.
**Size:** S.
**Notes / pitfalls:** libavformat may drop the first packets of a track trimmed by `elst` — keep `advanced_editlist` off; `AVStream.id` for MP4 is `track_ID`, for TS the PID.

### WI-1.233 — Extract `codec_private` for plugin `create()` and integrate with the plugin host
**Goal:** Deliver `avcC/hvcC/av1C` bytes (or `nullptr` for Annex B) to `ovt_AnalysisDecoderApi::create` for both unit-source policies.
**Depends on:** WI-1.232, WI-2.150 (plugin registry).
**Produces:** `core/src/demux/codec_private.cpp`; `unit_source_policy()` in `core/src/container/unit_source.cpp`; tests `codec_private_test.cpp`.
**Interface:** §8.7.6 table; backbone §0.6.2 `create(codec_private, len, threads, out)`; `CNT-0001`.
**Steps:** 1. Table rows for H264/HEVC/AV1/VP9 and Annex B detection. 2. Prefer `TrackInfo.codec_private[0]` when a tree exists. 3. `unit_source_policy(format)` and `CNT-0001`. 4. Annex B parameter-set pre-push ordering helper for chapter 7. 5. Tests comparing Role A and Role B bytes.
**Acceptance tests:** 1. Given `hev1_main10.mp4`, When extracted, Then `codec_private_fourcc == "hvcC"` and bytes equal the `hvcC` payload from the tree (`[demux][codec_private]`). 2. Given `syn_h265_gop60_1080p.265` (chapter 14 §14.3.3 recipe 26, Annex B), When the demuxer opens it, Then `codec_private` is empty and the first three units pushed are VPS/SPS/PPS. 3. Given `syn_av1_gop60_1080p.ivf` (chapter 14 §14.3.3 recipe 26), When the demuxer opens it, Then `fourcc == "none"`.
**Size:** S.
**Notes / pitfalls:** libavformat `extradata` for `mov` AVC is the `avcC` payload exactly; for Matroska it is `CodecPrivate` (same bytes).

### WI-1.234 — Implement the byte service: page cache, `ovt_container_bytes`, `ovt_container_box_at`, `ovt_stream_unit_bytes`
**Goal:** Provide the only byte-access path for the GUI/CLI/Python with a bounded LRU cache.
**Depends on:** WI-1.204, WI-1.231.
**Produces:** `core/src/container/byte_service.cpp`, `page_cache.{h,cpp}`, `paged_span.h`; tests `byte_service_test.cpp`; CI lint `tools/lint_no_direct_file_access.py`.
**Interface:** §8.8 C functions; `PageCache` parameters (64 × 64 KiB).
**Steps:** 1. `PageCache` with mutex, LRU, stats. 2. `ovt_container_bytes` with clipping and the 1 MiB limit. 3. `ovt_container_box_at` via interval search over the tree (binary search on sorted top-level, then descend). 4. `ovt_stream_unit_bytes` (raw + RBSP via chapter 7 EPB remover; AV1 re-framing). 5. Lint script for `app/` and `python/`.
**Acceptance tests:** 1. Given a 4 GiB file, When 10 000 random `ovt_container_bytes(offset, 16)` calls run, Then RSS growth ≤ 8 MiB and hit rate ≥ 90 % for sequential rows (`[container][bytes]`). 2. Given `ovt_container_bytes(size − 5, 16)`, When it is called, Then `out.size == 5` and `OVT_OK`. 3. Given an offset inside `moov/trak[0]/tkhd`, When `ovt_container_box_at` runs, Then it returns the `tkhd` offset with depth 2. 4. Given `grep -rn "mmap\|QFile::map" app/ python/`, When it runs, Then no match (CI lint).
**Size:** S.
**Notes / pitfalls:** every `ovt_Buffer` filled by the byte service is caller-owned (`ovt_buffer_free`, chapter 12 §12.3.1 rule 3a) — the page cache is never exposed by pointer; the thread-safety exception of §8.8 must be documented in the header.

### WI-1.235 — Add `ovt probe` container subcommand hooks
**Goal:** Expose every container view on the CLI as JSON and text.
**Depends on:** WI-1.224, WI-1.232, WI-1.234, WI-1.500.
**Produces:** `cli/src/cmd/probe.cpp`; `docs/spec/12-cli-and-api.md` cross-reference; tests `cli/tests/probe_test.sh`.
**Interface:** the `ovt probe` grammar is chapter 12 §12.2.2 (`--boxes`, `--tracks`, `--samples`, `--fragments`, `--edits`, `--enc`, `--hdr`, `--brands`, `--validate`, `--validate-level`, `--list-diagnostics`, `--demux-check`, `--raw-candidates`, `--bytes`, `--expand`, `-t`, `--format`); exit codes are chapter 12 §12.1.3 (an `Error`-severity diagnostic → exit 5 `CORRUPT` only with `--validate`; otherwise 0 with the diagnostics in the output). This WI delivers the core functions behind those flags: `ovt_container_expand_all`, the diagnostics-registry dump (`ovt_container_list_diagnostics`, chapter 12 §12.3.7) and the `--bytes` hex helper over `ovt_container_bytes`.
**Steps:** 1. `ovt_container_expand_all` (DFS over the lazy tree in a job, cancellation every 50 ms). 2. `ovt_container_list_diagnostics` from `diag_codes.inc` (one row per code: code, severity, message template, spec reference). 3. Hex helper (`--bytes OFFSET:LEN` → 16-bytes-per-row dump with offsets and ASCII column) over `ovt_container_bytes`. 4. Text-tree line format of chapter 12 §12.2.2 documented in `docs/spec`. 5. Unit tests for the three functions.
**Acceptance tests:** 1. Given any corpus MP4, When `ovt_container_expand_all` runs, Then no `BoxNode` outside `mdat` carries `CNT-0007` (`[container][probe]`). 2. Given `ovt_container_list_diagnostics`, When it is called, Then the buffer lists ≥ 150 codes, each exactly once, with severity and spec reference. (Tests of the subcommand itself are WI-1.503, chapter 16 §16.3.8.)
**Size:** S.
**Notes / pitfalls:** Text mode (chapter 12 §12.2.2) prints `fourcc @offset +size [vN f=…]` per node with indentation = depth × 2 so goldens are diffable. **Overlap:** this WI delivers only the C++/C functions the CLI calls (`ovt_container_expand_all`, the diagnostics-registry dump, the `--bytes` hex helper of the byte service) and the `docs/spec` cross-reference; WI-1.503 owns `cli/src/cmd/probe.cpp`; acceptance tests 1–2 move to WI-1.503. See chapter 16 §16.3.

### WI-1.236 — Build the golden-test harness and oracle normalisers
**Goal:** Deliver the chapter-8 contributions to the ISOBMFF golden tests of chapter 14: the container field maps and the `coverage` report (reduced; see Overlap).
**Depends on:** WI-1.235.
**Produces:** `tools/golden.py` (subcommand `coverage` only), `tools/oracle_maps/*.json` (`mp4box_fields.json`, `mp4ff_fields.json`, `mediainfo_hdr.json`; consumed by WI-0.555's normalisers in `tests/oracles/normalize/`).
**Interface:** `tools/golden.py coverage --features <FileFormatConformance data/file_features dir> --registry build/box_registry.json`; field-map JSON shape `{ "<oracle name>": "<ISO syntax name>" }`.
**Steps:** 1. Field maps (start with the 60 most common fields; grow per WI) and a unit test that every map value is a field name emitted by some hand decoder. 2. `coverage` report against FileFormatConformance `file_features`. 3. Hand the maps to WI-0.555 (chapter 14) and check the `box-tree` goldens of WI-1.571 consume them.
**Acceptance tests:** 1. Given `tools/golden.py coverage`, When it runs, Then it prints hand-decoder coverage ≥ 95 % of boxes exercised by the conformance corpus (`[container][coverage]`). 2. Given the field maps, When every mapped name is checked against the box registry, Then no unknown field name remains. (The former tests 1–2 — CI pass without oracles; `tkhd.track_ID` regression naming `moov/trak[0]/tkhd.track_ID` — moved to WI-1.571, chapter 16 §16.3.9.)
**Size:** M.
**Notes / pitfalls:** MP4Box XML attribute names change between versions — the oracle image of WI-0.554 pins them; run Python with `-I`. **Overlap:** this WI delivers only the `coverage` report (`tools/golden.py coverage` against FileFormatConformance `file_features`) and the container field maps `tools/oracle_maps/*.json` consumed by WI-0.555; oracle runners, normalisers and goldens are WI-0.554/WI-0.555/WI-0.564, the suite is WI-1.571. See chapter 16 §16.3.

### WI-1.237 — Write corpus manifests, fetcher integration and licence tags
**Goal:** Make every corpus of §8.9.3 fetchable and tagged for CI lanes.
**Depends on:** WI-0.551 (`tools/fetch_corpora.py` and manifest schema), WI-0.552 (manifest population).
**Produces:** container entries (tags `container-smoke`, `container-full`, `isobmff-broken`; `expected` blocks) in the single `tests/corpora/manifest.json` (WI-0.551/WI-0.552); the container section of `tests/corpora/README.md` (WI-0.040) listing licences, sizes and tags.
**Interface:** manifest JSON shape of chapter 14 §14.2.4 plus the `expected` object of §8.9.3.
**Steps:** 1. Select the ≤ 300 MB `container-smoke` tag set (one file per feature) and the `container-full` set. 2. SHA-256 and licence per file; LFS fetch for FileFormatConformance. 3. `expected` blocks (format, codec, hdr kind, diagnostics). 4. Document redistribution constraints (no committing media).
**Acceptance tests:** 1. Given a clean machine on a 100 Mbit/s link, When `tools/fetch_corpora.py --tags container-smoke` runs, Then all files download and hash-verify in ≤ 5 min. 2. Given a manifest entry with a wrong hash, When the fetcher runs, Then it exits non-zero naming the file.
**Size:** S.
**Notes / pitfalls:** Dolby test clips may require login — store mirrors or mark `optional: true`; CTA WAVE content is large — select ≤ 10 files. **Overlap:** this WI delivers only the container-specific entries (tags `container-smoke`, `container-full`, `expected` blocks) inside the single `tests/corpora/manifest.json` of WI-0.551/WI-0.552; no per-directory manifests and no `--lane` flag. See chapter 16 §16.3.

### WI-1.238 — Add fuzz targets for every container parser and generated per-decoder fuzzers
**Goal:** Satisfy FR-CNT-106 (§8.1.2, §8.9.4).
**Depends on:** WI-1.224, WI-1.229, WI-1.231, WI-1.230.
**Produces:** `core/tests/fuzz/fuzz_{isobmff_tree,isobmff_sample_table,isobmff_fragments,annexb_split,obu_split,ivf,y4m,raw_format_string,unit_split,demuxer_open}.cpp`; `tools/gen_fuzz_targets.py` → `core/tests/fuzz/gen/fuzz_box_<fourcc>.cpp`; `tools/extract_box_payloads.py`; CMake `OVT_ENABLE_FUZZ` lane; seed corpora under `core/tests/fuzz/seeds/`.
**Interface:** libFuzzer `LLVMFuzzerTestOneInput`; memory `RandomAccessFile` adapter.
**Steps:** 1. In-memory `RandomAccessFile`. 2. Hand targets. 3. Generator scanning `OVT_BOX_DECODER(` registrations. 4. Seed extraction. 5. CI nightly lane with 10 min per target and crash artefact upload.
**Acceptance tests:** 1. Given the `linux-x64-fuzz` preset, When `cmake --preset linux-x64-fuzz && cmake --build --preset linux-x64-fuzz && ctest --preset linux-x64-fuzz -L fuzz-smoke` runs, Then every target runs 60 s in the smoke lane without findings. 2. Given a known crasher from the broken corpus era (regression dir), When the target runs over its seed set, Then the crasher is present in the seeds and passes.
**Size:** M.
**Notes / pitfalls:** Set `-rss_limit_mb=2048` because sample tables can legitimately allocate; cap `kMaxSamples` lower (1 M) when the preprocessor symbol `OVT_ENABLE_FUZZ` is defined (set by the `OVT_ENABLE_FUZZ=ON` build, chapter 13 §13.2.2). **Overlap:** this WI also delivers the splitter targets formerly listed in WI-0.557 and registers every target with `ovt_add_fuzz_target` (WI-0.557); it does not create a second fuzz CMake lane. See chapter 16 §16.3.

### WI-1.239 — Add performance tests and the perf corpus generator
**Goal:** Enforce FR-CNT-018 and the demux throughput budget.
**Depends on:** WI-1.218, WI-1.230.
**Produces:** `tests/perf/core/bench_container.cpp` (per-area layout of chapter 14 §14.2.1, D14; benchmarks `bench_container_open_moov_1h`, `bench_container_sample_table_1h`, `bench_container_fragments_3600`, `bench_demux_packets_1h`, registered in the WI-0.560 harness, names per chapter 14 §14.9.3); container rows in `tests/perf/budgets.json`; the recipes `syn_mp4_frag_3600` (3 600 fragments) and `syn_heif_1000items` added to `tests/corpora/synthetic/recipes.json` (WI-0.553 generator).
**Interface:** Google Benchmark `BENCHMARK(...)` registrations; `ctest -L perf`; budgets/baselines per chapter 14 §14.9.
**Steps:** 1. Generator (deterministic seeds). 2. Timers around open/tree/sample table/fragments/demux-all-packets. 3. Baseline + 20 % regression gate. 4. Document reference machine.
**Acceptance tests:** 1. Given `syn_h264_1h_1080p.mp4`, When `bench_container_open_moov_1h` and `bench_container_sample_table_1h` run on the `m1-air-8gb` reference (chapter 14 §14.9.2), Then their medians, and that of `bench_demux_packets_1h` (no decode), meet the budgets of chapter 14 §14.9.3 (see `tests/perf/budgets.json`; D19 — this chapter repeats no numbers). 2. Given `syn_mp4_frag_3600`, When the benchmark runs, Then the `bench_container_fragments_3600` median meets its chapter 14 §14.9.3 budget (see `tests/perf/budgets.json`).
**Size:** S.
**Notes / pitfalls:** Warm the page cache once before timing; run perf tests single-threaded. **Overlap:** this WI delivers only the `bench_container_*` benchmarks in the harness of WI-0.560; the perf files are generated by `tests/corpora/synthetic/generate.py` of WI-0.553 (no `tools/make_perf_files.py`). See chapter 16 §16.3.

### Phase 4 (WI-4.200 … WI-4.216)

### WI-4.200 — Generate the EBML/Matroska schema tables
**Goal:** Produce `ebml_schema.gen.cpp` from `ebml_matroska.xml` and the EBML header schema.
**Depends on:** WI-1.203 (generator conventions).
**Produces:** `tools/gen_ebml_schema.py`, `tools/registry_inputs/ebml/{ebml_matroska.xml, ebml_header.xml}`, `core/src/container/ebml/ebml_schema.gen.cpp`, tests `ebml_schema_test.cpp`.
**Interface:** `struct EbmlElementDef { uint32_t id; const char* name; EbmlType type; const char* path; int min_occurs, max_occurs; const char* default_value; bool unknown_size_allowed; }`.
**Steps:** 1. Fetch pinned schema (RFC 9559 / CELLAR repo). 2. Parse `<element>` entries (`id`, `name`, `type`, `path`, `minOccurs`, `maxOccurs`, `default`, `unknownsizeallowed`, `recurring`). 3. Emit sorted table + `lookup(id)`. 4. `--check` CI mode.
**Acceptance tests:** 1. Given the committed inputs, When `tools/gen_ebml_schema.py --check` runs, Then it passes. 2. Given the generated table, When `lookup(0x18538067)` is called, Then `name == "Segment"` and `unknown_size_allowed`.
**Size:** S.
**Notes / pitfalls:** IDs in the XML are hex with the length marker included (`0x1A45DFA3`); paths use `\` separators and `(1*)` recursion markers.

### WI-4.201 — Implement the EBML reader core
**Goal:** Walk EBML elements into `BoxNode`s with spans, handling vints, unknown sizes, lazy `Cluster`s.
**Depends on:** WI-4.200.
**Produces:** `core/src/container/ebml/ebml_walk.cpp`; tests `ebml_walk_test.cpp`; fuzz `fuzz_ebml_tree`.
**Interface:** §8.6.7 EBML paragraph; reuse of `BoxNode` (`fourcc` = hex id string, `description` = name).
**Steps:** 1. vint id/size readers (RFC 8794 §4–5), reserved-id and all-ones detection (`CNT-EBML-0001`…`CNT-EBML-0005`, §8.5). 2. Element walk with schema-typed value decoding (uint/int/float/string/utf8/date/binary) into one `Field` with span. 3. Unknown-size handling (scan to a sibling-or-higher-level ID). 4. Lazy `Cluster` and eager `Cues`/`Tracks`/`Info`. 5. Diagnostics `CNT-EBML-xxxx` (invalid vint, element exceeding parent, unknown id, mandatory missing).
**Acceptance tests:** 1. Given matroska-test-files `test1..test8.mkv`, When walked, Then element `(offset,size,id)` set equals `mkvinfo -v -v -z` golden (`[container][ebml]`). 2. Given a live-recorded WebM with unknown-size `Segment`/`Cluster`s, When it is parsed, Then all clusters are found. 3. Given the seed corpus of `fuzz_ebml_tree`, When the target runs for 10 min under ASan/UBSan (`fuzz-nightly` lane, chapter 13 §13.7.1), Then no crash, leak or sanitizer report occurs.
**Size:** M.
**Notes / pitfalls:** `0xFF` single-byte size = unknown; CRC-32 elements precede siblings; `Void` elements are Opaque.

### WI-4.202 — Build the Matroska model: tracks, colour/HDR, DV block additions, sample table
**Goal:** Produce `TrackInfo`, `SampleTable`, `HdrMetadata`, `EncryptionInfo` for Matroska/WebM.
**Depends on:** WI-4.201, WI-1.208, WI-1.223.
**Produces:** `core/src/container/ebml/matroska_model.cpp`; tests `matroska_model_test.cpp`.
**Interface:** §8.6.7 elements of interest; `CodecPrivate` reuse with `ancestors = ["mkv","CodecPrivate"]`.
**Steps:** 1. `Tracks/TrackEntry` → `TrackInfo` (`CodecID` → `Codec`; `CodecPrivate` through group C decoders). 2. `Video/Colour` + `MasteringMetadata` → `HdrMetadata` box-level signals (`source = "mkv_colour"`). 3. `BlockAdditionMapping` DV config + `BlockAdditions` RPU detection → `nal_62_rpu`-equivalent signal `mkv_blockadd_dv`. 4. Cluster/Block/lacing expansion into `SampleTable` (lazy per cluster, timestamps in `TimestampScale`). 5. `ContentEncryption` → `EncryptionInfo`.
**Acceptance tests:** 1. Given `dv_p5.mkv`, When built, Then `kind == DolbyVision`, `dv_profile == 5` and signals include `mkv_blockadd_dv` (`[container][matroska]`). 2. Given `hdr10.mkv`, When it is parsed, Then `MaxCLL/MaxFALL` match `mediainfo --Output=JSON`. 3. Given `lacing_xiph.webm`, When it is parsed, Then per-frame `offset/size` equal the `mkvinfo -v -v -z` frame listing (ffprobe is unsuitable: its `pos` is the block position, not the frame).
**Size:** M.
**Notes / pitfalls:** Block timestamps are signed 16-bit relative to the cluster; `TimestampScale` default 1 000 000 ns.

### WI-4.203 — Matroska goldens and demux integration
**Goal:** Add mkvinfo/MediaInfo goldens and `sample_index` mapping for Matroska packets.
**Depends on:** WI-4.202, WI-1.232.
**Produces:** `tests/oracles/normalize/mkvinfo.py` (chapter 14 normaliser location), goldens under `tests/golden/ebml/`, `demux` additions for `pkt->pos` → block offset mapping; tests.
**Interface:** §8.7.3 step 3.
**Steps:** 1. Normaliser. 2. Goldens for the WebM/MKV corpus. 3. Block-offset mapping (first frame of a laced block shares the block pos; subsequent frames matched by order). 4. `--demux-check` for MKV.
**Acceptance tests:** 1. Given every MKV corpus file, When compared, Then goldens pass. 2. Given `lacing_xiph.webm`, When demuxed, Then `sample_index` increases by 1 per packet.
**Size:** S.
**Notes / pitfalls:** libavformat `AVStream.id` for Matroska is `TrackNumber`.

### WI-4.204 — Implement MPEG-TS packet sync and packet/adaptation-field decoding
**Goal:** Walk a TS file into packets with header and adaptation-field fields (incl. PCR) as `BoxNode`s, lazily.
**Depends on:** WI-1.200.
**Produces:** `core/src/container/mpegts/ts_packets.cpp`; tests `ts_packets_test.cpp`; fuzz `fuzz_mpegts`.
**Interface:** §8.6.7 MPEG-TS paragraph; node per packet (lazy windows of 65 536 packets), fields per ISO/IEC 13818-1 §2.4.3.
**Steps:** 1. Packet size detection (188/192/204) and resync. 2. Header + adaptation field decode with bit spans; PCR as `base·300 + ext`. 3. Per-PID statistics (count, CC errors, scrambled). 4. Diagnostics `CNT-TS-1001 TS_sync_loss`, `CNT-TS-1002 Sync_byte_error`, `CNT-TS-1006 Continuity_count_error`, `CNT-TS-2001 Transport_error`.
**Acceptance tests:** 1. Given tsduck-test `input/*.ts`, When walked, Then PID list and packet counts equal `tsanalyze --json` (`[container][mpegts]`). 2. Given a file with a 1-byte cut, When it is parsed, Then resync succeeds and `CNT-TS-1001` is emitted once.
**Size:** M.
**Notes / pitfalls:** CC does not increment for packets without payload or duplicates; 192-byte (M2TS) packets have a 4-byte timestamp prefix.

### WI-4.205 — Implement PSI/SI table parsing (PAT, PMT, CAT, descriptors; DVB SDT/EIT/TDT read-only)
**Goal:** Reassemble sections and decode PAT/PMT/CAT with ES descriptors into tree nodes and a program map.
**Depends on:** WI-4.204.
**Produces:** `core/src/container/mpegts/psi.cpp`, `descriptors.cpp`; tests `psi_test.cpp`.
**Interface:** 13818-1 §2.4.4 syntax; descriptor tags `0x02–0x0E`, `0x05 registration`, `0x0A ISO639`, `0x28 AVC video`, `0x2B AAC`, `0x38 HEVC video`, `0x6A AC-3 (DVB)`, `0x7A E-AC-3`, `0x56 teletext`, `0x59 subtitling`, `0x7F extension` (HEVC timing/HDR); CRC-32 check.
**Steps:** 1. Section reassembly per PID with `pointer_field`. 2. PAT/PMT/CAT decode + CRC. 3. Descriptor registry (hand table) with unknown-descriptor hex fallback. 4. DVB SDT/EIT/TDT/TOT basic decode. 5. Diagnostics `CNT-TS-1003 PAT_error`, `CNT-TS-1005 PMT_error`, `CNT-TS-1007 PID_error`, `CNT-TS-2002 CRC_error`, `CNT-TS-2007 CAT_error`.
**Acceptance tests:** 1. Given tsduck-test streams, When parsed, Then programs/PIDs/stream types equal `tstables --all-sections --xml` (`[container][mpegts][psi][needs-oracle:tstables]`; TSDuck is part of the oracle image of chapter 14 §14.2.6). 2. Given a PMT with a corrupted CRC, When it is parsed, Then `CNT-TS-2002`.
**Size:** M.
**Notes / pitfalls:** Sections can span packets and multiple sections can share a packet; `version_number` changes must replace tables.

### WI-4.206 — Implement PES reassembly, the TS `SampleTable` and the PCR/PTS/DTS timing table
**Goal:** Produce per-PID elementary streams as samples with timestamps and a timing table for the GUI graph.
**Depends on:** WI-4.205, WI-1.226.
**Produces:** `core/src/container/mpegts/pes.cpp`, `timing.cpp`; fills the `TsTiming` table of chapter 06 §6.4 (`pcr:[TsPcrPoint{packet_index, pcr}]`, `pts_dts:[TsPtsPoint{pid, packet_index, pts, dts}]`, `bitrate_per_pid:[TsPidBitrate{pid, window_ms, bps:[uint]}]`; schema text lives in chapter 06 only); tests `core/tests/container/mpegts_pes_test.cpp`.
**Interface:** 13818-1 §2.4.3.6–7 PES header; `SampleTable` per PID (`timescale 90000`, `offset` = first payload byte of the PES, `size` = reassembled size, `units` via Annex B).
**Steps:** 1. PES header decode (33-bit timestamps). 2. Reassembly on `payload_unit_start_indicator`/`PES_packet_length`. 3. `SampleTable` building (lazy) and unit splitting. 4. Timing table and derived bitrate. 5. Diagnostics `CNT-TS-2003 PCR_repetition_error (> 40 ms)`, `CNT-TS-2004 PCR_discontinuity_indicator_error`, `CNT-TS-2005 PCR_accuracy_error`, `CNT-TS-2006 PTS_error (> 700 ms)`.
**Acceptance tests:** 1. Given `ts_h264_dvb` (tsduck-test), When built, Then PES count and PTS/DTS per PES equal ffprobe `-show_packets` (`[container][mpegts][pes]`). 2. Given a stream with a 1 s PCR gap, When it is parsed, Then `CNT-TS-2003`.
**Size:** M.
**Notes / pitfalls:** `PES_packet_length == 0` is legal for video; PTS wraps at 2^33.

### WI-4.207 — Implement the TR 101 290 priority 1 and 2 check set and report
**Goal:** Consolidate `CNT-TS-1xxx/2xxx` into the TR 101 290 report with counts and first-occurrence offsets.
**Depends on:** WI-4.206.
**Produces:** `core/src/container/mpegts/tr101290.cpp`; fills the `TsReport` table of chapter 06 §6.4 (`checks:[TsCheck{id:string /*"1.1"*/, name, count, first_packet_index, first_offset}]`); core function behind `ovt probe --tr101290` (chapter 12 §12.2.2; the flag itself is WI-1.503's command file); tests `core/tests/container/tr101290_test.cpp`.
**Interface:** ETSI TR 101 290 v1.4.1 §5.2.1 (P1: 1.1 TS_sync_loss, 1.2 Sync_byte_error, 1.3 PAT_error_2, 1.4 Continuity_count_error, 1.5 PMT_error_2, 1.6 PID_error) and §5.2.2 (P2: 2.1 Transport_error, 2.2 CRC_error, 2.3 PCR_error/2.3a PCR_repetition_error/2.3b PCR_discontinuity_indicator_error, 2.4 PCR_accuracy_error, 2.5 PTS_error, 2.6 CAT_error).
**Steps:** 1. Timer semantics (PAT every 0.5 s, PMT every 0.5 s, PID timeout 5 s user-configurable). 2. Report builder. 3. Golden vs TSDuck (`tsp -P continuity` for CC errors, `tsanalyze` for PCR/PTS statistics) and DVB Inspector counts; exact equality for P1 counts, ±1 tolerance for timing-window checks. 4. CLI text/JSON.
**Acceptance tests:** 1. Given a clean broadcast capture, When the TR 101 290 report runs, Then it shows 0 for all P1 checks. 2. Given `syn_ts_cc_errors` (synthetic; recipe added by WI-4.207 to `recipes.json`), When the report runs, Then `Continuity_count_error == N` matching TSDuck `tsp -P continuity` count.
**Size:** S.
**Notes / pitfalls:** P3 checks are out of scope; PCR accuracy needs a stable bitrate model — use the TR's ±500 ns against the regression of PCR vs byte position.

### WI-4.208 — Implement the DASH MPD parser and segment URL expansion
**Goal:** Parse an MPD into periods/adaptation sets/representations with a fully expanded segment list.
**Depends on:** WI-1.200; an XML parser from vcpkg (`pugixml`, MIT).
**Produces:** `core/src/container/virtual_file/mpd.cpp`; `Manifest` FlatBuffer (`periods → adaptation_sets → representations → segments{url, byte_range, start_time, duration, number}`); tests on DASH-IF MPDs.
**Interface:** ISO/IEC 23009-1 §5.3 (`SegmentTemplate` `$RepresentationID$`, `$Number%05d$`, `$Time$`, `$Bandwidth$`; `SegmentTimeline S@t/@d/@r` incl. `r = −1`; `SegmentList`; `SegmentBase@indexRange` + `sidx`-driven subsegments; `BaseURL` resolution; `xlink` ignored with Info).
**Steps:** 1. XML load, namespace-tolerant. 2. Template expansion and timeline arithmetic (`presentationTimeOffset`, `timescale`, `availabilityStartTime` for static only; for a `SegmentTemplate` without a timeline `ManifestSegment.start_time = presentationTimeOffset + k × duration`, the value `$Time$` expands to). 3. `SegmentBase` → an injected `SidxResolver` callback `(url, index byte range) → parsed sidx references` derives the subsegments (no dependency on the sidx decoder or on HTTP; without a resolver a `SegmentBase` representation yields init plus one whole-resource segment and `CNT-DASH-0003`; WI-4.210 supplies the real resolver). Diagnostics `CNT-DASH-0001`…`0009` (§8.5); `ManifestPeriod.start/duration` are milliseconds, `−1` unknown (chapter 06 §6.4). 4. Tests over DASH-IF test vectors (static MPDs).
**Acceptance tests:** 1. Given `dashif/bbb_30fps.mpd`, When expanded, Then the segment URL list equals `tools/ref_mpd.py` (reference expansion using `python-mpegdash`) (`[container][dash]`). 2. Given `r = −1` timeline, When it is parsed, Then segments fill the period duration exactly.
**Size:** M.
**Notes / pitfalls:** `$Number$` starts at `startNumber` (default 1); `%0Nd` formatting; dynamic MPDs are snapshotted.

### WI-4.209 — Implement the HLS M3U8 parser (master and media playlists)
**Goal:** Parse `#EXT-X-STREAM-INF` variants and media playlists with `#EXT-X-MAP`, `#EXTINF`, `#EXT-X-BYTERANGE`, `#EXT-X-KEY`, `#EXT-X-DISCONTINUITY(-SEQUENCE)`, `#EXT-X-MEDIA-SEQUENCE`, `#EXT-X-PROGRAM-DATE-TIME`.
**Depends on:** WI-4.208 (shared `Manifest`).
**Produces:** `core/src/container/virtual_file/m3u8.cpp`; tests on Apple examples.
**Interface:** RFC 8216 + Apple HLS extensions (`CODECS`, `VIDEO-RANGE=PQ|HLG|SDR`, `SUPPLEMENTAL-CODECS` for DV).
**Steps:** 1. Line parser with attribute lists (quoted strings). 2. Variant selection data (`BANDWIDTH`, `RESOLUTION`, `CODECS`, `VIDEO-RANGE`). 3. Media playlist to segment list with byte ranges and init segments (`#EXT-X-MAP` may change per discontinuity). 4. Key info into `Manifest.encryption` (method, URI, IV) — no decryption.
**Acceptance tests:** 1. Given Apple `bipbop_adv_example_hevc/master.m3u8`, When parsed, Then variants count/attributes equal the reference parse (`tools/ref_m3u8.py` using `m3u8` PyPI) (`[container][hls]`). 2. Given a byterange playlist, When parsed, Then segments carry `byte_range{offset,length}` with offsets computed from previous entries when `@offset` is omitted.
**Size:** S.
**Notes / pitfalls:** `#EXT-X-BYTERANGE` without `@` continues from the previous segment's end; `#EXT-X-MAP` applies to following segments only.

### WI-4.210 — Implement `VirtualFile` and segment provenance
**Goal:** Present init+media segments as one `RandomAccessFile` with a `SegmentMap` and `ovt_container_segment_for_offset`.
**Depends on:** WI-4.208, WI-4.209; HTTP fetch via libcurl (vcpkg, curl licence) or platform HTTP (Android `HttpURLConnection` through JNI is **not** used — libcurl everywhere for determinism).
**Produces:** `core/src/container/virtual_file/{virtual_file.cpp, segment_cache.cpp}`; implementation of the C API `ovt_container_open_manifest(const char* url_or_path, const char* variant_selector, ovt_Container** out)` (chapter 12 §12.3.7); tests with a local HTTP server fixture (`python3 -I -m http.server` in test).
**Interface:** §8.6.7 DASH/HLS paragraph; `SegmentMap` entry fields; cache dir `cache/segments/<sha256(url)>`.
**Steps:** 1. Logical layout computation (sizes known from byte ranges or `HEAD`/`Content-Length`; unknown sizes fetched eagerly in order). 2. `read_exact` across segment boundaries. 3. Disk cache with size cap 2 GiB LRU. 4. Provenance JSON. 5. Tree: `styp` per segment stays top-level; `BoxNode.description` suffixed with ` [seg N]`.
**Acceptance tests:** 1. Given a local DASH-IF package served over HTTP, When opened, Then the tree equals the tree of the concatenated file (`cat init seg1..N`) and `ovt_container_segment_for_offset` returns `segment_index 3` for an offset inside segment 3 (`[container][virtual_file]`). 2. Given an HLS fMP4 variant, When its fragments are parsed, Then `TrackInfo`, `FragmentTimeline` and `tfdt` continuity checks work unchanged.
**Size:** M.
**Notes / pitfalls:** Segments may be served with `Content-Encoding: gzip` — disable via `Accept-Encoding: identity`; respect `OVT_OFFLINE=1` env to forbid network in tests.

### WI-4.211 — MXF: libavformat-only placeholder and deferred KLV reader decision record
**Goal:** Make `.mxf` open for playback/demux via Role B with a clear placeholder tree, and record the deferral.
**Depends on:** WI-1.230.
**Produces:** `core/src/container/mxf_placeholder.cpp`; `docs/adr/ADR-CNT-MXF.md` (Open question for the owner: build own KLV reader vs bmx-informed, effort 3–4 weeks); tests.
**Interface:** `ContainerTree.format == "mxf"`, root node `description = "MXF structure view not implemented (phase 4+)"`, `TrackInfo` from libavformat streams.
**Steps:** 1. Sniff (`06 0E 2B 34` with run-in search). 2. Tracks from `Demuxer::streams()`. 3. ADR text with bmx URL and licence (Apache-2.0).
**Acceptance tests:** 1. Given `tests/corpora/mxf/op1a_xdcam.mxf`, When probed, Then `format == "mxf"`, one video `TrackInfo` with `codec == MPEG2`, and playback packets are readable.
**Size:** S.
**Notes / pitfalls:** Do not start the KLV reader without an owner decision (backbone §0.2 "Open question" rule).

### WI-4.212 — CENC detail: `cbcs` pattern view, PlayReady/Widevine payload decoders in CLI, PIFF parity
**Goal:** Complete the encryption report: full `EncryptionInfo` CLI output, pattern-expanded hex ranges in the GUI model, PIFF ↔ CENC equivalence.
**Depends on:** WI-1.221, WI-1.216.
**Produces:** `core/src/container/isobmff/cenc_report.cpp` (the `cbcs` pattern view and the PlayReady/Widevine `pssh` payload decoders behind `ovt probe --enc --format json`, chapter 12 §12.2.2); tests `core/tests/container/cenc_report_test.cpp`.
**Interface:** `EncryptionInfo` complete; `PsshInfo.summary` text rules (`Widevine: provider=…, key_ids=[…], policy=…`; `PlayReady: KID=…, LA_URL=…, ALGID=…`).
**Steps:** 1. Report builder across all tracks. 2. PIFF uuid boxes mapped into the same `EncryptionInfo` fields. 3. Key-rotation summary (distinct KIDs with sample ranges). 4. Tests on Bento4 PlayReady/Widevine and Smooth Streaming corpora.
**Acceptance tests:** 1. Given `bento4_playready` (§8.9.3), When `ovt probe --enc --format json` runs, Then `pssh[0].summary` contains the KID from WRMHEADER and `samples[0].kid` equals it. 2. Given `smooth_piff.ismv`, When it is parsed, Then `EncryptionInfo.scheme_type == "piff"` and per-sample IVs are listed.
**Size:** S.
**Notes / pitfalls:** PlayReady KID in WRMHEADER is base64 of a little-endian GUID — byte-swap the first three fields to compare with `tenc.default_KID`.

### WI-4.213 — HEIF derived items, MIAF checks and item payload view
**Goal:** Resolve `grid`/`iovl`/`iden` derived images, run MIAF structural checks, and expose item payload bytes for the hex view.
**Depends on:** WI-1.214, WI-1.222.
**Produces:** `core/src/container/isobmff/heif_items.cpp`; fills the `ItemInfo` table of chapter 06 §6.4 (`item_id, type, name, properties:[string], extents:[ByteSpan], references:[ItemReference{type, to_item_id}], is_primary, is_hidden, derived_of:[uint], width, height`; root vector `ContainerRoot.items`); implementation of the C API `ovt_container_items(ovt_Container*, ovt_Buffer* out)` and `ovt_container_item_bytes(ovt_Container*, uint32_t item_id, ovt_Buffer* out)` (chapter 12 §12.3.7); tests `core/tests/container/heif_items_test.cpp`.
**Interface:** 23008-12 §6.6 (`grid` payload: `version u8`, `flags u8`, `rows_minus_one u8`, `columns_minus_one u8`, `output_width u16/u32`, `output_height u16/u32`; `iovl`: `version`, `flags`, `canvas_fill_value u16[4]`, `output_width/height`, `[horizontal_offset, vertical_offset]`); MIAF checks `CNT-8010..8014`.
**Steps:** 1. Item index with resolved extents (`idat` construction method). 2. Derived item payload decoders. 3. MIAF checks. 4. Item bytes service via page cache. 5. Tests on Nokia conformance and iPhone HEICs.
**Acceptance tests:** 1. Given `heif_heic_grid` (Nokia conformance, §8.9.3), When items are listed, Then the primary item is `grid` with `rows × columns` tiles referencing `hvc1` items and `output_width/height` equal `ispe` of the primary (`[container][heif][items]`). 2. Given an AVIF missing `pixi`, When it is parsed, Then `CNT-8012`.
**Size:** M.
**Notes / pitfalls:** `grid` `flags & 1` selects 32-bit output dimensions; tiles are in row-major order via `dimg` references.

### WI-4.214 — Phase-4 hand decoders (rows marked phase 4 in §8.3.1–§8.3.10)
**Goal:** Add the remaining decoders: `trgr`, `elng`, `hmhd`, `pdin`, `text` (QT), `dfLa`, `dac3/dec3`, `st3d/sv3d`, `ccst`, `chnl`, `padb`, `stdp`, `stsh`, `leva`, `grpl`, `xml /bxml`, `ipro`, 3GPP asset boxes, `tcmi`, `load`, `clip/crgn`, `matt/kmat`, `glbl`, Sony/Canon uuids.
**Depends on:** WI-1.216.
**Produces:** `core/src/container/isobmff/decoders/phase4_misc.cpp`; tests `core/tests/container/phase4_decoders_test.cpp`; goldens under `tests/golden/isobmff/`.
**Interface:** §8.3 rows with Phase 4.
**Steps:** 1. Audio codec configs (`dfLa`, `dac3/dec3`, `chnl`). 2. Spatial media and 3GPP. 3. Rare ISO boxes. 4. QuickTime leftovers. 5. Goldens vs MP4Box/mp4ff.
**Acceptance tests:** 1. Given `gpac_eac3_atmos` (GPAC test media, §8.9.3), When probed, Then `dec3.data_rate` and `num_ind_sub` equal the MP4Box golden. 2. Given a 360° MP4 with `sv3d`, When probed, Then `proj/equi` bounds decode. 3. Given the conformance corpus, When the box-coverage report (WI-1.224) runs, Then ≥ 99 % of its boxes are decoded by a hand decoder.
**Size:** M.
**Notes / pitfalls:** `dec3.chan_loc` exists only when `num_dep_sub > 0`.

### WI-4.215 — TS and Matroska demux (Role B) integration for the analysis path
**Goal:** Make `unit_source_policy` return `OwnReader` for `ebml`/`mpegts` and wire their `SampleTable`s into the analysis path.
**Depends on:** WI-4.202, WI-4.206, WI-1.233.
**Produces:** updates to `core/src/container/unit_source.cpp`, `core/src/demux/codec_private.cpp`; tests `core/tests/demux/role_equivalence_test.cpp` comparing Role A vs Role B unit streams on MKV/TS corpora.
**Interface:** §8.1.1 rule 4; `CNT-0001`.
**Steps:** 1. Policy switch. 2. Unit stream equivalence test (sequence of unit hashes from Role A equals Role B for the same track, after stripping libavformat-inserted parameter sets if any). 3. Seek-to-sample via Cues / PES index.
**Acceptance tests:** 1. Given every MKV/TS corpus file, When units are produced by both roles, Then the SHA-256 of concatenated unit bytes match (`[demux][equivalence]`). 2. Given `seek_to_sample(…, 1000)` on an MKV, When it is called, Then it lands on a Cues-listed keyframe ≤ 1000.
**Size:** S.
**Notes / pitfalls:** libavformat's Matroska demuxer may merge or split frames with lacing — compare at the unit level, not packet level.

### WI-4.216 — Container panel exports and Python bindings for container views
**Goal:** Expose `ContainerTree`, `SampleTable`, `FragmentTimeline`, `EditList`, `EncryptionInfo`, `HdrMetadata`, `TsTiming` in the Python module and as CSV exports.
**Depends on:** WI-1.235, WI-1.503, WI-4.540 (Python module skeleton).
**Produces:** the derived-view wrappers in `python/ovt/_container.py` (pybind11 wrappers returning FlatBuffer objects + `to_json()`/`to_pandas()`; file owned by WI-4.540); `--format csv` column sets for `ovt probe --samples`/`--fragments`/`--enc` (chapter 12 §12.2.2); tests `python/tests/test_container.py`.
**Interface:** `ovt.open(path).container.tree()`, `.tracks()`, `.sample_table(track)`, `.fragments(track)`, `.edit_list(track)`, `.encryption(track, first, count)`, `.hdr(track)`, `.bytes(offset, len)`; CSV columns = FlatBuffer field names in schema order.
**Steps:** 1. Bindings. 2. CSV writer. 3. Tests round-tripping JSON ↔ FlatBuffers.
**Acceptance tests:** 1. Given an MP4 `f.mp4`, When `python -c "import ovt; print(len(ovt.open('f.mp4').container.sample_table(0).dts))"` runs, Then it prints the sample count. 2. Given `f.mp4`, When `ovt probe f.mp4 --samples --format csv` runs, Then the header line is `index,dts,cts_offset,duration,offset,size,is_sync,sample_description_index,chunk_index,fragment_index`.
**Size:** S.
**Notes / pitfalls:** Never `eval` in Python wrappers (backbone §0.7.8); large tables go through `memoryview` not Python lists. **Overlap:** this WI extends `python/ovt/_container.py` of WI-4.540 with the derived views and the `csv` column sets of `ovt probe` (WI-1.503); no `python/ovt/container.py`, no `--csv` flag (`--format csv`). See chapter 16 §16.3.

---

### Glossary additions (chapter 8)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **Composition time (CTS) / decode time (DTS)** — presentation vs decoding timestamps in the track timescale; `ctts`/`trun` store `CTS − DTS`. `G:CTS` `G:DTS`
- **Composition-to-DTS shift** — the informative `cslg.compositionToDTSShift` hint (14496-12 §8.6.1.4): a constant such that CTS + shift ≥ DTS for every sample; stored in `SampleTable.composition_to_dts_shift`, never added to presentation times. `G:CompositionToDTSShift`
- **Movie fragment (`moof`)** — a self-contained set of sample tables (`traf`/`tfhd`/`tfdt`/`trun`) for samples stored in the following `mdat`. `G:MovieFragment`
- **CMAF structural brands (`cmfc`, `cmf2`, `cmfs`, `cmff`, `cmfl`)** — brands asserting CMAF track, 2nd-edition track, segment, fragment and chunk constraints (23000-19). `G:CMAFBrands`
- **CENC** — ISO/IEC 23001-7 Common Encryption: `schm`/`tenc`/`senc`/`saiz`/`saio`/`pssh`, schemes `cenc`, `cbc1`, `cens`, `cbcs`. `G:CENC`
- **KID / IV / subsample encryption** — key identifier (16 bytes), initialization vector (per-sample 8 or 16 bytes for `cenc`/`cens`/`cbc1`, constant 16 bytes for `cbcs`), and the clear/protected byte-range pairs within a sample. `G:KID` `G:IV` `G:Subsample`
- **HEIF item (`iinf`/`iloc`/`ipco`/`ipma`/`iref`)** — an image or metadata object addressed by item ID, with byte extents, properties and references instead of samples. `G:HEIFItem`
- **FileFormatConformance (MPEG)** — the MPEG GitHub repository holding machine-readable box definitions and the conformance corpus (ISO/IEC 14496-32). `G:FileFormatConformance`
- **Start code / zero_byte / trailing_zero_8bits** — Annex B framing: `00 00 01` start code prefix, optional preceding `zero_byte`, and zero padding after a NAL. `G:StartCode`
- **OBU low-overhead format (Section 5) vs Annex B (length-delimited)** — AV1 framing with per-OBU `obu_size` fields vs nested `temporal_unit_size`/`frame_unit_size`/`obu_length` lengths. `G:OBUFraming`
- **Temporal unit (TU) / frame unit** — AV1 grouping of OBUs for one presentation time; Annex B further groups OBUs per frame. `G:TemporalUnit`
- **Page cache (byte service)** — the 64 × 64 KiB LRU through which all file bytes reach front-ends. `G:PageCache`
