// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// SampleTable expansion (chapter 08 §8.4.1, WI-1.218). See sample_table.h.

#include "container/isobmff/sample_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "container/diagnostics.h"

namespace ovt::container::isobmff {

namespace {

constexpr uint64_t kMaxPayload = uint64_t{256} << 20U;  // largest box payload read (a 50 M-entry stsz)
constexpr uint64_t kCancelEvery = uint64_t{1} << 16U;   // samples between two cancel checks
constexpr uint64_t kInt64Max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
constexpr uint32_t kSignBit = uint32_t{1} << 31U;
constexpr size_t kFullBox = 4;  // version + flags
constexpr uint32_t kFragmentLocal = 0x10001;

using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

// Big-endian readers; every caller checks the bounds first.
uint32_t rd32(View b, size_t pos) {
  return (uint32_t{b[pos]} << 24U) | (uint32_t{b[pos + 1]} << 16U) | (uint32_t{b[pos + 2]} << 8U) | b[pos + 3];
}
uint64_t rd64(View b, size_t pos) {
  return (uint64_t{rd32(b, pos)} << 32U) | rd32(b, pos + 4);
}
std::string fourcc(View b, size_t pos) {
  return {b.begin() + static_cast<std::ptrdiff_t>(pos), b.begin() + static_cast<std::ptrdiff_t>(pos + 4)};
}

// §8.4.1: every addition is overflow-checked.
bool checked_add(uint64_t a, uint64_t b, uint64_t& out) {
  return !__builtin_add_overflow(a, b, &out);
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

// The payload of a box (after its header), read from the file; empty for nullptr.
Bytes payload_of(const io::RandomAccessFile& file, const fb::BoxNodeT* n) {
  if (n == nullptr || !n->file_span || n->file_span->length() < n->header_length) {
    return {};
  }
  Bytes v(std::min<uint64_t>(n->file_span->length() - n->header_length, kMaxPayload));
  v.resize(file.read(n->file_span->offset() + n->header_length, v.size(), v.data()));
  return v;
}

// A payload that must hold at least `min_size` bytes; empty otherwise (a truncated box is the walk's finding).
Bytes payload_min(const io::RandomAccessFile& file, const fb::BoxNodeT* n, size_t min_size) {
  Bytes v = payload_of(file, n);
  if (v.size() < min_size) {
    v.clear();
  }
  return v;
}

// Entries of an `entry_count`-prefixed table (after version/flags) that fit in the payload.
uint64_t table_entries(View p, size_t entry_size) {
  if (p.size() < kFullBox + 4) {
    return 0;
  }
  return std::min<uint64_t>(rd32(p, kFullBox), (p.size() - kFullBox - 4) / entry_size);
}

uint32_t track_id_of(const io::RandomAccessFile& file, const fb::BoxNodeT& trak) {
  const Bytes tkhd = payload_min(file, child(&trak, "tkhd"), 24);
  return tkhd.empty() ? 0 : rd32(tkhd, tkhd[0] == 1 ? 20U : 12U);
}

struct Run {  // one `stts` / `ctts` run
  uint32_t count;
  uint32_t value;
};

std::vector<Run> read_runs(View p) {
  std::vector<Run> runs;
  const uint64_t n = table_entries(p, 8);
  runs.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    const size_t pos = kFullBox + 4 + i * 8;
    runs.push_back({rd32(p, pos), rd32(p, pos + 4)});
  }
  return runs;
}

uint64_t total_of(const std::vector<Run>& runs) {
  uint64_t total = 0;
  for (const Run& r : runs) {
    total += r.count;  // at most 2^32 entries of 2^32 samples: cannot wrap
  }
  return total;
}

struct ChunkEntry {
  uint32_t first_chunk;
  uint32_t samples_per_chunk;
  uint32_t description_index;
};

// "first occurrence + total" bookkeeping for a finding that is raised once per table.
struct Tally {
  uint64_t first = 0;
  uint64_t total = 0;
  void add(uint64_t i) {
    if (total++ == 0) {
      first = i;
    }
  }
};

class Expander {
 public:
  Expander(const io::RandomAccessFile& file, const fb::BoxNodeT& trak, jobs::Cancel& cancel)
      : file_(file),
        cancel_(cancel),
        stbl_(child(child(child(&trak, "mdia"), "minf"), "stbl")),
        t_(std::make_unique<fb::SampleTableT>()) {
    t_->track_id = track_id_of(file, trak);
    if (const Bytes mdhd = payload_min(file, child(child(&trak, "mdia"), "mdhd"), 24); !mdhd.empty()) {
      t_->timescale = rd32(mdhd, mdhd[0] == 1 ? 20U : 12U);
    }
  }

