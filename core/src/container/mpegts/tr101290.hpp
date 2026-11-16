// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The ETSI TR 101 290 v1.4.1 priority 1 (§5.2.1) and priority 2 (§5.2.2) report of an MPEG-TS file (chapter 08
// §8.6.7, WI-4.207): the counts and first occurrences of the CNT-TS-1xxx / 2xxx checks as the `TsReport` table of
// chapter 06 §6.4. Priority 3 checks are out of scope. Public material only: the TR text and ISO/IEC 13818-1.
//
// Timer semantics (WI-4.207 step 1). A file has no arrival clock, so every timer runs on the PCR time axis of the
// file (TimeAxis of timing.hpp, over the PCRs of `File::pcr_pid`); without two usable PCRs the timer checks are
// not evaluated and only the structural ones count. The timers are:
//  - 1.3 PAT_error_2: no PAT section on PID 0 for more than `psi_timeout_ms` (0.5 s), from the first packet;
//  - 1.5 PMT_error_2: no PMT section on a PMT PID for more than 0.5 s, from the PAT section that named the PID;
//  - 1.6 PID_error: an elementary-stream PID of the program map without a packet for more than `pid_timeout_ms`
//    (5 s, user-configurable), from the first PMT section of its program; a PID with no packet at all stays the
//    whole-file form CNT-TS-1007 of the PSI parser;
//  - 2.6 CAT_error: scrambled packets (outside PIDs 0 and 1) without a CAT section for more than 0.5 s, within
//    the span of the scrambled packets; a file with scrambled packets and no CAT at all stays the whole-file form
//    CNT-TS-2007 of the PSI parser.
// Each exceeded interval is one error, located at the packet that starts it. Timer errors are listed in the
// report's diagnostics only; the container diagnostics list is unchanged.
//
// SPEC-ASSUMPTION(#593): the timer rules above and check 2.3 PCR_error, which counts the PCR intervals that have
// 2.3a or 2.3b or both (a 1 s PCR gap is one 2.3 error and one each of 2.3a and 2.3b), are the answers proposed
// in that issue.

#ifndef OVT_CONTAINER_MPEGTS_TR101290_HPP
#define OVT_CONTAINER_MPEGTS_TR101290_HPP

#include <cstdint>

namespace ovt::container::mpegts {

inline constexpr uint32_t kPsiTimeoutMs = 500;   // TR 101 290: PAT / PMT / CAT repetition limit
inline constexpr uint32_t kPidTimeoutMs = 5000;  // default PID_error timeout; user-configurable

struct Tr101290Options {
  uint32_t pid_timeout_ms = kPidTimeoutMs;
  uint32_t psi_timeout_ms = kPsiTimeoutMs;
};

}  // namespace ovt::container::mpegts

#endif  // OVT_CONTAINER_MPEGTS_TR101290_HPP
