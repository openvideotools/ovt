// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The TR 101 290 priority 1 and 2 report (chapter 08 §8.6.7, WI-4.207); semantics in tr101290.hpp.

#include "container/mpegts/tr101290.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "container/mpegts/ts_packets.hpp"
#include "container/tree_fb.hpp"

namespace ovt::container::mpegts {

namespace {

struct Row {
  const char* id;
  const char* name;
  uint64_t count = 0;
  bool seen = false;
  uint64_t first_offset = 0;

  void add(uint64_t n, uint64_t offset) {
    if (n == 0) {
      return;
    }
    count += n;
    if (!seen || offset < first_offset) {
      first_offset = offset;
      seen = true;
    }
  }
};

enum RowIndex : size_t { k11, k12, k13, k14, k15, k16, k21, k22, k23, k23a, k23b, k24, k25, k26, kRows };

// Packet index of the packet that holds file offset `off` (the next packet when `off` is in a sync-loss gap).
uint64_t packet_index_at(const File& f, uint64_t off) {
  if (f.runs.empty()) {
    return 0;
  }
  const uint64_t prefix = f.geometry.prefix;
  const auto it = std::upper_bound(f.runs.begin(), f.runs.end(), off,
                                   [prefix](uint64_t o, const Run& r) { return o < r.sync_offset - prefix; });
  if (it == f.runs.begin()) {
    return 0;
  }
  const Run& r = *std::prev(it);
  const uint64_t index = (off - (r.sync_offset - prefix)) / f.geometry.stride;
  if (index < r.count) {
    return r.first_packet + index;
  }
  return it != f.runs.end() ? it->first_packet : r.first_packet + r.count - 1;
}

// Collects the report: rows, plus the diagnostics of the timer checks.
class Builder {
 public:
  Builder(const File& f, const Tr101290Options& o) : f_(f), opt_(o), rows_(make_rows()) {
    const PcrStream* pcr = nullptr;
    for (const PcrStream& s : f.timing.pcr) {
      if (s.pid == f.pcr_pid) {
        pcr = &s;
      }
    }
    if (pcr != nullptr) {
      axis_ = TimeAxis(pcr->points);
    }
  }

  std::vector<uint8_t> build() {
    structural();
    if (axis_.valid() && f_.packet_count > 0) {
      timers();
    }
    return serialise();
  }

 private:
  const File& f_;
  Tr101290Options opt_;
  std::array<Row, kRows> rows_;
  TimeAxis axis_{std::vector<PcrPoint>{}};
  std::vector<Diagnostic> timer_diags_;

  static std::array<Row, kRows> make_rows() {
    return {{{"1.1", "TS_sync_loss"},
             {"1.2", "Sync_byte_error"},
             {"1.3", "PAT_error_2"},
             {"1.4", "Continuity_count_error"},
             {"1.5", "PMT_error_2"},
             {"1.6", "PID_error"},
             {"2.1", "Transport_error"},
             {"2.2", "CRC_error"},
             {"2.3", "PCR_error"},
             {"2.3a", "PCR_repetition_error"},
             {"2.3b", "PCR_discontinuity_indicator_error"},
             {"2.4", "PCR_accuracy_error"},
             {"2.5", "PTS_error"},
             {"2.6", "CAT_error"}}};
  }

  // Offset of the first diagnostic of `code`: the walker's, or the PSI parser's, whichever is earlier.
  [[nodiscard]] uint64_t first_of(DiagCode code) const {
    uint64_t best = UINT64_MAX;
    if (const auto it = f_.first_offsets.find(code); it != f_.first_offsets.end()) {
      best = it->second;
    }
    for (const PsiDiagnostic& d : f_.psi.diagnostics) {
      if (d.code == code) {
        best = std::min(best, d.offset);
        break;  // in emission order
      }
    }
    return best == UINT64_MAX ? 0 : best;
  }

  [[nodiscard]] uint64_t psi_count(DiagCode code) const {
    const auto it = f_.psi.diag_counts.find(code);
    return it == f_.psi.diag_counts.end() ? 0 : it->second;
  }