  std::unique_ptr<fb::SampleTableT> run() {
    if (stbl_ != nullptr && read_sizes()) {
      expand_durations();
      expand_composition();
      expand_chunks();
      check_file_bounds();
      expand_sync();
      expand_sdtp_and_priority();
      expand_groups();
      check_description_indexes();
    }
    finish();
    return std::move(t_);
  }

 private:
  const io::RandomAccessFile& file_;
  jobs::Cancel& cancel_;
  const fb::BoxNodeT* stbl_;
  std::unique_ptr<fb::SampleTableT> t_;
  uint64_t count_ = 0;       // samples the table keeps; only ever lowered after step 1
  bool overflowed_ = false;  // an offset overflow ended the chunk walk (CNT-2030)

  void diag(std::string_view code, std::string message, uint64_t box_offset) const {
    const DiagInfo* info = ovt::container::diag(code);
    auto d = std::make_unique<fb::DiagnosticT>();
    d->severity = info != nullptr ? static_cast<fb::Severity>(info->severity) : fb::Severity::Error;
    d->code = std::string(code);
    d->message = std::move(message);
    d->spec_ref = info != nullptr ? std::string(info->spec_ref) : std::string{};
    d->span = std::make_unique<fb::BitSpan>(box_offset * 8U, 0U);  // file-relative: the box the finding is about
    t_->diagnostics.push_back(std::move(d));
  }

  void pace(uint64_t i) const {
    if ((i % kCancelEvery) == 0) {
      cancel_.throw_if_requested();
    }
  }

  // Lowers the kept sample count to `n` (never raises it).
  void truncate_to(uint64_t n) { count_ = std::min(count_, n); }

  void overflow_at(uint64_t sample, const fb::BoxNodeT* box) {
    diag("CNT-2030", "64-bit overflow in sample table at sample " + std::to_string(sample), offset_of(box));
    truncate_to(sample);
  }

  // ---- step 1 and 4: sample count and sizes ---------------------------------------------------------------------

  // False when the track has no `stsz`/`stz2` (no samples) or `stz2` has an unusable field size.
  bool read_sizes() {
    if (const fb::BoxNodeT* stsz = child(stbl_, "stsz"); stsz != nullptr) {
      return read_stsz(stsz);
    }
    if (const fb::BoxNodeT* stz2 = child(stbl_, "stz2"); stz2 != nullptr) {
      return read_stz2(stz2);
    }
    return false;
  }

  uint64_t clamp_count(uint64_t declared, const fb::BoxNodeT* box) {
    if (declared > kMaxSamples) {
      diag("CNT-2031",
           "sample count " + std::to_string(declared) + " exceeds " + std::to_string(kMaxSamples) + ", table truncated",
           offset_of(box));
      return kMaxSamples;
    }
    return declared;
  }

  bool read_stsz(const fb::BoxNodeT* box) {
    const Bytes p = payload_min(file_, box, 12);
    if (p.empty()) {
      return false;
    }
    const uint32_t fixed = rd32(p, 4);
    uint64_t n = clamp_count(rd32(p, 8), box);
    if (fixed == 0) {
      n = std::min<uint64_t>(n, (p.size() - 12) / 4);
    }
    count_ = n;
    t_->size.resize(n);
    for (uint64_t i = 0; i < n; ++i) {
      t_->size[i] = fixed != 0 ? fixed : rd32(p, 12 + i * 4);
    }
    return true;
  }

