// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// PCR/PTS timing checks and the per-PID bitrate (chapter 08 §8.6.7, WI-4.206; ETSI TR 101 290 §5.2.2).

#include "container/mpegts/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <tuple>
#include <utility>

#include "container/mpegts/pes.hpp"
#include "container/mpegts/ts_packets.hpp"
#include "container/tree_fb.hpp"

namespace ovt::container::mpegts {

namespace {

constexpr size_t kPids = 8192;
constexpr uint64_t kRepetitionLimit = 40 * kPcrPerMs;  // TR 101 290 2.3a
constexpr uint64_t kJumpLimit = 100 * kPcrPerMs;       // TR 101 290 2.3b
constexpr double kAccuracyLimitNs = 500.0;             // TR 101 290 2.4
constexpr uint64_t kPtsLimit = 700 * kPtsPerMs;        // TR 101 290 2.5
constexpr uint64_t kTimelineJump = 10000 * kPcrPerMs;  // larger forward steps re-base the bitrate time axis

}  // namespace

std::string format_pid(uint16_t pid) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  std::string s = "0x0000";
  for (size_t i = 0; i < 4; ++i) {
    s[5 - i] = digits[(pid >> (4U * i)) & 15U];
  }
  return s;
}

TimingBuilder::TimingBuilder(TimingSink sink) : sink_(std::move(sink)), pcr_state_(kPids), pts_state_(kPids) {}

void TimingBuilder::packet(uint16_t pid, bool has_pcr, uint64_t pcr, bool discontinuity, uint64_t packet_index,
                           uint64_t offset) {
  pid &= 0x1FFFU;
  timing_.packet_pid.push_back(pid);
  if (discontinuity) {
    pts_state_[pid].valid = false;
  }
  if (!has_pcr) {
    return;
  }
  PcrState& s = pcr_state_[pid];
  if (s.stream < 0) {
    s.stream = static_cast<int32_t>(timing_.pcr.size());
    timing_.pcr.push_back(PcrStream{pid, {}, 0, 0, 0});
  }
  PcrStream& st = timing_.pcr[static_cast<size_t>(s.stream)];
  st.points.push_back({packet_index, pcr});
  if (!s.valid || discontinuity) {
    s.valid = true;
    s.have_prev = false;
  } else {
    check_pcr(s, st, pcr, offset);
  }
  s.last = pcr;
  s.last_offset = offset;
}

void TimingBuilder::check_pcr(PcrState& s, PcrStream& st, uint64_t pcr, uint64_t offset) {
  const uint64_t delta = (pcr + kPcrWrap - s.last) % kPcrWrap;
  const std::string on_pid = " on PID `" + format_pid(st.pid) + "`";
  bool ok = true;
  if (delta >= kPcrWrap / 2) {
    ++st.discontinuity_errors;
    ok = false;
    sink_(DiagCode::CNT_TS_2004,
          "PCR_discontinuity_indicator_error: PCR jump `-" + std::to_string((kPcrWrap - delta) / kPcrPerMs) +
              "` ms without `discontinuity_indicator`" + on_pid,
          offset);
  } else {
    if (delta > kRepetitionLimit) {
      ++st.repetition_errors;
      ok = false;
      sink_(DiagCode::CNT_TS_2003,
            "PCR_repetition_error: `" + std::to_string(delta / kPcrPerMs) + "` ms between PCRs" + on_pid + " (> 40 ms)",
            offset);
    }
    if (delta > kJumpLimit) {
      ++st.discontinuity_errors;
      sink_(DiagCode::CNT_TS_2004,
            "PCR_discontinuity_indicator_error: PCR jump `" + std::to_string(delta / kPcrPerMs) +
                "` ms without `discontinuity_indicator`" + on_pid,
            offset);
    }
  }
  if (ok && s.have_prev) {
    check_accuracy(s, st, delta, offset);
  }
  s.have_prev = ok;
  s.prev = s.last;
  s.prev_offset = s.last_offset;
}

