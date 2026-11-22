// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// EditList and the elst ↔ tfdt sanity check (chapter 08 §8.4.2, WI-1.220). See edit_list.h.

#include "container/isobmff/edit_list.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"

namespace ovt::container::isobmff {

namespace {

__extension__ using i128 = __int128;  // the 128-bit intermediate of §8.4.2 step 5

constexpr uint64_t kMaxPayload = uint64_t{64}
                                 << 20U;  // largest box payload read (an elst of 2^20 v1 entries is 20 MiB)
constexpr uint64_t kCancelEvery = uint64_t{1} << 16U;  // samples between two cancel checks
constexpr size_t kFullBox = 4;                         // version + flags

using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

uint16_t rd16(View b, size_t pos) {
  return static_cast<uint16_t>((uint32_t{b[pos]} << 8U) | b[pos + 1]);
}
uint32_t rd32(View b, size_t pos) {
  return (uint32_t{b[pos]} << 24U) | (uint32_t{b[pos + 1]} << 16U) | (uint32_t{b[pos + 2]} << 8U) | b[pos + 3];
}
uint64_t rd64(View b, size_t pos) {
  return (uint64_t{rd32(b, pos)} << 32U) | rd32(b, pos + 4);
}

const fb::BoxNodeT* child(const fb::BoxNodeT* n, std::string_view type) {
  if (n == nullptr) {
    return nullptr;
  }
  const auto it = std::ranges::find_if(n->children, [&](const auto& c) { return c && c->fourcc == type; });
  return it == n->children.end() ? nullptr : it->get();
}

uint64_t offset_of(const fb::BoxNodeT* n) {
  return n != nullptr && n->file_span ? n->file_span->offset() : 0;
}

// The payload of a box (after its header), read from the file, at least `min_size` bytes or empty.
Bytes payload_min(const io::RandomAccessFile& file, const fb::BoxNodeT* n, size_t min_size) {
  if (n == nullptr || !n->file_span || n->file_span->length() < n->header_length) {
    return {};
  }
  Bytes v(std::min<uint64_t>(n->file_span->length() - n->header_length, kMaxPayload));
  v.resize(file.read(n->file_span->offset() + n->header_length, v.size(), v.data()));
  if (v.size() < min_size) {
    v.clear();
  }
  return v;
}

// `mvhd` / `mdhd` share the layout of timescale and duration; an all-ones duration means "unknown" (0 here).
struct TimeHeader {
  uint32_t timescale = 0;
  uint64_t duration = 0;
};
TimeHeader read_time_header(const io::RandomAccessFile& file, const fb::BoxNodeT* box) {
  const Bytes p = payload_min(file, box, 20);
  if (p.empty()) {
    return {};
  }
  if (p[0] == 1) {
    if (p.size() < 32) {
      return {};
    }
    const uint64_t d = rd64(p, 24);
    return {rd32(p, 20), d == std::numeric_limits<uint64_t>::max() ? 0 : d};
  }
  const uint32_t d = rd32(p, 16);
  return {rd32(p, 12), d == std::numeric_limits<uint32_t>::max() ? 0 : d};
}

uint64_t read_tkhd_duration(const io::RandomAccessFile& file, const fb::BoxNodeT* box) {
  const Bytes p = payload_min(file, box, 24);
  if (p.empty()) {
    return 0;
  }
  if (p[0] == 1) {
    if (p.size() < 36) {
      return 0;
    }
    const uint64_t d = rd64(p, 28);
    return d == std::numeric_limits<uint64_t>::max() ? 0 : d;
  }
  const uint32_t d = rd32(p, 20);
  return d == std::numeric_limits<uint32_t>::max() ? 0 : d;
}

uint32_t read_track_id(const io::RandomAccessFile& file, const fb::BoxNodeT* box) {
  const Bytes p = payload_min(file, box, 24);
  return p.empty() ? 0 : rd32(p, p[0] == 1 ? 20U : 12U);
}

struct Entry {
  uint64_t segment_duration;  // movie timescale
  int64_t media_time;         // track timescale, -1 empty
  int16_t rate_integer;
  uint16_t rate_fraction;
};

// The entries of an `elst` payload (14496-12 §8.6.6), at most kMaxEditEntries and as many as fit the payload.
std::vector<Entry> read_entries(View p) {
  std::vector<Entry> out;
  if (p.size() < kFullBox + 4) {
    return out;
  }
  const bool wide = p[0] == 1;
  const size_t entry_bytes = wide ? 20 : 12;
  const auto n = std::min<uint64_t>({rd32(p, kFullBox), (p.size() - kFullBox - 4) / entry_bytes, kMaxEditEntries});
  out.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    const size_t pos = kFullBox + 4 + i * entry_bytes;
    Entry e{};
    if (wide) {
      e.segment_duration = rd64(p, pos);
      e.media_time = static_cast<int64_t>(rd64(p, pos + 8));
      e.rate_integer = static_cast<int16_t>(rd16(p, pos + 16));
      e.rate_fraction = rd16(p, pos + 18);
    } else {
      e.segment_duration = rd32(p, pos);
      e.media_time = static_cast<int32_t>(rd32(p, pos + 4));
      e.rate_integer = static_cast<int16_t>(rd16(p, pos + 8));
      e.rate_fraction = rd16(p, pos + 10);
    }
    out.push_back(e);
  }
  return out;
}

void add_diag(fb::EditListT& list, std::string_view code, std::string message, uint64_t box_offset) {
  const DiagInfo* info = ovt::container::diag(code);
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = info != nullptr ? static_cast<fb::Severity>(info->severity) : fb::Severity::Error;
  d->code = std::string(code);
  d->message = std::move(message);
  d->spec_ref = info != nullptr ? std::string(info->spec_ref) : std::string{};
  d->span = std::make_unique<fb::BitSpan>(box_offset * 8U, 0U);  // file-relative: the box the finding is about
  list.diagnostics.push_back(std::move(d));
}

uint64_t sat_add(uint64_t a, uint64_t b) {
  uint64_t r = 0;
  return __builtin_add_overflow(a, b, &r) ? std::numeric_limits<uint64_t>::max() : r;
}

// CTS = DTS + cts_offset of every sample (track timescale), saturating at the int64 range.
int64_t cts_of(const fb::SampleTableT& t, uint64_t i) {
  int64_t r = 0;
  if (__builtin_add_overflow(t.dts[i], int64_t{t.cts_offset[i]}, &r)) {
    return t.cts_offset[i] < 0 ? std::numeric_limits<int64_t>::min() : std::numeric_limits<int64_t>::max();
  }
  return r;
}

// §8.4.2 step 3: the samples in presentation (CTS, index) order. CTS is not sorted in decode order with B-frames, so
// the order is built once; a table whose CTS never decreases (no reordering) needs no index.
class DisplayOrder {
 public:
  DisplayOrder(const fb::SampleTableT& t, jobs::Cancel& cancel) : t_(t) {
    n_ = std::min<uint64_t>({t.count, t.dts.size(), t.cts_offset.size(), std::numeric_limits<uint32_t>::max()});
    bool sorted = true;
    for (uint64_t i = 1; i < n_ && sorted; ++i) {
      if (i % kCancelEvery == 0) {
        cancel.throw_if_requested();
      }
      sorted = cts_of(t_, i - 1) <= cts_of(t_, i);
    }
    if (!sorted) {
      order_.resize(n_);
      std::iota(order_.begin(), order_.end(), uint32_t{0});
      cancel.throw_if_requested();
      std::ranges::sort(order_, [&](uint32_t a, uint32_t b) {
        const int64_t ca = cts_of(t_, a);
        const int64_t cb = cts_of(t_, b);
        return ca != cb ? ca < cb : a < b;
      });
      cancel.throw_if_requested();
    }
  }