  bool read_stz2(const fb::BoxNodeT* box) {
    const Bytes p = payload_min(file_, box, 12);
    if (p.empty()) {
      return false;
    }
    const uint32_t field_size = p[7];
    if (field_size != 4 && field_size != 8 && field_size != 16) {
      diag("CNT-2006", "stz2.field_size " + std::to_string(field_size) + " is not 4, 8 or 16", offset_of(box));
      return false;
    }
    const uint64_t avail = (p.size() - 12) * 8U / field_size;
    const uint64_t n = std::min(clamp_count(rd32(p, 8), box), avail);
    count_ = n;
    t_->size.resize(n);
    for (uint64_t i = 0; i < n; ++i) {
      if (field_size == 4) {
        const uint8_t byte = p[12 + i / 2];
        t_->size[i] = (i % 2 == 0) ? byte >> 4U : byte & 0xFU;  // high nibble first
      } else if (field_size == 8) {
        t_->size[i] = p[12 + i];
      } else {
        t_->size[i] = (uint32_t{p[12 + i * 2]} << 8U) | p[13 + i * 2];
      }
    }
    return true;
  }

  // ---- step 2: durations and DTS --------------------------------------------------------------------------------

  void expand_durations() {
    const fb::BoxNodeT* box = child(stbl_, "stts");
    const Bytes p = payload_min(file_, box, 8);
    const std::vector<Run> runs = read_runs(p);
    const uint64_t covered = total_of(runs);
    if (covered != count_) {
      diag("CNT-2005", "stts covers " + std::to_string(covered) + " samples, stsz " + std::to_string(count_),
           offset_of(box != nullptr ? box : child(stbl_, "stsz")));
      truncate_to(covered);
    }
    t_->dts.assign(t_->size.size(), 0);
    t_->duration.assign(t_->size.size(), 0);
    uint64_t i = 0;
    uint64_t dts = 0;
    Tally zero;
    for (const Run& r : runs) {
      for (uint32_t k = 0; k < r.count && i < count_; ++k, ++i) {
        pace(i);
        t_->dts[i] = static_cast<int64_t>(dts);
        t_->duration[i] = r.value;
        if (r.value == 0 && i + 1 < covered) {
          zero.add(i);
        }
        if (!checked_add(dts, r.value, dts) || dts > kInt64Max) {
          overflow_at(i + 1, box);  // sample i + 1 has no representable DTS
          break;
        }
      }
    }
    if (zero.total > 0) {
      diag("CNT-2001",
           "stts.sample_delta is 0 for " + std::to_string(zero.total) + " non-last samples, first at sample " +
               std::to_string(zero.first),
           offset_of(box));
    }
  }

  // ---- step 3: composition offsets ------------------------------------------------------------------------------

  void expand_composition() {
    t_->cts_offset.assign(t_->size.size(), 0);
    const fb::BoxNodeT* ctts_box = child(stbl_, "ctts");
    const Bytes ctts = payload_min(file_, ctts_box, 8);
    if (!ctts.empty()) {
      read_ctts(ctts, ctts_box);
    }
    int64_t min_off = 0;
    int64_t max_off = 0;
    for (uint64_t i = 0; i < count_; ++i) {
      min_off = std::min<int64_t>(min_off, t_->cts_offset[i]);
      max_off = std::max<int64_t>(max_off, t_->cts_offset[i]);
    }
    const int64_t derived = -min_off;  // max_i(dts[i] - cts[i]); negative when every offset is positive
    t_->composition_to_dts_shift = std::max<int64_t>(0, derived);
    apply_cslg(derived, min_off, max_off);
  }

