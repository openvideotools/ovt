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

// WI-1.201 (ISOBMFF box header walk, §8.2.3).
X(CNT_0101, "CNT-0101", Warning, "trailing bytes in a container that cannot hold a box header", "14496-12 §4.2")
X(CNT_0102, "CNT-0102", Error, "truncated `largesize`", "14496-12 §4.2")
X(CNT_0103, "CNT-0103", Error, "`largesize < 16`", "14496-12 §4.2")
X(CNT_0104, "CNT-0104", Warning, "`size == 0` box not at top level", "14496-12 §4.2.2")
X(CNT_0105, "CNT-0105", Error, "nesting deeper than 32", "")
X(CNT_0106, "CNT-0106", Error, "`size < 8` (negative / tiny size)", "14496-12 §4.2")
X(CNT_0107, "CNT-0107", Error, "`uuid` box too short for usertype", "14496-12 §4.2")
X(CNT_0108, "CNT-0108", Error, "box exceeds parent/file end (truncated)", "14496-12 §4.2")
X(CNT_0109, "CNT-0109", Warning, "non-printable box type", "")
X(CNT_0110, "CNT-0110", Error, "FullBox shorter than 4 bytes", "14496-12 §4.2")
X(CNT_1006, "CNT-1006", Info, "QuickTime-style `meta` without version/flags", "QTFF")
