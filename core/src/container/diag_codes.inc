// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The CNT-* diagnostic registry (chapter 08 §8.5), one X-macro row per code; a code appears exactly once.
// X(identifier, "code", Severity, "message template", "spec reference")
// SPEC-ASSUMPTION(#138): the fifth column (spec reference) is an addition to the four-column example of §8.5.
// WI-1.200 adds the structure/walk codes CNT-0001..CNT-0007; later work items append their own rows.

X(CNT_0001, "CNT-0001", Info, "unit source policy for this stream (own reader vs libavformat)", "")
X(CNT_0002, "CNT-0002", Warning, "first box is not `ftyp`/`styp` (QuickTime without `ftyp` is legal)", "14496-12 §4.3")
X(CNT_0003, "CNT-0003", Info, "`moov` after `mdat` (not progressive/fast-start)", "")
X(CNT_0004, "CNT-0004", Error, "no `moov` and no `moof`", "14496-12 §8.2.1")
X(CNT_0005, "CNT-0005", Info, "fragment-only stream without `moov` (needs init segment)", "23000-19")
X(CNT_0006, "CNT-0006", Fatal,
  "file shorter than 8 bytes or first 8 bytes not a plausible box header after sniffing as ISOBMFF", "")
X(CNT_0007, "CNT-0007", Info,
  "box at offset `o` not yet expanded (lazy; call `ovt_container_expand`)", "")
X(CNT_IVF_0001, "CNT-IVF-0001", Info, "IVF `header_size` is `N` (≠ 32); first frame taken at that offset", "libvpx ivfdec.c")
X(CNT_IVF_0002, "CNT-IVF-0002", Info, "IVF header `frame_count` `N` differs from the walked count `M`", "libvpx ivfdec.c")
X(CNT_IVF_0003, "CNT-IVF-0003", Warning,
  "last IVF frame truncated: `frame_size` `N` exceeds EOF by `M` bytes (clamped)", "")

// WI-1.226 (Annex B splitter, §8.6.3).
X(CNT_ESB_0001, "CNT-ESB-0001", Warning, "`N` bytes of garbage before the first start code / after the last NAL unit",
  "H.264 clause B.1.1")
X(CNT_ESB_0002, "CNT-ESB-0002", Info, "more than 8 `trailing_zero_8bits` after NAL unit `n`", "H.264 clause B.1.2")
X(CNT_ESB_0003, "CNT-ESB-0003", Warning, "empty NAL unit after start code at offset `o`", "H.264 clause B.1.2")
X(CNT_ESB_0004, "CNT-ESB-0004", Info,
  "3-byte start code before SPS/PPS/VPS/AUD/first NAL of an access unit (`zero_byte` required)",
  "H.264 clause B.1.2, H.265 clause B.1.2")
X(CNT_ESB_0005, "CNT-ESB-0005", Error,
  "`forbidden_zero_bit` set in NAL header at offset `o` (false start code or corruption)", "H.264 §7.4.1")

// WI-1.227 (raw AV1 OBU reader, §8.6.4).
X(CNT_OBU_0001, "CNT-OBU-0001", Error,
  "OBU at offset `o` has `obu_has_size_field == 0` outside Annex B framing (undecodable); open fails with "
  "`OVT_E_UNSUPPORTED`",
  "AV1 §5.3.1")
X(CNT_OBU_0002, "CNT-OBU-0002", Info,
  "both Section 5 and Annex B framings parse; `framing` chosen by the first-byte rule", "AV1 §5.2, Annex B")
// SPEC-ASSUMPTION(#279): §8.5 has no row for a length that overruns its enclosing unit or for a malformed OBU
// header / leb128; CNT-OBU-0003 and CNT-OBU-0004 are the proposed rows.
X(CNT_OBU_0003, "CNT-OBU-0003", Warning,
  "length at offset `o` exceeds the bytes left in the file or enclosing unit; unit clamped to `N` bytes",
  "AV1 §5.3, Annex B")
X(CNT_OBU_0004, "CNT-OBU-0004", Error,
  "OBU header, `leb128` or length at offset `o` is malformed; the OBU or the rest of its enclosing unit is skipped",
  "AV1 §4.10.5, §5.3.2")
X(CNT_TS_1001, "CNT-TS-1001", Error,
  "TS_sync_loss: `N` consecutive packets without `0x47` at offset `o`", "TR 101 290 §5.2.1 (1.1)")
X(CNT_TS_1002, "CNT-TS-1002", Error, "Sync_byte_error at packet `k`", "TR 101 290 §5.2.1 (1.2)")
X(CNT_TS_1006, "CNT-TS-1006", Warning,
  "Continuity_count_error on PID `pid` at packet `k` (`expected`/`got`)", "TR 101 290 §5.2.1 (1.4)")
X(CNT_TS_2001, "CNT-TS-2001", Warning,
  "Transport_error: `transport_error_indicator` set at packet `k`", "TR 101 290 §5.2.2 (2.1)")

// WI-1.228 (Y4M reader, §8.6.5).
X(CNT_Y4M_0001, "CNT-Y4M-0001", Info, "Y4M header without `F` tag; 25:1 assumed", "yuv4mpeg(5)")
X(CNT_Y4M_0002, "CNT-Y4M-0002", Error, "Y4M `C` tag `value` unknown; open fails with `OVT_E_UNSUPPORTED`",
  "yuv4mpeg(5), FFmpeg `yuv4mpegdec.c`")
X(CNT_Y4M_0003, "CNT-Y4M-0003", Error, "`FRAME` magic missing at offset `o` (frame `i`); walk stopped", "yuv4mpeg(5)")
// WI-1.231 (unit splitter, §8.7.4). CNT-9001 is raised as a Warning for a zero-length NAL (§8.7.4 rule 1).
X(CNT_9001, "CNT-9001", Error,
  "sample bytes do not split into units cleanly (NAL length exceeds sample, OBU size overrun)",
  "14496-15 §5.3.4.2 / AV1-ISOBMFF §2.4")
X(CNT_9002, "CNT-9002", Warning, "Annex B start codes found inside an `avc1`/`hvc1` sample (muxer bug)",
  "14496-15 §4.3")
X(CNT_9003, "CNT-9003", Warning, "AV1 `OBU_TEMPORAL_DELIMITER` inside a sample", "AV1-ISOBMFF §2.4")