  void read_ctts(View p, const fb::BoxNodeT* box) {
    const std::vector<Run> runs = read_runs(p);
    const uint64_t covered = total_of(runs);
    if (covered != count_) {
      diag("CNT-2011", "ctts covers " + std::to_string(covered) + " samples, stsz " + std::to_string(count_),
           offset_of(box));
    }
    const bool signed_offsets = p[0] != 0;
    uint64_t i = 0;
    Tally big;
    for (const Run& r : runs) {
      if (!signed_offsets && (r.value & kSignBit) != 0 && i < count_) {
        big.add(i);
      }
      for (uint32_t k = 0; k < r.count && i < count_; ++k, ++i) {
        t_->cts_offset[i] = static_cast<int32_t>(r.value);  // v0 ≥ 2^31 is stored as negative (libavformat, GPAC)
      }
    }
    if (big.total > 0) {
      diag("CNT-2002",
           "v0 ctts offset >= 2^31 interpreted as negative, first in the run at sample " + std::to_string(big.first),
           offset_of(box));
    }
  }

  // `cslg` is informative (14496-12 §8.6.1.4): it fills composition_to_dts_shift and can raise CNT-2012, but never
  // changes a CTS.
  void apply_cslg(int64_t derived, int64_t min_off, int64_t max_off) {
    const fb::BoxNodeT* box = child(stbl_, "cslg");
    const Bytes p = payload_min(file_, box, kFullBox + 20);
    if (p.empty()) {
      return;
    }
    const bool wide = p[0] == 1 && p.size() >= kFullBox + 40;
    const auto field = [&](size_t k) {
      return wide ? static_cast<int64_t>(rd64(p, kFullBox + k * 8)) : static_cast<int32_t>(rd32(p, kFullBox + k * 4));
    };
    const int64_t shift = field(0);
    t_->composition_to_dts_shift = shift;
    if (shift < derived) {
      diag("CNT-2012",
           "cslg compositionToDTSShift " + std::to_string(shift) + " smaller than the ctts-derived shift " +
               std::to_string(derived),
           offset_of(box));
    } else if (field(1) != min_off || field(2) != max_off) {
      diag("CNT-2012",
           "cslg least/greatestDecodeToDisplayDelta " + std::to_string(field(1)) + "/" + std::to_string(field(2)) +
               " differ from ctts min/max " + std::to_string(min_off) + "/" + std::to_string(max_off),
           offset_of(box));
    }
  }

  // ---- step 5: chunk map and offsets ----------------------------------------------------------------------------

  [[nodiscard]] std::vector<ChunkEntry> read_stsc() const {
    const Bytes p = payload_min(file_, child(stbl_, "stsc"), 8);
    std::vector<ChunkEntry> out;
    const uint64_t n = table_entries(p, 12);
    out.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
      const size_t pos = kFullBox + 4 + i * 12;
      out.push_back({rd32(p, pos), rd32(p, pos + 4), rd32(p, pos + 8)});
    }
    return out;
  }

  // The chunk offsets of `stco`, or of `co64` when there is no `stco`.
  std::vector<uint64_t> read_chunk_offsets(const fb::BoxNodeT*& box) const {
    box = child(stbl_, "stco");
    const bool wide = box == nullptr;
    if (wide) {
      box = child(stbl_, "co64");
    }
    const Bytes p = payload_min(file_, box, 8);
    std::vector<uint64_t> out;
    const uint64_t n = table_entries(p, wide ? 8 : 4);
    out.reserve(n);
    for (uint64_t i = 0; i < n; ++i) {
      out.push_back(wide ? rd64(p, kFullBox + 4 + i * 8) : rd32(p, kFullBox + 4 + i * 4));
    }
    return out;
  }

  // Index of the first stsc entry that breaks first_chunk_1 == 1, strictly increasing, <= chunk_count; entries.size()
  // when all are valid.
  [[nodiscard]] size_t first_bad_entry(const std::vector<ChunkEntry>& e, uint64_t chunk_count) const {
    for (size_t k = 0; k < e.size(); ++k) {
      const bool bad =
          (k == 0 ? e[0].first_chunk != 1 : e[k].first_chunk <= e[k - 1].first_chunk) || e[k].first_chunk > chunk_count;
      if (bad) {
        diag("CNT-2003",
             "stsc.first_chunk of entry " + std::to_string(k) + " (" + std::to_string(e[k].first_chunk) +
                 ") is not 1, not above the previous entry, or past the chunk count " + std::to_string(chunk_count),
             offset_of(child(stbl_, "stsc")));
        return k;
      }
    }
    return e.size();
  }

