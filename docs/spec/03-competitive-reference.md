# 3. Competitive reference

> **Purpose of this chapter.** It records what the commercial analyzers do, so that every OVT requirement in Chapter 4 can be traced to a feature users already expect, and so that implementers understand *why* a requirement exists. It condenses the commercial-tool survey of Appendix A (compiled 2026-11-03); Appendix A and the URL list of Appendix F.1 are the source for every claim below. Nothing in this chapter is a requirement by itself; requirements live in Chapter 4 and cite this chapter as rationale. The UX conventions in §3.4 are the exception: they are written as normative statements and are referenced by `FR-GUI-*`.

---

## 3.1 Primary benchmarks

OVT's one-sentence goal (chapter 0 §0.0) names two products. These two are the **primary benchmarks**: when Chapter 4 says "parity", it means parity with the capabilities listed here for H.264/AVC, H.265/HEVC and AV1.

### 3.1.1 ViCueSoft VQ Analyzer (VQA)

Descendant of Intel Video Pro Analyzer (Intel transferred development and sales to ViCue Soft; <https://www.intel.com/content/www/us/en/developer/articles/tool/video-pro-analyzer.html>). Product page <https://vicuesoft.com/vq-analyzer>, user guide <https://vicuesoft.com/analyzer/userguide>, release notes <https://vicuesoft.com/analyzer/releasenotes>.

| Aspect | Condensed capability |
|---|---|
| Codecs | VVC, AV1, AVM/AV2, AVS3, HEVC v1 + RExt/SCC/SHVC/MV-HEVC, AVC High 8/10-bit, VP9, MPEG-2, JPEG XS, VC-3, APV. Widest in the industry. |
| Containers | MKV/WebM, MP4/MOV, MMT, TS/PS, AVI, ASF, FLV, IVF, PCAP (AVC), HEIF/HEIC/AVIF. |
| Platforms | Windows, macOS 10.12+, Ubuntu/SLES/CentOS. No Android. GUI and console are the same binary. |
| UI skeleton | Main Panel (zoom/pan canvas with overlay Mode), Stream View (Thumbnails, Bars/Buffer, HRD, Hierarchy, Metric, DPB refs), Syntax Info (tabbed per-codec tree with filter, hex toggle, spec tooltips), Stats tab (pie/bar, raw / area-normalised / bits-normalised), Selection Info (per-block read-out), Unit Info / HEX View (≤ 500 bytes inline, full HEX view on demand, bit range highlighted, byte search), Status/Errors (red tab, double-click to jump), Details popup (right-click a block). |
| Modes (F1–F12) | Coding Flow, Prediction (+Detail: reference arrays, MPM list, candidate lists, interpolation formulas, 4×4 motion buffer), Transform/Residuals (+Detail: quantised → dequantised → residual, QP derivation), Reconstruction, Loop Filter (edge colour by result, Bs, "why not filtered"), SAO, ALF, YUV, Info Overlays (bits heat map, QP map, PU type, ref index, per-block PSNR/SSIM, efficiency map, inter-memory-reads map, MV field), Statistics map, HDR/SEI tab. |
| Compare / debug | Dual View ("open dependent bitstream") with Delta; debug YUV load + Find First Difference; extraction of NAL units, minimum CVS, key frames, CTB syntax, YUV planes to CSV, MVs to CSV; profile/level/tile conformance checks. |
| CLI | `-hevc -av1 -avc …`, `-n`, `-dependent`, `-debug_yuv`, `-dump`, `-y4m`, `-md5`, `-display_order`, `-nocrop`, `-errors`, `-stats`, `-stream_stats`, `-psnr`; exports YUV/Y4M, per-frame MD5, error and statistics text, HRD CSV. |
| Price | Floating subscription $5,159–$13,199 / year; perpetual $18,599–$28,799; academic $1,199 (<https://shop.xlsoft.com/en/vicue-price/>). |

Companion products: **VQ Probe** (free metrics tool: PSNR/APSNR/XPSNR/PSNR-HVS/SSIM/MS-SSIM/VMAF/VMAF-NEG/CAMBI/CIEDE2000, RD curves, BD-rate, convex hull, encoding-ladder table, ROIs, SI/TI; <https://vicuesoft.com/vq-probe/>) and **VQ DVK** (decoder validation stream sets; <https://vicuesoft.com/blog/titles/dvk_advantages/>). OVT must at least ingest DVK-style stress streams without crashing (`NFR-00x` robustness).

### 3.1.2 Elecard StreamEye Studio (SE Studio)

Suite of four GUI apps and four consoles sharing a cross-process **Binding mode** (cursor/frame sync and shared comments). Studio page <https://www.elecard.com/products/video-analysis/streameye-studio>; StreamEye <https://www.elecard.com/products/video-analysis/streameye>; Stream Analyzer <https://elecard.com/products/video-analysis/stream-analyzer>; Video Quality Estimator <https://www.elecard.com/products/video-analysis/video-quality-estimator>; YUV Viewer <https://www.elecard.com/en/products/video-analysis/yuv-viewer>; release notes <https://www.elecard.com/docs/en/ReleaseNotes/index.html>.

| App | Condensed capability |
|---|---|
| **StreamEye** (frame/block analyzer) | Codecs MPEG-1/2, AVC, HEVC, MV-HEVC, VP9, AV1 (libaom 3.7), VVC, AVS3, APV. Containers TS/PS, MP4/MOV, MKV/WebM, AVI, MXF, FLV, IVF, HEIC, **DASH MPD and HLS playlist ingestion**. Views: **BarChart** (per-frame bars, colour by type, selectable element bits/QP/PSNR/temporal id; the primary navigation), Area Chart (bit distribution per coded element over time), Thumbnails, Info panel (Stream/Picture, Block info, Headers, DPB info, Block Presenter with decoded/predicted/unfiltered/residual/coefficients, Statistics), Stream Viewer (header text), HEX Viewer, Pixels Viewer, Message Viewer (conformance), Buffer tool (HRD/VBV fullness, cursor-synced), Picture difference (Compare, PSNR, PSNR Clip, Temperature, Subtraction, H/V split), Overlay Config (slices, tiles, partitions, MVs, SAO/ALF), BT.601/709/2020 switch, HDR format identification (HDR10, HDR Vivid), I/P/B/Key stepping, search by decode/display order, time or byte offset, CSV export, YUV save. Qt 6 since 2026. |
| **Stream Analyzer** (container/TS) | Structure trees for TS/PS, MP4/MOV, MKV/WebM, AVI, FLV, IVF, HEIC, DASH/HLS; video and audio syntax; ETSI TR 101 290 P1–P3; PSI/SI, SCTE-35, DVB subtitles; PCR/PTS/DTS graphs; bitrate graph with 10/100/250/500 ms windows; HDR10/HDR Vivid metadata checks; CSV export; save data portion. |
| **Video Quality Estimator** ("VideoQuest") | PSNR, APSNR, SSIM, DELTA, MSE, MSAD, VQM, NQI, VIF, VMAF, VMAF-phone; up to 5 files at once; ROI; automatic start-frame matching; three-panel UI (stream list, graphs, display side-by-side or layered with synchronised pan/zoom); Diff modes; CSV/JSON export. Reviewer pain points: no raw-source preview, same-resolution constraint, tiny graphs, locale-dependent CSV (<https://www.streamingmedia.com/Articles/Editorial/Featured-Articles/Elecard-Video-Quality-Estimator-Review-129136.aspx>). |
| **YUV Viewer** | Raw display/compare with PSNR/SSIM/VMAF and others, BT.601/709/2020, endian and upsampler selection, console for batch. |
| Platforms / price | Windows, macOS 10.13–26, Ubuntu 18.04–26.04; 16 GB RAM. No Android. Quote-only; VideoQuest historically $850 GUI + $400 console. |

### 3.1.3 Why these two

VQA defines the **per-decoding-stage depth** (every stage is a Mode with a Details popup) and the Selection → Syntax → Hex linkage that codec engineers expect. SE Studio defines the **navigation and container breadth** (bar-chart timeline, buffer analysis, container trees, DASH/HLS, HDR metadata identification) and the **cross-app synchronisation** that video platform teams expect. OVT reproduces both idioms in one application driven by one engine (ADR-003).

---

## 3.2 Secondary tools

Each entry is one line of capability and one line of what OVT takes from it.

| Tool | Capability summary (URL) | What OVT adopts |
|---|---|---|
| **Intel Video Pro Analyzer** (discontinued) | HEVC/VP9/AVC/MPEG-2; identical UI lineage to VQA; VP9 efficiency map; 10-frame evaluation limit. <https://www.intel.cn/content/dam/develop/external/us/en/documents/intelvideoproanalyzeruserguide-560483.pdf> | The F-key Mode model and letter hotkeys (§3.4). |
| **Solveig Zond 265** | HEVC/AVC/MPEG-2/AV1/EVC/VVC; frame-size bars with reference marking; translucent CU/PU/TU/QP/Bits/RefIdx/PSNR overlays; HRD from SEI; per-frame HDR/SEI parse; encoding-toolset summary; no-reference PSNR; CLI CSV & JSON with CU data and stage YUV dumps; free tier, $174–$1,794 / year. <https://www.solveigmm.com/en/products/zond/> | JSON + CSV CLI reports with per-CU data; encoding-toolset summary per stream; AV1 qindex per block. |
| **Parabola Explorer** (HEVC only, Windows ≤ 8.1) | Syntax tree down to CABAC bins with fractional-bit sizes; conformance messages with spec paragraph numbers; interactive DPB/RPL view; multi-process decoding with image caching. <https://parabolaresearch.com/explorer-hevc-analyzer.html> | Spec paragraph references on every node (`SyntaxNode.spec_ref`) and on every diagnostic; frame cache for fast cursor stepping. |
| **Codecian CodecVisa** (dormant since 2017) | HEVC/AVC/VP9/MPEG-2; whole-picture "Decode Stage Display" (final, pre-SAO, pre-deblock, predicted, residual); Pixel Dialog with per-stage values in dec/hex. <https://codecian.com/codecvisa.html> | Per-stage plane views as first-class `PlaneStage` values. |
| **Interra VEGA Media Analyzer** | QC-grade conformance; ABR DASH/HLS/CMAF checks; Trace Viewer linking syntax to hex; T-STD buffer model; XML/PDF reports; Linux/Docker CLI. <https://interrasystems.com/press-kit/IBC2023/Product-Datasheets/VEGA/Vega_Datasheet.pdf> | Machine-readable reports with stable error codes; headless CLI suited to containers. |
| **Tektronix MTS4EA / Sencore CMA 1820** | Broadcast test-equipment lineage; traces at every level; bitstream editor; Excel export; SCTE-35, A/V sync. <https://manualmachine.com/tektronix/mts4ea/10503446-datasheet> | Nothing in phases 0–4; bitstream editing is "future". |
| **Telestream Switch / Aurora** | QC playback with GOP/datarate timeline; captions; loudness; Aurora template-based automated QC with TekMOS no-reference score. <https://redsharknews.com/telestream-switch-5-pro-review-comprehensive-qc-software> | Timeline that shows GOP structure and data rate together. |
| **VideoQ VQV** | Waveform, vectorscope, gamut check, light-level (nits) statistics and heat map, PQ/HLG/SDR transfer previews, BT.601/709/2020, narrow/full range. <https://videoq.com/Downloads/VideoQ_VQV_Viewer-Analyzer_Datasheet.pdf> | HDR scopes and nits false colour (phase 4), range/matrix overrides (phase 1). |
| **vooya** | Raw YUV viewer, many packings, two-file diff with PSNR/MSE/SSIM and first differing pixel, stdin pipe input, CLI `--metrics stdout`. <https://www.offminor.de/vooya-command-line.html> | Explicit raw-format dialog with live preview; pipe-friendly CLI metrics. |
| **MediaInfo** (GPL/BSD, the only Android entry) | Metadata CLI/GUI/library, outputs Text/XML/JSON/EBUCore/PBCore, custom templates. <https://www.mankier.com/1/mediainfo-gui> | JSON probe output shape; template-free but schema-stable `ovt probe`. |

---

## 3.3 Consolidated feature matrix with OVT phase mapping

Legend: ● full, ◐ partial or edition-gated, ○ no. Columns: VQA = VQ Analyzer; SE = StreamEye Studio (all apps); Z = Zond 265; PE = Parabola Explorer; VEGA = Interra VEGA; Other = best of the remaining tools (named). **OVT phase** uses chapter 0 §0.8: 0 Foundations, 1 See the stream, 2 Look inside blocks, 3 Measure quality, 4 Deepen & extend, A Android/ChromeOS track, or **future** (not in any planned phase; the data model must not preclude it).

| # | Feature | VQA | SE | Z | PE | VEGA | Other | **OVT phase** |
|---|---|---|---|---|---|---|---|---|
| 1 | H.264/AVC and H.265/HEVC Main/Main10 (syntax + blocks) | ● | ● | ● | HEVC | ● | CodecVisa ● | **1** (syntax), **2** (blocks) |
| 2 | HEVC RExt / SCC | ● | ◐ | ○ | ○ | ● | ○ | **4** (HM oracle plugin) |
| 3 | AV1 | ● | ● | ● | ○ | ● | ○ | **1** (syntax), **2** (blocks) |
| 4 | AVM/AV2, VVC, VP9, MPEG-2, AVS3, APV, JPEG XS, EVC, LCEVC | ●/◐ | ◐ | ◐ | ○ | ◐ | ○ | **future** (plugin ABI must allow) |
| 5 | MP4/ISOBMFF/CMAF box tree with byte offsets | ● | ● | ● | ○ | ● | MediaInfo meta | **1** |
| 6 | IVF, Annex B, raw OBU, Y4M/raw YUV | ● | ● | ● | ES | ● | vooya raw | **1** |
| 7 | Matroska/WebM, MPEG-TS trees | ● | ● | ● | ○ | ● | ○ | **4** |
| 8 | DASH MPD / HLS playlist ingestion | ○ | ● | ○ | ○ | ● | ○ | **4** (ingestion/parse, FR-CNT-01x); manifest-level diagnostics **deferred** (chapter 17 §17.4 OI-16) |
| 9 | PCAP/RTP ingestion; TR 101 290; PSI/SI; SCTE-35 | ◐ | ● | ○ | ○ | ● | MTS4EA ● | **future** |
| 10 | Syntax element tree with spec names and descriptors | ● | ● | ● | ● (bins) | ● | ○ | **1** |
| 11 | Hex/binary view linked to the selected syntax element (bit range) | ● | ● | ● | ● | ● | ○ | **1** |
| 12 | Spec paragraph references per element / diagnostic | ● | ○ | hints | ● | ○ | ○ | **1** (`spec_ref`), tooltips **2** |
| 13 | CABAC state columns, bin-level view | ● | ○ | ○ | ● | ○ | ○ | **future** |
| 14 | Partition overlay (MB/CTU/CU/PU/TU/SB/TXB, slices, tiles) with click-to-select | ● | ● | ● | ● | ● | CodecVisa ● | **2** |
| 15 | Prediction overlay (intra directions, inter L0/L1, skip/merge) | ● | ● | ◐ | ● | ● | ● | **2** |
| 16 | Motion vectors incl. sub-block (OBMC/warped for AV1) | ● | ● | ○ | ● | ● | ● | **2** |
| 17 | 4×4 motion buffer view and MV CSV export | ● | ○ | ○ | ○ | ○ | ○ | **2** (`CellGrid.mv0/mv1`) |
| 18 | QP / qindex map | ● | ● | ● | ● | ● | ● | **2** |
| 19 | Bits heat map (estimated per block) | ● | ● | ● | ● | ● | ● | **2** |
| 20 | Reference-index map | ● | ● | ● | ○ | ● | ○ | **2** |
| 21 | Per-stage pictures (prediction, residual, pre-deblock, pre-SAO/CDEF, final) | ● | ● | CLI | ● | ● | CodecVisa ● | **2** (FR-BLK-014, FR-PLN-005); an AV1 film-grain-synthesis on/off toggle and `FilmGrainParams` view are **deferred** (chapter 17 §17.4 OI-16; analysis planes are grain-free, playback applies grain) |
| 22 | Residual / coefficient three-stage view (quantised → dequantised → residual) | ● | ● | dump | ● | ● | ● | **2** (FR-BLK-023; the libde265 patch 0003 that exports coefficients is a phase-2 WI) |
| 23 | Deblocking / SAO / CDEF / ALF parameters per block or edge | ● | ◐ | ○ | SAO | ● | ○ | **2** (params in `CellGrid`), edge detail **4** |
| 24 | Y / U / V / grey plane select; pixel value read-out | ● | ● | ● | ○ | ● | vooya ● | **1** |
| 25 | Frame-size bar chart as primary navigation, colour by type, QP plot | ● | ● | ● | ● | ● | Switch ● | **1** |
| 26 | Filmstrip / thumbnails with decode & display order, POC, ref arrows | ● | ● | ● | ● | ● | ○ | **2** |
| 27 | GOP / temporal-layer hierarchy and DPB contents | ● | ● | ◐ | ● | ● | ○ | **2** |
| 28 | HRD / VBV / CPB buffer curve | ● | ● | ● | ○ | ● T-STD | MTS4EA ● | **4** |
| 29 | Inter memory-bandwidth / efficiency map | ● | ○ | ○ | ○ | ○ | VPA VP9 | **future** |
| 30 | Statistics (pie/bar; raw, area-normalised, bits-normalised); encoding-toolset summary | ● | ● | ● | ● Pro | ● | ○ | **2** (FR-BLK-020, FR-BLK-021); the StreamEye *area chart* (bits per coded element over time) is **deferred** (chapter 17 §17.4 OI-16) |
| 31 | Conformance: syntax errors with jump-to-location | ● | ● | ● | ● | ● | ● | **1** |
| 32 | Conformance: decoded picture hash SEI, profile/level/tier limits | ● | ◐ | ● | ● | ● | ○ | **4** |
| 33 | HDR metadata display (VUI/`colr`, MDCV, CLL, HDR10+, DoVi RPU presence) | ● | ● | ● | ● | ● | MediaInfo ● | **1** (presence/values, FR-HDR-002); applying HDR10+ dynamic metadata and DoVi RPUs to the output **4** (FR-HDR-012/013); MaxCLL/MaxFALL measurement **3** (FR-HDR-006); HDR Vivid (CUVA) identification **deferred** (OI-16) |
| 34 | Correct PQ/HLG rendering on an HDR display; tone-mapped SDR fallback | ◐ | ○ | ○ | ○ | ○ | VQV ● | **1** |
| 35 | HDR scopes: nits false colour, luminance waveform, vectorscope, gamut clip | ○ | ○ | ○ | ○ | ○ | VQV ● | **4** |
| 36 | Full-reference metrics PSNR, SSIM, MS-SSIM, VMAF (+NEG, phone) | Probe | ● | ● | ○ | ● | ○ | **3** |
| 37 | Extended metrics PSNR-HVS, CIEDE2000, CAMBI, XPSNR, VIF | Probe | ◐ | ○ | ○ | ○ | ○ | **3** (via libvmaf host) |
| 38 | No-reference metric (EPSNR, NR-PSNR, TekMOS) | ○ | EPSNR | NR-PSNR | ○ | ○ | Aurora MOS | **3** (UVQ) |
| 39 | Per-frame metric timeline with click-to-seek; cumulative average | ● | ● | ● | ○ | ● | ● | **3** |
| 40 | Per-block PSNR/SSIM heat map overlay | ● | ◐ | ● | ○ | ○ | ● | **3** |
| 41 | A/B, side-by-side, split, difference (Subtraction, Temperature) views | ● | ● | ○ | diff | ● | vooya ● | **3** |
| 42 | Debug YUV load, first-mismatch finder | ● | ● | ○ | ◐ | ● | vooya ● | **3** |
| 43 | Reference YUV format dialog with preview; alignment and scaling | ◐ | ◐ | ○ | ○ | ● | ○ | **3** |
| 44 | ROI-restricted metrics | Probe | ● | ○ | ○ | ○ | ○ | **4** — open, **deferred** (FR-VQ-013, chapter 17 §17.4 OI-07) |
| 45 | RD curves, BD-rate, convex hull, ladder table | Probe | ○ | ○ | ○ | ○ | ○ | **3** (BD-rate, FR-VQ-017), convex hull and per-pair ladder table **4** (FR-VQ-101); a multi-rendition *ladder/batch VQ report* is **deferred** (chapter 17 §17.4 OI-16) |
| 46 | SI/TI, scene-change detection | Probe | ○ | ○ | ○ | ○ | ○ | **4** (FR-VQ-020, FR-VQ-021) |
| 47 | Multi-file (≥ 5) simultaneous metric runs | ○ | ● | ○ | ○ | ○ | ○ | **3** |
| 48 | GPU-accelerated metrics | Probe | ○ | ○ | ○ | ○ | ○ | **future** |
| 49 | Dual-stream synchronised view / cross-process binding; shared comments | ● | ● | ○ | ○ | ● | ● | **2** (in-process dual view, FR-GUI-021, WI-2.489); cross-process **future** |
| 50 | NAL/OBU and sub-stream extraction; save selected portion | ● | ● | ○ | ○ | ○ | MTS4EA | **1** (FR-SYN-018, `ovt extract` WI-1.508) |
| 51 | Bitstream editor | ○ | ○ | ○ | ○ | ○ | MTS4EA ● | **future** |
| 52 | Export CSV / JSON / XML of analysis and metrics | CSV/txt | CSV/JSON | CSV/JSON | print | XML/PDF | Excel | **1** (JSON), **2** (YUView CSV), **3** (metrics CSV/JSON); a cross-version JSON compatibility *policy* is **deferred** (chapter 17 §17.4 OI-16; `CliEnvelope.ovt_version`/`$schema` are present from phase 1) |
| 53 | CLI / batch with the same engine as the GUI | ● | ● | ● | ○ | ● | ● | **1** |
| 54 | Documented scripting API (Python) | ○ | ○ | ○ | ○ | ○ | ○ | **4** (Python package); C API from **1** |
| 55 | macOS, Linux | ●● | ●● | ●● | ○ | Linux CLI | ●● | **1** |
| 56 | Android / ChromeOS | ○ | ○ | ○ | ○ | ○ | MediaInfo only | **A** |
| 57 | Free of charge / open source | trial | demo | free tier | trial | trial | MediaInfo | **0** |

---

## 3.4 Commitments: must / should / nice, restated for OVT

The persona analysis of the commercial-tool survey (Appendix A) lists prioritised requirements from two personas. They are restated here as OVT commitments with the phase in which OVT meets them; Chapter 4 turns each into one or more `FR-*` entries (the right-hand column names the areas).

### 3.4.1 Persona A — codec researcher / encoder developer

**Must-have (OVT commits to all twelve for AVC, HEVC Main/Main10 and AV1):**

| # | Commercial must-have | OVT commitment | Phase | FR areas | FR ids (chapter 4) |
|---|---|---|---|---|---|
| A1 | Syntax tree with hex/binary linkage, bit offsets, element search | `SyntaxTree` with `BitSpan` on every node; hex pane highlights the selected node; search by element name and value | 1 | SYN, GUI | FR-SYN-001…017, FR-GUI-013, FR-GUI-014, FR-CNT-002 |
| A2 | Partition overlay with click-to-select → Selection Info → syntax tab | `FrameAnalysis.blocks` tree rendered by `VideoSurface`; `pick()` → Selection Info → syntax node → hex | 2 | BLK, GUI | FR-BLK-003, FR-BLK-017, FR-BLK-019, FR-GUI-009, FR-GUI-010, FR-GUI-011 |
| A3 | Prediction overlay: intra directions, L0/L1 MVs colour-coded, skip/merge, sub-block vectors | `CellGrid.pred/intra_mode_*`, `MotionVector` list with `flags` for sub-block/global/bi-pred | 2 | BLK | FR-BLK-005, FR-BLK-007, FR-BLK-011, FR-BLK-017 |
| A4 | QP map and bits heat map | `CellGrid.qp`, `CellGrid.bits_x8` | 2 | BLK | FR-BLK-015, FR-BLK-016, FR-BLK-017, FR-GUI-008 |
| A5 | Residual/coefficient three-stage view and per-stage pictures | `PlaneStage` {Prediction, Residual, PreDeblock, PreSaoOrCdef, PreLoopRestoration, Reconstruction} and the `Coefficients` stage with the three-stage Details view (FR-BLK-023) in phase 2 | 2 | BLK, PLN | FR-BLK-014, FR-BLK-023, FR-PLN-005 |
| A6 | Reference structure: filmstrip with POC/decode order, ref arrows, DPB, B-pyramid | `FrameInfo.declared_refs`, `dpb_after`, `temporal_id`; GOP/reference graph panel | 2 | DEC, GUI | FR-DEC-011, FR-BLK-012, FR-GUI-017, FR-GUI-018 |
| A7 | Frame-size bar chart as primary navigation with type colours and QP plot | Timeline panel fed from `StreamIndex.frames[].size_bytes/type/qp_base` | 1 | GUI | FR-GUI-003, FR-GUI-005, FR-GUI-020 |
| A8 | Conformance error list with jump-to-location | `Diagnostic{code, span, spec_ref}` on every tree; double-click navigates | 1 (syntax), 4 (hash SEI, levels) | SYN, GUI | FR-GEN-006, FR-SYN-013, FR-SYN-019, FR-SYN-020, FR-GUI-012, FR-GUI-026 |
| A9 | Debug YUV load + first-mismatch finder + per-block PSNR overlay | Reference/debug YUV ingestion through the normalisation stage; first-mismatch job; per-block maps | 3 | VQ, PLN | FR-PLN-008, FR-PLN-009, FR-CLI-016, FR-VQ-008 |
| A10 | CLI mirroring GUI: dump YUV/Y4M, MD5, stats, errors | `ovt dump`, `ovt analyze`, `ovt syntax --diagnostics-only`, same core library | 1–2 | CLI | FR-CLI-001, FR-CLI-007, FR-CLI-101, FR-DEC-006…008 |
| A11 | Statistics pie/bar normalised by area/bits with export | Statistics panel over `FrameAnalysis`; CSV/JSON export | 2 | BLK, GUI | FR-BLK-020, FR-BLK-021, FR-GUI-031 |
| A12 | Codec coverage AVC, HEVC (RExt/SCC), VVC, AV1, VP9, MPEG-2 | AVC, HEVC Main/Main10, AV1 in phases 1–2; RExt/SCC via HM oracle plugin in phase 4; VVC/VP9/MPEG-2 **future** through the `Codec` enum and plugin ABI | 1–4 / future | GEN, DEC | FR-GEN-003, FR-GEN-012, FR-DEC-012, FR-API-009 |

**Should-have:** deblocking/SAO/CDEF parameters and edge detail (phase 2, FR-BLK-023); HRD/CPB buffer curve with cursor sync (phase 4, FR-DEC-013, FR-GUI-032, WI-4.112/WI-4.470); dual view with synchronised pan/zoom/frame and delta overlay (phase 2, FR-GUI-021, WI-2.489); NAL/OBU and minimum-CVS extraction (phase 1, FR-SYN-018, FR-CLI-013); spec references as tooltips (phase 2, FR-SYN-017); 4×4 motion-buffer view and MV CSV export (phase 2, FR-BLK-024); encoding-toolset summary (phase 2, FR-BLK-021); GOP/temporal-id hierarchy chart (phase 2, FR-GUI-018). **Deferred (phase 5+; owner may pull forward, chapter 17 §17.4 OI-16):** area chart of bits per coded element over time; AV1 film-grain on/off toggle and `FilmGrainParams` view. **Deferred to future:** CABAC state/bin view; memory-bandwidth map.

**Nice-to-have:** per-sample interpolation formula popups, bitstream editor, PCAP/RTP, TR 101 290, stress-set coverage reports — all **future**; container trees for TS/MKV are phase 4.

### 3.4.2 Persona B — video quality / video platform engineer

**Must-have (OVT commits to all eight):**

| # | Commercial must-have | OVT commitment | Phase | FR areas | FR ids (chapter 4) |
|---|---|---|---|---|---|
| B1 | PSNR (per plane + weighted), SSIM, MS-SSIM, VMAF (+NEG, phone) with per-frame timeline, cumulative average, click-to-seek | Own PSNR/SSIM kernels; libvmaf host for VMAF family and MS-SSIM; metrics timeline panel | 3 | VQ, GUI | FR-VQ-001…011, FR-VQ-016, FR-GUI-030 |
| B2 | Side-by-side / split / overlay / difference views with Temperature and Subtraction, pixel-level zoom read-outs | `VideoSurface::set_compare(CompareMode)` (`Single`, `SideBySide`, `Split`, `Overlay`, `Difference`, `Flicker`; chapter 10 §10.2.4); pixel values drawn at ≥ 24 px per sample | 2 (compare), 1 (pixel read-out) | PLN, GUI | FR-PLN-003, FR-PLN-004, FR-PLN-007, FR-GUI-021 |
| B3 | Reference YUV/Y4M ingestion with explicit format dialog and preview | Raw-format dialog (phase 1) with live preview (phase 3); Y4M header auto-fill | 1 / 3 | VQ, GUI | FR-CNT-014, FR-CNT-105, FR-PLN-008 |
| B4 | Automatic frame alignment and resolution-mismatch scaling | Normalisation stage: temporal alignment search with confidence, scaling with declared kernel, crop | 3 | VQ | FR-VQ-002, FR-VQ-003, FR-VQ-004 |
| B5 | ROI-restricted metrics | Rectangular ROI list in `NormalizationConfig` | 4 — **deferred**, open | VQ | FR-VQ-013 (phase 4, open — chapter 17 §17.4 OI-07) |
| B6 | CSV/JSON export with percentile summaries | `MetricsResult` pooled fields; libvmaf-compatible JSON; CSV with `.`-decimal always | 3 | VQ, CLI | FR-VQ-012, FR-VQ-014, FR-CLI-009 |
| B7 | CLI batch `--reference/--distorted` quick compare | `ovt metrics --reference R --distorted D --metric psnr,ssim,vmaf` | 3 | CLI | FR-CLI-009, FR-VQ-007 |
| B8 | HDR metadata display and correct PQ/HLG tone-mapped preview with BT.601/709/2020 and full/narrow range | HDR metadata panel (phase 1); libplacebo pipeline with EDR output on macOS and `SurfaceView` dataspace on Android; matrix/range override | 1 (MaxCLL/MaxFALL *measurement* 3) | HDR, GUI | FR-HDR-001…005, FR-HDR-007, FR-HDR-011, FR-GUI-016, FR-HDR-101; FR-HDR-006 (phase 3) |

**Should-have:** RD curves and BD-rate (phase 3, FR-VQ-017), convex hull and ladder table (phase 4, FR-VQ-101); multi-file (≥ 5) runs (phase 3, FR-VQ-018); no-reference indicators (UVQ in phase 3, FR-VQ-009/010); CAMBI, CIEDE2000, PSNR-HVS, VIF via libvmaf (phase 3, FR-VQ-008) and XPSNR (phase 4, FR-VQ-020); scopes (phase 4, FR-HDR-010); SI/TI and scene-change detection (phase 4, FR-VQ-020/021); micro-burst bitrate graph (phase 4, with MPEG-TS, FR-CNT-016); GPU-accelerated metrics **future**. **Deferred (phase 5+; owner may pull forward, chapter 17 §17.4 OI-16):** subjective-ratings workflow (OI-11); multi-rendition ladder/batch VQ report; DASH/HLS manifest-level diagnostics; JSON output compatibility policy; HDR Vivid (CUVA) identification; device-compatibility check ("does this play on this device?").

**Nice-to-have:** object detection → ROI, loudness/caption inspection, delivery-spec templates, cross-instance comment sharing — all **future**.

### 3.4.3 Owner capabilities cross-check

The owner's five mandatory capabilities (chapter 0 §0.0) map onto the tables above: (1) raw encoded bitstream inspection = A1, A8, A10; (2) VQ metrics between a video and its reference = B1, B4–B7; (3) visual inspection of macroblocks/CUs/superblocks and motion vectors = A2–A4; (4) inspection of separate Y/U/V planes = matrix row 24 and B2; (5) container inspection with MP4 required = matrix row 5. The starter metrics PSNR, SSIM, VMAF, UVQ = B1 plus matrix row 38.

---

## 3.5 UX conventions OVT adopts (normative)

The following statements are **normative** for `ovt-studio` and the Android shell. They are restated and tested as `FR-GUI-*` requirements in Chapter 4; this section records their provenance. "Primary tools" means VQA/VPA and SE Studio unless another tool is named.

**UX-1 Three-pane skeleton.** `ovt-studio` MUST present, in its default layout, three regions: a **timeline** region (frame bar chart and filmstrip) along the bottom edge, a **picture** region (the `VideoSurface` canvas with overlays) in the centre, and a **side** region of tabbed panels (Container, Syntax + Hex, Selection Info, Statistics, Metrics, HDR, Diagnostics) on the right edge. Every panel MUST be detachable and re-dockable. *Source:* VQA Main Panel + Stream View + Syntax Info; SE BarChart + picture + Info panel; also PE, CodecVisa, VEGA.

**UX-2 F-key overlay modes and letter toggles.** Overlay modes MUST be bound to F1–F12 and letter toggles to single keys, all remappable, exactly as the canonical keyboard map of chapter 11 §11.4.3 (D8) defines them; chapter 4 FR-GUI-008 requires it and this chapter does not restate the table. In outline: F1 Coding flow, F2 Prediction, F3 Motion vectors, F4 Transform/residual, F5 QP map, F6 Bits heatmap, F7 Reference indices, F8 Loop filter, F9 In-loop filter parameters, F10 Metrics heatmap, F11 Plain picture, F12 HDR false colour; letters `P I G M T L H D K E S`, `A` analyse, `B` bookmark, planes `1`–`5`, `Space` play/pause, `←`/`→` frame step, `Shift+←`/`→` ±10 frames, `Ctrl+←`/`→` previous/next random access point, `Tab` block level, `Ctrl+↓`/`↑` next/previous error, `C`/`Shift+C` compare mode, `X` swap A/B, `Ctrl+B` hex↔binary, `Ctrl+D` dual view, `?` shows the map. The adopted convention is the *shape* of the VQA/VPA hotkey scheme (one F-key per analysis mode, single-letter toggles), not its exact assignments. *Source:* VQA/VPA Mode list and hotkeys.

**UX-3 Right-click = Details.** A right-click (secondary mouse button, or long-press on touch) on a block in any overlay mode MUST open the stage-specific Details view for that block (phase 2: syntax of the block; phase 4: coefficient and filter detail). *Source:* VQA Details popup.

**UX-4 Click-cycling through the block hierarchy.** Repeated left-clicks at the same canvas position MUST cycle the selection through the block hierarchy from the largest to the smallest enclosing block — H.264: MB → sub-macroblock partition; HEVC: CTU → CU → PU → TU; AV1: SB → block → TXB — and then wrap to the largest. The current level MUST be shown in the Selection Info header. *Source:* VQA CU→PU→TU cycling.

**UX-5 Selection → syntax → hex linkage.** Selecting a block MUST (a) populate Selection Info from its `Block` and codec extension table, (b) select the corresponding node in the Syntax panel (slice/tile-group header for the block's partition, and the block's own syntax when the plugin provides it), and (c) highlight the node's `BitSpan` in the Hex panel, mapping RBSP bit offsets to raw file bytes through `Unit.epb_positions`. Conversely, double-clicking a VCL unit or a diagnostic row MUST navigate to the owning frame and, if the diagnostic has a block position, select that block. *Source:* VQA/VPA, VEGA Trace Viewer, PE.

**UX-6 Frame-type colour language.** Wherever frames are drawn by type, OVT MUST use **red** for I/Key/IntraOnly, **blue** for P/Inter with single-direction references, **green** for B (bi-predicted) frames, with the exact tokens defined in Chapter 11 (`frame.i`, `frame.p`, `frame.b`); long-term references and AV1 ALTREF/GOLDEN arrows use a distinct hue defined there; a colour-blind-safe alternative palette (shapes plus viridis-derived hues) MUST be selectable. *Source:* VQA/VPA filmstrip, Zond bars.

**UX-7 Cursor synchronisation everywhere.** The current frame MUST be a single application-wide state: changing it from the timeline, filmstrip, syntax tree, diagnostics list, metrics plot, buffer graph or keyboard MUST update every other panel within one render frame (≤ 16 ms after the model update). In dual view (chapter 11 §11.10; FR-GUI-021, realised by WI-2.489), pan, zoom, plane mode and frame MUST be synchronised between the two surfaces unless the user unlocks them. *Source:* SE 2026 cursor sync and Binding mode; VQ Probe click-to-seek.

**UX-8 Decode/display order toggle.** Every frame-ordered view (timeline, filmstrip, metrics plot, frame stepping, search) MUST support both decode order and display order, switched globally by the `O` key or a toolbar toggle, and MUST label frames with both `FrameId.decode_index` and `FrameId.display_index` in tooltips. Frame search MUST accept decode index, display index, POC/order hint, time in seconds and file byte offset. *Source:* VQA, CodecVisa, SE search modes.

**UX-9 Spec terminology in all labels.** Every label that names a syntax element, a block type, a mode or a parameter MUST use the exact term and casing from the governing specification (H.264 §7.3, H.265 §7.3, AV1 §5) — for example `cu_skip_flag`, `PART_2NxN`, `PARTITION_HORZ_A`, `mb_type`, `sao_type_idx_luma` — and MUST NOT invent synonyms; a glossary tooltip MAY add a plain-language explanation. *Source:* PE, VQA.

**UX-10 Legends and derived values rather than bare numbers.** Every overlay mode MUST have a legend (`L`) and every derived quantity shown in Selection Info (e.g. `QpY`, `CtbSizeY`, `MiSize`, final MV in luma samples) MUST be shown next to the signalled syntax element it derives from, using `SyntaxNode.derived`. *Source:* VQA Loop Filter legend, Transform Detail.

**UX-11 One engine for GUI and CLI.** Everything shown in a panel MUST be obtainable from the `ovt` CLI as JSON or FlatBuffers from the same `libovt-core` build, and the GUI MUST NOT contain analysis logic that is not in `libovt-core` (ADR-003). *Source:* VQA console mode, Elecard consoles, Zond Business.

**UX-12 Wheel zoom about the cursor, drag pan, fit.** Mouse wheel (or pinch) zooms around the cursor position; primary-button drag pans; `R` or a double-click with any mouse button fits the picture to the viewport; zoom range 1/32× to 64× (FR-GUI-007); at ≥ 24 screen pixels per sample the picture region MUST draw sample values as text. *Source:* VQA/VPA, vooya magnifier, Appendix B.4.

---

## 3.6 Gaps OVT exploits

1. **Android and ChromeOS.** No commercial analyzer runs on Android; MediaInfo is the only Android entry in the matrix, and it shows metadata only. OVT's Android track (phase A) delivers the full three-pane analyzer with GPU-rendered overlays and HDR output through `SurfaceView` (ADR-014) on Pixel-class tablets and Chromebooks, with desktop-windowing behaviours (resizable windows, mouse hover, right-click, keyboard shortcuts, external displays). This is unique in the category.
2. **Price.** VQA costs $5,159–$13,199 per seat per year; SE Studio is quote-only (historically $850+ per app); VEGA is enterprise-priced; Zond is the only sub-$200 entry and lacks per-stage detail. OVT is free and GPL/LGPL (ADR-002), with no per-codec edition gating: AV1 ships in every build and AVC/HEVC decoders are runtime plugins (ADR-015) rather than paid tiers.
3. **Scripting API.** None of the surveyed tools exposes a documented scripting API beyond CLI flags. OVT exposes a stable C API (`core/include/ovt/ovt.h`, phase 1), FlatBuffers/JSON output from every CLI subcommand (phase 1), and a Python package `ovt` (phase 4) that returns the same `StreamIndex`, `FrameAnalysis`, `ContainerTree` and `MetricsResult` objects the GUI uses.
4. **Dormant competitors.** CodecVisa (last release 2017), Parabola Explorer (Windows ≤ 8.1) and Intel VPA (discontinued) leave HEVC-era users without updates; OVT's HEVC path (libde265 plugin + HM oracle) covers their users.
5. **Reviewer pain points avoided by design.** Raw-source format guessing without preview (OVT: format dialog with live preview, `FR-VQ`), same-resolution constraint (OVT: normalisation stage scales with a declared kernel), non-resizable charts (OVT: dockable, resizable panels), locale-dependent CSV (OVT: `.` decimal separator and `,` field separator always, `FR-CLI`).
6. **Correct HDR, verified.** Commercial analyzers identify HDR metadata but render PQ/HLG inconsistently (Intel community bug report; SE and Zond identify only). OVT treats correct PQ/HLG display as a phase 1 exit criterion (chapter 0 §0.8) with measured EDR output on macOS and `BT2020_PQ`/`BT2020_HLG` dataspaces on Android.

---

### Glossary additions (chapter 3)

- **Binding mode** — Elecard's cross-process synchronisation of the current frame and cursor between StreamEye Studio applications; OVT's equivalent is in-process cursor synchronisation (UX-7).
- **Details view** — a per-block drill-down opened by right-click that shows the stage-specific data (syntax, coefficients, filter inputs) for the selected block.
- **Dual view** — two `VideoSurface` instances showing two streams (or two stages of one stream) with synchronised frame, pan and zoom.
- **Filmstrip** — a horizontal strip of frame thumbnails ordered by decode or display order, annotated with frame type, POC/order hint and reference arrows.
- **Mode (overlay mode)** — the F-key-selected set of overlays and plane stage drawn on the picture region (UX-2).
- **Primary benchmark** — one of the two commercial products (VQ Analyzer, StreamEye Studio) whose feature set defines OVT parity.
- **Selection Info** — the side panel that lists the position, size, kind and all signalled syntax values of the currently selected block.
- **Timeline** — the bottom region of `ovt-studio` holding the frame-size bar chart, QP plot and filmstrip; the primary navigation control.
- **Click-cycling** — repeated left-clicks at the same canvas position selecting successively smaller enclosing blocks (UX-4).