// The middle PCR of three (prev, last, current) against the straight line through the outer two over byte
// position; all three intervals are within 40 ms here, so the products stay far inside a double's exact range.
void TimingBuilder::check_accuracy(const PcrState& s, PcrStream& st, uint64_t delta, uint64_t offset) {
  if (offset <= s.prev_offset || s.last_offset <= s.prev_offset) {
    return;
  }
  const uint64_t first = (s.last + kPcrWrap - s.prev) % kPcrWrap;
  const double span = static_cast<double>(first + delta);
  const auto where = static_cast<double>(s.last_offset - s.prev_offset) / static_cast<double>(offset - s.prev_offset);
  const double deviation_ns = (static_cast<double>(first) - span * where) * 1000.0 / 27.0;
  if (std::fabs(deviation_ns) <= kAccuracyLimitNs) {
    return;
  }
  ++st.accuracy_errors;
  if (st.accuracy_errors <= kMaxAccuracyDiagnostics) {
    sink_(DiagCode::CNT_TS_2005,
          "PCR_accuracy_error: `" + std::to_string(std::llround(deviation_ns)) + "` ns deviation on PID `" +
              format_pid(st.pid) + "` (> ±500 ns)",
          s.last_offset);
  }
}

void TimingBuilder::pes_start(uint16_t pid, int64_t pts, uint64_t offset) {
  PtsState& s = pts_state_[pid & 0x1FFFU];
  if (pts < 0) {
    return;
  }
  const auto v = static_cast<uint64_t>(pts);
  if (s.valid) {
    const uint64_t delta = (v + kTimestampWrap - s.last) % kTimestampWrap;
    if (delta >= kTimestampWrap / 2) {
      return;  // earlier than the latest PTS (reordering): not compared
    }
    if (delta > kPtsLimit) {
      ++timing_.pts_errors;
      sink_(DiagCode::CNT_TS_2006,
            "PTS_error: `" + std::to_string(delta / kPtsPerMs) + "` ms between PTS on PID `" + format_pid(pid) +
                "` (> 700 ms)",
            offset);
    }
  }
  s.valid = true;
  s.last = v;
}

Timing TimingBuilder::finish() {
  std::sort(timing_.pcr.begin(), timing_.pcr.end(),
            [](const PcrStream& a, const PcrStream& b) { return a.pid < b.pid; });
  return std::move(timing_);
}

namespace {

struct TimePoint {
  uint64_t packet_index = 0;
  double t = 0;  // 27 MHz ticks from the first point
};

// The PCR points as a strictly increasing time axis: wraps are unwrapped, and a step back or a step of more than
// kTimelineJump continues at the rate of the previous segment.
std::vector<TimePoint> timeline(const std::vector<PcrPoint>& pcr) {
  std::vector<TimePoint> out;
  for (size_t i = 0; i < pcr.size(); ++i) {
    if (out.empty()) {
      out.push_back({pcr[i].packet_index, 0});
      continue;
    }
    const TimePoint last = out.back();
    if (pcr[i].packet_index <= last.packet_index) {
      continue;
    }
    const uint64_t delta = (pcr[i].pcr + kPcrWrap - pcr[i - 1].pcr) % kPcrWrap;
    const auto dk = static_cast<double>(pcr[i].packet_index - last.packet_index);
    if (delta > 0 && delta <= kTimelineJump) {
      out.push_back({pcr[i].packet_index, last.t + static_cast<double>(delta)});
    } else if (out.size() >= 2) {
      const TimePoint prev = out[out.size() - 2];
      const double rate = (last.t - prev.t) / static_cast<double>(last.packet_index - prev.packet_index);
      out.push_back({pcr[i].packet_index, last.t + rate * dk});
    } else {
      out = {{pcr[i].packet_index, 0}};
    }
  }
  return out;
}

}  // namespace