  void expand_chunks() {
    const size_t n = t_->size.size();
    t_->offset.assign(n, 0);
    t_->chunk_index.assign(n, 0);
    t_->sample_description_index.assign(n, 0);
    const fb::BoxNodeT* co_box = nullptr;
    const std::vector<uint64_t> chunk_offsets = read_chunk_offsets(co_box);
    const std::vector<ChunkEntry> entries = read_stsc();
    const size_t valid = chunk_offsets.empty() ? 0 : first_bad_entry(entries, chunk_offsets.size());
    uint64_t s = 0;
    bool more_chunks = false;  // chunks (or samples of one) remain after the table is full
    for (size_t k = 0; k < valid && !more_chunks && !overflowed_; ++k) {
      // Entry k covers chunks first_chunk_k .. first_chunk_{k+1} - 1 (the last valid entry: up to the offender or to
      // chunk_count; an offender that does not rise above it leaves the entry its own first chunk).
      uint64_t last = chunk_offsets.size();
      if (k + 1 < entries.size()) {
        last = entries[k + 1].first_chunk > entries[k].first_chunk
                   ? std::min<uint64_t>(entries[k + 1].first_chunk - 1, last)
                   : entries[k].first_chunk;
      }
      for (uint64_t c = entries[k].first_chunk; c <= last && !more_chunks && !overflowed_; ++c) {
        more_chunks = !fill_chunk(c, chunk_offsets[c - 1], entries[k], s, co_box);
      }
    }
    if (more_chunks) {
      diag("CNT-2014", "stsc/stco describe more samples than stsz (" + std::to_string(count_) + ")", offset_of(co_box));
    }
    if (s < count_ && !overflowed_) {
      diag("CNT-2015", std::to_string(count_ - s) + " samples have no chunk, first sample " + std::to_string(s),
           offset_of(co_box != nullptr ? co_box : child(stbl_, "stsc")));
    }
  }

  // Assigns the samples_per_chunk samples of chunk `c` starting at sample `s`; false when the table was already full
  // and the chunk still holds samples.
  bool fill_chunk(uint64_t c, uint64_t chunk_offset, const ChunkEntry& e, uint64_t& s, const fb::BoxNodeT* co_box) {
    uint64_t cursor = chunk_offset;
    const uint16_t sdi = static_cast<uint16_t>(std::min<uint32_t>(e.description_index, 0xFFFFU));
    for (uint32_t j = 0; j < e.samples_per_chunk; ++j) {
      if (s >= count_) {
        return false;
      }
      pace(s);
      t_->offset[s] = cursor;
      t_->chunk_index[s] = static_cast<uint32_t>(c);
      t_->sample_description_index[s] = sdi;
      if (!checked_add(cursor, t_->size[s], cursor)) {
        ++s;  // sample s itself is complete; the offset of sample s + 1 does not fit
        overflow_at(s, co_box);
        overflowed_ = true;
        return true;
      }
      ++s;
    }
    return true;
  }

  // ---- step 6: offsets against the file --------------------------------------------------------------------------

  void check_file_bounds() {
    const uint64_t file_size = file_.size();
    const fb::BoxNodeT* box = child(stbl_, "stco") != nullptr ? child(stbl_, "stco") : child(stbl_, "co64");
    uint64_t bad = 0;
    for (uint64_t i = 0; i < count_; ++i) {
      uint64_t end = 0;
      if (t_->chunk_index[i] == 0 || (checked_add(t_->offset[i], t_->size[i], end) && end <= file_size)) {
        continue;
      }
      if (bad++ < kMaxListedOutOfFile) {
        diag("CNT-2007",
             "sample " + std::to_string(i) + " (offset " + std::to_string(t_->offset[i]) + ", size " +
                 std::to_string(t_->size[i]) + ") extends past the end of the file (" + std::to_string(file_size) +
                 " bytes)",
             offset_of(box));
      }
    }
    if (bad > kMaxListedOutOfFile) {
      diag("CNT-2007", std::to_string(bad) + " samples in total extend past the end of the file", offset_of(box));
    }
  }