  // The checks that need no clock; counts come from the exact counters, not from the capped diagnostic lists.
  void structural() {
    rows_[k11].add(f_.sync_loss_events, first_of(DiagCode::CNT_TS_1001));
    rows_[k12].add(f_.sync_byte_errors, first_of(DiagCode::CNT_TS_1002));
    rows_[k13].add(psi_count(DiagCode::CNT_TS_1003), first_of(DiagCode::CNT_TS_1003));
    uint64_t cc = 0;
    uint64_t te = 0;
    for (const PidStats& p : f_.pids) {
      cc += p.continuity_errors;
      te += p.transport_errors;
    }
    rows_[k14].add(cc, first_of(DiagCode::CNT_TS_1006));
    rows_[k15].add(psi_count(DiagCode::CNT_TS_1005), first_of(DiagCode::CNT_TS_1005));
    rows_[k16].add(psi_count(DiagCode::CNT_TS_1007), first_of(DiagCode::CNT_TS_1007));
    rows_[k21].add(te, first_of(DiagCode::CNT_TS_2001));
    rows_[k22].add(f_.psi.crc_errors, first_of(DiagCode::CNT_TS_2002));
    uint64_t rep = 0;
    uint64_t jump = 0;
    uint64_t acc = 0;
    uint64_t any = 0;
    for (const PcrStream& s : f_.timing.pcr) {
      rep += s.repetition_errors;
      jump += s.discontinuity_errors;
      acc += s.accuracy_errors;
      any += s.pcr_errors;
    }
    rows_[k23a].add(rep, first_of(DiagCode::CNT_TS_2003));
    rows_[k23b].add(jump, first_of(DiagCode::CNT_TS_2004));
    rows_[k23].add(any, std::min(first_of(DiagCode::CNT_TS_2003), first_of(DiagCode::CNT_TS_2004)));
    rows_[k24].add(acc, first_of(DiagCode::CNT_TS_2005));
    rows_[k25].add(f_.timing.pts_errors, first_of(DiagCode::CNT_TS_2006));
    rows_[k26].add(psi_count(DiagCode::CNT_TS_2007), first_of(DiagCode::CNT_TS_2007));
  }

  [[nodiscard]] double ms_between(uint64_t a, uint64_t b) const {
    return (axis_.ticks_at(b) - axis_.ticks_at(a)) / static_cast<double>(kPcrPerMs);
  }

  void timer_error(RowIndex row, DiagCode code, const std::string& what, uint64_t from, uint64_t to,
                   uint32_t limit_ms) {
    const uint64_t offset = f_.sync_offset(from);
    rows_[row].add(1, offset);
    if (timer_diags_.size() < kMaxDiagnostics) {
      timer_diags_.push_back({code,
                              what + " for `" + std::to_string(static_cast<uint64_t>(ms_between(from, to))) +
                                  "` ms after packet `" + std::to_string(from) + "` (> " + std::to_string(limit_ms) +
                                  " ms)",
                              offset});
    }
  }

  // Intervals longer than `limit_ms` between consecutive events `points` (ascending), clipped to [start, end].
  // The timer starts at `start`, or at the last event before it.
  template <class F>
  void for_gaps(const std::vector<uint64_t>& points, uint64_t start, uint64_t end, uint32_t limit_ms, F&& on_gap) {
    if (start > end) {
      return;
    }
    uint64_t prev = start;
    for (const uint64_t p : points) {
      if (p <= start) {
        prev = p;
        continue;
      }
      if (p > end) {
        break;
      }
      if (ms_between(prev, p) > limit_ms) {
        on_gap(prev, p);
      }
      prev = p;
    }
    if (prev < end && ms_between(prev, end) > limit_ms) {
      on_gap(prev, end);
    }
  }

