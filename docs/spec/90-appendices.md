**Appendices A–F.**

> Appendix sections are numbered `A.n`…`F.n`. `B.n` here means Appendix B (FOSS library survey); the architecture backbone is chapter 0 (`§0.n`).

> **Status:** informative reference material for the Open Video Tools (OVT) Requirements & Architecture Specification. These appendices condense the seven research reports (dated 2026-11-03) into self-contained fact tables. Nothing here reopens a decision in the Backbone (B.2, ADR-001 … ADR-016); where a fact bears on a decision, the ADR is cited. Facts about third-party software cite a URL (B.1 rule 7); facts about codecs cite spec sections. Items the research could not confirm against a primary source are marked **(verify)**.
>
> Contents: **A** Commercial feature inventory · **B** FOSS library and tool survey · **C** Codec syntax cross-reference · **D** Decoder internals extraction facts · **E** Licensing and patent facts · **F** URL index.

---

# Appendix A — Commercial feature inventory

This appendix records, as factual feature comparison, which feature areas commercial video analyzers offer according to their public product pages, release notes and published reviews. Chapter 3 (Competitive reference) derives must/should/nice priorities from it. Two products are commonly treated as the "full bitstream analyzer" category: **ViCueSoft VQ Analyzer** and **Elecard StreamEye Studio**. Of the tools surveyed, only MediaInfo has an Android app.

**Rules for this appendix.** Product names are trademarks of their respective owners and are used only to identify the products compared; OVT is not affiliated with, endorsed by or compatible with any of them. Entries state *what* a product does (codecs, containers, platforms, feature areas, metrics), each with a link to the public page it comes from. They do not quote vendor documentation, and they do not describe vendors' menus, panel layouts, key bindings, command-line flags or visual styles: OVT's own user interface is specified independently in chapter 11 and its CLI in chapter 12. No facts here come from installing, running or examining a commercial product.

## A.1 ViCueSoft VQ Analyzer (and VQ Probe, VQ DVK)

| Item | Facts | Source |
|---|---|---|
| Lineage | Successor product to Intel Video Pro Analyzer (VPA), which Intel discontinued. | https://www.intel.com/content/www/us/en/developer/articles/tool/video-pro-analyzer.html |
| Platforms | Windows, macOS and Linux desktop; no Android/iOS. | https://vicuesoft.com/vq-analyzer |
| GUI/CLI | Graphical and console modes. | https://vicuesoft.com/vq-analyzer |
| Codecs | VVC/H.266, AV1, AVM (AV2 development codec), AVS3, HEVC (incl. RExt, SCC, SHVC/MV-HEVC), AVC, VP9, MPEG-2, JPEG XS, VC-3, APV. | https://vicuesoft.com/vq-analyzer · https://vicuesoft.com/analyzer/releasenotes |
| Containers | MKV/WebM, MP4/MOV, MMT, MPEG-2 TS/PS, AVI, ASF, FLV, IVF, PCAP, HEIF/AVIF; raw elementary streams. | https://vicuesoft.com/analyzer/releasenotes |
| HDR | HDR and VBV buffer visualisation; SEI identification. | https://vicuesoft.com/vq-analyzer · https://vicuesoft.com/analyzer/releasenotes |
| Pricing | Commercial per-seat subscriptions and perpetual licences in several codec editions; an academic edition exists. | https://shop.xlsoft.com/en/vicue-price/ |

**Feature areas** (from the product page and release notes): syntax element tree per codec with links to the raw bytes and hex view; per-frame type, size and QP charts; HRD/VBV buffer analysis; reference and DPB structure; block partitioning, prediction mode and motion-vector visualisation; per-stage pictures (prediction, residual, reconstruction, in-loop filters) with block-level detail of intermediate values; QP, bit-cost and per-block PSNR/SSIM maps; memory-bandwidth analysis; stream statistics; comparison against a decoder-output YUV with first-difference search; two-stream comparison; conformance and error reporting; extraction of NAL units, sub-streams and decoded data to files; batch analysis from the console.

**VQ Probe (free metrics tool)** — https://vicuesoft.com/vq-probe/. Windows/macOS/Linux; compressed and raw inputs in common containers. Metrics: PSNR variants, XPSNR, PSNR-HVS, SSIM, MS-SSIM, VMAF, VMAF-NEG, CAMBI, CIEDE2000. Feature areas: side-by-side and difference comparison, per-frame metric curves with export, RD curves and BD-rate against an anchor, convex hull and encoding-ladder tables, regions of interest, SI/TI, scene-change detection, frame-size view; console tool.

**VQ DVK (Decoder Validation Kit)** — https://vicuesoft.com/vq-dvk/vq-dvk-av1. Conformance and stress stream sets for VVC, AV1, HEVC, VP9 and AVS3 with coverage reports. Relevant to OVT only as a consumer: OVT must ingest such stress streams gracefully (NFR robustness, B.7 rule 1).

## A.2 Elecard StreamEye Studio

| Item | Facts | Source |
|---|---|---|
| Composition | Suite of four applications, each with a console version: StreamEye, Stream Analyzer, Video Quality Estimator and YUV Viewer. Stream Inspector is a separate real-time monitoring product. | https://www.elecard.com/products/video-analysis/streameye-studio · https://elecard.com/products/monitoring/stream-inspector |
| Platforms | Windows, macOS and Linux desktop; no Android. | https://www.elecard.com/products/video-analysis/streameye |
| Pricing | Quote-based; a "Basic" edition sells some codecs as paid plugins. | https://elecard.com/question/elecard-streameye-and-elecard-streameye-basic-comparison |
| StreamEye codecs | MPEG-1/2, AVC, HEVC, MV-HEVC, VP9, AV1, VVC, AVS3, APV. | https://www.elecard.com/products/video-analysis/streameye |
| StreamEye containers | MPEG-1 SS, MPEG-2 TS/PS, MP4/MOV, MKV/WebM, AVI, MXF, FLV, IVF, HEIC; DASH and HLS manifests (one rendition). | https://www.elecard.com/products/video-analysis/streameye · https://www.elecard.com/docs/en/ReleaseNotes/index.html |
| Raw formats | Common planar, semi-planar and packed YUV and RGB formats. | https://www.elecard.com/products/video-analysis/streameye |

**StreamEye feature areas** (from the product page, edition comparison and release notes): per-frame size/type/QP chart used for navigation, with bitrate over a selected range; bit distribution across coded elements; frame thumbnails; stream, picture and block information; parameter-set and slice headers; DPB contents; per-stage block data (prediction, residual, coefficients, unfiltered and decoded pixels); statistics; hex view; pixel values; standards-compliance messages; HRD/VBV buffer analysis; picture comparison against a reference with PSNR/SSIM/VMAF; overlays for slices, tiles, partitions, motion vectors and in-loop filters; colour-matrix selection; HDR format and metadata identification; frame search by order, time or byte offset; synchronisation and shared comments between the suite's applications; CSV export and decoded-YUV export; console.

Quality metrics listed for StreamEye with a reference: PSNR, SSIM, DELTA, MSE, MSAD, VQM, NQI, EPSNR, VMAF.