  // ---- step 7: sync samples, sdtp, stdp, subs ---------------------------------------------------------------------

  // Marks the 1-based sample numbers of an stss/stps payload in `flags`; CNT-2009 for entries that are 0, past the
  // sample count or not increasing (they are ignored).
  void mark_sample_numbers(const fb::BoxNodeT* box, const Bytes& p, std::vector<bool>& flags) const {
    const uint64_t n = table_entries(p, 4);
    uint64_t prev = 0;
    Tally bad;
    for (uint64_t i = 0; i < n; ++i) {
      const uint32_t v = rd32(p, kFullBox + 4 + i * 4);
      if (v == 0 || v > count_ || v <= prev) {
        bad.add(i);
        continue;
      }
      prev = v;
      flags[v - 1] = true;
    }
    if (bad.total > 0) {
      diag("CNT-2009",
           std::to_string(bad.total) + " " + box->fourcc +
               " entries are 0, past the sample count or not increasing, first entry " + std::to_string(bad.first),
           offset_of(box));
    }
  }

  void expand_sync() {
    const fb::BoxNodeT* stss = child(stbl_, "stss");
    const Bytes p = payload_min(file_, stss, 8);
    if (stss == nullptr || p.empty()) {
      t_->is_sync.assign(t_->size.size(), true);  // 14496-12 §8.6.2: no stss, every sample is a sync sample
    } else {
      t_->is_sync.assign(t_->size.size(), false);
      mark_sample_numbers(stss, p, t_->is_sync);
    }
    const fb::BoxNodeT* stps = child(stbl_, "stps");
    if (const Bytes q = payload_min(file_, stps, 8); !q.empty()) {
      t_->is_partial_sync.assign(t_->size.size(), false);
      mark_sample_numbers(stps, q, t_->is_partial_sync);
    }
  }

  void expand_sdtp_and_priority() {
    t_->has_subsamples = child(stbl_, "subs") != nullptr;
    const fb::BoxNodeT* sdtp = child(stbl_, "sdtp");
    if (const Bytes p = payload_min(file_, sdtp, kFullBox); !p.empty()) {
      const uint64_t have = p.size() - kFullBox;
      if (have != count_) {
        diag("CNT-2010", "sdtp holds " + std::to_string(have) + " entries, stsz " + std::to_string(count_),
             offset_of(sdtp));
      }
      t_->sdtp.assign(t_->size.size(), 0);
      Tally open_gop;
      for (uint64_t i = 0; i < count_ && i < have; ++i) {
        t_->sdtp[i] = p[kFullBox + i];
        if (((t_->sdtp[i] >> 4U) & 3U) == 2 && !t_->is_sync[i]) {  // sample_depends_on == 2: an I picture
          open_gop.add(i);
        }
      }
      if (open_gop.total > 0) {
        diag("CNT-2018",
             std::to_string(open_gop.total) + " sdtp I-pictures are not in stss (open GOP?), first sample " +
                 std::to_string(open_gop.first),
             offset_of(sdtp));
      }
    }
    if (const Bytes p = payload_min(file_, child(stbl_, "stdp"), kFullBox); !p.empty()) {
      t_->degradation_priority.assign(t_->size.size(), 0);
      for (uint64_t i = 0; i < count_ && kFullBox + i * 2 + 2 <= p.size(); ++i) {
        t_->degradation_priority[i] = static_cast<uint16_t>((p[kFullBox + i * 2] << 8U) | p[kFullBox + i * 2 + 1]);
      }
    }
  }

