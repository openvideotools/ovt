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