**Stream Analyzer** (https://elecard.com/products/video-analysis/stream-analyzer): container structure and hex view for TS/PS, MP4/MOV, MKV/WebM, AVI, FLV, IVF, HEIC, DASH and HLS; video and audio syntax for the common codecs; ETSI TR 101 290 checks; PSI/SI, SCTE-35 and other broadcast tables; PCR/PTS/DTS analysis; bitrate graphs with short windows; HDR metadata checks; CSV export; console.

**Video Quality Estimator** (https://www.elecard.com/products/video-analysis/video-quality-estimator · https://www.elecard.com/question/what-metrics-video-quality-estimator-4.x-can-calculate): full-reference metrics PSNR, APSNR, SSIM, DELTA, MSE, MSAD, VQM, NQI, VIF, VMAF, VMAF-phone; several distorted files against one reference; regions of interest; automatic start-frame alignment; per-frame metric plots and difference views; CSV/JSON export; console.

**YUV Viewer** (https://www.elecard.com/en/products/video-analysis/yuv-viewer): raw video display and comparison with full-reference metrics; colour-matrix selection; console for batch use.

## A.3 Other tools

**Intel Video Pro Analyzer (discontinued).** HEVC, VP9, AVC, MPEG-2; common containers; file-based analysis. https://www.intel.com/content/www/us/en/developer/articles/tool/video-pro-analyzer.html

**Solveig Zond 265.** https://www.solveigmm.com/en/products/zond/. HEVC, AVC, MPEG-2, AV1, EVC, VVC; common containers and raw YUV; Windows/macOS/Linux. Feature areas: frame-size chart with reference marking; block-level overlays (partitions, QP, bits, reference index, PSNR); bitrate histogram; HRD/CPB analysis; HDR/SEI parsing; AV1 level validation; no-reference and full-reference metrics; conformance validation; CSV/JSON reports and per-stage YUV output from the CLI. Free and paid tiers.

**Codecian CodecVisa.** https://codecian.com/codecvisa.html. HEVC, AVC/MVC, VP9/VP8, MPEG-2; common containers; Windows/Linux/macOS; no release since 2017; no VVC/AV1. Feature areas: per-stage whole-picture display, per-stage pixel values, headers, block information, summary statistics, partition and motion-vector display, stream comparison, data dumps.

**Interra VEGA Media Analyzer.** https://www.interrasystems.com/video-codec-analyzer.php · https://interrasystems.com/Vega-cli.php. AV1, VVC, HEVC, AVC, VP8/9, MPEG-2, JPEG 2000, ProRes, AVS/AVS+, Dolby Vision; broadcast and ABR containers with manifest validation; audio. Feature areas: block overlays (bits, MV, QP, reference index), residual and in-loop filter views, per-stage pixels, syntax-to-hex linkage, buffer analysis, quality metrics, stream comparison, error reporting; Linux/Docker CLI with XML reports.

**Tektronix/Telestream MTS4EA.** https://manualmachine.com/tektronix/mts4ea/10503446-datasheet. AVC, MPEG-2, VC-1, MPEG-4 Part 2, H.263 and HEVC; buffer analysis, spatial bit and quantisation plots, MV histograms, syntax traces, bitstream editing, batch regression, YUV output; Windows.

**Sencore CMA 1820.** https://www.tvtechnology.com/opinions/sencore-delivers-hevc-analysis-capabilities-with-cma-1820. HEVC, AVC, MPEG-2, VC-1 and audio; frame-by-frame analysis, coding-tree structure, header syntax, PTS/DTS and A/V sync, SCTE-35.

**Parabola Explorer (HEVC only).** https://parabolaresearch.com/explorer-hevc-analyzer.html. HEVC Main/Main10; Windows. Feature areas: syntax tree down to CABAC bins with bit sizes, GOP and DPB/reference-list views, per-picture QP/size chart, block overlays, per-stage pictures, feature-usage statistics, conformance checks with spec paragraph references, profile/level validation.

**Telestream Switch and Aurora.** https://redsharknews.com/telestream-switch-5-pro-review-comprehensive-qc-software · https://www.trustradius.com/products/telestream-aurora/details. Switch: QC playback and container/codec property inspection on macOS and Windows, captions, loudness, multi-file comparison; not a bitstream analyzer. Aurora: automated file QC with a no-reference picture-quality score and delivery-spec templates.

**VideoQ VQV.** https://videoq.com/Downloads/VideoQ_VQV_Viewer-Analyzer_Datasheet.pdf. Windows raw/decoded viewer with broadcast scopes (waveform, vectorscope, gamut, histograms), HDR light-level measurement, PQ/HLG/SDR transfer functions and colour-matrix selection. Relevant as a reference point for chapter 10 HDR measurements.

**vooya (raw viewer, shareware).** https://www.offminor.de/. Raw video viewer for many planar/packed formats up to 16-bit and float; two-file difference with per-channel metrics; histograms; command-line and pipe input; macOS/Windows/Linux.

**MediaInfo (open source, BSD-2).** https://mediaarea.net/en/MediaInfo. CLI/GUI/library with Text, HTML, XML, JSON, EBUCore and PBCore output and template-based reports; very wide container/codec metadata coverage; official Android app. Its detailed trace output is a container oracle (B.2).

**MulticoreWare.** No commercial bitstream analyzer product could be located (https://en.wikipedia.org/wiki/X265). Not applicable.

## A.4 Consolidated feature matrix

Legend: ● = yes/full, ◐ = partial or edition-gated, ○ = no/unknown, per the public sources above. Abbreviations: VQA = VQ Analyzer, SE/SES = StreamEye (full) / Studio companion apps, Z = Zond 265, PE = Parabola Explorer, CV = CodecVisa, VEGA = Interra VEGA, MTS = Tektronix MTS4EA, VPA = Intel VPA (legacy), Sw = Switch, VQV = VideoQ, vo = vooya, MI = MediaInfo.

| Feature | VQA | SE/SES | Z | PE | CV | VEGA | MTS | VPA | Sw | VQV | vo | MI |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AVC / HEVC | ● | ● | ● | ○/● | ● | ● | ●/◐ | ● | play | play | ○ | meta |
| VVC | ● | ● | ◐ | ○ | ○ | ● | ○ | ○ | ○ | ○ | ○ | meta |
| AV1 / AVM(AV2) | ●/● | ●/○ | ●/○ | ○ | ○ | ●/○ | ○ | ○ | ○ | ○ | ○ | meta |
| VP9 / MPEG-2 | ● | ● | ○/● | ○ | ● | ● | ○/● | ● | ○/● | ○ | ○ | meta |
| AVS3 / APV / JPEG XS | ●/●/● | ●/●/○ | ○ | ○ | ○ | AVS+ | ○ | ○ | ○ | ○ | ○ | ○ |
| EVC / LCEVC | ○ | ○ | EVC | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| TS/PS, MP4, MKV | ● | ● | ● | ES only | ● | ● | ● | ● | ● | via ffmpeg | ○ | ● |
| DASH/HLS manifest ingest | ○ | ● | ○ | ○ | ○ | ● | ○ | ○ | ○ | ○ | ○ | ○ |
| PCAP / RTP | ● | ○ | ○ | ○ | ○ | ● | ○ | ○ | ○ | ○ | ○ | ○ |
| Syntax element tree | ● | ● | ● | ● (bins) | headers | ● | traces | ● | ○ | ○ | ○ | ○ |
| HEX view linked to syntax | ● | ● | ● | ● | per-MB | ● | ● | ● | ○ | ○ | ○ | ○ |
| Spec quotations / paragraph refs | ● | ○ | hints | ● | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| CU/PU/TU partition overlay | ● | ● | ● | ● | ● | ● | MB | ● | ○ | ○ | ○ | ○ |
| Prediction mode overlay | ● | ● | ◐ | ● | ● | ● | ● | ● | ○ | ○ | ○ | ○ |
| Motion vectors (+ affine/subblock) | ● | ● | ○ | ● | ● | ● | ● | ● | ○ | ○ | ○ | ○ |
| QP map | ● | ● | ● | ● | avg | ● | plot | ● | ○ | ○ | ○ | ○ |
| Bits heat map | ● | ● | ● | ● | ● | ● | ● | ● | ○ | ○ | ○ | ○ |
| Residual / coefficient view | ● (3 stages) | ● | CLI dump | ● | ● | ● | ● | ● | ○ | ○ | ○ | ○ |
| Deblocking / SAO / ALF detail | ● | ◐ | ○ | SAO | pre-filter | ● | ○ | ● | ○ | ○ | ○ | ○ |
| Per-stage picture (pred/recon/unfiltered) | ● | ● | CLI | ● | ● | ● | ● | ● | ○ | ○ | ○ | ○ |
| Y/U/V plane select | ● | ● | ● | ○ | ● | ● | ● | ● | ○ | ● | ● | ○ |
| Reference / DPB graph | ● | ● | ◐ | ● | ref list | ● | ○ | ● | ○ | ○ | ○ | ○ |
| Frame-size / bitrate chart | ● | ● | ● | ● | ○ | ● | ● | ● | ● | ● | ○ | ○ |
| GOP / hierarchy view | ● | ● | ● | ● | ○ | ● | ○ | ● | ● | ● | ○ | ○ |
| HRD / VBV / CPB buffer curve | ● | ● | ● | ○ | ○ | ● (T-STD) | ● | ● | ○ | ○ | ○ | ○ |
| Memory-bandwidth / efficiency map | ● | ○ | ○ | ○ | ○ | ○ | ○ | VP9 | ○ | ○ | ○ | ○ |
| PSNR/SSIM per block overlay | ● | ◐ | ● | ○ | ○ | ○ | fidelity | ● | ○ | ○ | ○ | ○ |
| Per-frame metrics timeline | ● | ● | ● | ○ | ○ | ● | ● | ● | ○ | ○ | ○ | ○ |
| VMAF | Probe | ● | ● | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| No-reference metric | ○ | EPSNR | NR-PSNR | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| RD curves / BD-rate / convex hull | Probe | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ |
| HDR metadata display | ● | ● (HDR10/Vivid) | ● | ● (blog) | ○ | ● (DoVi) | ○ | ● | ◐ | ● | ○ | ● |
| HDR rendering / light-level scopes | ◐ | ○ | ○ | ○ | ○ | ○ | ○ | ◐ | ◐ | ● | exposure | ○ |
| Conformance / error list | ● | ● (AVC/HEVC) | ● | ● | ○ | ● | ● | ● | ○ | ○ | ○ | ○ |
| TR 101 290 / PSI-SI / SCTE-35 | ○ | ● (Stream Analyzer) | ○ | ○ | ○ | ● | ● | ○ | ○ | ○ | ○ | ○ |
| Dual stream sync compare | ● | ● (Binding) | ○ | ○ | ● | ● | ○ | ● | ● | ○ | ● | ○ |
| Debug YUV / first-difference | ● | ● | ○ | diff | ● | ● | ● | ● | ○ | ○ | ● | ○ |
| Statistics (pie/bar, normalised) | ● | ● | ● | ● (Pro) | ● | ● | ● | ● | ○ | ○ | ○ | ○ |
| Export CSV/JSON/XML | CSV/txt | CSV/JSON | CSV/JSON | print | dump | XML/PDF | Excel | txt | ○ | BMP | metrics | XML/JSON/EBUCore |
| NAL/sub-stream extraction | ● | ● (save portion) | ○ | ○ | ○ | ○ | ● editor | ● | ● export | ○ | ○ | ○ |
| CLI / batch | ● | ● | ● (Business) | ○ | ○ | ● Docker | ● | ● | ○ | ○ | ● | ● |
| Platforms Win/mac/Linux | ●●● | ●●● | ●●● | Win | ●●● | Win/Linux CLI | Win | ●●● | Win/mac | Win | ●●● | ●●● |
| Android | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ○ | ● |
| Free tier | trial | demo | ● | trial | trial | trial | ○ | 10 frames | Player | ○ | shareware | ● OSS |

## A.5 Usability lessons (recorded for Chapter 11)

This section used to record the user-interface conventions of the surveyed products. It now keeps only generic, product-neutral usability lessons; OVT's layout, panels and key bindings are designed independently in chapter 11.

1. Users expect a timeline of per-frame data, a picture canvas with switchable overlays, and a syntax/statistics view of the current selection to be visible together.
2. Selecting a block, a syntax element or a byte range should select the same item in every other view, and navigation from an error to the picture and block it concerns should take one action.
3. Decode order and display order both matter to codec engineers; every view that lists frames should say which order it uses and allow switching.
4. Numbers are easier to use with legends and derivations shown beside them.
5. Every analysis available in the GUI should also be available from the command line for batch use.
6. Labels should use the terminology of the codec specifications.

Pain points reported in published reviews (to avoid): guessing raw-source formats without a preview, requiring equal resolutions for comparison, charts that cannot be resized, and locale-dependent CSV defaults.

---

# Appendix B — FOSS library and tool survey

Each table gives licence, language, platforms, maintenance status, what the component exposes, and **how OVT uses it** per ADR-006/007/008/011: *dependency* (linked or built into OVT), *oracle* (used only in tests to validate OVT's own output), *reference* (design or code read, not linked), *not used*.

## B.1 Decoders and parsers

| Component | Licence | Lang | Platforms (macOS arm64 / Android arm64 / Linux) | Maintenance | What it exposes | OVT use |
|---|---|---|---|---|---|---|
| **FFmpeg** (libavformat, libavcodec, ffprobe) — https://ffmpeg.org · https://github.com/FFmpeg/FFmpeg | LGPL-2.1+; GPL-2+ with `--enable-gpl`; `--enable-version3` → LGPL-3/GPL-3 | C | all (VideoToolbox hwaccel on macOS; `*_mediacodec` decoders on Android via NDK, `--enable-mediacodec --enable-jni`; VA-API on Linux) | very active; releases ~every 6 months (7.x 2024–25, 8.x 2025–26) | Demux (`av_read_frame`; `h264_mp4toannexb`/`hevc_mp4toannexb`, `av1_frame_split/merge`, `extract_extradata`, `filter_units`, `*_metadata`, `trace_headers` BSFs, https://ffmpeg.org/ffmpeg-bitstream-filters.html). CBS header parser (`libavcodec/cbs*.h`, **private**, not installed; `ff_cbs_*` not exported). `trace_headers` BSF logs every header syntax element with name, bit position, bit string, value for AV1/H.264/H.265/(M)JPEG/MPEG-2/VP9: `ffmpeg -i in.mp4 -c:v copy -bsf:v trace_headers -f null -`. ffprobe `-show_packets -show_frames -show_streams -of json` with `frame.side_data_list` (https://ffmpeg.org/ffprobe.html). **`AVCodecContext.export_side_data` flags:** `AV_CODEC_EXPORT_DATA_MVS` (`mvs`) → `AV_FRAME_DATA_MOTION_VECTORS` (`AVMotionVector{source,w,h,src_x,src_y,dst_x,dst_y,flags,motion_x,motion_y,motion_scale}`) implemented for **H.264** (via `ff_print_debug_info2` in `mpegutils.c`), MPEG-1/2/4, H.263, VC-1, VP3/Theora — **not HEVC, not AV1, not VP9**; `AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS` (`venc_params`) → `AV_FRAME_DATA_VIDEO_ENC_PARAMS` (`AVVideoEncParams` per-frame qp + `AVVideoBlockParams{src_x,src_y,w,h,delta_qp}`) for **H.264** (`AV_VIDEO_ENC_PARAMS_H264`, per MB), **MPEG-2**, **VP9** — **not HEVC, not AV1**; `AV_CODEC_EXPORT_DATA_FILM_GRAIN` for HEVC, H.264, libdav1d, native av1dec; `..._PRFT`, `..._ENHANCEMENTS` (LCEVC). `hevc/hevcdec.c` and `hevc/refs.c` reference only `FILM_GRAIN`; `libdav1d.c` exports film grain, HDR metadata, T.35, Dolby Vision but no MVs/blocks; native `av1dec.c` is a header parser + hwaccel shell (no software AV1 reconstruction in FFmpeg). HEVC MV export patches (Dec 2019–Feb 2020) **never merged** (https://lists.mplayerhq.hu/pipermail/ffmpeg-devel/2020-January/255350.html · https://ffmpeg.org/pipermail/ffmpeg-devel/2020-February/257074.html). `codecview` filter (`vf_codecview.c`): `mv=pf+bf+bb`, `qp`, `block=1` from these side data. Legacy `-debug qp`/`mb_type` (`FF_DEBUG_QP=16`, `FF_DEBUG_MB_TYPE=8`) print text tables; `FF_DEBUG_VIS_*`/`-vismv` removed (Oct 2017, final in 4.4; https://ffmpeg.org/pipermail/ffmpeg-cvslog/2017-October/110005.html). MV export origin: https://ffmpeg.org/pipermail/ffmpeg-devel/2014-August/161593.html. MediaCodec: https://www.ffmpeg.org/doxygen/trunk/mediacodecdec_8c.html | **Dependency** (ADR-006 demux; ADR-008 H.264 MVs/QP via public API in `ovt-plugin-ffmpeg`; ADR-009 playback path incl. HW). `trace_headers` = **oracle** for `core/src/syntax/` (ADR-007). CBS = **reference** only (private API). Phase-2 patch `0001-h264-export-mb-params.patch` extends `AVVideoBlockParams` (MB type, ref_idx, intra modes, CBP) with upstreaming attempt. |
| **dav1d** — https://code.videolan.org/videolan/dav1d · https://github.com/videolan/dav1d | BSD-2-Clause | C99 + asm (x86 AVX2/AVX-512/SSSE3, ARMv7/v8 NEON, RISC-V, PPC, LoongArch) | all (Meson cross files; used by Chrome, Firefox, VLC, AOSP `external/libdav1d`) | very active; 1.5.x in 2025 | Public API (`dav1d.h`, `picture.h`, `headers.h`, `data.h`): `dav1d_open/send_data/get_picture/close/flush`, `dav1d_parse_sequence_header()`; every `Dav1dPicture` carries full `Dav1dSequenceHeader*` and `Dav1dFrameHeader*` (quantizer, segmentation, loop filter, CDEF, restoration, film grain, tiling, global motion, refs) — frame-level syntax for free, no bit offsets. No mode-info export, no debug dump, no inspection analogue (`-Dlogging`/`Dav1dLogger` print errors only). Per-block `Av1Block` persists only in frame-threaded two-pass mode (see D.5). | **Dependency for AV1 playback only** (ADR-008/009); **no fork**. `Dav1dFrameHeader` may be used as a cross-check for the OBU parser. |
| **libaom** — https://aomedia.googlesource.com/aom/ (GitHub mirror https://github.com/mozilla/aom is stale) | BSD-2-Clause + AOM Patent License 1.0 | C + asm/intrinsics (x86, NEON incl. arm64, SVE) | all; Android via `build/cmake/toolchains/arm64-android.cmake` | active; v3.x several releases/year | **Inspection API**: build `-DCONFIG_INSPECTION=1` (and `-DCONFIG_ACCOUNTING=1`). `aom/aomdx.h`: `typedef void (*aom_inspect_cb)(void *decoder, void *ctx); struct aom_inspect_init { aom_inspect_cb inspect_cb; void *inspect_ctx; }`; control `AV1_SET_INSPECTION_CALLBACK` (returns `AOM_CODEC_INCAPABLE` when compiled without inspection; note prefix `AV1_`, not `AV1D_`), `AV1_GET_ACCOUNTING`. Callback fires after every decoded frame (newer versions also expose `inspect_sb_cb` per superblock). `av1/decoder/inspection.h` (source-tree header, not installed): `ifd_init(insp_frame_data*, w, h)`, `ifd_inspect(fd, decoder, skip_not_transform)`, `ifd_clear(fd)`; `insp_frame_data`/`insp_mi_data` fields in D.4. `examples/inspect.c` JSON flags `-bs/-ts/-tt/-m/-uvm/-mm/-ct/-r/-mv/-s/-f/-df/-c/-cfl/-ibc/-plt/-uvp/-dq/-si/-sb/-a/-A`. `Accounting` gives per-symbol bit costs with syntax-element names — the only FOSS per-block bit-cost source. Other controls: `AV1D_GET_FRAME_HEADER_INFO`, `AV1D_GET_TILE_DATA`, `AV1D_GET_TILE_SIZE`, `AV1D_SET_OUTPUT_ALL_LAYERS`, `AV1D_SET_OPERATING_POINT`, `AV1D_EXT_TILE_DEBUG`, `AV1D_GET_DISPLAY_SIZE`, `AV1D_GET_BIT_DEPTH`, `AV1D_SET_IS_ANNEXB`, `AV1D_SET_ROW_MT`, `AV1D_SET_SKIP_LOOP_FILTER` (all present in `aom/aomdx.h` of libaom ≥ 3.9). Cost: 2–4× slower than dav1d; inspection builds slower still; `CONFIG_INSPECTION` is off in all distro builds → OVT ships its own libaom. Inspection code maintained because aomanalyzer/AVM tooling use it (AVM copy: https://aomedia.googlesource.com/avm/+/1b651f3806129c2f444f19a246ac1844beae9e3f/av1/decoder/inspection.h); not covered by conformance tests; has broken on new tools before (mitigation: pin version, vendor `inspection.h`). | **Dependency** (ADR-008: `ovt-plugin-aom`, upstream API, build flags via `third_party/libaom.cmake`). Small `ifd_inspect` extension patch for missing fields is an upstreaming candidate (D.4). |
| **libgav1** — https://chromium.googlesource.com/codecs/libgav1/ (AOSP `platform/external/libgav1`) | Apache-2.0 | C++ | all | low; last tagged 0.20.0; superseded in AOSP by dav1d | `ObuParser` (`src/obu_parser.{h,cc}`) fully parses sequence/frame headers, tile info, metadata OBUs (`ObuMetadata`: HDR CLL, MDCV, T.35); no block-level API (block data in `Tile`/`BlockParameters`). | **Reference** for the OBU parser in `core/src/syntax/`. |
| **libde265** — https://github.com/strukturag/libde265 | LGPL-3.0+ (library); dec265/sherlock265 MIT | C++ | CMake; Linux/macOS/Windows; builds on arm64 (used in libheif); SIMD **SSE4.1 kernels only** (`libde265/x86/sse*.cc`, selected by the `HAVE_SSE4_1`/`HAVE_X86` compile check in v1.0.15 — there are no AVX2/AVX-512 kernels), **no NEON** | low-moderate; security fixes, 1.0.x releases through 2024–25 (v1.0.15 is the pinned tag, chapter 7 §7.6.2; the exact release date and any newer 1.0.x tag are re-checked at WI-2.162); no new features | Main, Main10 (conformance-exact); the README also lists RExt chroma formats, but the codec-internals survey (D.2) finds no RExt/SCC/MV-HEVC support, so OVT relies on libde265 for **Main/Main10 only** and routes RExt/SCC to the HM oracle plugin (chapter 7 §7.6.2; Open item OI-03), WPP/tile MT, "decodes almost all conformance streams". Public API `de265.h`: `de265_set_parameter_int(ctx, DE265_DECODER_PARAM_DUMP_{SPS,VPS,PPS,SLICE}_HEADERS, fd)`, `de265_get_image_NAL_header()`; no block accessors. `dec265` options: `-q -t threads -c check-hash -f frames -o output -d dump headers -n nal input -V videogfx -L no-logging -0 noaccel -B write-bytestream -m measure PSNR -s ssim map -e errmap -T highest-TID -v verbose --disable-deblocking --disable-sao`. Internal `de265_image` holds every CTB/CB/PB/TB/intra/MV/QP datum in `MetaDataArray`s (D.2). `libde265/visualize.h` exports `draw_CB_grid, draw_TB_grid, draw_PB_grid, draw_PB_pred_modes, draw_intra_pred_modes, draw_QuantPY, draw_Motion, draw_Slices, draw_Tiles` (drawing helpers used by sherlock265). | **Dependency** (ADR-008: `ovt-plugin-de265` with `patches/0001-de265-internals-api.patch`, read-only, upstreaming mandatory). Dynamic linking (LGPL-3). |
| **HM** (JCT-VC HEVC reference) — https://vcgit.hhi.fraunhofer.de/jvet/HM (mirror https://github.com/listenlink/HM) | BSD-3-Clause | C++ | CMake; anywhere incl. arm64; no SIMD (~1–5 fps 1080p, estimate) | maintained by JCT-VC/JVET (HM-18.x); conformance-complete incl. SCC/RExt/SHVC/MV-HEVC | `TComDataCU` public getters (D.3). `TAppDecoder` CLI: `-b -o -s SkipFrames -t MaxTemporalLayer -d OutputBitDepth --OutputColourSpaceConvert --SEIDecodedPictureHash --SEINoDisplay --SEIColourRemappingInfoFilename --OutputDecodedSEIMessagesFilename -l TarDecLayerIdSetFile`. Compile-time `RExt__DECODER_DEBUG_BIT_STATISTICS` (per-syntax-element bit statistics, ~10% runtime, printed at end, not per CU), `ENC_DEC_TRACE` (`g_hTrace`, `TraceDec.txt`: every syntax element with bit position), `DEC_NUH_TRACE`, `DEBUG_CABAC_BINS`, `DEBUG_STRING`. Software manual: https://hevc.hhi.fraunhofer.de/svn/svn_HEVCSoftware/tags/HM-16.20/doc/software-manual.pdf | **Dependency (optional)**: `ovt-plugin-hm` bit-exact oracle and RExt/SCC (ADR-008), via an own thin wrapper over public getters, no patch. YUView's `libHMDecoder`/`libHM` (https://github.com/ChristianFeldmann/libHMDecoder · https://github.com/ChristianFeldmann/libHM) is **reference** (unmaintained since ~2019). |
| **VTM / VVdeC** — https://vcgit.hhi.fraunhofer.de/jvet/VVCSoftware_VTM · https://github.com/ChristianFeldmann/VTM | BSD-3 | C++ | all | active | VTM `ENABLE_TRACING` + `--TraceFile/--TraceRule/--TraceChannelsList` CABAC traces (VTM-only; not in HM); VVdeC exposes no internals. | **Not used** (VVC is a future codec); the HM wrapper pattern applies later. |
| **openHEVC** — https://github.com/OpenHEVC/openHEVC | LGPL-2.1 | C | — | **dead** (~2017–18; README points to FFmpeg) | Research fork of Libav HEVC. | **Not used.** |
| **JM** (H.264 reference, `ldecod`) — https://iphome.hhi.de/suehring/tml/ · https://iphome.hhi.de/suehring/tml/doc/ldec/html/index.html | custom permissive ITU/ISO reference-software terms | C, no SIMD, no threading, global state (~10–30 fps 1080p, estimate) | buildable anywhere | maintained by HHI per H.264 edition (JM 19.0, 2015 last public tag) | `#define TRACE 1` writes `trace_dec.txt` (`p_Dec->p_trace`): one line per syntax element `@ bitpos element_name bitstring (value)` incl. MB-level (`mb_type`, `ref_idx`, `mvd`, `coeff`). CLI `-i -o -r -p pocScale -uv -s -lp`; config `defdec.cfg`. (`-p TraceFile` does not exist.) Structs `Macroblock`/`StorablePicture` (D.6). | **Oracle** for the H.264 syntax parser and for the trace format; not a runtime component. |
| **openh264** — https://github.com/cisco/openh264 | BSD-2 | C++ | all | active | Constrained Baseline–Main-ish decoder (`ISVCDecoder::DecodeFrame2`; `DECODER_OPTION_TRACE_LEVEL` controls logging only); no internals, no High/10-bit. | **Not used.** |
| **GStreamer codecparsers** (gst-plugins-bad `gst-libs/gst/codecparsers`) — https://gstreamer.freedesktop.org/documentation/codecparsers/index.html · https://gstreamer.freedesktop.org/documentation/codecparsers/gstav1parser.html | LGPL-2.1+ | C (GLib only) | all; official Android/macOS binaries; buildable standalone | very active (AV1 parser since 1.18, H.266 since 1.22, `GstAV1DecoderConfigRecord` since 1.30) | `GstH264NalParser` (`gst_h264_parser_identify_nalu[_avc]`, `parse_nal/sps/pps/sei/slice_hdr/subset_sps`; full SPS/PPS/SliceHdr/SEIMessage structs incl. VUI, HRD, scaling lists, RPLM, pred weight table, dec_ref_pic_marking); `GstH265Parser` (`identify_nalu`, `parse_vps/sps/pps/sei/slice_hdr` incl. RExt/SCC, ST-RPS); `GstAV1Parser` (`identify_one_obu`, `parse_sequence_header_obu`, `parse_temporal_delimiter_obu`, `parse_frame_header_obu`, `parse_frame_obu`, `parse_tile_group_obu`, `parse_metadata_obu`, `parse_tile_list_obu`, `gst_av1_parser_reference_frame_update`, Annex B vs low-overhead via `gst_av1_parser_reset`); bit writers `GstH264BitWriter`, `GstH265BitWriter`, `GstAV1BitWriter`. Headers only; **no per-field bit offsets**. GSoC-2014 "codecanalyzer" (GTK, https://gitlab.freedesktop.org/gstreamer/gst-devtools · https://bugzilla.gnome.org/show_bug.cgi?id=731853) was abandoned 2015 (its `analyzersink` XML was 282 MB per 1000 frames — cautionary). | **Oracle** for `core/src/syntax/` (ADR-007); **reference** for struct layouts. |
| **Chromium `media/video` parsers** — https://chromium.googlesource.com/chromium/src/+/main/media/video/ | BSD-3 | C++ | — | active | `h264_parser.cc`, `h265_parser.cc`, `av1_decoder.cc` (wraps libgav1 `ObuParser`), `vp9_parser.cc`; header-level, fuzzed, entangled with `base/`. | **Reference.** |
| **Intel media-driver / Mesa VA-API** | MIT/BSD | C/C++ | — | active | Header parsers filling `VAPictureParameterBuffer*` (Mesa `src/gallium/frontends/va/`, `vl_*_bitstream.c`). | **Not used.** |
| **h264bitstream** — https://github.com/aizvorski/h264bitstream | LGPL-2.1 | C99 | all | unmaintained since ~2017 (stable, tiny) | NAL, SPS, PPS, slice header, AUD, SEI framing (payloads unparsed) read/write; `h264_analyze` CLI; no slice data; keeps only last SPS/PPS. Python counterpart `h26x-extractor` (MIT, https://pypi.org/project/h26x-extractor/). H264Naked (MIT Qt wrapper, https://github.com/julee/h264naked). | **Reference.** |
| **h26forge** — https://github.com/h26forge/h26forge | MIT | Rust | — | last push Jan 2024 | Complete H.264 syntax encoder/decoder down to macroblock/residual level, built for fuzzing non-compliant streams. | **Reference** (and a source of hostile test streams for fuzz corpora). |
| **h264-reader** — https://docs.rs/h264-reader | MIT/Apache-2.0 | Rust (v0.9) | — | maintained (dholroyd/scottlamb) | Annex B & AVCC framing, RBSP de-emulation, SPS/PPS/SEI/slice-header parsing with parameter-set `Context`. "Not a video decoder." | **Not used** (ADR-001 C++). |
| **hevcesbrowser** — https://github.com/virinext/hevcesbrowser · https://github.com/virinext/hevcesbrowser/releases | GPL-2.0 (only) | C++/Qt5 + `hevcparser` lib | desktop | dormant; last release 1.0.80.120121 (12 Jan 2021) | Full HEVC VPS/SPS/PPS/slice-header/SEI syntax tree with hex pane for Annex B ES only. | **Reference** only (GPL-2-only is incompatible with OVT's GPL-3-or-later app and LGPL core). |
| **CodecScope** — https://github.com/codec2021/codecscope | GPL-2 | C++ → WebAssembly | web | active 2024–25 | hevcesbrowser's HEVC parser plus H.264/VVC/AV1/VP9 parsers, syntax tree with bit-level field decoding, hex view, MP4/TS/FLV/WebM/IVF demux, GOP timeline; WebCodecs/WASM decoding for pixels. | **Reference** for tree↔hex UI design (licence as above). |
| **YUView** — https://github.com/IENT/YUView · https://ient.github.io/YUView · https://github.com/IENT/YUView/wiki | GPL-3.0 | C++17/Qt6 (qmake; QPainter rendering) | Windows/macOS/Linux (Flathub, AppImage); no Android | ~3.5k commits; one maintainer (Christian Feldmann); ~1 release/yr; last push Dec 2024 (ecosyste.ms); releases 2.10 (VVC parsing, vvdec) … 2.14 (newer FFmpeg) | Architecture: `playlistItem` model (raw YUV/RGB, image sequence, compressed, text, difference, overlay; XML playlist); `decoderBase` (`pushData/decodeNextFrame/getRawFrameData/resetDecoder`) with `decoderBaseSingleLib` subclasses that `QLibrary`-load libraries at runtime: `decoderFFmpeg` (MVs only via `export_mvs`), `decoderLibde265`, `decoderHM`, `decoderVTM`, `decoderVVDec`, `decoderDav1d` (https://github.com/IENT/YUView/wiki/External-Decoders). `cacheStatistics()` fills `statisticsData` (`StatisticsType` grids, `addBlockValue(x,y,w,h,value)`, vectors, `ColorMapper`). Own bitstream parsers for AVC/HEVC/VVC/AV1/OBU and MP4/TS/Annex B (NAL/OBU tree, bitrate plot, syntax-element view) — but **no hex view tied to the tree**. **Statistics CSV** ("Statistics Extension Syntax", https://github.com/IENT/YUView/wiki/Statistics-Extension-Syntax): semicolon-separated; header lines start with `%`: `%;syntax-version;v1.x`, `%;seq-specs;<basename>;<layer>;<w>;<h>;<fps>`, `%;type;<id>;<name>;map\|range\|vector`, then `mapColor`, `range`/`defaultRange <min> <max> <colormap>` (heat, jet, hsv, hot, cool, …), `scaleToBlockSize`, `vectorColor`, `scaleFactor`; data rows `POC;x;y;w;h;typeID;value[;vecY]` grouped by POC and type. Gaps: no residual/coefficient/predicted-picture views, no HRD/DPB, no bit accounting, no conformance checks, no scripting API, no VMAF/SSIM, no mobile. All three "rich internals" decoders YUView uses are IENT forks that were never upstreamed. | **Reference + code reuse** under GPL-3 in `app/` (raw-format readers, difference/overlay items, decoder wrappers) per ADR-002; **export format** (YUView CSV, B.7 rule 9, ADR-010). Decoder-plugin + generic block-statistics model adopted (ADR-008). |
| **aomanalyzer** — https://github.com/xiph/aomanalyzer · https://people.videolan.org/~negge/AOM_Analyzer_update_2021-Aug-13.pdf · https://github.com/xiph/aomanalyzer/issues | MIT | TypeScript + React/Electron | browser / macOS/Linux/Windows | dormant (last push May 2023; newest issue Apr 2024; 188 commits, ~34 open issues, no releases) | Loads Emscripten build of libaom `examples/inspect.c` (`inspect.js`/`decoder.js`; exports `open_file, read_frame, set_layers, set_compress, get_plane*, get_frame_width/height, get_bit_depth`; `Module.on_frame_decoded_json`). Layered overlays (block/tx partitions, modes, MVs, ref frames, skip, segment, CDEF, filter, tx type), histograms of bits/symbols/block size/tx size/mode/skip, Block Info & Frame Info tabs, film-grain visualiser, split-view A/B with blind-vote URLs, deep links; VP9 via libvpx inspect build. | **Reference** for overlay semantics and the `inspect.c` JSON schema (OVT exports an aom-inspect JSON subset, B.7 rule 9). |
| **GitlHEVCAnalyzer** — https://github.com/lheric/GitlHEVCAnalyzer | "modified Apache-2.0" with non-commercial clause → **not OSI open source** | C++/Qt5 | desktop | dormant since ~2016 | CU/PU/TU, MVs, intra modes, bit heat-map, tiles, frame timeline; 8-bit 4:2:0; reads text dumps from a patched HM (HM-4.0…12.0). | **Not used** (licence gate fails on non-commercial). |
| **HEVCBSAnalyzer / H264BSAnalyzer** — https://github.com/latelee/HEVCBSAnalyzer | LGPL | C++ (Windows MFC) | Windows only | — | NAL tree + hex + playback via static ffmpeg; h264bitstream + HM 16.6. | **Not used.** |
| **videoparser-ng** (AVEQ/VQEG) — https://vqeg.github.io/software-tools/quality%20analysis/videoparser-ng | LGPL-2.1 | C++ | CLI/API | maintained | Per-frame QP, MV statistics and frame metadata as NDJSON for AVC/HEVC/VP9/AV1 on top of FFmpeg side data. | **Reference** (and potential collaborator). |
| **PyAV** — https://github.com/PyAV-Org/PyAV | BSD | Python | all | active | `frame.side_data` incl. `MOTION_VECTORS` (`av.sidedata.motionvectors`). | **Reference** for prototyping only. |
| **rav1e / rav1d** — https://github.com/xiph/rav1e · https://github.com/memorysafety/rav1d | BSD-2 | Rust | — | active | rav1e is encoder-only; rav1d is a line-by-line port of dav1d with the same lack of export. SVT-AV1 removed its decoder in v2.0.0 (2024); only the encoder remains. | **Not used.** |
| **vooya / YUVToolkit / other YUV viewers** — https://offminor.de/ · https://github.com/dryotta/yuvtoolkit | closed / GPL | — | — | YUVToolkit dead | See A.3 for vooya. | **Reference** for raw-format preview UX. |

## B.2 Container libraries and tools

| Library / tool | Licence | Lang | Android / macOS arm64 | Box tree API | Field-level decode | Offsets per box | fMP4/CMAF | CENC | HDR boxes | Maintenance | OVT use |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **GPAC / MP4Box / libgpac** — https://github.com/gpac/gpac · https://wiki.gpac.io · https://github.com/gpac/gpac/wiki/mp4box-dump-opts · https://github.com/gpac/gpac/wiki/custom-boxes | LGPL-2.1+ (some optional deps GPL; commercial via Motion Spell, https://gpac.io/licensing) | C | yes (official NDK scripts) / yes (Homebrew) | internal structs (`GF_Box` in `include/gpac/internal/isomedia_dev.h`, not ABI-stable); XML dump `MP4Box -diso/-dmp4`, `-dxml`, `-disox`, `-dts` (per-sample timing **with byte offsets**), `-dchunk`, `-dnal`, `-dsap`, `-info/-infox`, `-boxes` (XML of every known box: type, container, spec, default fields; the option is `MP4Box -boxes`); `gf_isom_*` accessors (`GF_ISOM_OPEN_READ`, `gf_isom_open_progressive`, `gf_isom_refresh_fragmented`) | all known boxes; broadest coverage (ISOBMFF, NAL FF avcC/hvcC/vvcC/lhvC, av1C, vpcC, dvcC/dvvC/dac3/dec3/dac4, DTS, Opus/FLAC, HEIF/MIAF, CENC/CENS/CBC1/CBCS, ISMA/OMA, 3GPP, QuickTime, colr/mdcv/clli/cclv, emsg/prft/sidx/ssix/styp/mfra, tx3g/wvtt/stpp, udta/iTunes, EVTE); `inspect` filter decodes SEI incl. HDR10+ and DV RPU presence | **no** in XML dump; sample offsets via `-dts` | excellent (reference DASH/CMAF packager; MPD/HLS ingestion) | excellent | colr/mdcv/clli/dvcC | very active; v26.07.0 (July 2026, ABI 16.21) | **Oracle** (`MP4Box -diso/-dts` golden files); fallback inspection backend if the own reader stalls (ADR-006). Python (ctypes) and Node bindings exist. |
| **Bento4** — https://www.bento4.com/documentation · https://github.com/axiomatic-systems/Bento4 · https://www.bento4.com/about | GPL-2 **or commercial** (dual) | C++ | yes (NDK build via CMake) / yes | `AP4_File`/`AP4_Atom` tree, `AP4_AtomInspector` visitor; `AP4_ByteStream` custom I/O; `AP4_LinearReader` for fMP4 | most; `mp4dump` (`--format text|json`) | partial (OI-04) | excellent | excellent (DRM-centric) | partial (dvcC/dvvC; mdcv/clli coverage unconfirmed, OI-04) | moderate (~1.6.0-64x; many open issues) | **Not used** as a dependency (GPL-2-only incompatible with GPL-3/LGPL); **oracle** via shelling out only. Tools: mp4info, mp4dump, mp4extract, mp4edit, mp4encrypt/decrypt, mp4fragment, mp4dash, mp4hls, mp42ts. |
| **FFmpeg libavformat / ffprobe** — https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/mov.c | LGPL-2.1+ | C | yes / yes | none; `-v trace` prints `type:'moov' parent:'root' sz: N pos: M` flat lines | interpreted only (stream side data: Mastering Display, Content Light Level, DV configuration record, HDR10+ from SEI; colr → primaries/trc/matrix; `-show_packets` with `pos`; `-dump_attachment`) | trace only | demux only | demux only | side data | very active | **Dependency** for demux (ADR-006); **oracle** for interpreted values. |
| **mp4parse-rust** (Mozilla) — https://github.com/mozilla/mp4parse-rust | MPL-2.0 | Rust (+ `mp4parse_capi`) | yes / yes | internal | track-metadata subset: ftyp/moov/trak/mdia/stsd, avcC/hvcC/av1C/vpcC/esds/Opus/FLAC/ALAC, colr (validated), pssh, sinf/schm/tenc, edts/elst, stts/ctts/stsc/stsz/stco/co64/stss, meta/HEIF; `moof` detected only | yes (`BoxHeader` size + offset-to-payload) | moov only | pssh/tenc | colr only (no mdcv/clli/dvcC/emsg/sidx) | Mozilla-maintained (0.17.x) | **Reference** for a hardened reader. |
| **`mp4` crate** (alfg) and forks `mp4box`, `mp4forge` — https://crates.io/crates/mp4 · https://docs.rs/mp4box | MIT | Rust | yes / yes | structs | small subset (moov tree, avc1/hev1/vp09/av01/mp4a/tx3g, trex/moof/traf/tfhd/trun) | no | basic | no | no | slow (0.14.x) | **Not used.** |
| **`mp4-atom`** (kixelated) — https://docs.rs/mp4-atom · https://github.com/kixelated/mp4-atom | MIT | Rust (0.17.x, 2026) | yes / yes | `Atom` trait `Decode/Encode`, `ReadFrom/WriteTo`, `Any::decode` enum, `Header::read_from` to skip `mdat`; unknown atoms kept raw; `tokio`/`serde` features; no validation | broad: ftyp/moov/…/stsd (avc1/hev1/hvc1/vp08/vp09/av01/mp4a/opus), avcC/hvcC/av1C/vpcC/esds/dOps, colr, pasp, btrt, edts/elst, sample tables, moof/traf/tfhd/tfdt/trun, mfra/tfra, sidx, styp, prft, emsg, emib/emeb, pssh/sinf/schm/tenc (senc/saiz/saio and mdcv/clli/dvcC coverage unconfirmed, OI-04), uuid | no (DIY) | good | partial | colr (others unconfirmed, OI-04) | active (used by moq-rs) | **Reference** for box list. |
| **mp4ff** (Eyevinn) — https://github.com/Eyevinn/mp4ff · https://pkg.go.dev/github.com/Eyevinn/mp4ff | MIT | Go (v0.5x–0.6x, pre-1.0) | gomobile / yes | `mp4.File{Init, Segments, Fragment}`; `DecodeFile` with `DecModeLazyMdat`; `Info(w, specificBoxLevels, indent)` printer (`mp4ff-info -l all`); `ReadData/CopyData` | broad: AVC/HEVC/AV1/VVC/VP8-9/AVS3/MJPEG; AAC/AC-3/E-AC-3/AC-4/Opus/IAMF/MPEG-H; WebVTT/TTML; colr, pasp, btrt, sinf/schm/schi/tenc, senc/saiz/saio/sbgp/sgpd(seig), pssh, emsg, prft, sidx, styp, mfra, timed metadata; SPS/PPS/SEI helpers (`sei` package parses HDR SEI); mdcv/clli not advertised (unconfirmed, OI-04) | `StartPos` on key containers (not on every box type, OI-04) | excellent | excellent | colr (others unconfirmed, OI-04) | very active | **Oracle** (`mp4ff-info -l all`, `mp4ff-pslister`, `mp4ff-nallister`, `mp4ff-encrypt/decrypt`, `mp4ff-crop`, `mp4ff-mvhevc`). |
| **go-mp4** (abema) — https://github.com/abema/go-mp4 | MIT | Go | gomobile / yes | declarative struct tags `mp4:"N,size=..."`; `ReadBoxStructure` callback walk with `BoxInfo{Offset, Size, HeaderSize, Type}`; `mp4tool dump`; `AddBoxDef` | broad (14496-12/-14/-30, 23001-7, 3GPP, AV1, Opus/FLAC, VP, AC-3/E-AC-3) | **yes** | good | yes | some | moderate (500+ stars) | **Reference**: cleanest "box tree with offsets" API and a model for OVT's schema-driven `BoxNode` design. |
| **mp4box.js** (GPAC) — https://github.com/gpac/mp4box.js · viewer https://gpac.github.io/mp4box.js/test/filereader.html | BSD-3-Clause | JS/TS (v1.x → TypeScript) | web only | box objects with `box.start`, `size`, `hdr_size` for every box; streaming `appendBuffer` API | broad incl. colr/mdcv/clli/cclv, codec configs, CENC, emsg/sidx/styp/prft, HEIF items | **yes** | good | yes | colr/mdcv/clli/cclv | active | **Oracle** (`BoxParser` tables = list of boxes/fields) in tests. **codem-isoboxer** (MIT, https://www.npmjs.com/package/codem-isoboxer): partial, low activity, not used. |
| **L-SMASH** — https://github.com/l-smash/l-smash | ISC | C | buildable | `boxdumper` (every field) | very complete (2016-era) | yes in dump | no | no | no | dormant since ~2018 | **Reference** for field completeness. |
| **MP4v2** — https://github.com/enzo1982/mp4v2 | MPL 1.1 | C/C++ | — | `mp4info`/`mp4file --dump` | limited modern boxes | — | — | — | — | light | **Not used.** |
| **mp4parser / isoviewer** (sannies) — https://github.com/sannies/mp4parser · https://github.com/sannies/isoviewer | Apache-2.0 | Java | yes / yes | `IsoFile` tree; ISO Viewer Swing GUI with box tree + hex | broad (2019-era) | yes | partial | yes | no | dormant since ~2019 | **Reference** (UI). |
| **DigiDNA ISOBMFF** — https://github.com/DigiDNA/ISOBMFF | MIT | C++ | buildable / yes | parser tree + `ISOBMFF-Dump`, custom-box registration | moderate (HEIF-centric) | partial | no | no | colr? | low commit rate | **Reference.** (Apple ships no FOSS ISOBMFF parser.) |
| **MediaInfo / MediaInfoLib** — https://mediaarea.net/en/MediaInfo · https://github.com/MediaArea/MediaInfoLib · https://mediaarea.net/MediaInfo/License | BSD-2-Clause | C++ | yes (official Android app) / yes | trace text only (`mediainfo --Details=1`, GUI Debug ▸ Advanced ▸ Trace): hierarchical parse of MP4/MKV/TS with **offset, size and every field** | very broad; reports HDR Format (HDR10, HDR10+, DV profile/level/compatibility from dvcC/dvvC, HLG), mastering display and CLL from boxes and SEI, colour description | yes in trace | yes | detect | yes | very active | **Oracle** (ADR-006) and stream-summary reference. |
| **libebml + libmatroska** — https://github.com/Matroska-Org/libebml | LGPL-2.1 | C++ | yes | element tree | per EBML schema | — | — | — | `Colour` master (MatrixCoefficients, MaxCLL, MaxFALL, MasteringMetadata), `BlockAdditionMapping` (DV RPU) | active | **Reference**; OVT generates its EBML reader from the schema (phase 4). |
| **mkvinfo** (MKVToolNix) — https://mkvtoolnix.download | GPL-2 | C++ | desktop | `mkvinfo -v -v -z` dumps every element with offsets/sizes and hex | yes | yes | — | — | yes | active | **Oracle** for the EBML tree. Rust `matroska-demuxer` (MIT/Apache), `ebml-iterable`/`webm-iterable` (MIT), Go `ebml-go` (Apache-2.0) are alternatives, not used. |
| **TSDuck** — https://github.com/tsduck/tsduck | BSD-2 | C++20 | Win/Linux/macOS/BSD, x86_64 & arm64; **no Android target** | `tsp` plugins, `tsanalyze`, `tstables`, `tsdump`; Python/Java bindings | PSI/SI descriptors (long tail) | — | — | — | — | single maintainer, active | **Oracle**; optional desktop wrapper for TR 101 290 (phase 4). libdvbpsi (LGPL) and Rust `mpeg2ts-reader` (MIT/Apache) noted, not used. |
| **DVB Inspector** — https://github.com/EricBerendsen/dvbinspector | GPL-3 | Java GUI | desktop | PSI/SI tree, bitrate graphs | yes | — | — | — | — | active | **Reference** for TS views. |
| **bmx** (BBC) — https://github.com/bbc/bmx | Apache-2.0 | C++ | — | `mxf2raw --info`; KLV tree + metadata sets | — | — | — | — | — | active | **Reference/oracle** for the later MXF reader. libMXF (BSD-3), FFmpeg mxf demuxer, MXFInspect (Windows, GPL) noted. |
| **dovi_tool / `dolby_vision` crate** — https://github.com/quietvoid/dovi_tool | MIT | Rust (+ C API `libdovi`) | all | — | DV RPU profiles 4/5/7/8; `info`, `export` (JSON/CSV); reads HEVC Annex B and MKV | — | — | — | RPU (NAL type 62 / T.35) | active, de-facto reference | **Oracle** for OVT's DV RPU field display (FFmpeg `dovi_rpu.c` is the in-process source, ADR-005). |
| **hdr10plus_tool / `hdr10plus` crate** — https://github.com/quietvoid/hdr10plus_tool | MIT | Rust | all | — | parses/validates ST 2094-40 SEI, JSON export | — | — | — | HDR10+ | active | **Oracle** for HDR10+ SEI parsing. |

**Registry and schema sources for the generated box tables (ADR-006, `tools/` generator):**

| Source | Content | Licence | URL |
|---|---|---|---|
| **MP4RA** | Registry of 282 ISO-family boxes + 42 `udta` codes + 16 QuickTime codes with code, description, specification, container list; also brands, codecs, handlers, item types, schemes. Data as CSV in the repo `CSV/` directory (`boxes.csv`, `boxes-udta.csv`, `boxes-qt.csv`, `brands.csv`, `handlers.csv`, `sample-entries.csv`, `sample-groups.csv`, `schemes.csv`, `track-references.csv`, `item-types.csv`; chapter 8 §8.2.10 keeps the list in `tools/mp4ra_sources.json`). Names only — no field syntax. MP4RA is the authority for names. | registry | https://mp4ra.org/registered-types/boxes · https://github.com/mp4ra/mp4ra.github.io |
| **MPEGGroup FileFormatConformance** (ISO/IEC 14496-32 framework) | `data/standard_features/<spec>/` JSON box definitions: fourcc, description, type (Box/FullBox/SampleEntry…), allowed containers, versions, flags, spec `syntax` text; covers 14496-12, -14, -15, -30, 23008-12; generated dictionary + search UI. CENC (23001-7) boxes: coverage unconfirmed (OI-04) → plan for hand additions. Conformance media via Git LFS. | BSD-3-Clause-Clear | https://github.com/MPEGGroup/FileFormatConformance · https://mpeggroup.github.io/FileFormatConformance |
| **MPEGGroup isobmff-kaitai-experiment** | Kaitai Struct `.ksy` for 14496-12 and -15 boxes; **archived read-only Oct 2026**. | — | https://github.com/MPEGGroup/isobmff-kaitai-experiment |
| **GPAC `MP4Box -boxes`** | XML of every GPAC-known box with containers and spec; independent cross-check of MP4RA. | LGPL output | https://github.com/gpac/gpac/wiki/mp4box-dump-opts |
| **mp4box.js `BoxParser` tables** | Second independent list of boxes and fields. | BSD-3 | https://github.com/gpac/mp4box.js |
| **EBML / Matroska schema** (`ebml_matroska.xml`, RFC 8794, IETF CELLAR) | Complete machine-readable element schema: ID, type, parent path, ranges, defaults → parser generation. | IETF | https://github.com/ietf-wg-cellar/matroska-specification |

**HDR/DV box and bitstream facts for the Container chapter.** Box level: `colr` (nclx primaries/transfer/matrix/full-range; `rICC`/`prof` ICC blobs), `mdcv` (ST 2086 primaries/white point/luminance), `clli` (MaxCLL/MaxFALL), `cclv`, `dvcC`/`dvvC`/`dvwC` (DV configuration record: version, profile, level, RPU/el/bl present flags, bl_signal_compatibility_id), sample-entry 4CCs `dvh1`/`dvhe`/`dva1`/`dvav`/`hvc1`+`dvvC`, `av1C` colour bits, `vpcC` colour fields, HEIF `colr` in `ipco`. All tiny fixed-layout boxes parsed by OVT itself. Bitstream level: ST 2086 / CLL / HDR10+ (ST 2094-40) / ST 2094-10 arrive as SEI (HEVC/AVC) or metadata OBUs (AV1); DV RPU is HEVC NAL type 62 / unregistered SEI / T.35. Vendor quirks corpus to keep: Apple `wave`, `glbl`, `chan`, Canon/GoPro/DJI `udta`, Samsung/Google motion-photo `uuid`, `moov` after `mdat`, 64-bit `co64`, `stco` overflow, size 0 = to-EOF, size 1 = largesize, QuickTime `wide`/`skip`/negative sizes. Secondary containers: IVF = 32-byte file header + 12-byte frame headers; Annex B / OBU splitters ≈ 100 lines.

The container survey's effort estimate for the own ISOBMFF reader (one experienced engineer, elapsed): core reader + offset/bit tracking + registry generator + ~40 essential boxes + tree/hex model 3–4 weeks; sample tables, edit lists, fragments/CMAF, CENC, HDR/DV, codec configuration records, HEIF items +4–6 weeks; derived timelines/validators, DASH/HLS virtual file, exports/CLI +3 weeks (≈ 3 months to Elecard level); EBML 2 weeks; TS + PSI/SI + TR 101 290 subset 3–4 weeks; IVF/ES 2–3 days; MXF 3–4 weeks. Roughly 80–120 boxes need hand-written field decoders; all others get generated shells (type, FullBox version/flags, container validation, spec reference, undecoded payload span).

## B.3 Metrics libraries

**libvmaf (Netflix)** — https://github.com/Netflix/vmaf · https://github.com/Netflix/vmaf/releases · https://github.com/Netflix/vmaf/blob/master/libvmaf/README.md · https://deepwiki.com/Netflix/vmaf/4.4-optimization-techniques

| Item | Fact |
|---|---|
| Licence | BSD-2-Clause-Patent (BSD+Patent) since 2020, explicit patent grant; models under the same licence. |
| Releases | v3.0.0 (Dec 2023) removed legacy `compute_vmaf()`, added `vmaf_cuda_*` (`vmaf_cuda.h`); v3.1.0 added `--aom_ctc v7.0`; v3.2.0 (June 2026) adds experimental batch threading / picture-pool modes, `motion_v2` feature, CAMBI `effective_eotf` and hi-res speed-up, `<2160p@1.5H>` CSF option, ships **VMAF v1.0.16 SDR models**; ffmpeg-quality-metrics notes v1 models require libvmaf ≥ 3.2.1. *(Informative; OVT pins libvmaf ≥ 3.0.0 and the `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1` models — D5; the v1 models are not relied upon by any requirement.)* |
| Metrics | VMAF (fusion of VIF ×4 scales, ADM/DLM, motion), VMAF NEG (enhancement-gain limits clamped to 1.0), CAMBI (full- or no-reference banding; PQ EOTF option; bit depths to 16; heatmap output), PSNR (dB clipping options, per plane), PSNR-HVS / PSNR-HVS-M (Daala port), SSIM, MS-SSIM, CIEDE2000 (cross-platform precision fixed in 2.2.0), AOM CTC presets `--aom_ctc v1.0 … v7.0`. |
| Models (v0 family) | `vmaf_v0.6.1` (default, 1080p at 3H), `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1` (2160p at 1.5H), `vmaf_b_v0.6.3` (bootstrapped, confidence intervals), phone transform (`--phone_model` / model-embedded `enable_transform` polynomial). |
| Models (v1) | VMAF v1 (blog "VMAF v1: Good Is Not Good Enough", June 2026, mirror https://hackernews-dynamic.seasondane.workers.dev/news/48612057) adds an **AIM (additive impairments)** component derived from ADM and generalises across viewing conditions (no separate phone polynomial). Full model list: check `resource/doc/models.md`. Not used by OVT (D5). |
| HDR | HDR-VMAF (with Dolby, 2021–23) **not open-sourced**, "tailored to internal pipelines" (https://csimagazine.com/csi/netflix-reveals-hdrvmaf-solution.php). v1 notes mention SDR only. SDR VMAF on PQ 10-bit runs but is not validated. |
| Bit depth / formats | 8, 10, 12, 16-bit planar YUV (`-b`), 4:2:0/4:2:2/4:4:4 and 4:0:0 (since 2.3.0). Feature extractors are luma-only except CIEDE2000 and chroma PSNR variants. Mixed depth: lower input promoted internally. |
| API | `vmaf_init`, `vmaf_use_features_from_model`, `vmaf_use_feature`, `vmaf_read_pictures` (ref/dist pairs, `vmaf_picture` arbitrary stride), `vmaf_score_at_index`, `vmaf_score_pooled` (mean / min / harmonic mean), `vmaf_feature_score_*`, `vmaf_write_output` (XML/JSON/CSV/SUB); `VmafConfiguration.n_threads`, `n_subsample`. CUDA path needs nvcc + NVIDIA (irrelevant on macOS/Android). |
| SIMD | Runtime-dispatched x86 AVX2/AVX-512; fixed-point integer VIF/ADM (≈2× over float); arm64 NEON at least for VIF (`vif_neon.c`), ADM/motion NEON partial. Arch Linux ARM builds 3.2.0 on armv8. |
| Build | Meson/Ninja; NASM only on x86; macOS (Homebrew), Linux, Windows clean; Android via Meson cross-files (no official NDK file); `-Dbuilt_in_models` compiles models into the library (useful on Android). Use the fixed-point default build, not `-Denable_float`, for CLI parity. |
| Reproducibility rule | To match `vmaf --reference … --distorted … --model version=vmaf_v0.6.1 --feature psnr --json` bit-for-bit: same planes (no extra conversion), same bit depth, same `n_subsample`, same model JSON, same pooling, same libvmaf version; FFmpeg `libvmaf` differs when an auto `scale`/`format` is inserted. Golden tests use `python/test/resource` vectors. |
| Performance | Scales to ~4–8 threads per instance; VMAF 1080p ≈ 100–200 fps on an M-series laptop (report estimate); 1080p in the tens of fps on flagship Android SoCs; ARMv8.2 dotprod/i8mm help integer VIF. |
| vmaf-torch | https://pypi.org/project/vmaf-torch · https://arxiv.org/abs/2310.15578 — unofficial PyTorch port with NEG, CUDA, `.compile()`; mean abs diff vs libvmaf 0.0063, max 0.027 on the Netflix public dataset; licence not stated on PyPI. Sidecar-only. |

OVT use: **dependency** (ADR-011 libvmaf host; `third_party/libvmaf.cmake`, Meson).

**FFmpeg libavfilter metric filters** (LGPL-2.1+ core; GPL with `--enable-gpl`). OVT use: **oracle only** (ADR-011 rejects FFmpeg filters as the engine because of framesync frame duplication on rate mismatch, timestamp-driven pairing, GPL coupling, and no per-block maps).

| Filter | What | Notes |
|---|---|---|
| `psnr` | per-plane PSNR + average, MSE, stats file, `output_max` | any planar 8/10/12/16-bit; `psnr_avg` is the mean of planes in linear MSE, not JVET-weighted |
| `ssim` | per-plane SSIM (8×8 block, Gaussian-free) | not identical to Wang et al. 11×11 Gaussian; differs from libvmaf SSIM |
| `libvmaf` | wrapper around libvmaf | `model=version=vmaf_v0.6.1:feature=name=psnr\|cambi`; `n_threads`, `n_subsample`, `log_path`, `log_fmt`, `pool` |
| `xpsnr` | eXtended PSNR (Fraunhofer HHI) | in FFmpeg since 7.1 (`vf_xpsnr.c`, © 2024 Helmrich/Lehmann/Stoffers, LGPL 2.1+, bit depths 6–16, 1–3 planes; https://ffmpeg.org/doxygen/7.1/vf__xpsnr_8c_source.html); single option `stats_file` |
| `identity` | per-plane fraction of identical pixels | lossless verification |
| `msad` | mean sum of absolute differences | sanity metric |
| `vif` | pixel-domain VIF (4 scales, luma) | 8-bit only historically (check pix_fmt list) |
| `ssim360` | SSIM with spherical sampling | Facebook contribution |
| `siti` | Spatial/Temporal Information (ITU-T P.910) | no-reference; added 2022 (https://ffmpeg.org/pipermail/ffmpeg-devel/2022-January/291775.html); implements the pre-2023 definition; `siti-tools` (Robitza) implements P.910 2023 |
| `blockdetect`, `blurdetect` | no-reference blockiness / blur | stream health |
| `scale`, `zscale`, `colorspace`, `format` | pre-processing | `zscale` (zimg) is the only one handling PQ/HLG transfer + BT.2020 + range + dithering correctly |

**XPSNR** — reference plugin https://github.com/fraunhoferhhi/xpsnr (custom Fraunhofer BSD-3-like licence with explicit **"no patent rights"** clause; marked "kept for reference, not maintained"); canonical LGPL version is FFmpeg's. Paper: Helmrich et al., ITU Journal 2020, https://www.itu.int/dms_pub/itu-s/opb/journal/S-JOURNAL-ICTS.V3I1-2020-8-PDF-E.pdf. Weights per-block (N×N, N from resolution) squared error by spatial (high-pass) and temporal activity; per-plane XPSNR and weighted combination; supports 8-bit ref vs 10-bit test. OVT: own port (~600-line filter) to avoid libavfilter; document the patent caveat.

**VQMT (EPFL MMSPG)** — https://github.com/rolinh/VQMT · https://epfl.ch/labs/mmspg/?p=846. C++/OpenCV, 8-bit raw YUV only; PSNR, SSIM, MS-SSIM, VIFp, PSNR-HVS, PSNR-HVS-M. **Research-only licence, no commercial distribution — not FOSS.** OVT: oracle in CI only; never link or vendor.

**Daala tools (Xiph)** — https://gitlab.xiph.org/xiph/daala (`tools/`): `dump_psnr`, `dump_psnrhvs`, `dump_ssim`, `dump_msssim`, `dump_fastssim`, `dump_ciede2000` (BSD-2). Historical reference for PSNR-HVS-M and the CIEDE2000 variant used by AWCY; libvmaf's `psnr_hvs` and `ciede` were ported from them. Still built by AWCY workers. OVT: oracle.

**tiny_ssim / x264 `ssim.c`** — `tools/tiny_ssim.c` in libvpx (BSD) and x264 (GPL); same algorithm in libaom `aom_dsp/ssim.c`. Fast 4×4-block SSIM that encoders report with `--tune ssim`/`--psnr`; does **not** match FFmpeg's or libvmaf's SSIM. OVT: own ~100-line implementation (`ssim_x264` descriptor id, B.5.7) so encoder console output can be reproduced.

**AWCY (arewecompressedyet)** — https://github.com/xiph/awcy (MIT). Metric set per encode via libvmaf `--aom_ctc` presets and Daala tools: PSNR (Y,U,V), PSNR-HVS, SSIM, MS-SSIM, CIEDE2000, VMAF, VMAF-NEG, APSNR (average PSNR from mean MSE), encoding time. BD-rate via `bd_rate_jm.py` and `bd_rate_report.py`; libaom convex-hull framework `tools/convexhull_framework/src/CalcBDRate.py` (https://aomedia.googlesource.com/aom/+/0ad65f40d495fe2d1ba7be8f4cbc396e82db2eb5/tools/convexhull_framework/src/CalcBDRate.py). OVT: reference; libvmaf-compatible exports keep AWCY tooling consumable.

**bjontegaard (FAU-LMS)** — https://github.com/FAU-LMS/bjontegaard · https://pypi.org/project/bjontegaard (BSD-3). `bd_rate()`/`bd_psnr()` with `method='cubic' | 'pchip' | 'akima'`; PCHIP reproduces the JVET Excel sheet to ≥ 10 decimals; authors recommend Akima as more stable; MATLAB twin FAU-LMS/bjontegaard-matlab. Definitions OVT must implement and expose explicitly: classic Bjøntegaard (VCEG-M33) 3rd-order polynomial fit of log(rate) vs PSNR over 4 points integrated over the overlapping PSNR range; JCT-VC/JVET CTC piecewise cubic Hermite (PCHIP); Y, U, V separately plus weighted YUV PSNR = (6·PSNR_Y + PSNR_U + PSNR_V)/8; AOM CTC v2+ BD-rate on VMAF, VMAF-NEG, PSNR-HVS, SSIM, MS-SSIM, CIEDE2000, APSNR, with 6-point (or 4-point) overlap and "clip to overlap" semantics. OVT: own port unit-tested against the FAU package (phase 3 `ovt bdrate`).

**ffmpeg-quality-metrics** — https://github.com/slhck/ffmpeg-quality-metrics (MIT). Drives FFmpeg for PSNR, SSIM, VMAF (v0 & v1 models with CAMBI/PSNR-HVS/CIEDE2000 features), VIF, MSAD; scales distorted to reference (bicubic default); `--dist-delay` frame-offset trimming; `--vmaf-10bit` promotion; JSON/CSV with global mean/min/max/std; Plotly Dash viewer. OVT: CLI-ergonomics reference only (too slow as an engine).

**VQEG and standardised FR metrics** — https://www.vqeg.org; `vqeg/software-tools` (`siti-tools`, `pyvqm`). ITU-T J.144 / J.247 / J.341 metrics (NTIA VQM, PEVQ, Opticom) are proprietary or export-controlled; NTIA VQM public MATLAB release (https://its.ntia.gov/software/) under a US-government licence, impractical to port. ITU-T P.1204.3 bitstream-based reference (`bitstream_mode3_p1204_3`, MIT) is no-reference, out of scope. OVT: not used.

**HDR metrics**

| Metric | Reference implementation | Licence | Practicality / OVT use |
|---|---|---|---|
| HDR-VDP-2 / HDR-VDP-3 (Mantiuk et al.) | https://sourceforge.net/p/hdrvdp/ , v3.0.7 (2023-05), MATLAB or Octave; paper https://arxiv.org/abs/2304.13625 | GPLv2 | Image metric, seconds per frame in Octave; needs display model (absolute luminance); no C/Python port. **Sidecar only** (ADR-011). |
| PU21 encoding → PU-PSNR / PU-SSIM / PU-MS-SSIM | https://github.com/gfxdisp/pu21 (MATLAB; "use only the `banding_glare` variant"); paper https://www.cl.cam.ac.uk/~rkm38/pdfs/mantiuk2021_PU21.pdf; single closed-form equation over absolute luminance 0.005–10 000 cd/m² | MIT (per repo LICENSE) | Trivial to port: PQ/HLG code values → cd/m² via display model → PU21 → ordinary PSNR/SSIM. **Own port** (`pu21` WorkingDomain). ColorVideoVDP ships a PU-PSNR in Python. |
| ΔE-ITP (ITU-R BT.2124) | No canonical FOSS lib; ICtCp from BT.2100; ΔE = 720·√(ΔI² + 0.25·ΔCt² + ΔCp²) | spec | Easy; needs linear RGB → LMS → PQ → ICtCp; libplacebo and zimg ICtCp conversions as reference. **Own implementation** (`de_itp`, `ICtCp` domain). |
| wPSNR, PSNR-L100, DE100, PSNR in PQ (JVET HDR CTC, JVET-T2011 / JCTVC-Z1020) | VTM/HM `--PrintHdrMetrics` / `--PrintWPSNR` | BSD-3 (VTM) | Per-pixel MSE weight from local luma lookup; CIEDE2000 on linear light at 100 nits. **Own port from VTM** (small). |
| HDR-VQM (Narwaria et al.) | MATLAB paper supplement | unclear | Research only; not used. |
| ColorVideoVDP (cvvdp) | https://github.com/gfxdisp/ColorVideoVDP · https://arxiv.org/abs/2401.11485 — Python/PyTorch, CUDA or Apple MPS (default on Apple Silicon since v0.5.6); PQ/HLG with display models, EXR linear, raw YUV 8/10-bit; outputs JOD score, heatmap, distogram; also PU-PSNR | MIT | Best open FR HDR + colour video metric; CPU "much longer"; no Android; macOS via MPS; Linux needs CUDA. **Sidecar** (phase 4). |
| FovVideoVDP | https://github.com/gfxdisp/FovVideoVDP (PyTorch/MATLAB) | MIT | Foveated predecessor, superseded by cvvdp. Sidecar only. |
| SSIMPLUS (SSIMWAVE/IMAX) | closed | — | Not available. |
| HDR-VMAF | Netflix internal | — | Not released; watch the vmaf repo. |

Domain conventions recorded for Chapter 9: JVET CTC computes PSNR on **PQ-coded Y'CbCr code values** (optionally wPSNR); PU21 metrics convert to absolute linear light then the PU domain (defensible for PQ-vs-HLG comparison); PSNR in linear light is universally discouraged; HLG needs a system gamma / peak luminance assumption (BT.2100 OOTF) before absolute-luminance metrics. OVT's `WorkingDomain` enum (B.5.7) covers `CodeValue` (default), `LinearLight`, `PU21`, `ICtCp`, `ModelRGB`.

**UVQ — Google Universal Video Quality (no-reference)** — https://github.com/google/uvq (Apache-2.0); original TF README mirror https://huggingface.co/spaces/ychenhq/VideoCrafterXtend/raw/c6204515d60211b445d18ba599d59e059b7c7e79/uvq/README.md; third-party re-implementation https://cutcutcodec.readthedocs.io/latest/build/api/cutcutcodec.core.analysis.video.quality.uvq_google.html; dataset https://media.withyoutube.com.

| Item | Fact |
|---|---|
| Paper | Wang, Ke, Talebi, Yim, Birkbeck, Adsumilli, Milanfar, Yang, "Rich features for perceptual quality assessment of UGC videos", CVPR 2021; Google AI Blog 2022. Trained/validated on YouTube-UGC. |
| Architecture (v1.0) | ContentNet (3 862 YouTube-8M labels), CompressionNet (16 levels on a 4×4 grid of sub-regions; temporal, short frame stack per 1 s chunk), DistortionNet (26 KADID-10k distortion types on a 2×2 grid), AggregationNet (concatenated features → MOS). Feature dump 25 600 floats per 1-second chunk. EfficientNet-style CNNs. Original geometry (1280×720 rescale, 5 frames per 1 s chunk) **to be confirmed from code** before porting. |
| Output | MOS-scale score (≈1–5; README examples 3.88 for v1.5, 4.06 for v1.0) for seven feature combinations; `compression_content_distortion` is the official predictor. Per-chunk labels (compression level map, distortion type map, content labels) usable as overlays. |
| Generations | UVQ 1.0 (2021–22) TensorFlow 2 (`uvq_main.py --model_dir models`, Python ≥ 3.6, TF unpinned). Current repo is **PyTorch**: `uvq_pytorch/` (1.0 re-implementation) and `uvq1p5_pytorch/` (UVQ 1.5); CLI `python uvq_inference.py <video> --model_version 1.5 --fps 1 --device cuda|cpu --threads N --batch_size --transpose`; FFmpeg/ffprobe on PATH; Python 3.13 tested; CPU ≈ 13.8 s for a 20 s 1080p clip on 8 cores. Default sampling 1 fps (`--fps -1` = all frames). **No TFLite/ONNX artefacts published.** |
| Feasibility | macOS: the published UVQ models are TensorFlow SavedModels → **ONNX** via `tf2onnx` (D11; the PyTorch re-implementation in the current repository is not OVT's export source) → ONNX Runtime (CoreML EP). Android: ONNX Runtime Mobile with the XNNPACK EP (NNAPI is deprecated from API 35; OVT uses XNNPACK — D11, chapter 9 §9.6) or onnx2tf → LiteRT (not chosen); quantise fp16/int8 after checking drift on a YouTube-UGC subset; expect a few hundred ms per 1 s chunk at 1 fps on a flagship SoC. Weights Apache-2.0 (redistributable); tens of MB per sub-net → downloadable asset for the APK. |
| Preprocessing OVT owns | decode → sample at configured fps → rotate per `--transpose`/display matrix → resize to model geometry with the reference's interpolation (bilinear via FFmpeg `scale`) → RGB float normalisation → 1 s chunks → sub-nets → aggregate. Golden tests vs reference CLI on YouTube-UGC clips (phase 3 exit: within 0.01 MOS). |
| Caveats | UGC MOS model, not a codec-comparison metric; not comparable to VMAF 0–100; report on its own axis; opt-in in BD-rate. |

OVT use: **dependency** via ONNX Runtime host (ADR-011); Python sidecar as fallback.

**Other metric building blocks**: **zimg** (WTFPL) for colour/range/transfer conversion with `error_diffusion` dithering and float intermediates — **dependency** (normalisation stage). Alignment, scaling, chroma-siting, bit-depth-promotion, frame-rate and pooling policies from the metrics survey are specified normatively in Chapter 9; the facts to retain here: a full-range dist vs limited-range ref drops PSNR by ~10 dB; VMAF with bicubic vs Lanczos upscaling differs by ~1–2 points; SSIM constants C1/C2 scale with (2^b − 1)²; VMAF's motion feature is frame-rate dependent; libvmaf exposes CAMBI heatmaps but no VMAF per-block maps (fusion model not valid on tiles).

## B.4 UI toolkits and graphics/HDR facts

**Toolkits** (ADR-004 selected Qt 6; the rest are recorded as rejected alternatives with the facts that decided it).

| Toolkit | HDR surface | Android / ChromeOS | Large-data widgets | Licence | Key facts and sources | OVT |
|---|---|---|---|---|---|---|
| **Qt 6** (Qt Quick/QML, QRhi) | `QRhiSwapChain::Format`: `SDR`, `HDRExtendedSrgbLinear` (FP16 scRGB, "recommended for desktop HDR"), `HDR10` (RGB10A2 BT.2020 PQ), `HDRExtendedDisplayP3Linear` (FP16; iOS/visionOS); `isFormatSupported()` per screen; `hdrInfo()` luminance/headroom. **Swapchain-level only**: the Qt Quick scene graph is SDR-unaware (no SDR-white scaling); feature requests QTBUG-119193 / QTBUG-134590 open. Metal backend sets `CAMetalLayer.wantsExtendedDynamicRangeContent` + `CGColorSpace`. Wayland `color-management-v1` client merged 2025-11-07 (Qt 6.11 dev, cherry-picked to 6.10.1). Android HDR via Qt backends **not plumbed** → native `SurfaceView` sibling (ADR-014). | Qt 6.8 LTS: Android 9–16 (API 28–36), ABIs arm64-v8a/x86_64/x86/armeabi-v7a, Clang 17, NDK r26b documented (OVT pins r27c, chapter 13 §13.1), JDK 17, Gradle 8.10/AGP 8.6.0 with Qt Gradle Plugin 1.2 for Qt 6.8.3 (chapter 13 §13.1/§13.4; Qt 6.9+ templates move to Gradle 8.14/AGP 8.10); multi-ABI only with CMake; Android Studio plugin for `QtQuickView` embedding; Fragments cannot host Qt. ChromeOS **not supported by Qt** (June 2024: no CI plans; bugs fixed only if reproducible on plain Android; apps run "with some rough edges"). | `QTreeView`/QML `TreeView` (6.3+) model-virtualised, 100k+ nodes with `fetchMore()`; `QSortFilterProxyModel` recursive filtering is O(n) per keystroke → filter on a background thread. QHexView (MIT, Qt 5/6, `QMappedFileBuffer` mmap, range highlighting, comments). Plots: QCustomPlot (GPLv3/commercial, ~1M points with adaptive sampling), Qt Graphs (GPL for OSS), custom `QQuickRhiItem` instanced lines (6.7+). Distance-field glyph cache handles thousands of labels; `QPainter` text at 50k labels/frame is CPU-bound. Docking: KDDockWidgets (BSD/GPL dual). | LGPL-3 most modules; GPL-3-only for OSS users: Qt Charts, Qt Graphs, Qt Data Visualization, Qt Virtual Keyboard, Qt Wayland Compositor; static linking "may mean the application is no longer a work that uses the library". No Qt module is AGPL. | https://doc.qt.io/qt-6/qrhiswapchain.html · https://www.phoronix.com/news/Qt-color-management-v1 · https://doc.qt.io/qt-6.8/android.html · https://www.qt.io/licensing/open-source-lgpl-obligations · https://bugreports.qt.io/browse/QTBUG-134590 · https://bugreports.qt.io/browse/QTBUG-119193 · https://lists.qt-project.org/pipermail/interest/2024-June/039755.html · https://github.com/Dax89/QHexView | **Selected** (ADR-004). GUI binary GPL-3 unlocks Qt Graphs / QCustomPlot. |
| **Flutter (Impeller)** | No HDR surface; FP16 wide-gamut path clamped to P3 at SDR luminance (engine PRs #41965, #46051 are wide gamut, not HDR). HDR only through platform views: `video_player_hdr` v1.2.0 (Aug 2026) supports Android and iOS only, SDR fallback on macOS. Platform views on macOS "aren't fully functional", no gesture support, run on the platform thread; **no Linux platform views**; Android HCPP experimental since 3.44 (API 34 + Impeller Vulkan, transparency bugs). | Good mobile; desktop stable (3.47, 2026-08-12, Impeller default on Windows/Linux/macOS; P3 default on macOS). | `ListView.builder`; hex/tree only as small community packages; `fl_chart`, syncfusion (commercial). | BSD | https://startdebugging.net/de/2026/08/flutter-3-47-impeller-default-renderer-on-desktop/ · https://github.com/flutter/engine/pull/46051 · https://pub.dev/packages/video_player_hdr · https://docs.flutter.dev/platform-integration/macos/platform-views · https://docs.flutter.dev/platform-integration/platform-views | Rejected. |
| **Dear ImGui + SDL3** | SDL3 window props `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, `SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT` (1.0 on Apple EDR; scRGB SDR white on Windows), `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`, event `SDL_EVENT_WINDOW_HDR_STATE_CHANGED`; GPU `SDL_GPUSwapchainComposition` `SDR`, `SDR_LINEAR`, `HDR_EXTENDED_LINEAR` (FP16 scRGB), `HDR10_ST2084` (A2R10G10B10 PQ) gated by `SDL_WindowSupportsGPUSwapchainComposition`. Works on Windows (DXGI), macOS (Metal EDR); Wayland `color-management-v1` client in recent 3.2.x; Android depends on Vulkan driver. ImGui renders into FP16/PQ trivially (scale UI colours to SDR white in shader; mpv, DuckStation precedent). | SDL3 provides `NativeActivity` glue; mobile UX poor (touch, IME). | `ImGuiListClipper`, ImPlot (MIT, millions of points, linked axes, pixel picking), `imgui_memory_editor` (imgui_club, MIT); no accessibility, no native dialogs/menus (NFD / portable-file-dialogs). | MIT / zlib | https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties · https://wiki.libsdl.org/SDL3/SDL_GPUSwapchainComposition · https://github.com/ocornut/imgui/wiki/Useful-Extensions · https://github.com/epezent/implot | Rejected as product UI; a possible "lab" front-end over the toolkit-free `VideoSurface` (B.6.4). |
| **Slint** | None; no custom-GPU-item API. | Android port Rust-only (NLnet grant 2023-06 → 2025-06). | None (no tree/hex). | GPLv3 / royalty-free / commercial | https://docs.slint.dev/latest/docs/slint/guide/platforms/android · https://nlnet.nl/project/SlintAndroid/ | Rejected. |
| **egui/eframe + wgpu** | wgpu `SurfaceColorSpace`: `Srgb`, `ExtendedSrgbLinear`, `ExtendedSrgb`, `DisplayP3`, `ExtendedDisplayP3`, `Bt2100Pq` (RGB10A2), `Bt2100Hlg`; `DisplayHdrInfo`; `Auto` never selects HDR; wgpu does not tone-map or gamut-map. Backends Metal (EDR), DX12, Vulkan (Linux Wayland via Mesa ≥ 25.1); GL/WebGPU SDR. egui has **no HDR mode** (`egui_wgpu::preferred_framebuffer_format` picks 8-bit sRGB; dithering PR #4497: "HDR output would need separate work"); iced has the same gap. | `winit` + `android-activity` (GameActivity/NativeActivity); IME, accessibility, lifecycle weak. | `egui_plot`, `ScrollArea::show_rows`; no mature hex/tree. FFI via `bindgen`; `rsmpeg` (FFmpeg 6/7/8), `ffmpeg-next` maintenance-only. | MIT/Apache | https://wgpu.rs/doc/src/wgpu/documentation/color/hdr_surfaces.rs.html · https://github.com/emilk/egui/pull/4497 · https://discourse.iced.rs/t/wide-gamut-and-hdr-support/456 · https://dev.to/yeauty/the-state-of-media-processing-in-rust-2026-what-each-crate-actually-covers-2k2c | Rejected (documented fallback stack). |
| **Web (Tauri/Electron, WebGPU, Canvas)** | Chrome 129 WebGPU HDR: `GPUCanvasConfiguration.toneMapping.mode = "extended"` with `rgba16float` on desktop, Android, WebView; **ChromeOS excluded**; WebGL HDR "planned later"; no PQ/HLG output or headroom query; Canvas 2D HDR headroom at Intent-to-Prototype (July 2025). WebKit WebGPU HDR behind preview flag (default in Safari 26.2 per Mozilla tracking); Canvas2D/WebGL unsupported. Firefox HDR meta-bug NEW. Tauri WebView: WebKitGTK on Linux (no HDR, no WebGPU default). Frame transport native → WebView is a bandwidth/latency liability at 4K 10-bit. | — | Virtualised DOM libs, Monaco-style hex, uPlot/ECharts. | — | https://developer.chrome.com/blog/new-in-webgpu-129 · https://groups.google.com/a/chromium.org/g/blink-dev/c/rBQIRHUEAe8 · https://groups.google.com/a/chromium.org/g/blink-dev/c/S7jaeH2J7Rw · https://bugs.webkit.org/show_bug.cgi?id=276502 · https://bugzilla.mozilla.org/show_bug.cgi?id=HDR | Rejected (web is a non-target). |
| **GTK4** | GTK 4.16 Vulkan default on Wayland; `color-management-v1` client **disabled by default** after GTK4 apps appeared "super bright" on Plasma HDR (KDE bug 495647; workaround `GDK_DISABLE=color-mgmt`). Linux-first; macOS second-class; no Android. | — | — | LGPL-2.1 | https://bugs.kde.org/show_bug.cgi?id=495647 · https://pete.lostsource.net/posts/2026-07-09-the-pipe-is-open-nobody-turned-on-the-water.html | Rejected. |
| **Avalonia (.NET)** | Skia (SkiaSharp 2.88; 3.0 migration issue #15503) 8-bit surfaces; no HDR/10-bit, no plan. | Android supported; .NET runtime size. | — | MIT | https://github.com/AvaloniaUI/Avalonia/issues/15503 | Rejected. |
| **Kotlin/Compose Multiplatform** | Skiko (Skia) on Metal/OpenGL/D3D, experimental Graphite; no HDR/wide-gamut API; 1.8 iOS stable, 1.9 web beta. Android HDR via `AndroidView`-hosted `SurfaceView`. Desktop on JVM (GC pauses); two FFI mechanisms (JNI/Panama vs cinterop). | native Android | — | Apache-2.0 | https://www.neowin.net/amp/compose-multiplatform-19-lands-pushing-web-target-to-beta/ | Rejected. |
| **Native per platform** (Compose + SwiftUI + GTK/Qt) | SwiftUI/AppKit alone has the best HDR story (CAMetalLayer EDR, `CAEDRMetadata.hlg/.hdr10`) but excludes Linux/Android. | — | — | — | https://developer.apple.com/documentation/quartzcore/caedrmetadata | Rejected (60–70 % of front-end code written three times). |

**Per-platform HDR API facts** (normative consequences are in ADR-005/014 and Chapter 10).

| Platform | Facts | Sources |
|---|---|---|
| **macOS** | `CAMetalLayer.wantsExtendedDynamicRangeContent = true`; `pixelFormat = .bgr10a2Unorm` (or `.rgba16Float` for linear scRGB); `colorspace = CGColorSpace(name: .itur_2100_PQ)` / `.itur_2100_HLG` / `.extendedLinearSRGB`. Headroom: `NSScreen.maximumExtendedDynamicRangeColorComponentValue` (iOS: `UIScreen.currentEDRHeadroom`); `CAMetalLayer.edrMetadata` (`CAEDRMetadata.hdr10(minLuminance:maxLuminance:opticalOutputScale:)`, `.hlg`) feeds the system tone mapper; "Performing your own tone mapping" = render linear scRGB with EDR headroom. Graceful SDR fallback (headroom 1.0). MoltenVK presents `VK_COLOR_SPACE_HDR10_ST2084_EXT` / `EXTENDED_SRGB_LINEAR_EXT` on CAMetalLayer (mpv/libplacebo path). Prefer FP16 scRGB linear output on macOS (compositor owns final OOTF; SDR white = 1.0). | https://developer.apple.com/documentation/metal/using-color-spaces-to-display-hdr-content · https://developer.apple.com/documentation/metal/performing-your-own-tone-mapping · https://developer.apple.com/documentation/quartzcore/caedrmetadata · https://github.com/mpv-player/mpv/discussions/14138 |
| **Linux** | `color-management-v1` merged into wayland-protocols 1.41 (Feb 2025, after ~5 years). Mesa 25.1 Vulkan WSI exposes `VK_EXT_swapchain_colorspace` spaces and `VK_EXT_hdr_metadata` when the compositor supports the protocol. Compositors: KWin (Plasma 6.0 first, now v1), Mutter (GNOME 48), wlroots (March 2025; Sway 1.12 HDR10 on Vulkan renderer), Hyprland partial (issue #4933 open). Clients: mpv 0.41 (2025-12-21; `color-management-v1` v2 + `color-representation-v1`; gpu-next default), Qt 6.10.1/6.11, SDL3; GTK disabled by default. NVIDIA proprietary driver does not use Mesa WSI and lagged. **X11/Xwayland: no HDR by design. OpenGL: no HDR path.** Requirement: Vulkan, Wayland, Plasma ≥ 6.0 or GNOME ≥ 48, Mesa ≥ 25.1; fall back to tone-mapped SDR when `vkGetPhysicalDeviceSurfaceFormatsKHR` returns only `SRGB_NONLINEAR`. Prefer RGB10A2 PQ with static HDR10 metadata via `VK_EXT_hdr_metadata` / Wayland `image_description`. | https://www.phoronix.com/news/Wayland-CM-HDR-Merged · https://www.phoronix.com/news/Mesa-Vulkan-WSI-HDR-CM · https://www.phoronix.com/news/GNOME-wp_color_management_v1 · https://github.com/hyprwm/Hyprland/issues/4933 · https://github.com/mpv-player/mpv/releases/tag/v0.41.0 · https://linuxiac.com/mpv-0-41-media-player-switches-to-gpu-next-renderer/ · https://forums.developer.nvidia.com/t/vulkan-extensions-needed-for-hdr-is-missing/334268 · https://bugs.kde.org/show_bug.cgi?id=487856 |
| **Android** | API ≥ 24 for HDR playback; HLG10 mandatory for HDR-capable devices since Android 13; `Display.getHdrCapabilities()` (HDR10, HDR10+, HLG, Dolby Vision + luminance). Render to **`SurfaceView`**; `TextureView` on API ≥ 33 "transcodes HDR to SDR, which can clip colors and cause banding". Native: `ANativeWindow_setBuffersDataSpace(window, ADATASPACE_BT2020_PQ | ADATASPACE_BT2020_HLG)` on `AHardwareBuffer`/EGL surface; SurfaceFlinger composites as HDR; attach SMPTE 2086 + CTA-861.3 metadata. Android 14: `Display.getHdrSdrRatio`; Android 15: `Window.setDesiredHdrHeadroom` / `SurfaceView.setDesiredHdrHeadroom` (requires `COLOR_MODE_HDR`); Google (Sept 2025) recommends ~2× headroom for mixed HDR/SDR scenes, 5–8× for full-screen HDR, warns SDR UI looks grey next to HDR. Wide gamut via `EGL_GL_COLORSPACE_DISPLAY_P3_EXT`, `VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT` / `EXTENDED_SRGB_LINEAR_EXT` with FP16 or RGB10A2. **Vulkan `VK_COLOR_SPACE_HDR10_ST2084_EXT` not reliably exposed** (Arm: depends on GPU + display-controller integration; Galaxy S20 exposed only `SRGB_NONLINEAR`, Note 9 more) → EGL/GLES or Vulkan-to-`AHardwareBuffer` + explicit dataspace (mpv-android `vo_gpu` path). Screenshots are SDR-tone-mapped; Ultra HDR is an image format (exports only). MediaCodec via FFmpeg only through NDK (`--enable-mediacodec --enable-jni`); use `AMediaCodec` output buffers (not `Surface`) when pixel access is needed. | https://developer.android.com/media/grow/hdr-playback · https://android-developers.googleblog.com/2025/09/hdr-and-user-interfaces.html · https://developer.android.com/training/wide-color-gamut · https://source.android.com/docs/core/graphics/hdr-screenshots · https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/54708/vulkan-android-mali-gpu-drivers-do-not-expose-other-color-spaces-on-android-devices-with-hdr10-hdr10-display/181934 |
| **ChromeOS (Android app)** | ChromeOS has had HDR output to HDR external monitors over USB-C and on a few 10-bit internal panels since ~ChromeOS 85/86 (2020) for Chrome's own video path. Android apps run in ARCVM (virtio-gpu/Venus Vulkan); SurfaceFlinger buffers are composited by Exo/Ash, not the Android HWC. Google's ChromeOS Android guidance covers windowing, input, displays and says **nothing about HDR**; no public document confirms a PQ `SurfaceView` passes through as HDR → **unverified**, week-one hardware spike (ADR-014). Certain: ship **x86_64 and arm64-v8a** ABIs (most Chromebooks are Intel/AMD); free-form resizable windows with keyboard and mouse (also Android 15 QPR1 / Android 16 QPR1 desktop windowing on tablets) → desktop idioms required (state preservation on resize, hover, right-click, scroll wheel, Ctrl shortcuts, multi-pane, external displays); Google has stated ChromeOS is moving onto the Android stack, so the Android app is the long-term Chromebook deliverable (not Crostini). | https://chromeunboxed.com/hdr-chromebooks-chrome-os-displays-bright-contrast-external-monitor/ · https://developer.android.com/develop/adaptive-apps/guides/chromeos/build-adaptive-apps-for-chromeos · https://liliputing.com/google-is-bringing-desktop-windowing-to-android-tablet/ |
| **Windows (non-target)** | Qt `HDRExtendedSrgbLinear` and SDL3 support DXGI/Vulkan HDR; nothing in the architecture precludes a later port. | — |
| **WebGPU / OpenGL** | WebGPU "extended" = scRGB-like, no PQ/HLG, no headroom. OpenGL: no HDR swapchain on any desktop platform (EGL colourspace extensions on Android only) → GL/GLES is the SDR fallback renderer. | see above |

**Cross-platform abstraction layers**

| Layer | HDR surfaces | Platforms | Licence | Notes | OVT |
|---|---|---|---|---|---|
| **libplacebo** (C) — https://github.com/haasn/libplacebo | Renders into any `pl_swapchain`; Vulkan (incl. MoltenVK), OpenGL/GLES 3.0, D3D11; PQ/HLG/scRGB output with tone/gamut mapping | Win/mac/Linux/Android (mpv-android, VLC) | LGPL-2.1+ | `pl_renderer` from `pl_frame` with per-plane `pl_plane_data`, `pl_color_repr`/`pl_color_space`; dynamic tone mapping with scene histogram and scene-change detection (`spline` default, `PL_TONE_MAP_BT2390`, BT.2446a, Reinhard/Hable), gamut mapping (`perceptual`, `relative`, `saturation`, `absolute`, `desaturate`, `darken`, `clip`), ICC/BT.1886, 3D LUTs, `pl_dither_params` (blue-noise / error diffusion), Dolby Vision P5/P8 → PQ/SDR reshaping, film grain, custom shaders, HDR10+ (`AV_FRAME_DATA_DYNAMIC_HDR_PLUS`) per-scene tone mapping. | **Dependency** (ADR-005, keystone). |
| **mpv render API** (libmpv) | via gpu-next/libplacebo; `mpv_render_context` + `MPV_RENDER_PARAM_*` | all | LGPL-2.1+/GPL | Whole player; too opaque for per-frame analysis. | Reference. |
| **wgpu** (Rust) | `Bt2100Pq`, `Bt2100Hlg`, `ExtendedSrgbLinear`, `DisplayP3`, `DisplayHdrInfo` | Vulkan/Metal/DX12 (GL/WebGPU SDR) | MIT/Apache | No tone mapping. | Not used. |
| **SDL3 GPU** | `HDR_EXTENDED_LINEAR`, `HDR10_ST2084` + window HDR props | Win/mac/Linux Wayland/Android (driver-dependent) | zlib | Windowing + input; SDL_shadercross. | Not used (lab front-end option). |
| **Dawn / ANGLE** | Dawn: WebGPU HDR as Chromium; ANGLE: GLES over Metal/Vulkan, EGL colourspace exts only | all | BSD | Heavy. | Not used. |
| **MoltenVK** | Vulkan colourspaces → CAMetalLayer EDR | macOS/iOS | Apache-2.0 | Single Vulkan + libplacebo pipeline on macOS. | **Dependency** (ADR-005). |
| **zimg** | CPU colourspace/resize/dither (no display) | all | WTFPL | Reference-quality CPU path for exports/tests. | **Dependency** (ADR-011 normalisation). |
| **Skia** | FP16/P3 surfaces; HDR only via client-managed EDR layer; no PQ output in public API | all | BSD | Flutter/Avalonia/Compose inherit the limitation. | Not used. |

**Dolby Vision fact**: only RPU parsing (FFmpeg `libavcodec/dovi_rpu.c`, https://code.ffmpeg.org/Traneptora/FFmpeg/blame/commit/c62b364dcb549bc0a882af7da3e338d443f78128/libavcodec/dovi_rpu.c) and P5/P8 → PQ conversion (libplacebo) are possible without a Dolby licence; "true DV" display-side mapping needs Dolby's engine; the UI must label this (chapter 10).

**Rendering performance facts for Chapter 10**: a 4K HEVC frame has 8 160 CTUs (64×64); 4×4 MV blocks at 4K ≈ 250k instances → one instanced draw call; labels as instanced glyph quads from a pre-baked SDF/bitmap atlas, shown only above a zoom threshold; heatmaps as per-block attribute texture + LUT (viridis/turbo); picking via integer division / quadtree for blocks and GPU id-buffer (R32U target, one-pixel readback) for MV arrows; keep A and B planes in GPU memory at native bit depth (R16_UNORM); nearest-neighbour magnification with sample values drawn as text above ~24 px per sample. HDR measurements computed in linear-light nits on the GPU (R32F + CPU readback of reductions): per-frame MaxCLL (max of max(R,G,B), CTA-861.3, optionally 99.99th percentile), FALL; false colour bands (<1, 1–100, 100–203 SDR white, 203–1000, 1000–4000, >4000 nits), luminance waveform with log nit axis (0.0001–10 000; PQ-code axis optional), BT.2020 vectorscope with 709 target, gamut-clip overlay, PQ-code histogram, CIE 1931 xy plot; point-sprite accumulation with additive blending for waveform/vectorscope. Binary size estimate with Qt libs: ~40–60 MB.

**Language-choice facts recorded for ADR-001**: libaom's `aom_codec_*` inspection hooks (`AV1D_SET_IS_ANNEXB`, `AV1_GET_ACCOUNTING`, `AV1D_EXT_TILE_DEBUG`) have no maintained Rust binding; `rsmpeg` tracks FFmpeg 8, `ffmpeg-next` is maintenance-only; `cxx-qt` (KDAB, MIT/Apache) would add a translation layer; macOS hardened runtime needs `com.apple.security.cs.disable-library-validation` only when loading unsigned plugins; Linux packaging via Flatpak (KDE runtime for Qt) or AppImage (`linuxdeploy-plugin-qt`).

---

# Appendix C — Codec syntax cross-reference

These tables are the **checklist** for two artefacts: the codec extension tables of the Data Model (chapter 6: `AvcMb`, `HevcCu`, `HevcPu`, `HevcTu`, `Av1Block`, `Av1Txb`, `*FilterParams`, `PartitionInfo`; rule in B.5.4: *every syntax element the decoder reads for a block is stored in the extension table with the exact spec name*) and the Selection Info panel (chapter 11). Spec editions: **H.264** = ITU-T Rec. H.264 (08/2021); **H.265** = ITU-T Rec. H.265 (09/2023); **AV1** = AOM AV1 Bitstream & Decoding Process Specification v1.0.0 with Errata 1 (2019-01-08), section numbers per that PDF (https://aomediacodec.github.io/av1-spec/av1-spec.pdf · https://www.itu.int/rec/T-REC-H.264 · https://www.itu.int/rec/T-REC-H.265). Terminology follows B.1 rule 8. The "Layer" column maps onto the OVT model: Unit / SyntaxNode (B.5.2–B.5.3) for the header layers, FrameInfo for picture/DPB, PartitionInfo for slices/tiles, Block + CellGrid + MotionVector + extension table for block layers, SymbolCost for bits.

## C.1 H.264 / AVC

| Layer | Data an analyzer shows | Spec syntax / semantics |
|---|---|---|
| **Byte stream / NAL** (`Unit`) | NAL unit list in decode order: `nal_unit_type` (1 non-IDR slice, 2–4 data partitions A/B/C, 5 IDR slice, 6 SEI, 7 SPS, 8 PPS, 9 AUD, 10/11 end of seq/stream, 12 filler, 13 SPS ext, 14 prefix NAL, 15 subset SPS, 19 aux slice, 20 slice ext (SVC/MVC), 21 3D-AVC), `nal_ref_idc`, byte offset, size, emulation-prevention bytes removed (count and positions → `Unit.epb_positions`) | 7.3.1, 7.4.1, Annex B |
| **Parameter sets** (`StreamIndex.parameter_sets`) | SPS: `profile_idc`/`constraint_set*_flag`/`level_idc`, `chroma_format_idc`, `bit_depth_luma_minus8`/`bit_depth_chroma_minus8`, `log2_max_frame_num_minus4`, `pic_order_cnt_type`, `max_num_ref_frames`, `pic_width_in_mbs_minus1`, `pic_height_in_map_units_minus1`, `frame_mbs_only_flag`, `mb_adaptive_frame_field_flag`, `direct_8x8_inference_flag`, frame cropping, VUI (timing, HRD, colour description, bitstream_restriction). PPS: `entropy_coding_mode_flag` (CAVLC/CABAC), `num_slice_groups_minus1` (FMO), `num_ref_idx_l0/l1_default_active_minus1`, `weighted_pred_flag`, `weighted_bipred_idc`, `pic_init_qp_minus26`, `chroma_qp_index_offset`, `second_chroma_qp_index_offset`, `deblocking_filter_control_present_flag`, `constrained_intra_pred_flag`, `transform_8x8_mode_flag`, scaling matrices | 7.3.2.1, 7.3.2.2, Annex E |
| **SEI** (`StreamIndex.sei`) | Buffering period, pic timing (`pic_struct`, clock timestamps), pan-scan, recovery point, user data registered (ITU-T T.35: HDR10+/CTA-861 dynamic metadata, closed captions), user data unregistered (x264 opts), frame packing, display orientation, mastering display colour volume (137), content light level (144), alternative transfer characteristics (147), green metadata | 7.3.2.3, Annex D |
| **Slice** (`PartitionInfo`) | `first_mb_in_slice`, `slice_type` (P, B, I, SP, SI and "all slices same type" variants 5–9), `pic_parameter_set_id`, `frame_num`, `field_pic_flag`/`bottom_field_flag`, `idr_pic_id`, POC syntax (`pic_order_cnt_lsb`, `delta_pic_order_cnt_bottom`, `delta_pic_order_cnt[]`), `direct_spatial_mv_pred_flag`, `num_ref_idx_active_override_flag`, ref pic list modification, pred weight table (`luma_log2_weight_denom`, per-ref `luma_weight`/`luma_offset`, chroma), dec ref pic marking (MMCO ops, `long_term_reference_flag`), `cabac_init_idc`, `slice_qp_delta`, `disable_deblocking_filter_idc`, `slice_alpha_c0_offset_div2`, `slice_beta_offset_div2`, slice byte size, MB count | 7.3.3, 7.4.3 |
| **Macroblock (16×16 luma)** (`Block` kind MB/SubMB, `AvcMb`) | `mb_skip_flag` / `mb_skip_run`; `mb_field_decoding_flag` (MBAFF); `mb_type` → I_NxN (with `transform_size_8x8_flag` → I_4x4 vs I_8x8), I_16x16_<predmode>_<cbpchroma>_<cbpluma> (24 variants), I_PCM; P_L0_16x16, P_L0_L0_16x8, P_L0_L0_8x16, P_8x8, P_8x8ref0, P_Skip; B_Direct_16x16, B_L0/L1/Bi_16x16, B_*_16x8 / 8x16 (21 combinations), B_8x8, B_Skip; `sub_mb_type` per 8×8: P_L0_8x8/8x4/4x8/4x4, B_Direct_8x8, B_L0/L1/Bi_8x8/8x4/4x8/4x4 | 7.3.5, 7.4.5 Tables 7-11, 7-13, 7-14, 7-17, 7-18 |
| **Intra prediction** (`CellGrid.intra_mode_y/c`) | Intra4x4PredMode (9: V, H, DC, DDL, DDR, VR, HD, VL, HU) per 4×4, Intra8x8PredMode (9) per 8×8, Intra16x16PredMode (4: V, H, DC, Plane), `intra_chroma_pred_mode` (4: DC, H, V, Plane); flags `prev_intra4x4_pred_mode_flag` / `rem_intra4x4_pred_mode` ("was the predicted mode used" overlay) | 8.3.1–8.3.4 |
| **Inter prediction** (`MotionVector`, `CellGrid.mv*/ref*`) | Per partition: `ref_idx_l0/l1` (→ resolved reference POC / frame_num, short/long term), `mvd_l0/l1` and reconstructed mvL0/mvL1 in quarter-sample units (chroma 1/8; stored ×4 in OVT's 1/16 units), direct mode (spatial/temporal), prediction direction (L0/L1/Bi), weighted prediction weights actually applied (explicit/implicit) | 8.4.1, 8.4.2.3 |
| **Residual** (`AvcMb`, `CellGrid.cbf/tx_*`) | `coded_block_pattern` (luma 4 bits for 8×8 quadrants + chroma 2 bits), `transform_size_8x8_flag`, `mb_qp_delta` → QP_Y (0–51, wraps mod 52 per 7.4.5), QP_C derived from `chroma_qp_index_offset` (Table 8-15), coefficient counts / total_coeff per 4×4, DC/AC split for I_16x16, PCM samples | 7.3.5.3, 7.4.5, 8.5 |
| **Deblocking** (`CellGrid.deblock_bs_*`, `AvcFilterParams`) | Derived bS (0–4) per edge, filterLeftMbEdgeFlag/filterTopMbEdgeFlag, α/β table indices, `disable_deblocking_filter_idc` effect | 8.7 |
| **Picture / DPB** (`FrameInfo`, `RefSlot`) | `frame_num`, PicOrderCnt (TopFieldOrderCnt, BottomFieldOrderCnt), picture structure (frame / top / bottom / complementary field pair, MBAFF → `PictureStructure`), reference marking (short-term / long-term / unused), DPB occupancy after each picture (`dpb_after`), RefPicList0/1 after init + modification, `num_reorder_frames`, output vs decode order | 8.2.1, 8.2.4, 8.2.5, C.4 |
| **Bit accounting** (`Block.bits_x8`, `SymbolCost`) | Bits per NAL/slice (exact); bits per MB requires instrumenting the entropy reader: CAVLC → bit position before/after each MB; CABAC has no per-MB bit boundary → record `(bytes consumed × 8 − bits in range register)` delta or an entropy-estimated cost per bin as libaom accounting does | 9.2, 9.3 |

## C.2 H.265 / HEVC

| Layer | Data | Spec |
|---|---|---|
| **NAL** (`Unit`) | `nal_unit_type` 0–63 (TRAIL_N/R, TSA_N/R, STSA_N/R, RADL_N/R, RASL_N/R, BLA_W_LP/W_RADL/N_LP, IDR_W_RADL/N_LP, CRA, VPS 32, SPS 33, PPS 34, AUD 35, EOS 36, EOB 37, FD 38, PREFIX_SEI 39, SUFFIX_SEI 40; 62 = UNSPEC carrying DV RPU), `nuh_layer_id`, `nuh_temporal_id_plus1` | 7.3.1.2, 7.4.2.2 Table 7-1 |
| **VPS / SPS / PPS** | VPS: max layers/sub-layers, timing/HRD. SPS: `chroma_format_idc`, `separate_colour_plane_flag`, pic size, conformance window, bit depths, `log2_max_pic_order_cnt_lsb_minus4`, sub-layer ordering (`sps_max_dec_pic_buffering`, `sps_max_num_reorder_pics`, `sps_max_latency_increase`), `log2_min_luma_coding_block_size_minus3`, `log2_diff_max_min_luma_coding_block_size` (→ CTB 16/32/64), `log2_min_luma_transform_block_size_minus2`, `log2_diff_max_min_luma_transform_block_size` (TB 4..32), `max_transform_hierarchy_depth_inter/intra`, `scaling_list_enabled_flag` + lists, `amp_enabled_flag`, `sample_adaptive_offset_enabled_flag`, `pcm_enabled_flag` (+ PCM bit depths / sizes / `pcm_loop_filter_disabled_flag`), short-term RPS sets, long-term ref pics, `sps_temporal_mvp_enabled_flag`, `strong_intra_smoothing_enabled_flag`, VUI, range extension (`transform_skip_rotation_enabled_flag`, `implicit_rdpcm_enabled_flag`, `extended_precision_processing_flag`, `cabac_bypass_alignment_enabled_flag`, …), SCC extension (`palette_mode_enabled_flag`, `motion_vector_resolution_control_idc`, `intra_boundary_filtering_disabled_flag`). PPS: `dependent_slice_segments_enabled_flag`, `output_flag_present_flag`, `num_extra_slice_header_bits`, `sign_data_hiding_enabled_flag`, `cabac_init_present_flag`, `init_qp_minus26`, `constrained_intra_pred_flag`, `transform_skip_enabled_flag`, `cu_qp_delta_enabled_flag` + `diff_cu_qp_delta_depth` (→ quantization group size), `pps_cb_qp_offset`/`pps_cr_qp_offset`, `weighted_pred_flag`/`weighted_bipred_flag`, `transquant_bypass_enabled_flag`, `tiles_enabled_flag` + column/row boundaries + `loop_filter_across_tiles_enabled_flag`, `entropy_coding_sync_enabled_flag` (WPP), deblocking control (`pps_beta_offset_div2`, `pps_tc_offset_div2`), `lists_modification_present_flag`, `log2_parallel_merge_level_minus2`, range ext (`log2_max_transform_skip_block_size_minus2`, `cross_component_prediction_enabled_flag`, `chroma_qp_offset_list_enabled_flag`), SCC ext (`pps_curr_pic_ref_enabled_flag` = intra block copy, palette predictor initializers) | 7.3.2.1–7.3.2.3, 7.4.3 |
| **SEI** | Buffering period, pic timing, recovery point, **decoded picture hash** (MD5/CRC/checksum — conformance check in OVT, phase 4), active parameter sets, MDCV (137), CLL (144), alternative transfer (147), user data registered T.35 (HDR10+), time code, frame field info | Annex D |
| **Slice segment** (`PartitionInfo`) | `first_slice_segment_in_pic_flag`, `no_output_of_prior_pics_flag`, `dependent_slice_segment_flag`, `slice_segment_address` (CTB raster address), `slice_type` (B 0, P 1, I 2), `pic_output_flag`, `colour_plane_id`, `slice_pic_order_cnt_lsb`, `short_term_ref_pic_set_sps_flag`/`short_term_ref_pic_set_idx` or explicit `st_ref_pic_set`, long-term entries, `slice_temporal_mvp_enabled_flag`, `slice_sao_luma_flag`/`slice_sao_chroma_flag`, `num_ref_idx_active_override_flag`, ref pic list modification, `mvd_l1_zero_flag`, `cabac_init_flag`, `collocated_from_l0_flag`/`collocated_ref_idx`, pred weight table, `five_minus_max_num_merge_cand`, `use_integer_mv_flag` (SCC), `slice_qp_delta`, `slice_cb_qp_offset`/`slice_cr_qp_offset`, `cu_chroma_qp_offset_enabled_flag`, deblocking override (`slice_deblocking_filter_disabled_flag`, `slice_beta_offset_div2`, `slice_tc_offset_div2`), `slice_loop_filter_across_slices_enabled_flag`, entry points (`num_entry_point_offsets`, `offset_len_minus1`, `entry_point_offset_minus1[]` → tile/WPP substream byte ranges) | 7.3.6, 7.4.7 |
| **CTU / CTB** (`Block` kind CTU, `HevcFilterParams`) | CTB address (raster + tile scan), slice idx, tile idx; SAO per CTB per component: `sao_merge_left_flag`/`sao_merge_up_flag`, `SaoTypeIdx` (0 off, 1 band, 2 edge), `sao_offset_abs[4]`, `sao_offset_sign`, `sao_band_position`, `sao_eo_class` | 7.3.8.2, 7.3.8.3, 7.4.9.3 |
| **Coding quadtree / CU** (`Block` kind CU, `HevcCu`) | `split_cu_flag` per depth (CtDepth 0–3), CU size 8–64, `cu_transquant_bypass_flag`, `cu_skip_flag`, `pred_mode_flag` (MODE_INTER/MODE_INTRA; MODE_SKIP derived), `palette_mode_flag` (SCC), `part_mode` → PartMode PART_2Nx2N, PART_2NxN, PART_Nx2N, PART_NxN (intra 8×8 only, inter min-CU only), AMP PART_2NxnU, PART_2NxnD, PART_nLx2N, PART_nRx2N; `pcm_flag` + pcm samples; `rqt_root_cbf`; `cu_qp_delta_abs`/`cu_qp_delta_sign_flag` → QpY per quantization group, QpY_pred derivation (left/above/previous), QP'Y, QP'Cb/QP'Cr with chroma offsets, `cu_chroma_qp_offset_flag`/`cu_chroma_qp_offset_idx` | 7.3.8.4, 7.3.8.5, 7.4.9.5, 8.6.1 |
| **PU / intra** (`Block` kind PU, `HevcPu`, `CellGrid.intra_mode_*`) | `prev_intra_luma_pred_flag`, `mpm_idx`, `rem_intra_luma_pred_mode` → IntraPredModeY (0 planar, 1 DC, 2–34 angular) per PB (PART_NxN gives 4 per CU), `intra_chroma_pred_mode` → IntraPredModeC (incl. mode 4 = derived from luma; 4:2:2 remap Table 8-3) | 7.3.8.5, 8.4.2, 8.4.3 |
| **PU / inter** (`HevcPu`, `MotionVector`) | `merge_flag`, `merge_idx` (0–4), `inter_pred_idc` (PRED_L0, PRED_L1, PRED_BI), `ref_idx_l0`/`ref_idx_l1`, `mvd_coding` (`abs_mvd_greater0_flag`, `abs_mvd_greater1_flag`, `abs_mvd_minus2`, `mvd_sign_flag`), `mvp_l0_flag`/`mvp_l1_flag`; derived mvL0/mvL1 (quarter-sample luma, 1/8 chroma; stored ×4), refIdx, predFlagL0/L1, merge candidate source (spatial A0/A1/B0/B1/B2, temporal, combined bi, zero → `MotionVector.predictor`), TMVP collocated picture | 7.3.8.6, 8.5.3.2 |
| **Transform tree / TU** (`Block` kind TU, `HevcTu`, `CellGrid.tx_*`) | `split_transform_flag` per depth (interSplitFlag / intra NxN forced split), TU sizes 4–32, `cbf_luma`, `cbf_cb`, `cbf_cr` (4:2:2 has two chroma TBs), `transform_skip_flag` per component, `explicit_rdpcm_flag`/`explicit_rdpcm_dir_flag`, residual coding: `last_sig_coeff_x_prefix/suffix`, `last_sig_coeff_y_prefix/suffix`, `coded_sub_block_flag`, `sig_coeff_flag`, `coeff_abs_level_greater1_flag`, `coeff_abs_level_greater2_flag`, `coeff_sign_flag` (sign data hiding), `coeff_abs_level_remaining` — analyzer derives #nonzero coeffs, coefficient energy, DST-VII (4×4 intra luma) vs DCT-II; `cross_component_prediction` alpha (`log2_res_scale_abs_plus1`, `res_scale_sign_flag`, RExt); scaling list in use | 7.3.8.8–7.3.8.12, 8.6.2–8.6.4 |
| **Deblocking** (`CellGrid.deblock_bs_*`) | Edge flags on 8×8 grid, bS (0/1/2) per 4-sample edge segment, β/tC from QP average + offsets, `pcm_loop_filter_disabled_flag`, strong/normal filter decision | 8.7.2 |
| **Picture / RPS / DPB** (`FrameInfo`, `RefSlot`) | PicOrderCntVal, NoRaslOutputFlag, RPS lists RefPicSetStCurrBefore/StCurrAfter/StFoll/LtCurr/LtFoll, RefPicList0/1 after construction, DPB fullness, `pic_output_flag`, bumping/output order, temporal sub-layer | 8.3.1–8.3.4, C.5 |
| **Bits** | Bits per slice segment (exact), per substream via entry points (exact), per CTU/CU (instrumented CABAC position; HM `TComDataCU::getTotalBits/getTotalBins` is the reference implementation) | 9.3 |

## C.3 AV1

| Layer | Data | Spec |
|---|---|---|
| **OBU** (`Unit`) | `obu_type` (1 OBU_SEQUENCE_HEADER, 2 OBU_TEMPORAL_DELIMITER, 3 OBU_FRAME_HEADER, 4 OBU_TILE_GROUP, 5 OBU_METADATA, 6 OBU_FRAME, 7 OBU_REDUNDANT_FRAME_HEADER, 8 OBU_TILE_LIST, 15 OBU_PADDING), `obu_extension_flag` → `temporal_id`, `spatial_id`, `obu_has_size_field`, `obu_size`; temporal unit grouping; Annex B length-delimited vs Section 5 low-overhead | 5.3, 6.2, Annex B |
| **Sequence header** | `seq_profile` (Main/High/Professional), `still_picture`, `reduced_still_picture_header`, operating points (`operating_point_idc`, `seq_level_idx`, `seq_tier`), `frame_width_bits_minus_1`/`frame_height_bits_minus_1`, `max_frame_width_minus_1`/`max_frame_height_minus_1`, `frame_id_numbers_present_flag`, `use_128x128_superblock`, `enable_filter_intra`, `enable_intra_edge_filter`, `enable_interintra_compound`, `enable_masked_compound`, `enable_warped_motion`, `enable_dual_filter`, `enable_order_hint` (+ `order_hint_bits_minus_1`), `enable_jnt_comp`, `enable_ref_frame_mvs`, `seq_force_screen_content_tools`, `seq_force_integer_mv`, `enable_superres`, `enable_cdef`, `enable_restoration`, `color_config` (`BitDepth`, `mono_chrome`, `color_primaries`, `transfer_characteristics`, `matrix_coefficients`, `color_range`, `subsampling_x/y`, `chroma_sample_position`, `separate_uv_delta_q`), `film_grain_params_present`, timing info / decoder model info | 5.5 |
| **Metadata OBU** | `metadata_type`: 1 METADATA_TYPE_HDR_CLL (`max_cll`, `max_fall`), 2 METADATA_TYPE_HDR_MDCV (primaries, white point, max/min luminance), 3 METADATA_TYPE_SCALABILITY, 4 METADATA_TYPE_ITUT_T35 (`itu_t_t35_country_code`, payload → HDR10+ ST 2094-40, DV RPU-in-T35), 5 METADATA_TYPE_TIMECODE | 5.8, 6.7 |
| **Frame header** (`FrameInfo`, `Av1FilterParams`) | `show_existing_frame` (+ `frame_to_show_map_idx`), `frame_type` (KEY_FRAME, INTER_FRAME, INTRA_ONLY_FRAME, SWITCH_FRAME), `show_frame`, `showable_frame`, `error_resilient_mode`, `disable_cdf_update`, `allow_screen_content_tools`, `force_integer_mv`, `current_frame_id`, `frame_size_override_flag`, `order_hint`, `primary_ref_frame`, `refresh_frame_flags` (8-bit mask), `ref_order_hint[]`, frame size + render size + superres (`use_superres`, `coded_denom`), `allow_intrabc`, `ref_frame_idx[7]` (LAST, LAST2, LAST3, GOLDEN, BWDREF, ALTREF2, ALTREF → slot 0–7), `allow_high_precision_mv`, `is_filter_switchable`/`interpolation_filter`, `is_motion_mode_switchable`, `use_ref_frame_mvs`, `disable_frame_end_update_cdf`, `tile_info` (`uniform_tile_spacing_flag`, TileCols/TileRows, `context_update_tile_id`, `tile_size_bytes_minus_1`), `quantization_params` (`base_q_idx` 0–255, `DeltaQYDc`, `DeltaQUDc`/`DeltaQUAc`/`DeltaQVDc`/`DeltaQVAc`, `using_qmatrix`, `qm_y`/`qm_u`/`qm_v`), `segmentation_params` (`segmentation_enabled`, `segmentation_update_map`, `segmentation_temporal_update`, per-segment features SEG_LVL_ALT_Q, ALT_LF_Y_V, ALT_LF_Y_H, ALT_LF_U, ALT_LF_V, REF_FRAME, SKIP, GLOBALMV), `delta_q_params` (`delta_q_present`, `delta_q_res`), `delta_lf_params` (`delta_lf_present`, `delta_lf_res`, `delta_lf_multi`), `loop_filter_params` (`loop_filter_level[4]`: Y vertical, Y horizontal, U, V; `loop_filter_sharpness`, `loop_filter_delta_enabled`, ref/mode deltas), `cdef_params` (`cdef_damping_minus_3`, `cdef_bits`, `cdef_y_pri_strength[]`/`cdef_y_sec_strength[]`, `cdef_uv_pri_strength[]`/`cdef_uv_sec_strength[]`), `lr_params` (FrameRestorationType[plane] RESTORE_NONE/WIENER/SGRPROJ/SWITCHABLE, LoopRestorationSize[plane] 64/128/256), TxMode (ONLY_4X4, TX_MODE_LARGEST, TX_MODE_SELECT), `reference_select`, `skip_mode_present` (+ derived SkipModeFrame[2]), `allow_warped_motion`, `reduced_tx_set`, `global_motion_params` per ref (IDENTITY, TRANSLATION, ROTZOOM, AFFINE + `gm_params[ref][6]`), `film_grain_params` (`apply_grain`, `grain_seed`, `update_grain`, scaling points Y/Cb/Cr, `grain_scaling_minus_8`, `ar_coeff_lag`, `ar_coeffs_y_plus_128`/`ar_coeffs_cb_plus_128`/`ar_coeffs_cr_plus_128`, `ar_coeff_shift_minus_6`, `grain_scale_shift`, `cb_mult`/`cb_luma_mult`/`cb_offset`, `cr_mult`/`cr_luma_mult`/`cr_offset`, `overlap_flag`, `clip_to_restricted_range`) | 5.9, 6.8 |
| **Tile group / tile** (`PartitionInfo`) | `tg_start`, `tg_end`, tile sizes (`tile_size_minus_1`), per-tile byte range, tile index ↔ MI column/row ranges, `context_update_tile_id` | 5.11.1, 6.10.1 |
| **Superblock / partition** (`Block` kind SB) | 64×64 or 128×128 SB; `partition` per square block (PARTITION_NONE, HORZ, VERT, SPLIT, HORZ_A, HORZ_B, VERT_A, VERT_B, HORZ_4, VERT_4; 4:1 types only for 16..64, T-shapes not for 8×8); 22 block sizes (4×4, 4×8, 8×4, 8×8, 8×16, 16×8, 16×16, 16×32, 32×16, 32×32, 32×64, 64×32, 64×64, 64×128, 128×64, 128×128, 4×16, 16×4, 8×32, 32×8, 16×64, 64×16); per SB: `cdef_idx[]` (one per 64×64), `delta_qindex` / `delta_lf` read at SB level; `use_wiener`/`use_sgrproj` + unit coefficients read when the SB covers a new LR unit | 5.11.4, 5.11.5, 5.11.56–5.11.58, 6.10.4 |
| **Block — common** (`Block` kind Block, `Av1Block`) | `mi_row`/`mi_col`, `bsize`, `skip_mode` (derived two-ref zero-residual mode), `skip` (no residual), `segment_id` (0–7, `seg_id_predicted`), `is_inter`, `use_intrabc`, CurrentQIndex (from delta_q), delta LF values (4), `cdef_idx` of its 64×64 | 5.11.5–5.11.8, 6.10.5–6.10.10 |
| **Block — intra** | `y_mode` (DC_PRED, V_PRED, H_PRED, D45_PRED, D135_PRED, D113_PRED, D157_PRED, D203_PRED, D67_PRED, SMOOTH_PRED, SMOOTH_V_PRED, SMOOTH_H_PRED, PAETH_PRED), `angle_delta_y`/`angle_delta_uv` (−3..+3 × 3°), `uv_mode` (same 13 + UV_CFL_PRED), CfL `cfl_alpha_signs`, `cfl_alpha_u`/`cfl_alpha_v` (−16..16), `use_filter_intra` + `filter_intra_mode` (5), palette: `palette_size_y`/`palette_size_uv` (2–8), colours, `color_index_map`, intra-BC `mv` (integer, whole-pel), intra edge filter / upsampling derived | 5.11.7, 5.11.22–5.11.24, 5.11.42–5.11.46, 7.11.2 |
| **Block — inter** (`Av1Block`, `MotionVector`) | `ref_frame[0]` (LAST_FRAME..ALTREF_FRAME = 1..7, INTRA_FRAME 0), `ref_frame[1]` (NONE −1, compound ref, or INTRA_FRAME for interintra), `comp_mode` single/compound, `comp_ref_type` unidirectional/bidirectional, `y_mode` ∈ {NEARESTMV, NEARMV, GLOBALMV, NEWMV} or compound {NEAREST_NEARESTMV, NEAR_NEARMV, NEAREST_NEWMV, NEW_NEARESTMV, NEAR_NEWMV, NEW_NEARMV, GLOBAL_GLOBALMV, NEW_NEWMV}, `drl_mode` → ref MV stack index, `mv[2]` (1/8-pel when `allow_high_precision_mv`, else 1/4; stored ×2 in OVT 1/16 units), `mv_joint`, `interintra` + `interintra_mode` (II_DC_PRED, II_V_PRED, II_H_PRED, II_SMOOTH_PRED) + `wedge_interintra` + `wedge_index`, `motion_mode` (SIMPLE, OBMC, LOCALWARP; derived LocalValid, LocalWarpParams), compound type: `comp_group_idx` / `compound_idx` → COMPOUND_WEDGE (`wedge_index`, `wedge_sign`), COMPOUND_DIFFWTD (`mask_type`), COMPOUND_DISTANCE (jnt_comp, weights from order-hint distance), COMPOUND_AVERAGE; `interp_filter[2]` (EIGHTTAP, EIGHTTAP_SMOOTH, EIGHTTAP_SHARP, BILINEAR; dual filter per direction), global motion applied flag | 5.11.23, 5.11.25–5.11.33, 7.10, 7.11.3 |
| **Transform** (`Block` kind TXB, `Av1Txb`, `CellGrid.tx_*`) | TxSize (19 sizes: 4×4…64×64 incl. 1:2, 2:1, 1:4, 4:1 rectangles; 64-point transforms zero beyond 32), `tx_depth`/`txfm_split` (inter var-tx tree, up to 2 levels), TxType (16: DCT_DCT, ADST_DCT, DCT_ADST, ADST_ADST, FLIPADST_DCT, DCT_FLIPADST, FLIPADST_FLIPADST, ADST_FLIPADST, FLIPADST_ADST, IDTX, V_DCT, H_DCT, V_ADST, H_ADST, V_FLIPADST, H_FLIPADST; + WHT_WHT for lossless), tx set (`reduced_tx_set`), per TB: `all_zero`, `eob`, coefficient levels / signs / `dc_sign`, Golomb extra; quantizer matrix level | 5.11.16–5.11.21, 5.11.39, 5.11.47–5.11.51, 7.12, 7.13 |
| **Loop filters** (`Av1FilterParams`, `CellGrid.filter_idx`) | Deblock: per-block derived filter level per direction/plane (from `loop_filter_level`, segment ALT_LF, ref/mode deltas, delta_lf), filter length; CDEF: per 64×64 `cdef_idx` → `cdef_y_pri/sec_strength`, `cdef_uv_*`, damping, skip (all blocks skipped → no CDEF), per-8×8 direction (derived, 8 dirs) and variance; Loop restoration: per LR unit per plane `restoration_type` (RESTORE_NONE/WIENER/SGRPROJ), Wiener coeffs (7-tap symmetric, LrWiener[plane][unit][pass][3]), sgrproj `lr_sgr_set` (16 sets) + `xqd[2]`; superres upscale; film grain synthesised post-output (not part of the reference picture) | 7.14, 7.15, 7.16, 7.17, 7.18.3 |
| **Reference structure** (`FrameInfo.declared_refs`, `RefSlot`) | 8 reference slots (RefValid, RefFrameType, RefOrderHint, RefFrameId, saved CDFs/segmentation/LF deltas/GM params/film grain), `refresh_frame_flags` per frame → slot timeline; `ref_frame_idx[7]` → which slot each named ref uses; `show_existing_frame` (ALTREF re-shown; a KEY shown via show_existing resets refs), hidden frames (`FrameId.display_index = -1`), `order_hint` distance, OrderHints[refFrame], RefFrameSignBias (forward/backward), `use_ref_frame_mvs` motion-field projection sources, SkipModeFrame[0/1]; temporal unit → frame count (frames vs shown frames) | 6.8.2, 7.20, 7.21, 7.9 |
| **Bits** (`SymbolCost`) | Bits per OBU (exact), per tile (exact from `tile_size`); per block/symbol: multi-symbol range coder with no per-block byte boundaries; libaom `CONFIG_ACCOUNTING` records a fractional cost per decoded symbol in 1/8-bit units (`AccountingSymbol.bits`) tagged with (x,y) context and symbol name — the only practical "bits per block" for AV1 and the technique OVT applies to CABAC | 8.2, libaom `av1/decoder/accounting.h` |

## C.4 Common / codec-agnostic

- **Decoded planes** (`PlaneRef`, `Picture`): Y/U/V (or GBR for 4:4:4 RGB), bit depth 8–16, chroma format 400/420/422/444, cropped vs coded size (H.264 frame cropping, HEVC conformance window, AV1 render size + superres upscaled width) → `FrameInfo.coded_size` / `crop`.
- **Signal stages** (`PlaneStage`): prediction, residual (inverse transformed), reconstruction before in-loop filtering, after deblocking, after SAO (HEVC) / after CDEF / after loop restoration (AV1), after film grain (AV1; H.264/HEVC film-grain SEI); "coefficients" view (dequantised levels). Decoders either store intermediate planes (memory × stages) or re-decode with filters disabled (the libde265 and dav1d analyzer forks take the second approach: `export_prediction`, `export_prefilter`).
- **Per-frame stats** (`FrameInfo`, Chapter 6 derived views): decode index, display/POC index, frame/slice type, size in bytes, average QP and QP histogram (`FrameAnalysis.qp_hist`), fraction of intra/inter/skip area, MV magnitude stats, PSNR/SSIM vs reference, PTS/DTS, temporal layer, shown vs hidden (AV1).
- **GOP structure graph**: frames in decode order with arrows to referenced frames (declared in headers, or actually used per block), hierarchy by temporal id, display-order reordering.
- **Reference dependency graph**: edges weighted by blocks/pixels actually referencing each ref (`RefSlot.usage_cells`), DPB occupancy timeline (`FrameInfo.dpb_after`).

---

# Appendix D — Decoder internals extraction facts

Source-level facts verified by the research against upstream trees (FFmpeg master, libde265 master, libaom main, dav1d master mirror, YUView develop, HM mirror; Nov 2026). These facts justify ADR-008 and are the implementation reference for `plugins/ovt-plugin-{ffmpeg,de265,aom,hm}`. Items marked **(unverified)** could not be confirmed against source.

## D.1 FFmpeg — H.264, HEVC, AV1 export status

**H.264 native decoder (`libavcodec/h264dec.c`, `h264dec.h`, `mpegutils.c`)**

| Fact | Detail |
|---|---|
| Per-picture tables | `H264Picture` holds `int8_t *qscale_table`, `int16_t (*motion_val[2])[2]` (4×4-granular MV field), `uint32_t *mb_type` (bitmask of `MB_TYPE_*`: INTRA4x4, INTRA16x16, INTRA_PCM, 16x16, 16x8, 8x16, 8x8, INTERLACED, DIRECT2, SKIP, L0/L1 per partition…), `int8_t *ref_index[2]` (per 8×8); pool-allocated in `alloc_picture()`. Full-frame tables → frame-threaded decoding still exports. HW-accelerated decode produces no tables (`ff_print_debug_info2` returns early). |
| MV export | Set `AV_CODEC_EXPORT_DATA_MVS` in `AVCodecContext.export_side_data` (CLI `-export_side_data mvs`; legacy `-flags2 +export_mvs` / `AV_CODEC_FLAG2_EXPORT_MVS` still exists and is what YUView's `decoderFFmpeg.cpp` sets via `dictSet(opts, "flags2", "+export_mvs")`). Implemented in `ff_print_debug_info2()` (`libavcodec/mpegutils.c`), called from `finalize_frame()` in h264dec.c only when `CONFIG_MPEGVIDEODEC`. Iterates MBs and both directions; skips a direction unless `HAS_MV_EXT()` (intra MBs produce nothing); emits 1 (16×16), 2 (16×8/8×16) or 4 (8×8, sampled at quadrant centres) `AVMotionVector`s per MB per list via `add_mb()`. **Does not descend into 8×8 sub-partitions** (8×4/4×8/4×4 collapsed to one vector per 8×8). `source` is −1 (L0) / +1 (L1) only (header comment: "XXX: set exact relative ref frame reference instead of a +/- 1 direction") → **`ref_idx` is lost**; `flags` always 0; `motion_scale` = 4 for H.264 (`shift = 1 + quarter_sample`). Direct/skip MBs exported as ordinary inter MBs with derived MVs. Field/MBAFF: vertical MV doubled when `IS_INTERLACED` for 16×8/8×16 only; no field parity. |
| QP export | `AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS` (`-export_side_data venc_params`) → `AV_FRAME_DATA_VIDEO_ENC_PARAMS` with `AV_VIDEO_ENC_PARAMS_H264`: one `AVVideoBlockParams` per MB, `src_x = x*16, src_y = y*16, w = h = 16, delta_qp = qscale_table[mb_xy] − par->qp`, `par->qp = pps->init_qp`, `delta_qp[1][0]/[2][0]` from chroma offsets (`libavutil/video_enc_params.h`). Gives final QP_Y per MB. Commit: https://git.qt.io/saraposo/ffmpeg/-/commit/064b875e894f45ea9e0426696559645d87af0bc3 |
| Text debug | `-debug qp` / `-debug mb_type` (`FF_DEBUG_QP = 16`, `FF_DEBUG_MB_TYPE = 8`) print a per-MB character grid from `ff_print_debug_info2()`: `P` PCM, `A` AC-pred, `i` intra4x4, `I` intra16x16, `d` direct+skip, `D` direct, `S` skip, `>`/`<` single list, `X` bi; `+`/`-`/`|` for 8×8/16×8/8×16; `=` interlaced. `FF_DEBUG_VIS_*`, `-vismv` removed (FFmpeg 4.0 era). `codecview=mv=pf+bf+bb` draws exported `AVMotionVector`s; `codecview=qp=1` draws from `AV_FRAME_DATA_VIDEO_ENC_PARAMS`. |
| **Not available** from public API | Partition shapes below 8×8; intra prediction modes (not stored per MB after decoding — `intra4x4_pred_mode` lives in slice-context caches, `H264SliceContext.intra4x4_pred_mode_cache`); CBP (`cbp_table`); `transform_size_8x8_flag`; `ref_idx` / ref POC; mvd; weighted prediction tables; deblocking bS; bits per MB; slice boundaries per MB (`slice_table` internal); prediction/residual planes; pre-deblock reconstruction. SPS/PPS fields only via `h->ps.sps` in-process. |
| Phase-2 patch (ADR-008) | Extend `ff_print_debug_info2`/`h264dec.c` to export MB type + ref_idx + intra modes + CBP through `AVVideoBlockParams` (room for extension) or a new side-data type (the 2020 `AVEncodeInfo` proposal is the precedent); requires keeping `intra4x4_pred_mode`/`cbp` tables per picture (new allocations in `alloc_picture`). Report estimate for a full "analysis build" fork: ~1.5–3 kLOC across `h264_cavlc.c`/`h264_cabac.c` (record), `h264_mb.c`, `h264_slice.c`, `h264dec.c` (alloc/export), `h264_loopfilter.c` (snapshot). OVT keeps the patch small and upstreams it. |

**HEVC native decoder (`libavcodec/hevc/hevcdec.c`, new directory layout since 2024)**

| Fact | Detail |
|---|---|
| Export status | No `AV_FRAME_DATA_MOTION_VECTORS`, no `AV_FRAME_DATA_VIDEO_ENC_PARAMS`, no `export_mvs` handling; only `AV_CODEC_EXPORT_DATA_FILM_GRAIN` (`hevcdec.c`, `refs.c`). |
| Internal data | `qp_y_tab`, `tab_mvf` (per-4×4 `MvField` with `mv[2]`, `ref_idx[2]`, `pred_flag`), `skip_flag`, `cu.pred_mode`, `tab_ipm` (intra pred mode per 4×4), `cbf_luma`, `tab_slice_address`, deblocking bS tables in `HEVCFrame`/`HEVCLocalContext` — freed/recycled, never attached to output frames. |
| History | "HEVC: Export motion vectors to frame side data" by Asaf Kave (Dec 2019–Feb 2020; proposed a new `AVEncodeInfo` block-info side data) approved by Lynne but never pushed (author lacked push rights; Paul B Mahol asked for it to be combined with the libavutil patch). 2024 HEVC side-data work (LCEVC, MV-HEVC view ids, interlaced) adds nothing block-level. Status FFmpeg 7.1/8.0: no block-level export **(unverified negative for 2024–26 patchwork)**. Estimated patch to add `VIDEO_ENC_PARAMS` + `MOTION_VECTORS` export: ~300–500 lines, two files; `vp9` QP export was merged in 2020 (`vp9_export_enc_params` precedent in `libavcodec/vp9.c`). |
| OVT | Opportunistic upstreaming only (ADR-008 uses libde265 for HEVC). |

**AV1**: FFmpeg `av1` native decoder is hwaccel-only (parses OBUs with CBS for VAAPI/NVDEC/VideoToolbox/D3D12/Vulkan). `libdav1d.c` exports film grain params (`AV_FRAME_DATA_FILM_GRAIN_PARAMS`), MDCV, CLL, ITU-T T.35 (dav1d ≥ 6.9 loop over `n_itut_t35`), Dolby Vision config, colour description; options `oppoint`, `alllayers`, `max_frame_delay`. No block data. CBS (`cbs_av1.c`, `cbs_h264.c`, `cbs_h265.c`) is a header-level read/write parser (OBU/NAL/slice header/SEI; `H265RawVPS/SPS/PPS/SliceHeader/SEI*`, `AV1RawOBUHeader/SequenceHeader/FrameHeader/TileGroup/Metadata{HDRCLL,HDRMDCV,Scalability,ITUTT35,Timecode}`); `CodedBitstreamContext.trace_enable/trace_level`; slice data (`H265RawSlice.data`, `AV1RawTileData`) are opaque blobs. CBS lifting would need `get_bits.h`, `put_bits.h`, `cbs_internal.h`, `refstruct` — OVT does not do this (ADR-007).

## D.2 libde265 — `de265_image` metadata and the internals API

| Fact | Detail |
|---|---|
| Public API | `de265.h`: no block-level accessors; `DE265_DECODER_PARAM_DUMP_{SPS,VPS,PPS,SLICE}_HEADERS`, `de265_get_image_NAL_header()`. |
| `de265_image` (`libde265/image.h`) `MetaDataArray<>`s | `ctb_info` (`CTB_info`: slice header idx, slice address, SAO info, deblocking/pcm/bypass flags); `cb_info` (`CB_ref_info`: `log2CbSize:3, PartMode:3, ctDepth:2, PredMode:2, pcm_flag:1, cu_transquant_bypass:1, int8_t QP_Y`); `pb_info` (`PBMotion`: `predFlag[2], refIdx[2], MotionVector mv[2]` with `int16_t x,y`); `intraPredMode` / `intraPredModeC` (per 4×4); `tu_info` (split/cbf flags per 4×4); `deblk_info` (edge flags + bS). Each array has a `log2UnitSize` layout. |
| Accessors | `get_pred_mode, get_cu_skip_flag, get_PartMode, get_log2CbSize, get_ctDepth, get_pcm_flag, get_cu_transquant_bypass, get_QPY, get_mv_info, get_IntraPredMode, get_IntraPredModeC, get_SliceHeaderIndex, get_SliceAddrRS, get_sao_info, get_deblk_flags, get_deblk_bS`; sherlock265 also uses `get_cu_size`, `get_log2CbSize`. |
| **Not persisted** | `merge_flag`/`merge_idx`/`mvd`/`mvp_lX_flag` (`PBMotionCoding` is transient in `slice.cc` `read_prediction_unit`), `transform_skip_flag`, cbf per TU component (only via `tu_info` bits), coefficients, bits per CU (CABAC decoder in `cabac.cc` tracks `bitstream_curr`). |
| Filters | `deblock.cc` and `sao.cc` operate in place on `img`; SAO already uses a copy → pre-filter snapshots behind a flag are cheap. |
| YUView's `de265_internals.h` (Feldmann fork, https://github.com/ChristianFeldmann/libde265 · header copy https://github.com/IENT/YUView/blob/develop/YUViewLib/src/decoder/externalHeader/libde265/de265_internals.h · pyde265 https://github.com/kloppjp/pyde265) | Exports: `de265_internals_set_parameter_bool(DE265_INTERNALS_DECODER_PARAM_SAVE_{PREDICTION,RESIDUAL,TR_COEFF})`, `de265_internals_get_image_plane(img, signal, channel, &stride)` (reconstruction / prediction / residual / transform coefficients — the fork hijacks the output planes to carry the alternative signal), `de265_internals_get_CTB_Info_Layout`, `de265_internals_get_CTB_sliceIdx`, `de265_internals_get_CB_Info_Layout`, `de265_internals_get_CB_info`, `de265_internals_get_PB_Info_layout`, `de265_internals_get_PB_info` (refPOC0, refPOC1, x0, y0, x1, y1), `de265_internals_get_IntraDir_Info_layout`, `de265_internals_get_intraDir_info` (luma, chroma), `de265_internals_get_TUInfo_Info_layout`, `de265_internals_get_TUInfo_info`. Thin C exports over the `MetaDataArray`s (packed per-unit uint16 arrays + a layout struct giving unit size/stride); ~500 lines, header + one source file, touches no decoding code. Statistics YUView derives: slice index, part size, pred mode (INTRA/INTER/SKIP), PCM flag, transquant bypass, ref POC 0/1, MV 0/1, intra dir luma/chroma, transform depth. **No QP** (although `CB_ref_info.QP_Y` exists — a 20-line addition); ref POC is relative because the fork hardcodes POC 0 (an open item in YUView). Fork predates 2017; internal class layout unchanged since. |
| OVT patch `0001-de265-internals-api.patch` scope (ADR-008) | (1) public read-only C API over `cb_info/pb_info/intraPredMode/tu_info/deblk_info/ctb_info` plus persist `merge_flag/merge_idx/mvd/mvp_idx/transform_skip/cbf[3]/rqt depth` (currently transient in `slice.cc` `read_coding_unit/read_prediction_unit/read_transform_unit`); (2) per-CU bit counting: record `(bitstream_curr − bitstream_start)*8 − bits_needed adjustment` at CU start/end in `cabac.cc`; (3) optional pre-deblock/pre-SAO plane copies behind a flag; (4) slice header / RPS / ref list / DPB getters (`slice.h` `slice_segment_header`, `decctx.h`). Report estimates: 1–2 kLOC total; rebase cost near zero. Upstreaming: struktur AG (Dirk Farin) responsive on GitHub. |
| Performance | No NEON → ~2–4× slower than FFmpeg (estimate); with instrumentation ≈ 20–60 fps 1080p (estimate). WPP/tile threads work with inspection. |

## D.3 HM — `TComDataCU` getters

Verified on the GitHub mirror (`source/Lib/TLibCommon/TComDataCU.h`): `getDepth, getWidth, getHeight, getPredictionMode, getPartitionSize, getSkipFlag, getMergeFlag, getMergeIndex, getIntraDir(ChannelType), getQP, getCbf(comp), getTransformIdx, getTransformSkip, getCUMvField(RefPicList)` (→ `TComCUMvField::getMv/getRefIdx/getMvd`), `getIPCMFlag, getCUTransquantBypass, getInterDir, getMVPIdx, getChromaQpAdj, getExplicitRdpcmMode, getCrossComponentPredictionAlpha, getTotalBits, getTotalBins, getCtuRsAddr`. Everything an HEVC analyzer needs including RExt (and SCC in the SCM branch), with no patch to HM. YUView's `libHM` wrapper exposes `libHMDec_new_decoder/push_nal_unit/get_picture`, `libHMDEC_get_POC/..._image_plane`, and generic internals `libHMDEC_get_internal_type_number/_name/_type/_max/_vector_scaling/_description`, `libHMDEC_get_internal_info(ctx, pic, typeIdx, &nrValues, &callAgain)` returning `libHMDec_BlockValue{x,y,w,h,value,value2}`, `libHMDEC_clear_internal_info`; types self-describing (`LIBHMDEC_TYPE_FLAG/RANGE/RANGE_ZEROCENTER/VECTOR/INTRA_DIR`) — a useful pattern, but OVT's `ovt-plugin-hm` emits `FrameAnalysis` FlatBuffers directly (ADR-010). Own wrapper against current HM: 1–2 weeks (report estimate). The VTM wrapper (`libVTMDec_*`) has internals calls commented out in YUView.

## D.4 libaom — inspection and accounting structures

**`insp_frame_data`** (`av1/decoder/inspection.h`): `Accounting *accounting` (CONFIG_ACCOUNTING); `insp_mi_data *mi_grid`; `int16_t frame_number`; `int show_frame, frame_type, base_qindex, mi_rows, mi_cols, tile_mi_rows, tile_mi_cols`; `int16_t y_dequant[MAX_SEGMENTS][2], u_dequant[..][2], v_dequant[..][2]`; `int delta_q_present_flag, delta_q_res, show_existing_frame`; plus `superblock_size`, `width/height`, and (shared with decoder buffers) `recon_frame_buffer`, `predicted_frame_buffer`, `prefiltered_frame_buffer`.

**`insp_mi_data`** (per 4×4 MI): `insp_mv mv[2]` (row, col int16); `int16_t ref_frame[2], mode, uv_mode, bsize, skip, segment_id, dual_filter_type, filter[2], tx_type, tx_size, cdef_level, cdef_strength, cfl_alpha_idx, cfl_alpha_sign, current_qindex, compound_type, motion_mode, intrabc, palette, uv_palette, sb_bits`. `tx_type`/`tx_size` come from the transform maps and are −1 for skipped blocks.

**Missing from inspection** (all present in `MB_MODE_INFO`, `av1/common/blockd.h`: `bsize, partition, mode, uv_mode, current_qindex, mv[2], ref_frame[2], interp_filters, motion_mode, num_proj_ref, overlappable_neighbors, wm_params, filter_intra_mode_info, palette_mode_info, angle_delta[2], cfl_alpha_signs/idx, tx_size, skip_txfm, segment_id, seg_id_predicted, use_intrabc, comp_group_idx, compound_idx, ref_mv_idx, skip_mode, delta_lf_from_base, delta_lf[4]`, `interintra_mode, use_wedge_interintra, interintra_wedge_index, interinter_comp{type,wedge_index,wedge_sign,mask_type}`): `angle_delta`, `filter_intra_mode`, interintra type/mode/wedge, `wedge_index/sign`, `drl`/`ref_mv_idx`, warp params, `delta_lf`, `skip_mode`, per-block MVD, LR unit params, per-SB `cdef_idx` (only derived strength), palette colours, coefficients, `partition` type (HORZ_A/B, VERT_A/B, HORZ_4/VERT_4 are reconstructable from the bsize layout). Extending `ifd_inspect` is a ~100-line patch (estimated 100–300 lines incl. pre-filter snapshots) — upstreaming candidate since inspection is AOM's own debug API.

**Accounting** (`av1/decoder/accounting.h`): `AccountingSymbol { AccountingSymbolContext{int16 x,y}; uint32 id; uint32 bits /* 1/8-bit units, AOM_ACCT_BITRES=3 */; uint32 samples; }` + `AccountingDictionary` (≤ 256 symbol names). The decoder calls `aom_accounting_set_context(x,y)` per block and `aom_accounting_record(name, bits)` per symbol → bits per 4×4 MI and per syntax element (→ `SymbolCost`, `CellGrid.bits_x8`). Control `AV1_GET_ACCOUNTING`.

**`examples/inspect.c`** JSON per frame: `frame, showFrame, frameType, baseQIndex, tileCols/Rows, deltaQPresentFlag, deltaQRes, config/configString`; per-layer 2D arrays `blockSize, transformSize, transformType, mode, uv_mode, motionVectors, skip, filter, cdef, referenceFrame, dualFilterType, delta_q, seg_id, compound_type, mm, cfl, ibc, plt, uvp, sb`; `symbolsMap/symbols [id,bits,samples]`; optional RLE compression (`-x`); flags `-bs -ts -tt -m -uvm -mm -ct -r -mv -s -f -df -c -cfl -ibc -plt -uvp -dq -si -sb -a -A`; Emscripten build exports `EMSCRIPTEN_KEEPALIVE open_file/read_frame`, `Module.on_frame_decoded_json`. OVT's aom-inspect JSON export subset (B.7 rule 9) follows these names.

**Pre-filter planes**: libaom keeps `cm->cur_frame->buf` and applies deblock/CDEF/LR in place (CDEF/LR use line buffers) → copies in the callback chain (`av1_loop_filter_frame`, `av1_cdef_frame`, `av1_loop_restoration_filter_frame` in `av1/decoder/decodeframe.c`) or decode with `AV1D_SET_SKIP_LOOP_FILTER` **(unverified in current header)**. Row-MT works with inspection (callback per frame). Speed: 3–10× slower than dav1d; inspection + accounting slower (accounting allocates per symbol); ≈ 5–20 fps 1080p (estimate).

## D.5 dav1d — `Av1Block` facts (for the record; not used by OVT per ADR-008)

Verified in `src/levels.h`, `src/internal.h`, `src/decode.c`: `Av1Block { uint8_t bl, bs, bp; uint8_t intra, seg_id, skip_mode, skip, uvtx; union { intra: {y_mode, uv_mode, tx, pal_sz[2]; int8 y_angle, uv_angle, cfl_alpha[2]}; inter: { union { {mv mv[2]; wedge_idx, mask_sign, interintra_mode} | {mv mv2d; int16 matrix[4]} }; comp_type, inter_mode, motion_mode, drl_idx; int8 ref[2]; max_ytx, filter2d, interintra_type, tx_split0; uint16 tx_split1 } } }`. Missing from it: coefficients, palette colours (`frame_thread.pal`), delta_q/delta_lf (tile-state `last_delta_lf`, QP in `ts->dqmem`), LR params (`f->lf.lr_mask`, `Av1Restoration`), CDEF idx (`Av1Filter.cdef_idx`, `t->cur_sb_cdef_idx_ptr`). Persistence: in `decode_b()`, `Av1Block b_mem, *const b = t->frame_thread.pass ? &f->frame_thread.b[t->by * f->b4_stride + t->bx] : &b_mem;` → the per-4×4 block array persists for the frame **only in frame-threaded two-pass mode**; MV field persists in `refmvs_temporal_block *mvs`; loop-filter levels in `Av1Filter *mask` / `lf.lvl`. No public inspection API, no debug dump. YUView's route: `decoderDav1d.cpp` resolves `dav1d_default_analyzer_settings` / `dav1d_set_analyzer_settings(Dav1dContext*, const Dav1dAnalyzerFlags*)` (flags `export_blkdata`, `export_prediction`, `export_prefilter`, `export_bitsperblk`, `export_bitsused`, `export_invisible_frames`) from `libdav1d-internals`; picture carries `blk_data` cast to `Av1Block*` indexed `[y * b4_stride + x]`, walked from 128×128 by `parseBlockRecursive`; 27 statistics derived. Fork (https://github.com/ChristianFeldmann/dav1d, 2019–20 dav1d 0.5–0.7 era) never upstreamed; analyzer branch name **(unverified)**. A current-dav1d patch would be ~600–1000 LOC: `inspect` setting forcing `frame_thread.b/cbi/pal/pal_idx` allocation in single pass, per-SB `cdef_idx`, per-block `qidx`/`delta_lf`, LR params, tile byte ranges into a ref-counted `Dav1dPicture.inspect_data`, pre-filter copies after `dav1d_filter_sbrow_deblock/cdef/resize/lr` (`src/recon_tmpl.c`, `src/thread_task.c`), and a `dav1d_msac_tell_frac()` over `MsacContext.buf_pos/buf_end/cnt` (`src/msac.h`) for bit accounting. Upstream has historically rejected analyzer features touching hot paths; a proposal modelled on libaom (NULL-callback check per block) is the only acceptable route (ADR-008: no dav1d fork).

## D.6 JM trace facts

`ldecod` compiled with `#define TRACE 1` writes `trace_dec.txt` (`p_Dec->p_trace`): one line per syntax element `@ bitpos element_name bitstring (value)`, including MB-level elements (`mb_type`, `ref_idx`, `mvd`, `coeff`) — the de-facto trace format commercial analyzers emulate; OVT's `ovt syntax --trace` output follows it (chapter 12). JM structs `Macroblock`/`StorablePicture`: `mv_info` per 4×4 with `mv[2]`, `ref_idx[2]`, `ref_pic[2]`; `mb_type`, `b8mode/b8pdir`, `qp`, `cbp`, `ipredmode` per 4×4, `i16mode`, `luma_transform_size_8x8_flag`, `DFDisableIdc`, `DFAlphaC0Offset/BetaOffset` — the natural H.264 source of truth if a reference-grade engine were acceptable (slow, no threading; OVT uses it as an oracle only). Source: https://iphome.hhi.de/suehring/tml/doc/ldec/html/ldecod_8c_source.html.

## D.7 Why not an analysis-only parser (recorded for ADR-008)

Headers are cheap (a few kLOC per codec). Block-level H.264 requires CAVLC **and** CABAC decoding plus neighbour availability, MBAFF, direct-mode MV derivation and ref-list construction — a decoder minus reconstruction, ~15–25 kLOC; HEVC CABAC + CU/PU/TU trees + merge/AMVP derivation similar; AV1 symbol decoding with adaptive CDFs depends on neighbouring reconstructed syntax and the reference MV stack of previous frames, so one re-implements most of the decoder; intra-edge/palette/CfL views need pixels anyway. Correctness rule: show both syntax (`mvd`, `merge_idx`) and derived values (MV, ref POC) and take both from the decoder at the same point — never derive in the UI. Hardware decoders (MediaCodec, VideoToolbox, VA-API/VDPAU, D3D11/12, NVDEC) return pixels only and may differ (film grain application, 8- vs 10-bit output); analysis runs on the software path; a "verify HW vs SW" diff is a phase-4 view.

## D.8 Memory and performance estimates (basis for chapter 0 §0.7 rules 3–4)

| Quantity | 1080p | 4K | 8K | Notes |
|---|---|---|---|---|
| 4×4 cells (`CellGrid`) | 480×270 = 129.6k | 960×540 = 518k | 1920×1080 = 2.07M | |
| Core SoA grid ≈ 40 B/cell | 5 MB | 21 MB | 83 MB | grids compress 5–20× with zstd (ADR-010 cache) |
| Block tree, HEVC worst case ≈ 1 CU per 8×8, ~64 B/node | 32k nodes ≈ 2 MB | 130k ≈ 8 MB | 520k ≈ 33 MB | AV1 similar; H.264 16×16 MBs trivial |
| 4:2:0 10-bit plane set per stage (1.5 × 2 B × pixels) | 6 MB | 25 MB | 100 MB | recon + pred + residual (i16) + 2 pre-filter stages ≈ 5× → 30 / 125 / 500 MB per frame |
| libaom symbol accounting | — | 1–3 M symbols × 16 B = 16–48 MB | — | per frame at 4K |

Policy derived (B.7 rule 3): keep grids/trees for an LRU of 64 frames, planes for an LRU of 4 frames (report suggested 2–4); regenerate pre-filter planes by re-decoding the GOP with filters disabled on demand; `uint8/uint16` columns; zstd-compressed cache keyed by stream hash + frame index. Speed estimates: libde265 with instrumentation ≈ 20–60 fps 1080p; libaom inspect + accounting ≈ 5–20 fps 1080p; HM ≈ 1–5 fps 1080p; JM ≈ 10–30 fps 1080p; reference-grade engines 10–100× slower than FFmpeg → background full-stream analysis of a 2-hour 4K AV1 file takes hours, hence "analysis around the current frame" + sampled frames + explicit whole-stream job with checkpointing (ADR-009). GOPs of 2–10 s = 48–600 frames between random access points; header-only first pass (unit index, frame types, sizes, POC/order_hint, ref structure, SEI) in seconds for a whole file (B.7: ≤ 2 s for 1-hour 1080p H.264 on an M1). Threading: libde265 WPP/tile threads and libaom row-MT work with inspection; dav1d frame threading is the mode that persists `Av1Block`. Random access: index IRAP/key frames and AV1 `refresh_frame_flags`/`show_existing_frame` chains in the header pass; AV1 hidden ALTREF frames make decode-order index ≠ shown-frame index (`FrameId` keeps both). Interlaced/MBAFF H.264 and field-coded HEVC use the frame-coordinate grid plus `PictureStructure` per block.

---

# Appendix E — Licensing and patent facts

Facts behind ADR-002 (LGPL-2.1-or-later `core/` and `render/`; GPL-3.0-or-later `app/`, `cli/`, `android/`; BSD-2-Clause ABI headers; CI licence gate), ADR-015 (runtime-loaded AVC/HEVC decoder plugins) and ADR-016 (DCO, trademark, distribution channels). Nothing here is legal advice. Dates of "last activity" are from GitHub/ecosyste.ms snapshots.

## E.1 Dependency licence table

| Component | Licence | Notes | OVT layer |
|---|---|---|---|
| FFmpeg | LGPL-2.1+ default; **GPL-2+ with `--enable-gpl`** (x264, x265, libvidstab, some filters); `--enable-nonfree` (fdk-aac) makes it undistributable; `--enable-version3` → LGPL-3/GPL-3 for some libs (https://www.ffmpeg.org/legal.html) | LGPL build: link dynamically, ship exact sources, do not forbid reverse engineering, credit FFmpeg (trademark of Fabrice Bellard). A GPL build may be shipped in-process only by the GPL app (`ovt-plugin-ffmpeg` is LGPL/GPL per build). | third_party / plugin |
| dav1d | BSD-2-Clause | no block-level API | third_party (playback) |
| libaom | BSD-2-Clause + **AOM Patent License 1.0** (https://aomedia.org/license/patent-license/) | ship PATENTS text with binaries | `ovt-plugin-aom` |
| libde265 | LGPL-3.0+ | HEVC; README warns HEVC is patented and the library carries no patent licence (struktur AG sells commercial ones) | `ovt-plugin-de265` (dynamic) |
| openHEVC | LGPL-2.1 | dead | not used |
| HM / VTM / vvdec | BSD-3-Clause | slow but complete; vvdec also BSD-3 (https://www.cnx-software.com/2023/02/13/vvenc-vvdec-h-266-open-source-video-encoder-and-decoder-work-on-x86-and-arm/) | `ovt-plugin-hm` |
| JM | custom permissive ITU/ISO reference-software terms | oracle only | tests |
| libvmaf | BSD-2-Clause-Plus-Patent | explicit patent grant; models same licence | core (metrics) |
| google/uvq | Apache-2.0 | weights redistributable | core (metrics; ONNX export) |
| zimg | WTFPL | | core |
| FlatBuffers | Apache-2.0 | | core |
| libplacebo | LGPL-2.1+ | dynamic link on Android (ship `.so`) | render |
| GPAC | LGPL-2.1+ (commercial via Motion Spell, https://gpac.io/licensing) | oracle / fallback | tests |
| Bento4 | GPL-2 **or** commercial (https://www.bento4.com/about) | GPL-2-only is incompatible with GPL-3 and with LGPL core → **not used** as a dependency | tests (shell out) |
| Qt 6 | LGPL-3 (most modules) / GPL-3 (Charts, Graphs, Data Visualization, Virtual Keyboard, Wayland Compositor, …) / commercial (https://www.qt.io/licensing/open-source-lgpl-obligations) | LGPL-3 relink obligation disappears for a GPL-3 app with published sources; GPL-only modules usable by `app/` | app |
| QHexView | MIT | | app |
| QCustomPlot | GPLv3 / commercial | usable by GPL-3 `app/` | app (optional) |
| KDDockWidgets | BSD / GPL dual | | app (optional) |
| SDL2/3, Dear ImGui, ImPlot | zlib / MIT | lab front-end only | optional |
| MediaInfoLib | BSD-2-Clause (https://mediaarea.net/MediaInfo/License) | oracle | tests |
| mp4ff, go-mp4, mp4-atom, mp4box.js | MIT / MIT / MIT / BSD-3 | oracles / references | tests |
| MPEG FileFormatConformance data | BSD-3-Clause-Clear | registry input to `tools/` generator | tools |
| dovi_tool, hdr10plus_tool | MIT | oracles | tests |
| YUView | GPL-3.0 | code reuse permitted only in GPL-3 `app/` (never in `core/`) | app |
| aomanalyzer | MIT | reference | — |
| h26forge | MIT | reference / fuzz corpus | tests |
| h264bitstream | LGPL-2.1 | reference | — |
| hevcesbrowser, CodecScope | GPL-2.0-only | reference only (incompatible unless "or later") | — |
| GitlHEVCAnalyzer | "modified Apache-2.0" + non-commercial clause | **fails the licence gate**; not used | — |
| VQMT | research-only | **not FOSS**; oracle in CI only, never linked or vendored | tests |
| XPSNR reference plugin | Fraunhofer BSD-3-like with explicit **no-patent-rights** clause | FFmpeg's LGPL version is canonical; OVT's own port carries a patent caveat in THIRD_PARTY_NOTICES | core (metrics) |
| HDR-VDP-3 | GPLv2 | sidecar only (Octave) | sidecar |
| ColorVideoVDP, FovVideoVDP, PU21 | MIT | sidecar / own port | sidecar / core |
| vmaf-torch | unstated on PyPI | sidecar only; check repo | sidecar |
| TSDuck | BSD-2 | optional desktop wrapper | optional |
| libebml/libmatroska | LGPL-2.1 | reference | — |
| bmx | Apache-2.0 | reference (MXF) | — |
| ONNX Runtime | MIT | UVQ host | core |

## E.2 Licence compatibility rules and matrix

Rules (https://www.gnu.org/licenses/license-list.html · https://docs.debricked.com/opentext-core-sca-blogs/blogs/oss-licenses-part-6-license-compatibility-and-dual-licensing · https://opensource.com/law/11/9/mpl-20-copyleft-and-license-compatibility):

- GPL-2.0-only and GPL-3.0 are mutually incompatible; "or-later" code can be upgraded.
- LGPL-3 code (libde265, Qt) may be combined into GPL-3 works but **not** GPL-2-only works.
- LGPL-2.1 code (FFmpeg, libplacebo, GPAC, h264bitstream) may be relicensed to GPL-2 or GPL-3 at will.
- Apache-2.0 code (FlatBuffers, UVQ, bmx) combines with GPL-3 but not GPL-2.
- MPL-2.0 combines with GPL-2/3 and LGPL via its secondary-licence clause.
- Permissive (MIT/BSD/zlib) combines with anything.

| Project licence option | FFmpeg GPL build | libde265 (LGPL-3+) | Qt (LGPL-3 + GPL-3-only modules) | libplacebo, GPAC (LGPL-2.1+) | YUView code reuse (GPL-3) | hevcesbrowser (GPL-2) | Bento4 (GPL-2-only) | Apache-2.0 deps | Embedding by companies |
|---|---|---|---|---|---|---|---|---|---|
| **GPL-3.0-or-later** | ✔ (GPL-2+ parts upgrade to v3) | ✔ | ✔ incl. GPL-only modules | ✔ | ✔ copy freely | only if "or later" | ✘ | ✔ | ✘ (use yes; embed no) |
| GPL-2.0-or-later files, binaries conveyed as GPL-3 | ✔ | ✔ in the GPL-3 binary; files using libde265 must be 2+ not 2-only | ✔ | ✔ | ✔ but copied files become GPL-3-only (the 2+ grant erodes) | ✔ | ✘ as GPL-3 | ✔ in GPL-3 binary | ✘ |
| GPL-2.0-only | ✔ | ✘ | ✘ (Qt LGPL-3) | ✔ | ✘ | ✔ | ✔ | ✘ | ✘ |
| **LGPL / MPL-2.0 core + GPL-3 app** (ADR-002) | ✔ in app | ✔ | ✔ | ✔ | ✔ in app only | n/a | ✘ | ✔ | ✔ core only |
| Apache-2.0 / BSD everywhere | LGPL build only | dynamic only | dynamic, LGPL modules only | dynamic | ✘ | ✘ | ✘ | ✔ | ✔✔ |
| AGPL-3 | ✔ | ✔ | ✔ | ✔ | ✔ | — | ✘ | ✔ | ✘ — **excluded by owner** |

The VLC precedent for the split: libVLC LGPL-2.1+, VLC app GPL-2+. Discipline required: no GPL code (YUView) may leak into `core/` or `render/`.

**AGPL/SSPL components to keep out** (CI licence gate fails on `AGPL*`, `SSPL*`, `CC-BY-NC*`, `BUSL*`; run `license-checker`/`pip-licenses` for any npm/Python helper): Ghostscript (AGPL-3; use Qt `QPdfWriter` or cairo for PDF reports), iText (AGPL), MinIO / Grafana / MongoDB-SSPL / Elastic-SSPL (use Postgres/SQLite/Prometheus if a results server is ever considered), ReportLab-plus / Plotly Dash Enterprise-style dual-licensed charting backends. No mainstream codec, demuxer or renderer library is AGPL. Linking an AGPL library would make the whole app AGPL.

## E.3 Linking and distribution mechanics per platform (GPL-3 app)

**Android (APK/AAB with bundled `.so`), including Chromebooks.** Under GPL-3 static vs dynamic no longer matters for OVT's own and GPL/LGPL dependencies: FFmpeg, libde265, libplacebo, dav1d and libaom may be built into one `.so` behind the JNI boundary as long as the published tree reproduces exactly that APK (GPL-3 §6 "Corresponding Source"; LGPL-3 §4(d)(0) satisfied because the LGPL parts are then part of a GPL-3 work). Publish the Gradle project, NDK version pin and third-party build scripts (`third_party/VERSIONS.md`, ADR-012). **Google Play**: nothing in the Developer Distribution Agreement conflicts with the GPL (VLC, Termux historically, KeePassDX, NewPipe mirrors, Syncthing-Fork are distributed there). Play App Signing / AAB re-signs the APK, so a user cannot drop in a rebuilt APK under the same signature; GPL-3 §6 "Installation Information" applies to software conveyed with or for a "User Product" as part of a transaction transferring the device — an app download is not that transaction, so re-installability of Play-signed builds is not required under the FSF's interpretation; still publish the signing-free reproducible build so a rebuilt APK can be sideloaded. Play's 2023+ developer verification and Android-16-era sideload restrictions make **F-Droid** (builds from source, own signature, licence shown per app; https://lwn.net/Articles/746733/) the natural second channel; also GitHub-Releases APKs with reproducible builds. Qt on Android under GPL-3: any module may be used; the LGPL relink problem (https://forum.qt.io/topic/100839/how-to-comply-with-lgpl-on-android) disappears because sources are published. Chromebooks: same APK via Play (x86_64 for Intel/AMD Chromebooks, arm64 for others).

**macOS (.app bundle).** Notarised DMG direct download plus Homebrew cask. Static or dynamic linking free under GPL-3; publish exact source + build scripts (CMake presets, vcpkg manifest). **Never submit to the Mac App Store**: the FSF and VLC concluded the App Store Usage Rules add "further restrictions" (GPLv2 §6 / GPLv3 §10); VLC was pulled in January 2011 and returned only after relicensing to LGPL/MPL (https://www.engadget.com/2011-01-09-the-gpl-the-app-store-and-you.html · https://mailman.videolan.org/pipermail/vlc-devel/2010-November/077072.html · https://lists.blender.org/pipermail/bf-committers/2011-July/032718.html). Code signing/notarisation is not a GPL problem for direct downloads; hardened runtime needs `com.apple.security.cs.disable-library-validation` only for loading unsigned plugins.

**Linux.** GPL-3 is the easiest licence for distro inclusion and Flathub; the obstacle is patents: Fedora/RHEL strip AVC/HEVC decoders from FFmpeg and disabled Mesa's VA-API H.264/H.265 on Red Hat legal advice (https://www.phoronix.com/news/Fedora-Disable-Bad-VA-API · https://lwn.net/Articles/911212/), while Debian/Arch/Flathub ship them. OVT's AVC/HEVC decoder plugins are separable packages (`-codecs-free` / `-codecs-nonfree` split as RPM Fusion does) so a Fedora-compliant package exists (ADR-015). Packaging: Flatpak (KDE runtime), AppImage (`linuxdeploy-plugin-qt`), distro packages.

## E.4 Patents

**AVC/HEVC pools.** H.264 (Via LA, formerly MPEG LA) and HEVC (Access Advance, Via LA's HEVC pool, Velos Media, plus unaffiliated holders) licence decoders per unit; pools target device and commercial-software vendors and have never pursued a non-commercial FOSS project.

- **Access Advance software policy** (Nov 2016; https://streaminglearningcenter.com/blogs/hevc-advance-makes-software-royalty-free.html · https://www.streamingmedia.com/Articles/News/Online-Video-News/HEVC-Advance-Makes-Some-Software-Royalty-Free-114938.aspx): waives royalties for software-only HEVC encode/decode downloaded after device sale, naming VLC, HandBrake and Firefox, but **excludes "standalone products marketed as providing advanced HEVC video functions" priced ≥ $45 (or ≥ $3.75/month)**. A free analyzer fits the waiver; a paid fork would not. Binds neither Via LA, Velos nor non-pool holders.
- Via LA's AVC licence has a zero-royalty tier (first 100k units/year); MPEG LA/Via LA have never sued a FOSS decoder project.
- FFmpeg's position (https://www.ffmpeg.org/legal.html): developers do not read patents; the project does not know whether it infringes; private use is low-risk; commercial redistribution is the redistributor's problem.
- VLC ships x264/x265/FFmpeg decoders in all builds, relying on French law's non-enforceability of software patents and on the decoder-is-user-installed logic for per-unit pool models.
- Risk for a free GPL analyzer: low, identical to VLC/HandBrake/YUView. Since the owner decision of 2026-11-12 (ADR-015) the default download ships the pool-covered playback decoders; the split build (`OVT_PATENT_CODECS_IN_CORE=OFF`, used for the Google Play base module and the PyPI wheel pending a legal check) and the plugin split remain the mitigation. Exposure is mainly for a company redistributing internally at scale — the plugin split lets it substitute a licensed decoder.

**AV1.** The AOM Patent License 1.0 (https://aomedia.org/license/patent-license/) is a royalty-free grant from AOM members for Necessary Claims of any conforming Encoder/Decoder *Implementation* (not just libaom), with **defensive termination** on AV1 patent suits and a duty to ship the licence text with binaries. Sisvel runs a non-AOM AV1/VP9 pool; the EU investigated AOM's terms (closed 2023); AV1 is not literally patent-free, yet every browser and distro ships AV1 decoders. OVT ships libaom (with `CONFIG_INSPECTION`/`CONFIG_ACCOUNTING`) and dav1d in official builds and includes PATENTS in `LICENSES/THIRD_PARTY_NOTICES.md`.

**"Bring your own decoder" practice** (YUView `QLibrary` plugins, Fedora/RPM Fusion, GStreamer plugin packs, Elecard's external-decoder mode) — the model for ADR-015:
1. Official binaries contain parsers (syntax parsing and visualisation are not what the pools licence), the AV1/VP9/MPEG-2 (expired) stack and libvmaf; the default download is built **without** AVC/HEVC decoders (`--disable-decoder=h264,hevc` or a separate FFmpeg plugin file).
2. Decoders are runtime-loaded plugins with a stable C ABI (`dlopen`/`QLibrary`): on Android extra `.so` files in app-private storage or a separate "codec pack" APK/split; on macOS dylibs under `~/Library/Application Support/<app>/decoders`; on Linux system FFmpeg/libde265 from the distro. Users may point OVT at FFmpeg, libde265, HM/VTM, vvdec or a vendor's proprietary decoder. Under GPL-3 the plugin ABI is a published interface (hence BSD-2-Clause headers, ADR-002); a proprietary plugin loaded by the user at runtime is the user's own combination, not OVT's distribution (GCC plugins before the runtime-library exception, Linux kernel modules, Kodi binary add-ons — accepted practice, not FSF-endorsed; document the position).
3. One-click fetching of decoder packs from third-party hosts (Homebrew, distro repos, F-Droid "codec pack" app, user builds).
4. Fast path for HEVC/AVC pixels: FFmpeg with `export_mvs`/`venc_params`; deep path: libde265/HM inspection plugins.

## E.5 Governance facts

- **DCO vs CLA** (https://writing.kemitchell.com/2021/07/02/DCO-Not-CLA · https://opensource.com/article/18/3/cla-vs-dco-whats-difference · https://bestpractices.linuxfoundation.org/_sources/ip/contribution-mechanisms.rst.txt): the DCO (`Signed-off-by`, as in the Linux kernel, GNOME, GitLab, Node.js) certifies provenance with no paperwork; it does not itself grant a licence — inbound = outbound. A CLA would add only a patent grant beyond GPL-3 §11's implicit one and a relicensing option (Qt/x265/Bento4 style). ADR-016: DCO; core-library contributions explicitly under LGPL-2.1-or-later in the DCO text; no copyright-assignment CLA (deters encoder-vendor contributors).
- **Foundation hosting**: start with none (GitHub/GitLab org + Open Collective or Software Freedom Conservancy for funds), like YUView, aomanalyzer (xiph), mpv. GPL-3 is welcome at **VideoLAN** (dav1d, x264, VLC; strong patent posture; CI farm) and **Xiph.Org** (aomanalyzer). Linux Foundation hosting needs neutral trademark ownership, documented open governance, one LF member plus five sponsoring organisations (https://www.linuxfoundation.org/projects/hosting/). AOM hosts only AV1/AV2 codec work. SFC is a fiscal sponsor comfortable with GPL enforcement.
- **Trademark**: distinctive name (not confusable with StreamEye / VQ Analyzer / Zond / YUView), registered in at least one jurisdiction (maintainer LLC or fiscal sponsor as owner), Mozilla/Debian-style policy: unmodified redistribution and F-Droid/distro builds may use the name; modified builds may not without permission. Credit FFmpeg as its legal page requests.

---

# Appendix F — URL index

Every URL cited by the research behind Appendices A–E, deduplicated and grouped by topic; one per line with a short label.

## F.1 Commercial tools

- https://vicuesoft.com/vq-analyzer — ViCueSoft VQ Analyzer product page
- https://vicuesoft.com/analyzer/releasenotes — VQ Analyzer release notes
- https://vicuesoft.com/analyzer/userguide — VQ Analyzer user guide (7.4)
- https://vicuesoft.com/blog/titles/V5.0_Launch — VQ Analyzer 5.0 launch blog
- https://vicuesoft.com/vq-probe/ — VQ Probe product page
- https://vicuesoft.com/probe/userguide — VQ Probe user guide (3.6)
- https://vicuesoft.com/blog/titles/dvk_advantages/ — VQ DVK advantages blog
- https://vicuesoft.com/vq-dvk/vq-dvk-av1 — VQ DVK AV1
- https://shop.xlsoft.com/en/vicue-analyzer/ — xlsoft VQ Analyzer product page
- https://shop.xlsoft.com/en/vicue-price/ — xlsoft VQ Analyzer price list
- https://www.intel.com/content/www/us/en/developer/articles/tool/video-pro-analyzer.html — Intel VPA discontinuation notice
- https://www.intel.cn/content/dam/develop/external/us/en/documents/intelvideoproanalyzeruserguide-560483.pdf — Intel VPA 2.3 user guide (PDF)
- https://www.intel.com/content/www/us/en/developer/articles/troubleshooting/find-quality-issues-vpa.html — Intel VPA "find quality issues" article
- https://community.intel.com/t5/Media-Intel-oneAPI-Video/Intel-Video-Analyzer-Pro-BUG-report-regarding-High-Dynamic-Range/m-p/1090691/highlight/true — Intel VPA HDR bug thread
- https://www.elecard.com/products/video-analysis/streameye — Elecard StreamEye
- https://www.elecard.com/products/video-analysis/streameye-basic — Elecard StreamEye Basic
- https://elecard.com/question/elecard-streameye-and-elecard-streameye-basic-comparison — StreamEye Full vs Basic comparison
- https://www.elecard.com/products/video-analysis/streameye-studio — Elecard StreamEye Studio
- https://www.elecard.com/docs/en/ReleaseNotes/index.html — Elecard release notes
- https://elecard.com/products/video-analysis/stream-analyzer — Elecard Stream Analyzer
- https://www.elecard.com/products/video-analysis/stream-analyzer — Elecard Stream Analyzer (www)
- https://www.elecard.com/products/video-analysis/video-quality-estimator — Elecard Video Quality Estimator
- https://www.elecard.com/question/what-metrics-video-quality-estimator-4.x-can-calculate — VQE metrics FAQ
- https://www.elecard.com/en/products/video-analysis/yuv-viewer — Elecard YUV Viewer
- https://elecard.com/products/monitoring/stream-inspector — Elecard Stream Inspector
- https://www.softpedia.com/get/Multimedia/Video/Other-VIDEO-Tools/Elecard-StreamEye-Studio.shtml — Softpedia StreamEye Studio listing
- https://platform.softwareone.com/product/elecard-streameye-studio/PCP-9579-1164 — SoftwareOne StreamEye Studio
- https://platform.softwareone.com/product/elecard-streameye-basic/PCP-3082-7951 — SoftwareOne StreamEye Basic
- https://www.streamingmedia.com/Articles/Editorial/Featured-Articles/Elecard-Video-Quality-Estimator-Review-129136.aspx — Streaming Media VQE review (2019)
- https://www.solveigmm.com/en/products/zond/ — Solveig Zond 265
- https://www.solveigmm.com/en/products/zond/history/linux/ — Zond 265 Linux history
- https://codecian.com/ — Codecian site
- https://codecian.com/codecvisa.html — CodecVisa user guide
- https://alternativeto.net/software/codecvisa/about — CodecVisa on AlternativeTo
- https://www.interrasystems.com/video-codec-analyzer.php — Interra VEGA codec analyzer
- https://interrasystems.com/press-kit/IBC2023/Product-Datasheets/VEGA/Vega_Datasheet.pdf — VEGA datasheet (IBC 2023)
- https://interrasystems.com/Vega-cli.php — VEGA CLI
- https://www.interrasystems.com/av1-analyzer.php — Interra AV1 analyzer
- https://manualmachine.com/tektronix/mts4ea/10503446-datasheet — Tektronix MTS4EA datasheet
- https://www.electronicspecifier.com/products/design-automation/new-mts4eav7-adds-hevc-h-265-standard-support-to-elementary-stream-analyzer — MTS4EAV7 HEVC support
- https://www.tvtechnology.com/opinions/sencore-delivers-hevc-analysis-capabilities-with-cma-1820 — Sencore CMA 1820 (TV Technology)
- https://prnewswire.com/news-releases/sencore-debuts-new-hevc-analysis-product-at-cable-tec-expo-2013-225790251.html — Sencore CMA 1820 press release
- https://parabolaresearch.com/explorer-hevc-analyzer.html — Parabola Explorer
- https://parabolaresearch.com/downloads/Parabola-Explorer-Datasheet.pdf — Parabola Explorer datasheet
- https://parabolaresearch.com/blog/2012-11-16-introducing-parabola-explorer.html — Parabola Explorer introduction (2012)
- https://redsharknews.com/telestream-switch-5-pro-review-comprehensive-qc-software — Telestream Switch 5 review
- https://www.streamingmedia.com/PressRelease/Telestream-Brings-New-Professional-Visual-Inspection-and-QC-Capabilities-to-Switch_38804.aspx — Telestream Switch 2015 press release
- https://www.trustradius.com/products/telestream-aurora/details — Telestream Aurora (TrustRadius)
- https://videoq.com/Downloads/VideoQ_VQV_Viewer-Analyzer_Datasheet.pdf — VideoQ VQV datasheet
- https://www.offminor.de/vooya-usage.html — vooya usage
- https://www.offminor.de/vooya-command-line.html — vooya command line
- https://offminor.de/ — vooya home
- https://www.mankier.com/1/mediainfo-gui — MediaInfo man page
- https://en.wikipedia.org/wiki/X265 — x265 (MulticoreWare search result)

## F.2 FFmpeg

- https://ffmpeg.org — FFmpeg home
- https://github.com/FFmpeg/FFmpeg — FFmpeg source mirror
- https://ffmpeg.org/ffmpeg-bitstream-filters.html — bitstream filters documentation (`trace_headers` etc.)
- https://ffmpeg.org/ffprobe.html — ffprobe documentation
- https://ffmpeg.org/pipermail/ffmpeg-devel/2014-August/161593.html — MV export origin (2014)
- https://ffmpeg.org/pipermail/ffmpeg-cvslog/2017-October/110005.html — removal of deprecated debug_mv visualisation
- https://lists.mplayerhq.hu/pipermail/ffmpeg-devel/2020-January/255350.html — unmerged HEVC MV export patches (Jan 2020)
- https://lists.mplayerhq.hu/pipermail/ffmpeg-devel/2020-January/255407.html — HEVC MV export thread (Jan 2020, sibling)
- https://ffmpeg.org/pipermail/ffmpeg-devel/2020-February/257074.html — HEVC MV export thread (Feb 2020)
- https://ffmpeg.org/pipermail/ffmpeg-devel/2022-January/291775.html — `siti` filter patch thread
- https://www.ffmpeg.org/doxygen/trunk/mediacodecdec_8c.html — MediaCodec decoder (doxygen)
- https://www.ffmpeg.org/doxygen/trunk/vf__codecview_8c_source.html — `codecview` filter source
- https://ffmpeg.org/doxygen/7.1/vf__xpsnr_8c_source.html — `xpsnr` filter source (7.1)
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/mpegutils.c — `ff_print_debug_info2`, MV export
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/h264dec.c — H.264 decoder (`finalize_frame`, VIDEO_ENC_PARAMS export)
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/h264dec.h — `H264Picture`
- https://github.com/FFmpeg/FFmpeg/blob/master/libavutil/motion_vector.h — `AVMotionVector`
- https://github.com/FFmpeg/FFmpeg/blob/master/libavutil/video_enc_params.h — `AVVideoEncParams`
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/hevc/hevcdec.c — HEVC decoder
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/libdav1d.c — libdav1d wrapper
- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/vp9.c — VP9 (`vp9_export_enc_params` precedent)
- https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/mov.c — mov demuxer
- https://git.qt.io/saraposo/ffmpeg/-/commit/064b875e894f45ea9e0426696559645d87af0bc3 — h264 QP export commit (mirror)
- https://code.ffmpeg.org/Traneptora/FFmpeg/blame/commit/c62b364dcb549bc0a882af7da3e338d443f78128/libavcodec/dovi_rpu.c — Dolby Vision RPU parser
- https://www.ffmpeg.org/legal.html — FFmpeg legal / licensing page

## F.3 AV1: libaom, dav1d, libgav1, aomanalyzer

- https://aomedia.googlesource.com/aom/ — libaom repository
- https://github.com/mozilla/aom — libaom GitHub mirror (stale)
- https://aomedia.googlesource.com/aom/+/refs/heads/main/aom/aomdx.h — `aomdx.h` (inspection controls)
- https://aomedia.googlesource.com/aom/+/refs/heads/main/av1/decoder/inspection.h — `inspection.h`
- https://aomedia.googlesource.com/aom/+/refs/heads/main/av1/decoder/inspection.c — `inspection.c`
- https://aomedia.googlesource.com/aom/+/refs/heads/main/av1/decoder/accounting.h — `accounting.h`
- https://aomedia.googlesource.com/aom/+/refs/heads/main/examples/inspect.c — `examples/inspect.c` (main)
- https://aomedia.googlesource.com/aom/+/HEAD/examples/inspect.c — `examples/inspect.c` (HEAD)
- https://aomedia.googlesource.com/aom/+/refs/heads/main/av1/common/blockd.h — `blockd.h` (`MB_MODE_INFO`)
- https://aomedia.googlesource.com/avm/+/1b651f3806129c2f444f19a246ac1844beae9e3f/av1/decoder/inspection.h — AVM copy of `inspection.h`
- https://searchfox.org/firefox-main/source/third_party/aom/examples/inspect.c — `inspect.c` (Firefox searchfox)
- https://github.com/xiph/aomanalyzer — aomanalyzer
- https://github.com/xiph/aomanalyzer/issues — aomanalyzer issues
- https://people.videolan.org/~negge/AOM_Analyzer_update_2021-Aug-13.pdf — AOM Analyzer 2021 status deck
- https://awesome.ecosyste.ms/projects/github.com%2Fxiph%2Faomanalyzer — aomanalyzer activity (ecosyste.ms)
- https://code.videolan.org/videolan/dav1d — dav1d repository
- https://github.com/videolan/dav1d — dav1d GitHub mirror
- https://github.com/ChristianFeldmann/dav1d — YUView dav1d internals fork
- https://github.com/ChristianFeldmann/dav1d/releases — YUView dav1d fork binaries
- https://github.com/memorysafety/rav1d — rav1d (Rust port of dav1d)
- https://github.com/xiph/rav1e — rav1e encoder
- https://chromium.googlesource.com/codecs/libgav1/ — libgav1
- https://chromium.googlesource.com/codecs/libgav1/+/refs/heads/main/src/obu_parser.h — libgav1 `ObuParser` (chromium)
- https://android.googlesource.com/platform/external/libgav1/+/refs/heads/main/libgav1/src/obu_parser.h — libgav1 `ObuParser` (AOSP)
- https://aomediacodec.github.io/av1-spec/av1-spec.pdf — AV1 specification PDF
- https://aomedia.org/license/patent-license/ — AOM Patent License 1.0

## F.4 HEVC and H.264 decoders, reference software, parsers

- https://github.com/strukturag/libde265 — libde265
- https://github.com/ChristianFeldmann/libde265 — YUView libde265 internals fork
- https://github.com/ChristianFeldmann/libde265/releases — libde265 internals binaries
- https://github.com/IENT/YUView/blob/develop/YUViewLib/src/decoder/externalHeader/libde265/de265_internals.h — `de265_internals.h` copy in YUView
- https://github.com/kloppjp/pyde265 — pyde265
- https://vcgit.hhi.fraunhofer.de/jvet/HM — HM reference software
- https://github.com/listenlink/HM — HM GitHub mirror used for verification
- https://hevc.hhi.fraunhofer.de/svn/svn_HEVCSoftware/tags/HM-16.20/doc/software-manual.pdf — HM-16.20 software manual (historical; OVT pins HM-18.0, chapter 13 §13.1 — the manual's CLI options are unchanged in 18.0)
- https://github.com/ChristianFeldmann/libHMDecoder — libHMDecoder (YUView HM wrapper)
- https://github.com/ChristianFeldmann/libHM — libHM (YUView HM wrapper, releases)
- https://vcgit.hhi.fraunhofer.de/jvet/VVCSoftware_VTM — VTM
- https://github.com/ChristianFeldmann/VTM — YUView VTM wrapper
- https://www.cnx-software.com/2023/02/13/vvenc-vvdec-h-266-open-source-video-encoder-and-decoder-work-on-x86-and-arm/ — vvenc/vvdec BSD-3 article
- https://github.com/OpenHEVC/openHEVC — openHEVC (dead)
- https://iphome.hhi.de/suehring/tml/ — JM reference software
- https://iphome.hhi.de/suehring/tml/doc/ldec/html/index.html — JM ldecod documentation
- https://iphome.hhi.de/suehring/tml/doc/ldec/html/ldecod_8c_source.html — JM `ldecod.c` source
- https://github.com/cisco/openh264 — openh264
- https://github.com/aizvorski/h264bitstream — h264bitstream
- https://github.com/julee/h264naked — H264Naked
- https://pypi.org/project/h26x-extractor/ — h26x-extractor (Python)
- https://docs.rs/h264-reader — h264-reader (Rust)
- https://github.com/h26forge/h26forge — h26forge
- https://github.com/virinext/hevcesbrowser — hevcesbrowser
- https://github.com/virinext/hevcesbrowser/releases — hevcesbrowser releases
- https://github.com/codec2021/codecscope — CodecScope
- https://github.com/lheric/GitlHEVCAnalyzer — GitlHEVCAnalyzer
- https://github.com/latelee/HEVCBSAnalyzer — HEVCBSAnalyzer
- https://gstreamer.freedesktop.org/documentation/codecparsers/index.html — GStreamer codecparsers
- https://gstreamer.freedesktop.org/documentation/codecparsers/gstav1parser.html — GstAV1Parser
- https://gitlab.freedesktop.org/gstreamer/gst-devtools — gst-devtools (former codecanalyzer)
- https://bugzilla.gnome.org/show_bug.cgi?id=731853 — GStreamer codecanalyzer bug 731853
- https://chromium.googlesource.com/chromium/src/+/main/media/video/ — Chromium `media/video` parsers
- https://github.com/PyAV-Org/PyAV — PyAV
- https://vqeg.github.io/software-tools/quality%20analysis/videoparser-ng — videoparser-ng
- https://www.itu.int/rec/T-REC-H.264 — ITU-T H.264
- https://www.itu.int/rec/T-REC-H.265 — ITU-T H.265

## F.5 YUView and other FOSS viewers

- https://github.com/IENT/YUView — YUView repository
- https://ient.github.io/YUView — YUView site
- https://github.com/IENT/YUView/wiki — YUView wiki
- https://github.com/IENT/YUView/wiki/Statistics-Extension-Syntax — YUView statistics CSV format
- https://github.com/IENT/YUView/wiki/External-Decoders — YUView external decoders
- https://github.com/IENT/YUView/releases — YUView releases
- https://awesome.ecosyste.ms/projects/github.com%2Fient%2Fyuview — YUView activity (ecosyste.ms)
- https://deepwiki.com/IENT/YUView/3.1.1-decoder-system — YUView decoder-system overview
- https://github.com/dryotta/yuvtoolkit — YUVToolkit

## F.6 Containers

- https://github.com/gpac/gpac — GPAC
- https://wiki.gpac.io — GPAC wiki
- https://github.com/gpac/gpac/wiki/mp4box-dump-opts — MP4Box dump options
- https://github.com/gpac/gpac/wiki/custom-boxes — GPAC custom boxes
- https://gpac.io/licensing — GPAC licensing
- https://github.com/gpac/mp4box.js — mp4box.js
- https://gpac.github.io/mp4box.js/test/filereader.html — mp4box.js ISOBMFF box viewer
- https://www.bento4.com/documentation — Bento4 documentation
- https://github.com/axiomatic-systems/Bento4 — Bento4 repository
- https://www.bento4.com/about — Bento4 licensing
- https://github.com/mozilla/mp4parse-rust — mp4parse-rust
- https://crates.io/crates/mp4 — `mp4` crate
- https://docs.rs/mp4box — `mp4box` crate
- https://docs.rs/mp4-atom — `mp4-atom` docs
- https://github.com/kixelated/mp4-atom — `mp4-atom` repository
- https://github.com/Eyevinn/mp4ff — mp4ff
- https://pkg.go.dev/github.com/Eyevinn/mp4ff — mp4ff Go docs
- https://github.com/abema/go-mp4 — go-mp4
- https://www.npmjs.com/package/codem-isoboxer — codem-isoboxer
- https://github.com/l-smash/l-smash — L-SMASH
- https://github.com/enzo1982/mp4v2 — MP4v2
- https://github.com/sannies/mp4parser — mp4parser (Java)
- https://github.com/sannies/isoviewer — ISO Viewer
- https://github.com/DigiDNA/ISOBMFF — DigiDNA ISOBMFF
- https://mp4ra.org/registered-types/boxes — MP4RA boxes registry
- https://github.com/mp4ra/mp4ra.github.io — MP4RA data repository
- https://github.com/MPEGGroup/FileFormatConformance — MPEG FileFormatConformance
- https://mpeggroup.github.io/FileFormatConformance — FileFormatConformance search UI
- https://github.com/MPEGGroup/isobmff-kaitai-experiment — MPEG Kaitai experiment (archived)
- https://github.com/Matroska-Org/libebml — libebml
- https://github.com/ietf-wg-cellar/matroska-specification — Matroska/EBML schema (IETF CELLAR)
- https://mkvtoolnix.download — MKVToolNix (mkvinfo)
- https://github.com/tsduck/tsduck — TSDuck
- https://github.com/EricBerendsen/dvbinspector — DVB Inspector
- https://github.com/bbc/bmx — bmx (MXF)
- https://mediaarea.net/en/MediaInfo — MediaInfo
- https://github.com/MediaArea/MediaInfoLib — MediaInfoLib
- https://mediaarea.net/MediaInfo/License — MediaInfo licence
- https://github.com/quietvoid/dovi_tool — dovi_tool
- https://github.com/quietvoid/hdr10plus_tool — hdr10plus_tool

## F.7 Metrics

- https://github.com/Netflix/vmaf — libvmaf
- https://github.com/Netflix/vmaf/releases — libvmaf releases
- https://github.com/Netflix/vmaf/blob/master/libvmaf/README.md — libvmaf build README
- https://deepwiki.com/Netflix/vmaf/4.4-optimization-techniques — libvmaf optimisation overview (NEON facts)
- https://hackernews-dynamic.seasondane.workers.dev/news/48612057 — VMAF v1 announcement mirror
- https://netflixtechblog.medium.com/feed — Netflix Tech Blog feed
- https://csimagazine.com/csi/netflix-reveals-hdrvmaf-solution.php — HDR-VMAF status (CSI)
- https://pypi.org/project/vmaf-torch — vmaf-torch (PyPI)
- https://arxiv.org/abs/2310.15578 — vmaf-torch paper
- https://github.com/fraunhoferhhi/xpsnr — XPSNR reference plugin
- https://www.itu.int/dms_pub/itu-s/opb/journal/S-JOURNAL-ICTS.V3I1-2020-8-PDF-E.pdf — XPSNR paper (ITU Journal 2020)
- https://github.com/rolinh/VQMT — VQMT mirror
- https://epfl.ch/labs/mmspg/?p=846 — VQMT original (EPFL MMSPG)
- https://gitlab.xiph.org/xiph/daala — Daala tools
- https://github.com/xiph/awcy — AWCY
- https://aomedia.googlesource.com/aom/+/0ad65f40d495fe2d1ba7be8f4cbc396e82db2eb5/tools/convexhull_framework/src/CalcBDRate.py — libaom convex-hull BD-rate script
- https://github.com/FAU-LMS/bjontegaard — bjontegaard (FAU-LMS)
- https://pypi.org/project/bjontegaard — bjontegaard (PyPI)
- https://github.com/slhck/ffmpeg-quality-metrics — ffmpeg-quality-metrics
- https://www.vqeg.org — VQEG
- https://its.ntia.gov/software/ — NTIA VQM software
- https://sourceforge.net/p/hdrvdp/ — HDR-VDP
- https://arxiv.org/abs/2304.13625 — HDR-VDP-3 paper
- https://github.com/gfxdisp/pu21 — PU21
- https://www.cl.cam.ac.uk/~rkm38/pdfs/mantiuk2021_PU21.pdf — PU21 paper
- https://github.com/gfxdisp/ColorVideoVDP — ColorVideoVDP
- https://arxiv.org/abs/2401.11485 — ColorVideoVDP paper
- https://github.com/gfxdisp/FovVideoVDP — FovVideoVDP
- https://github.com/google/uvq — UVQ
- https://huggingface.co/spaces/ychenhq/VideoCrafterXtend/raw/c6204515d60211b445d18ba599d59e059b7c7e79/uvq/README.md — UVQ original TF README mirror
- https://cutcutcodec.readthedocs.io/latest/build/api/cutcutcodec.core.analysis.video.quality.uvq_google.html — UVQ third-party re-implementation
- https://media.withyoutube.com — YouTube-UGC dataset

## F.8 UI toolkits

- https://doc.qt.io/qt-6/qrhiswapchain.html — Qt `QRhiSwapChain` (HDR formats)
- https://www.phoronix.com/news/Qt-color-management-v1 — Qt merges Wayland color-management-v1
- https://doc.qt.io/qt-6.8/android.html — Qt 6.8 for Android
- https://www.qt.io/licensing/open-source-lgpl-obligations — Qt LGPL obligations
- https://bugreports.qt.io/browse/QTBUG-134590 — Qt HDR scene-graph request
- https://bugreports.qt.io/browse/QTBUG-119193 — Qt HDR feature request
- https://lists.qt-project.org/pipermail/interest/2024-June/039755.html — Qt on ChromeOS (interest list)
- https://forum.qt.io/topic/100839/how-to-comply-with-lgpl-on-android — Qt LGPL compliance on Android
- https://github.com/Dax89/QHexView — QHexView
- https://startdebugging.net/de/2026/08/flutter-3-47-impeller-default-renderer-on-desktop/ — Flutter 3.47 Impeller default on desktop
- https://docs.flutter.dev/platform-integration/platform-views — Flutter platform views (Android, HCPP)
- https://docs.flutter.dev/platform-integration/macos/platform-views — Flutter macOS platform views
- https://github.com/flutter/engine/pull/46051 — Flutter wide-gamut engine PR
- https://pub.dev/packages/video_player_hdr — video_player_hdr
- https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties — SDL3 window HDR properties
- https://wiki.libsdl.org/SDL3/SDL_GPUSwapchainComposition — SDL3 GPU swapchain compositions
- https://github.com/ocornut/imgui/wiki/Useful-Extensions — Dear ImGui useful extensions
- https://github.com/epezent/implot — ImPlot
- https://docs.slint.dev/latest/docs/slint/guide/platforms/android — Slint Android
- https://nlnet.nl/project/SlintAndroid/ — NLnet Slint Android grant
- https://wgpu.rs/doc/src/wgpu/documentation/color/hdr_surfaces.rs.html — wgpu HDR surfaces
- https://github.com/emilk/egui/pull/4497 — egui dithering PR (HDR out of scope)
- https://discourse.iced.rs/t/wide-gamut-and-hdr-support/456 — iced HDR discussion
- https://dev.to/yeauty/the-state-of-media-processing-in-rust-2026-what-each-crate-actually-covers-2k2c — Rust media crates state (2026)
- https://developer.chrome.com/blog/new-in-webgpu-129 — Chrome 129 WebGPU HDR
- https://groups.google.com/a/chromium.org/g/blink-dev/c/rBQIRHUEAe8 — WebGPU HDR Intent to Ship
- https://groups.google.com/a/chromium.org/g/blink-dev/c/S7jaeH2J7Rw — Canvas 2D HDR headroom intent
- https://bugs.webkit.org/show_bug.cgi?id=276502 — WebKit HDR canvas bug
- https://bugzilla.mozilla.org/show_bug.cgi?id=HDR — Firefox HDR meta-bug
- https://bugs.kde.org/show_bug.cgi?id=495647 — GTK4 "super bright" on Plasma HDR
- https://pete.lostsource.net/posts/2026-07-09-the-pipe-is-open-nobody-turned-on-the-water.html — Linux HDR adoption commentary
- https://github.com/AvaloniaUI/Avalonia/issues/15503 — Avalonia SkiaSharp 3 tracking
- https://www.neowin.net/amp/compose-multiplatform-19-lands-pushing-web-target-to-beta/ — Compose Multiplatform 1.9

## F.9 Graphics APIs and HDR platforms

- https://developer.apple.com/documentation/metal/using-color-spaces-to-display-hdr-content — Apple: colour spaces for HDR content
- https://developer.apple.com/documentation/metal/performing-your-own-tone-mapping — Apple: performing your own tone mapping
- https://developer.apple.com/documentation/quartzcore/caedrmetadata — Apple `CAEDRMetadata`
- https://github.com/mpv-player/mpv/discussions/14138 — mpv MoltenVK discussion
- https://www.phoronix.com/news/Wayland-CM-HDR-Merged — Wayland color-management-v1 merged
- https://www.phoronix.com/news/Mesa-Vulkan-WSI-HDR-CM — Mesa 25.1 Vulkan WSI colour management
- https://www.phoronix.com/news/GNOME-wp_color_management_v1 — GNOME 48 Mutter colour management
- https://github.com/hyprwm/Hyprland/issues/4933 — Hyprland HDR tracking issue
- https://bugs.kde.org/show_bug.cgi?id=487856 — KDE/Krita HDR discussion
- https://forums.developer.nvidia.com/t/vulkan-extensions-needed-for-hdr-is-missing/334268 — NVIDIA Vulkan HDR extensions thread
- https://github.com/mpv-player/mpv/releases/tag/v0.41.0 — mpv 0.41 release
- https://linuxiac.com/mpv-0-41-media-player-switches-to-gpu-next-renderer/ — mpv 0.41 gpu-next default
- https://github.com/haasn/libplacebo — libplacebo
- https://developer.android.com/media/grow/hdr-playback — Android HDR playback guide
- https://android-developers.googleblog.com/2025/09/hdr-and-user-interfaces.html — Android "HDR and user interfaces" (headroom)
- https://developer.android.com/training/wide-color-gamut — Android wide colour gamut (EGL/Vulkan colourspaces)
- https://source.android.com/docs/core/graphics/hdr-screenshots — Android HDR screenshots
- https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/54708/vulkan-android-mali-gpu-drivers-do-not-expose-other-color-spaces-on-android-devices-with-hdr10-hdr10-display/181934 — Arm forum: Vulkan HDR colourspaces on Android
- https://chromeunboxed.com/hdr-chromebooks-chrome-os-displays-bright-contrast-external-monitor/ — ChromeOS HDR history
- https://developer.android.com/develop/adaptive-apps/guides/chromeos/build-adaptive-apps-for-chromeos — ChromeOS adaptive-app guidance
- https://liliputing.com/google-is-bringing-desktop-windowing-to-android-tablet/ — Android desktop windowing on tablets

## F.10 Licensing, patents, governance

- https://www.gnu.org/licenses/license-list.html — FSF licence list
- https://docs.debricked.com/opentext-core-sca-blogs/blogs/oss-licenses-part-6-license-compatibility-and-dual-licensing — licence compatibility overview
- https://opensource.com/law/11/9/mpl-20-copyleft-and-license-compatibility — MPL-2.0 compatibility
- https://www.engadget.com/2011-01-09-the-gpl-the-app-store-and-you.html — GPL and the App Store (VLC removal)
- https://mailman.videolan.org/pipermail/vlc-devel/2010-November/077072.html — VLC App Store GPL thread
- https://lists.blender.org/pipermail/bf-committers/2011-July/032718.html — Blender on App Store GPL issue
- https://lwn.net/Articles/746733/ — F-Droid (LWN)
- https://www.phoronix.com/news/Fedora-Disable-Bad-VA-API — Fedora disables VA-API H.264/H.265
- https://lwn.net/Articles/911212/ — Fedora Mesa codec removal (LWN)
- https://streaminglearningcenter.com/blogs/hevc-advance-makes-software-royalty-free.html — HEVC Advance software royalty policy
- https://www.streamingmedia.com/Articles/News/Online-Video-News/HEVC-Advance-Makes-Some-Software-Royalty-Free-114938.aspx — HEVC Advance software policy (Streaming Media)
- https://writing.kemitchell.com/2021/07/02/DCO-Not-CLA — DCO not CLA
- https://opensource.com/article/18/3/cla-vs-dco-whats-difference — CLA vs DCO
- https://bestpractices.linuxfoundation.org/_sources/ip/contribution-mechanisms.rst.txt — LF contribution mechanisms
- https://www.linuxfoundation.org/projects/hosting/ — Linux Foundation project hosting
