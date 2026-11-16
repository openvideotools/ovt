// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The PCR/PTS timing checks and the derived per-PID bitrate of the MPEG-TS reader (chapter 08 §8.6.7, WI-4.206).
// Source of truth: ISO/IEC 13818-1 §2.4.2.2 (PCR) and ETSI TR 101 290 §5.2.2 checks 2.3a, 2.3b, 2.4 and 2.5.
//
// SPEC-ASSUMPTION(#569): a file has no arrival clock, so the checks use the PCR / PTS values themselves, as the TR
// words them: two consecutive PCRs of a PID more than 40 ms apart are CNT-TS-2003; a PCR that goes back, or more
// than 100 ms forward, without discontinuity_indicator is CNT-TS-2004 (a 1 s gap of missing PCRs is both);
// CNT-TS-2005 compares each PCR with the straight line through its two neighbours over byte position (at most
// kMaxAccuracyDiagnostics per PID are listed, the count stays exact); CNT-TS-2006 compares each PTS with the
// latest earlier one of the PID (a PTS that goes back, as with B-frames, is not compared). The bitrate time axis is
// the PCR of `pcr_pid`, interpolated linearly over packet index and extrapolated at both ends.

#ifndef OVT_CONTAINER_MPEGTS_TIMING_HPP
#define OVT_CONTAINER_MPEGTS_TIMING_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "container/diagnostics.h"

namespace ovt::container::mpegts {

inline constexpr uint64_t kPcrPerMs = 27000;                                       // 27 MHz ticks per millisecond
inline constexpr uint64_t kPtsPerMs = 90;                                          // 90 kHz ticks per millisecond
inline constexpr std::array<uint32_t, 4> kBitrateWindowsMs = {10, 100, 250, 500};  // FR-CNT-016
inline constexpr uint32_t kMaxAccuracyDiagnostics = 100;                           // CNT-TS-2005 entries listed per PID
inline constexpr uint64_t kMaxBitrateCells = uint64_t{1} << 24U;  // PIDs × windows; a larger window size is left out

// "0x01AB": the PID notation of the CNT-TS messages.
[[nodiscard]] std::string format_pid(uint16_t pid);

struct PcrPoint {
  uint64_t packet_index = 0;
  uint64_t pcr = 0;  // 27 MHz
};

struct PcrStream {
  uint16_t pid = 0;
  std::vector<PcrPoint> points;
  uint64_t repetition_errors = 0;     // CNT-TS-2003
  uint64_t discontinuity_errors = 0;  // CNT-TS-2004
  uint64_t accuracy_errors = 0;       // CNT-TS-2005
};

struct Timing {
  std::vector<PcrStream> pcr;        // PIDs that carry a PCR, ascending
  std::vector<uint16_t> packet_pid;  // PID of every transport packet, by packet index
  uint64_t pts_errors = 0;           // CNT-TS-2006
};

struct PidBitrate {
  uint16_t pid = 0;
  uint32_t window_ms = 0;
  std::vector<uint32_t> bps;  // bits per second of each window, from the time of packet 0
};

using TimingSink = std::function<void(DiagCode code, std::string message, uint64_t offset)>;

class TimingBuilder {
 public:
  explicit TimingBuilder(TimingSink sink);

  // Every transport packet in file order. `has_pcr` with `pcr` (27 MHz) when its adaptation field carries one;
  // `discontinuity` is its discontinuity_indicator (it restarts the PCR and PTS checks of the PID); `offset` the
  // file offset of its sync byte.
  void packet(uint16_t pid, bool has_pcr, uint64_t pcr, bool discontinuity, uint64_t packet_index, uint64_t offset);

  // A PES of `pid` began with timestamp `pts` (-1 absent) in the packet at `offset`.
  void pes_start(uint16_t pid, int64_t pts, uint64_t offset);

  [[nodiscard]] Timing finish();

 private:
  struct PcrState {
    bool valid = false;
    bool have_prev = false;
    uint64_t last = 0;
    uint64_t last_offset = 0;
    uint64_t prev = 0;
    uint64_t prev_offset = 0;
    int32_t stream = -1;  // index into timing_.pcr
  };
  struct PtsState {
    bool valid = false;
    uint64_t last = 0;
  };
  TimingSink sink_;
  Timing timing_;
  std::vector<PcrState> pcr_state_;
  std::vector<PtsState> pts_state_;

  void check_pcr(PcrState& s, PcrStream& st, uint64_t pcr, uint64_t offset);
  void check_accuracy(const PcrState& s, PcrStream& st, uint64_t delta, uint64_t offset);
};

// Bitrate per PID and window (kBitrateWindowsMs) on the time axis of `pcr` (the points of one PID). Empty when
// there are fewer than two usable PCRs; a window size that needs more than kMaxBitrateCells values over all PIDs is
// left out (2 h at 10 ms is 720 000 windows per PID).
[[nodiscard]] std::vector<PidBitrate> compute_bitrates(const std::vector<uint16_t>& packet_pid,
                                                       const std::vector<PcrPoint>& pcr, uint32_t packet_bytes);

}  // namespace ovt::container::mpegts

#endif  // OVT_CONTAINER_MPEGTS_TIMING_HPP
