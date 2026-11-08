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