  // ---- step 8: sample groups --------------------------------------------------------------------------------------

  [[nodiscard]] const fb::BoxNodeT* find_sgpd(const std::string& grouping_type) const {
    for (const auto& c : stbl_->children) {
      if (!c || c->fourcc != "sgpd") {
        continue;
      }
      const Bytes p = payload_min(file_, c.get(), 8);
      if (!p.empty() && fourcc(p, 4) == grouping_type) {
        return c.get();
      }
    }
    return nullptr;
  }

  uint32_t sgpd_entry_count(const fb::BoxNodeT* sgpd) const {
    const Bytes p = payload_min(file_, sgpd, 12);
    if (p.empty()) {
      return 0;
    }
    const size_t pos = 8 + (p[0] == 1 ? 4U : 0U) + (p[0] >= 2 ? 4U : 0U);
    return pos + 4 <= p.size() ? rd32(p, pos) : 0;
  }

  void expand_group(const fb::BoxNodeT* sbgp) {
    const Bytes p = payload_min(file_, sbgp, 12);
    if (p.empty()) {
      return;
    }
    auto g = std::make_unique<fb::GroupMembershipT>();
    g->grouping_type = fourcc(p, 4);
    size_t pos = 8;
    if (p[0] == 1) {
      if (p.size() < 16) {
        return;
      }
      g->grouping_type_parameter = rd32(p, pos);
      pos += 4;
    }
    g->description_index.assign(t_->size.size(), 0);
    const std::vector<Run> runs =
        read_runs(View(p).subspan(pos - 4));  // read_runs expects the 4 bytes before entry_count
    uint64_t i = 0;
    uint32_t highest = 0;
    for (const Run& r : runs) {
      for (uint32_t k = 0; k < r.count && i < count_; ++k, ++i) {
        g->description_index[i] = r.value;
        if (r.value < kFragmentLocal) {
          highest = std::max(highest, r.value);
        }
      }
    }
    const fb::BoxNodeT* sgpd = find_sgpd(g->grouping_type);
    if (sgpd == nullptr) {
      diag("CNT-2021", "sbgp '" + g->grouping_type + "' has no sgpd with the same grouping_type", offset_of(sbgp));
    } else {
      g->sgpd_offset = offset_of(sgpd);
      if (const uint32_t entries = sgpd_entry_count(sgpd); highest > entries) {
        diag("CNT-2021",
             "sbgp '" + g->grouping_type + "' uses description index " + std::to_string(highest) + ", sgpd has " +
                 std::to_string(entries) + " entries",
             offset_of(sbgp));
      }
    }
    t_->groups.push_back(std::move(g));
  }

  void expand_groups() {
    for (const auto& c : stbl_->children) {
      if (c && c->fourcc == "sbgp") {
        expand_group(c.get());
      }
    }
  }

  // ---- step 9 -----------------------------------------------------------------------------------------------------

  void check_description_indexes() {
    const fb::BoxNodeT* stsd = child(stbl_, "stsd");
    const Bytes p = payload_min(file_, stsd, 8);
    if (p.empty()) {
      return;
    }
    const uint32_t entries = rd32(p, 4);
    Tally bad;
    for (uint64_t i = 0; i < count_; ++i) {
      if (t_->chunk_index[i] != 0 &&
          (t_->sample_description_index[i] == 0 || t_->sample_description_index[i] > entries)) {
        bad.add(i);
      }
    }
    if (bad.total > 0) {
      diag("CNT-2004",
           std::to_string(bad.total) + " samples have a sample_description_index of 0 or above stsd.entry_count " +
               std::to_string(entries) + ", first sample " + std::to_string(bad.first),
           offset_of(child(stbl_, "stsc")));
    }
  }