std::vector<PidBitrate> compute_bitrates(const std::vector<uint16_t>& packet_pid, const std::vector<PcrPoint>& pcr,
                                         uint32_t packet_bytes) {
  const std::vector<TimePoint> axis = timeline(pcr);
  if (axis.size() < 2 || packet_pid.empty()) {
    return {};
  }
  // Time of packet k: linear in packet index inside a segment, extrapolated with the first / last segment.
  size_t seg = 0;
  const auto time_of = [&axis, &seg](uint64_t k) {
    while (seg + 2 < axis.size() && k >= axis[seg + 1].packet_index) {
      ++seg;
    }
    const TimePoint& a = axis[seg];
    const TimePoint& b = axis[seg + 1];
    const double rate = (b.t - a.t) / static_cast<double>(b.packet_index - a.packet_index);
    return a.t + rate * (static_cast<double>(k) - static_cast<double>(a.packet_index));
  };
  const double t0 = time_of(0);
  std::vector<double> rel(packet_pid.size());
  for (size_t k = 0; k < packet_pid.size(); ++k) {
    rel[k] = time_of(k) - t0;
  }
  const double total = rel.back();
  if (!(total >= 0)) {
    return {};
  }

  std::vector<int32_t> slot(kPids, -1);
  std::vector<uint16_t> pids;
  for (const uint16_t pid : packet_pid) {
    if (slot[pid] < 0) {
      slot[pid] = 0;
      pids.push_back(pid);
    }
  }
  std::sort(pids.begin(), pids.end());
  for (size_t i = 0; i < pids.size(); ++i) {
    slot[pids[i]] = static_cast<int32_t>(i);
  }

  std::vector<PidBitrate> out;
  for (const uint32_t window : kBitrateWindowsMs) {
    const double width = static_cast<double>(window) * static_cast<double>(kPcrPerMs);
    const double windows = total / width + 1;
    if (windows * static_cast<double>(pids.size()) > static_cast<double>(kMaxBitrateCells)) {
      continue;
    }
    const auto bins = static_cast<size_t>(windows);
    std::vector<std::vector<uint32_t>> packets(pids.size(), std::vector<uint32_t>(bins, 0));
    for (size_t k = 0; k < packet_pid.size(); ++k) {
      const auto bin = std::min(bins - 1, static_cast<size_t>(std::max(0.0, rel[k]) / width));
      ++packets[static_cast<size_t>(slot[packet_pid[k]])][bin];
    }
    const uint64_t bits_per_packet = uint64_t{packet_bytes} * 8U;
    for (size_t i = 0; i < pids.size(); ++i) {
      PidBitrate b{pids[i], window, std::vector<uint32_t>(bins)};
      for (size_t j = 0; j < bins; ++j) {
        b.bps[j] =
            static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX, packets[i][j] * bits_per_packet * 1000U / window));
      }
      out.push_back(std::move(b));
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const PidBitrate& a, const PidBitrate& b) { return a.pid < b.pid; });
  return out;
}

std::vector<uint8_t> build_ts_timing(const File& f) {
  treefb::Fbb fbb;
  std::vector<PcrPoint> points;
  for (const PcrStream& s : f.timing.pcr) {
    if (s.pid == f.pcr_pid) {
      points = s.points;
    }
  }
  std::vector<flatbuffers::Offset<fb::TsPcrPoint>> pcr;
  pcr.reserve(points.size());
  for (const PcrPoint& p : points) {
    pcr.push_back(fb::CreateTsPcrPoint(fbb, p.packet_index, p.pcr));
  }

  struct Pts {
    uint64_t packet_index;
    uint16_t pid;
    int64_t pts;
    int64_t dts;
  };
  std::vector<Pts> all;
  for (const PesStream& s : f.pes) {
    for (const PesPacket& p : s.packets) {
      all.push_back({p.first_packet, s.pid, p.pts, p.dts});
    }
  }
  std::sort(all.begin(), all.end(), [](const Pts& a, const Pts& b) {
    return std::tie(a.packet_index, a.pid) < std::tie(b.packet_index, b.pid);
  });
  std::vector<flatbuffers::Offset<fb::TsPtsPoint>> pts;
  pts.reserve(all.size());
  for (const Pts& p : all) {
    pts.push_back(fb::CreateTsPtsPoint(fbb, p.pid, p.packet_index, p.pts, p.dts));
  }

  std::vector<flatbuffers::Offset<fb::TsPidBitrate>> bitrate;
  for (const PidBitrate& b : compute_bitrates(f.timing.packet_pid, points, kPacketBytes)) {
    bitrate.push_back(fb::CreateTsPidBitrate(fbb, b.pid, b.window_ms, fbb.CreateVector(b.bps)));
  }
  fbb.Finish(fb::CreateTsTiming(fbb, fbb.CreateVector(pcr), fbb.CreateVector(pts), fbb.CreateVector(bitrate), f.pcr_pid,
                                static_cast<uint8_t>(f.geometry.stride)));
  const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
  return {bytes.begin(), bytes.end()};
}

}  // namespace ovt::container::mpegts