  void timers() {
    const uint64_t last = f_.packet_count - 1;
    const auto arrivals = [this](uint16_t pid) -> const std::vector<uint64_t>& {
      static const std::vector<uint64_t> none;
      const auto it = f_.psi.arrivals.find(pid);
      return it == f_.psi.arrivals.end() ? none : it->second;
    };
    const uint32_t psi = opt_.psi_timeout_ms;

    for_gaps(arrivals(kPatPid), 0, last, psi, [&](uint64_t a, uint64_t b) {
      timer_error(k13, DiagCode::CNT_TS_1003, "PAT_error_2: no PAT section on PID `0x0000`", a, b, psi);
    });

    std::vector<uint16_t> es_pids;
    std::vector<uint64_t> es_start;
    for (const Program& p : f_.psi.programs) {
      const auto named = f_.psi.pmt_named_at.find(p.pmt_pid);
      if (named == f_.psi.pmt_named_at.end()) {
        continue;
      }
      const std::vector<uint64_t>& a = arrivals(p.pmt_pid);
      const uint16_t pmt_pid = p.pmt_pid;
      for_gaps(a, named->second, last, psi, [&](uint64_t from, uint64_t to) {
        timer_error(k15, DiagCode::CNT_TS_1005, "PMT_error_2: no PMT section on PID `" + format_pid(pmt_pid) + "`",
                    from, to, psi);
      });
      // The PID timeout of the program's elementary streams runs from its first PMT section.
      if (p.has_pmt && !a.empty()) {
        for (const EsInfo& es : p.streams) {
          if (es.pid < kPidCount && es.pid != kNullPid &&
              std::find(es_pids.begin(), es_pids.end(), es.pid) == es_pids.end()) {
            es_pids.push_back(es.pid);
            es_start.push_back(a.front());
          }
        }
      }
    }
    if (f_.first_scrambled <= f_.last_scrambled && !arrivals(kCatPid).empty()) {
      for_gaps(arrivals(kCatPid), f_.first_scrambled, f_.last_scrambled, psi, [&](uint64_t a, uint64_t b) {
        timer_error(k26, DiagCode::CNT_TS_2007, "CAT_error: scrambled packets without a CAT section on PID `0x0001`", a,
                    b, psi);
      });
    }
    pid_timeouts(es_pids, es_start, last);
  }

  void pid_timeouts(const std::vector<uint16_t>& pids, const std::vector<uint64_t>& start, uint64_t last) {
    if (pids.empty()) {
      return;
    }
    constexpr uint64_t kNone = UINT64_MAX;
    std::vector<uint64_t> since(kPidCount, kNone);  // packet index of the last packet of the PID (or its start)
    std::vector<uint64_t> from(kPidCount, kNone);   // when the PID's timer starts
    for (size_t i = 0; i < pids.size(); ++i) {
      from[pids[i]] = start[i];
    }
    const uint32_t limit = opt_.pid_timeout_ms;
    const auto check = [&](uint16_t pid, uint64_t a, uint64_t b) {
      if (ms_between(a, b) > limit) {
        timer_error(k16, DiagCode::CNT_TS_1007, "PID_error: no packet on PID `" + format_pid(pid) + "`", a, b, limit);
      }
    };
    for (uint64_t k = 0; k < f_.timing.packet_pid.size(); ++k) {
      const uint16_t pid = f_.timing.packet_pid[k];
      if (from[pid] == kNone || k < from[pid]) {
        continue;
      }
      if (since[pid] == kNone) {
        since[pid] = from[pid];
      }
      check(pid, since[pid], k);
      since[pid] = k;
    }
    for (const uint16_t pid : pids) {
      if (since[pid] != kNone && since[pid] < last) {  // a PID without any packet is CNT-TS-1007 of the PSI parser
        check(pid, since[pid], last);
      }
    }
  }

  [[nodiscard]] std::vector<uint8_t> serialise() const {
    treefb::Fbb fbb;
    std::vector<flatbuffers::Offset<fb::TsCheck>> checks;
    checks.reserve(rows_.size());
    for (const Row& r : rows_) {
      const uint64_t index = r.count > 0 ? packet_index_at(f_, r.first_offset) : 0;
      checks.push_back(fb::CreateTsCheck(fbb, fbb.CreateString(r.id), fbb.CreateString(r.name), r.count, index,
                                         r.count > 0 ? r.first_offset : 0));
    }
    std::vector<flatbuffers::Offset<fb::Diagnostic>> diags;
    for (const Diagnostic& d : f_.diagnostics) {
      if (diags.size() < kMaxDiagnostics) {
        diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
      }
    }
    for (const Diagnostic& d : timer_diags_) {
      if (diags.size() < kMaxDiagnostics) {
        diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
      }
    }
    fbb.Finish(fb::CreateTsReport(fbb, fbb.CreateVector(checks), f_.packet_count, fbb.CreateVector(diags)));
    const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
    return {bytes.begin(), bytes.end()};
  }
};

}  // namespace

std::vector<uint8_t> build_ts_report(const File& f, const Tr101290Options& options) {
  return Builder(f, options).build();
}

}  // namespace ovt::container::mpegts