  // Cuts every per-sample vector to the final count and reports zero-size samples.
  void finish() {
    Tally zero;
    for (uint64_t i = 0; i < count_; ++i) {
      if (t_->size[i] == 0) {
        zero.add(i);
      }
    }
    if (zero.total > 0) {
      diag("CNT-2013", std::to_string(zero.total) + " zero-size samples, first sample " + std::to_string(zero.first),
           offset_of(child(stbl_, "stsz") != nullptr ? child(stbl_, "stsz") : child(stbl_, "stz2")));
    }
    t_->count = count_;
    t_->size.resize(count_);
    t_->dts.resize(count_);
    t_->duration.resize(count_);
    t_->cts_offset.resize(count_);
    t_->offset.resize(count_);
    t_->chunk_index.resize(count_);
    t_->sample_description_index.resize(count_);
    t_->is_sync.resize(count_);
    t_->is_partial_sync.resize(std::min<size_t>(t_->is_partial_sync.size(), count_));
    t_->sdtp.resize(std::min<size_t>(t_->sdtp.size(), count_));
    t_->degradation_priority.resize(std::min<size_t>(t_->degradation_priority.size(), count_));
    t_->fragment_index.assign(count_, -1);
    for (const auto& g : t_->groups) {
      g->description_index.resize(count_);
    }
  }
};

}  // namespace

const fb::BoxNodeT* find_trak(const BoxNodes& top_level, size_t trak_index) {
  const auto moov = std::ranges::find_if(top_level, [](const auto& n) { return n && n->fourcc == "moov"; });
  if (moov == top_level.end()) {
    return nullptr;
  }
  size_t seen = 0;
  for (const auto& c : (*moov)->children) {
    if (c && c->fourcc == "trak" && seen++ == trak_index) {
      return c.get();
    }
  }
  return nullptr;
}

std::unique_ptr<fb::SampleTableT> expand_sample_table(const io::RandomAccessFile& file, const fb::BoxNodeT& trak,
                                                      jobs::Cancel& cancel) {
  return Expander(file, trak, cancel).run();
}

std::unique_ptr<fb::SampleTableT> slice_sample_table(const fb::SampleTableT& table, uint64_t first, uint64_t count) {
  auto out = std::make_unique<fb::SampleTableT>();
  out->track_id = table.track_id;
  out->timescale = table.timescale;
  out->has_subsamples = table.has_subsamples;
  out->composition_to_dts_shift = table.composition_to_dts_shift;
  for (const auto& d : table.diagnostics) {
    out->diagnostics.push_back(std::make_unique<fb::DiagnosticT>(*d));
  }
  const uint64_t begin = std::min(first, table.count);
  const uint64_t end = count == 0 ? table.count : std::min(table.count, begin + std::min(count, table.count - begin));
  out->count = end - begin;
  const auto cut = [&](const auto& src, auto& dst) {
    if (!src.empty()) {
      dst.assign(src.begin() + static_cast<std::ptrdiff_t>(begin), src.begin() + static_cast<std::ptrdiff_t>(end));
    }
  };
  cut(table.dts, out->dts);
  cut(table.cts_offset, out->cts_offset);
  cut(table.duration, out->duration);
  cut(table.offset, out->offset);
  cut(table.size, out->size);
  cut(table.is_sync, out->is_sync);
  cut(table.is_partial_sync, out->is_partial_sync);
  cut(table.sample_description_index, out->sample_description_index);
  cut(table.chunk_index, out->chunk_index);
  cut(table.fragment_index, out->fragment_index);
  cut(table.sdtp, out->sdtp);
  cut(table.degradation_priority, out->degradation_priority);
  for (const auto& g : table.groups) {
    auto ng = std::make_unique<fb::GroupMembershipT>();
    ng->grouping_type = g->grouping_type;
    ng->grouping_type_parameter = g->grouping_type_parameter;
    ng->sgpd_offset = g->sgpd_offset;
    cut(g->description_index, ng->description_index);
    out->groups.push_back(std::move(ng));
  }
  return out;
}

flatbuffers::DetachedBuffer serialise(const fb::SampleTableT& table) {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::SampleTable::Pack(fbb, &table));
  return fbb.Release();
}

}  // namespace ovt::container::isobmff