  [[nodiscard]] uint64_t size() const { return n_; }
  // The sample at display position `pos`.
  [[nodiscard]] uint64_t at(uint64_t pos) const { return order_.empty() ? pos : order_[pos]; }
  [[nodiscard]] int64_t cts_at(uint64_t pos) const { return cts_of(t_, at(pos)); }

  // The number of leading display positions whose CTS satisfies `pred` (which must hold for a prefix).
  template <class Pred>
  [[nodiscard]] uint64_t prefix_count(Pred pred) const {
    uint64_t lo = 0;
    uint64_t hi = n_;
    while (lo < hi) {
      const uint64_t mid = lo + (hi - lo) / 2;
      if (pred(cts_at(mid))) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

 private:
  const fb::SampleTableT& t_;
  uint64_t n_ = 0;
  std::vector<uint32_t> order_;
};

// The sample ranges of one segment (§8.4.2 step 3): first = the sample with the largest CTS <= media_time (the earliest
// sample when none is), last = the sample with the largest CTS < media_time + segment_duration * track / movie. -1/-1
// when no sample is covered.
void resolve_range(const DisplayOrder& order, fb::EditSegmentT& seg, uint32_t movie_ts, uint32_t track_ts) {
  seg.first_sample = -1;
  seg.last_sample = -1;
  if (order.size() == 0 || movie_ts == 0 || track_ts == 0) {
    return;
  }
  const int64_t mt = seg.media_time;
  const i128 span =
      i128{seg.segment_duration} * track_ts;  // exact: compare (cts - mt) * movie_ts < segment_duration * track_ts
  const uint64_t upto = order.prefix_count([&](int64_t c) { return c <= mt; });
  const uint64_t before_end = order.prefix_count([&](int64_t c) { return (i128{c} - mt) * movie_ts < span; });
  const uint64_t first_pos = upto == 0 ? 0 : upto - 1;
  if (before_end == 0 || before_end - 1 < first_pos) {
    return;
  }
  seg.first_sample = static_cast<int64_t>(order.at(first_pos));
  seg.last_sample = static_cast<int64_t>(order.at(before_end - 1));
}

// The state of one build_edit_list() call: the track's header values, the display order and the list under
// construction.
class Builder {
 public:
  Builder(const io::RandomAccessFile& file, const BoxNodes& top_level, const fb::BoxNodeT& trak,
          const fb::SampleTableT& table, jobs::Cancel& cancel, EditListBuild& out)
      : file_(file), table_(table), out_(out), list_(*out.list), order_(table, cancel) {
    const auto moov = std::ranges::find_if(top_level, [](const auto& n) { return n && n->fourcc == "moov"; });
    const fb::BoxNodeT* tkhd = child(&trak, "tkhd");
    elst_ = child(child(&trak, "edts"), "elst");
    out_.has_elst = elst_ != nullptr;
    out_.diag_offset = out_.has_elst ? offset_of(elst_) : offset_of(&trak);
    list_.track_id = read_track_id(file, tkhd);
    const TimeHeader movie = read_time_header(file, moov == top_level.end() ? nullptr : child(moov->get(), "mvhd"));
    const TimeHeader media = read_time_header(file, child(child(&trak, "mdia"), "mdhd"));
    tkhd_duration_ = read_tkhd_duration(file, tkhd);
    track_ts_ = media.timescale != 0 ? media.timescale : table.timescale;
    // A track without mvhd falls back to the track timescale: the presentation time is then the media time.
    movie_ts_ = movie.timescale != 0 ? movie.timescale : track_ts_;
    out_.movie_timescale = movie_ts_;
    // The media end is the composition end max(CTS + duration): an edit that removes the composition delay starts at
    // media_time == delay and (as written by ffmpeg) keeps the full duration, so it ends there, not at mdhd.duration.
    // Without samples (fragmented movie) mdhd.duration, 0 = unknown.
    mdhd_duration_ = media.duration;
    media_duration_ = media.duration;
    const auto n = std::min<uint64_t>({table.count, table.dts.size(), table.cts_offset.size(), table.duration.size()});
    if (n != 0) {
      int64_t end = 0;
      for (uint64_t i = 0; i < n; ++i) {
        end = std::max(end, cts_of(table, i) > std::numeric_limits<int64_t>::max() - table.duration[i]
                                ? std::numeric_limits<int64_t>::max()
                                : cts_of(table, i) + int64_t{table.duration[i]});
      }
      media_duration_ = static_cast<uint64_t>(end);
    }
  }

  void run() {
    if (out_.has_elst) {
      place_entries(read_entries(payload_min(file_, elst_, kFullBox + 4)));
    } else {
      implicit_segment();
    }
    finish();
  }

 private:
  const io::RandomAccessFile& file_;
  const fb::SampleTableT& table_;
  EditListBuild& out_;
  fb::EditListT& list_;
  DisplayOrder order_;
  const fb::BoxNodeT* elst_ = nullptr;
  uint64_t tkhd_duration_ = 0;
  uint32_t track_ts_ = 0;
  uint32_t movie_ts_ = 0;
  uint64_t media_duration_ = 0;  // composition end (or mdhd.duration), track timescale, 0 = unknown
  uint64_t mdhd_duration_ = 0;

  void diag(std::string_view code, std::string message) { add_diag(list_, code, std::move(message), out_.diag_offset); }

  // Step 1: one implicit normal segment covering every sample.
  void implicit_segment() {
    uint64_t dur = tkhd_duration_;
    if (dur == 0 && track_ts_ != 0) {
      const i128 rescaled = i128{mdhd_duration_ != 0 ? mdhd_duration_ : media_duration_} * movie_ts_ / track_ts_;
      dur = static_cast<uint64_t>(std::min<i128>(rescaled, std::numeric_limits<uint64_t>::max()));
    }
    auto seg = std::make_unique<fb::EditSegmentT>();
    seg->segment_duration = dur;
    seg->media_time = 0;
    seg->media_rate = 1.0;
    seg->presentation_start = 0;
    seg->presentation_end = dur;
    seg->kind = "normal";
    seg->first_sample = order_.size() == 0 ? -1 : static_cast<int64_t>(order_.at(0));
    seg->last_sample = order_.size() == 0 ? -1 : static_cast<int64_t>(order_.at(order_.size() - 1));
    list_.total_presentation_duration = dur;
    list_.segments.push_back(std::move(seg));
  }

  // Steps 2 and 3: classify and place every entry on the presentation timeline, resolve its samples.
  void place_entries(const std::vector<Entry>& entries) {
    uint64_t start = 0;
    for (size_t k = 0; k < entries.size(); ++k) {
      const Entry& e = entries[k];
      auto seg = std::make_unique<fb::EditSegmentT>();
      seg->segment_duration = e.segment_duration;
      seg->media_time = e.media_time;
      seg->media_rate = e.rate_integer + e.rate_fraction / 65536.0;
      seg->presentation_start = start;
      start = sat_add(start, e.segment_duration);
      seg->presentation_end = start;
      seg->first_sample = -1;
      seg->last_sample = -1;
      classify(k, e, *seg);
      if (seg->kind == "normal" || seg->kind == "dwell") {
        resolve_range(order_, *seg, movie_ts_, track_ts_);
        if (seg->kind == "dwell" && seg->first_sample >= 0) {
          seg->last_sample = seg->first_sample;  // a dwell freezes one frame
        }
        check_media_bounds(k, e);
      }
      list_.segments.push_back(std::move(seg));
    }
    list_.total_presentation_duration = start;
    if (tkhd_duration_ != 0 && start != tkhd_duration_) {
      diag("CNT-3004", "the edit durations add up to " + std::to_string(start) + ", tkhd.duration is " +
                           std::to_string(tkhd_duration_));
    }
  }

  void classify(size_t k, const Entry& e, fb::EditSegmentT& seg) {
    if (e.media_time == -1) {
      seg.kind = "empty";
    } else if (e.rate_integer == 0) {
      seg.kind = "dwell";
    } else if (e.rate_integer != 1 || e.rate_fraction != 0) {
      seg.kind = "rate";
      diag("CNT-3003", "edit " + std::to_string(k) + " has media_rate " + std::to_string(seg.media_rate));
    } else {
      seg.kind = "normal";
    }
  }

  // CNT-3001 / CNT-3002 of a normal or dwell edit; skipped while the media duration is unknown (fragmented movies).
  void check_media_bounds(size_t k, const Entry& e) {
    if (media_duration_ == 0 || e.media_time < 0) {
      return;
    }
    if (static_cast<uint64_t>(e.media_time) >= media_duration_) {
      diag("CNT-3001", "edit " + std::to_string(k) + " starts at media_time " + std::to_string(e.media_time) +
                           ", beyond the media duration " + std::to_string(media_duration_));
      return;
    }
    if (movie_ts_ == 0) {
      return;
    }
    // SPEC-ASSUMPTION(#793): segment_duration is rounded to the movie timescale; an excess below one movie tick
    // (rescaled to the track timescale) is that rounding, not a finding.
    const i128 end = i128{e.media_time} + i128{e.segment_duration} * track_ts_ / movie_ts_;
    const i128 excess = end - media_duration_;
    const i128 tick = (i128{track_ts_} + movie_ts_ - 1) / movie_ts_;
    if (excess >= tick) {
      const auto shown = static_cast<uint64_t>(std::min<i128>(excess, std::numeric_limits<uint64_t>::max()));
      diag("CNT-3002", "edit " + std::to_string(k) + " exceeds the media by " + std::to_string(shown));
    }
  }

  // Step 4: implied_start_offset and negative_cts_compensation.
  void finish() {
    const auto first_normal = std::ranges::find_if(list_.segments, [](const auto& s) { return s->kind == "normal"; });
    if (first_normal != list_.segments.end()) {
      list_.implied_start_offset = (*first_normal)->media_time;
    }
    // SPEC-ASSUMPTION(#793): "0 = none" (chapter 06): without an offset there is no composition delay to compensate.
    list_.negative_cts_compensation = order_.size() != 0 && list_.implied_start_offset != 0 &&
                                      list_.implied_start_offset == int64_t{table_.cts_offset[0]};
  }
};

}  // namespace

EditListBuild build_edit_list(const io::RandomAccessFile& file, const BoxNodes& top_level, size_t trak_index,
                              const fb::SampleTableT& table, jobs::Cancel& cancel) {
  EditListBuild out;
  out.list = std::make_unique<fb::EditListT>();
  const fb::BoxNodeT* trak = find_trak(top_level, trak_index);
  if (trak == nullptr) {
    return out;
  }
  Builder(file, top_level, *trak, table, cancel, out).run();
  return out;
}

bool sample_presentation_time(const fb::EditListT& list, const fb::SampleTableT& table, uint64_t sample_index,
                              uint32_t movie_timescale, int64_t* out_pts_movie) {
  const uint64_t n = std::min<uint64_t>(table.dts.size(), table.cts_offset.size());
  if (out_pts_movie == nullptr || sample_index >= n || table.timescale == 0 || movie_timescale == 0) {
    return false;
  }
  const int64_t cts = cts_of(table, sample_index);
  for (const auto& seg : list.segments) {
    if (seg->first_sample < 0 || seg->last_sample < 0 || static_cast<uint64_t>(seg->first_sample) >= n ||
        static_cast<uint64_t>(seg->last_sample) >= n) {
      continue;  // empty edit, rate edit or a segment without samples
    }
    i128 pts = 0;
    if (seg->kind == "dwell") {
      if (sample_index != static_cast<uint64_t>(seg->first_sample)) {
        continue;
      }
      pts = seg->presentation_start;
    } else if (seg->kind == "normal") {
      if (cts < cts_of(table, static_cast<uint64_t>(seg->first_sample)) ||
          cts > cts_of(table, static_cast<uint64_t>(seg->last_sample))) {
        continue;
      }
      pts = i128{seg->presentation_start} + (i128{cts} - seg->media_time) * movie_timescale / table.timescale;
    } else {
      continue;
    }
    if (pts < std::numeric_limits<int64_t>::min() || pts > std::numeric_limits<int64_t>::max()) {
      return false;
    }
    *out_pts_movie = static_cast<int64_t>(pts);
    return true;
  }
  return false;
}

void check_elst_tfdt(EditListBuild& build, const FragmentBuild& fragments) {
  if (build.list == nullptr || fragments.timeline.fragments.empty() || fragments.samples.count() == 0) {
    return;
  }
  fb::EditListT& list = *build.list;
  // Rule 4: CMAF forbids a multi-segment edit list in a file with `moof`.
  const auto normals = std::ranges::count_if(list.segments, [](const auto& s) { return s->kind == "normal"; });
  if (build.has_elst && normals > 1) {
    add_diag(list, "CNT-3012", std::to_string(normals) + " normal edits in a fragmented file", build.diag_offset);
  }
  // Rule 1: T0 of the first fragment (tfdt, or the inferred start), C0 and the duration of its first sample.
  auto t0 = static_cast<uint64_t>(fragments.samples.dts[0]);
  for (const auto& f : fragments.timeline.fragments) {
    if (f && !f->trafs.empty() && f->trafs[0]) {
      t0 = f->trafs[0]->has_tfdt ? f->trafs[0]->tfdt : f->trafs[0]->computed_dts_start;
      break;
    }
  }
  const i128 c0 = fragments.samples.cts_offset[0];
  const i128 d0 = fragments.samples.duration[0];
  const i128 expected = i128{t0} + c0;  // the first presented composition time
  if (build.has_elst && normals > 0) {
    // Rules 2-3.
    const i128 m = list.implied_start_offset;
    const i128 gap = m > expected ? m - expected : expected - m;
    if (m != expected && m != c0 && gap > d0) {
      add_diag(list, "CNT-3010",
               "elst media_time " + std::to_string(list.implied_start_offset) +
                   " does not match first fragment presentation time " + std::to_string(static_cast<int64_t>(expected)),
               build.diag_offset);
    }
  } else if (!build.has_elst && t0 > 0 && c0 > 0) {
    add_diag(list, "CNT-3011", "presentation starts at " + std::to_string(static_cast<int64_t>(expected)) + ", not 0",
             build.diag_offset);
  }
}

flatbuffers::DetachedBuffer serialise(const fb::EditListT& list) {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::EditList::Pack(fbb, &list));
  return fbb.Release();
}

}  // namespace ovt::container::isobmff
