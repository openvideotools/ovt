// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// FragmentTimeline builder (chapter 08 §8.4.3, WI-1.219a/b). See fragments.h.

#include "container/isobmff/fragments.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "container/diagnostics.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

constexpr uint64_t kMaxPayload = uint64_t{256} << 20U;  // largest box payload read (a 16 Mi-entry trun)
constexpr auto kInt64Max = std::numeric_limits<int64_t>::max();
constexpr uint64_t kCancelEvery = uint64_t{1} << 16U;  // samples between two cancel checks

// tfhd flags (14496-12 §8.8.7)
constexpr uint32_t kBaseDataOffset = 0x1;
constexpr uint32_t kSdiPresent = 0x2;
constexpr uint32_t kDurationPresent = 0x8;
constexpr uint32_t kSizePresent = 0x10;
constexpr uint32_t kFlagsPresent = 0x20;
constexpr uint32_t kDurationIsEmpty = 0x10000;
constexpr uint32_t kDefaultBaseIsMoof = 0x20000;
// trun flags (14496-12 §8.8.8)
constexpr uint32_t kDataOffset = 0x1;
constexpr uint32_t kFirstSampleFlags = 0x4;
constexpr uint32_t kSampleDuration = 0x100;
constexpr uint32_t kSampleSize = 0x200;
constexpr uint32_t kSampleFlags = 0x400;
constexpr uint32_t kSampleCts = 0x800;

// One box header found by a scan (the tree's top level, or the children of a moof / traf / mfra).
struct Hdr {
  std::string type;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint32_t header = 8;
  [[nodiscard]] uint64_t end() const noexcept { return offset + size; }
  [[nodiscard]] uint64_t payload() const noexcept { return offset + header; }
};

uint64_t be(std::span<const uint8_t> b) {
  uint64_t v = 0;
  for (const uint8_t x : b) {
    v = (v << 8U) | x;
  }
  return v;
}

// The box headers in [begin, end). Stops at the first header that does not fit (the walk reports those, §8.2.3).
std::vector<Hdr> scan(const io::RandomAccessFile& file, uint64_t begin, uint64_t end) {
  std::vector<Hdr> out;
  uint64_t pos = begin;
  while (pos < end && end - pos >= 8) {
    std::array<uint8_t, 16> b{};
    if (file.read(pos, 8, b.data()) != 8) {
      break;
    }
    Hdr h{std::string(b.begin() + 4, b.begin() + 8), pos, be(std::span(b).first(4)), 8};
    if (h.size == 1) {
      if (end - pos < 16 || file.read(pos + 8, 8, std::span(b).subspan(8).data()) != 8) {
        break;
      }
      h.size = be(std::span(b).subspan(8, 8));
      h.header = 16;
    } else if (h.size == 0) {
      h.size = end - pos;
    }
    if (h.type == "uuid") {
      h.header += 16;
    }
    if (h.size < h.header || h.size > end - pos) {
      break;
    }
    out.push_back(std::move(h));
    pos += out.back().size;
  }
  return out;
}

std::vector<Hdr> children(const io::RandomAccessFile& file, const Hdr& h) {
  return scan(file, h.payload(), h.end());
}

std::vector<uint8_t> payload(const io::RandomAccessFile& file, const Hdr& h) {
  std::vector<uint8_t> v(std::min(h.size - h.header, kMaxPayload));
  v.resize(file.read(h.payload(), v.size(), v.data()));
  return v;
}

const Hdr* first_of(const std::vector<Hdr>& boxes, std::string_view type) {
  const auto it = std::ranges::find_if(boxes, [&](const Hdr& h) { return h.type == type; });
  return it == boxes.end() ? nullptr : &*it;
}

Hdr hdr_of(const fb::BoxNodeT& n) {
  Hdr h{n.fourcc, 0, 0, n.header_length};
  if (n.file_span) {
    h.offset = n.file_span->offset();
    h.size = n.file_span->length();
  }
  return h;
}

uint32_t u32(BitReader& br) {
  return static_cast<uint32_t>(br.u(32));
}
uint64_t u32_or_64(BitReader& br, bool wide) {
  return br.u(wide ? 64 : 32);
}

// ---- box payload parsers (FullBox: version + flags first). Each returns what it read before a ParseError. ----

struct Tfhd {
  bool ok = false;
  uint32_t flags = 0;
  uint32_t track_id = 0;
  uint64_t base_data_offset = 0;
  uint32_t sdi = 0;
  uint32_t duration = 0;
  uint32_t size = 0;
  uint32_t sample_flags = 0;
};

Tfhd parse_tfhd(std::span<const uint8_t> p) {
  Tfhd t;
  try {
    BitReader br(p);
    br.skip_bits(8);
    t.flags = static_cast<uint32_t>(br.u(24));
    t.track_id = u32(br);
    t.base_data_offset = (t.flags & kBaseDataOffset) != 0 ? br.u(64) : 0;
    t.sdi = (t.flags & kSdiPresent) != 0 ? u32(br) : 0;
    t.duration = (t.flags & kDurationPresent) != 0 ? u32(br) : 0;
    t.size = (t.flags & kSizePresent) != 0 ? u32(br) : 0;
    t.sample_flags = (t.flags & kFlagsPresent) != 0 ? u32(br) : 0;
    t.ok = true;
  } catch (const ParseError&) {  // truncated tfhd: the decoder reports it (CNT-1014); the traf is skipped
  }
  return t;
}

std::optional<uint64_t> parse_tfdt(std::span<const uint8_t> p) {
  try {
    BitReader br(p);
    const bool wide = br.u(8) == 1;
    br.skip_bits(24);
    return u32_or_64(br, wide);
  } catch (const ParseError&) {
    return std::nullopt;
  }
}

std::optional<uint32_t> parse_u32_after_fullbox(std::span<const uint8_t> p) {
  try {
    BitReader br(p);
    br.skip_bits(32);
    return u32(br);
  } catch (const ParseError&) {
    return std::nullopt;
  }
}

struct Trun {
  uint32_t version = 0;
  uint32_t flags = 0;
  uint32_t count = 0;    // samples whose entries are present (sample_count clamped to the payload)
  uint32_t written = 0;  // sample_count as written
  int32_t data_offset = 0;
  uint32_t first_flags = 0;
  std::vector<uint8_t> bytes;  // the payload; entries start at `entries`
  size_t entries = 0;
  uint32_t entry_size = 0;
};

Trun parse_trun(std::vector<uint8_t> p) {
  Trun t;
  t.bytes = std::move(p);
  try {
    BitReader br(t.bytes);
    t.version = static_cast<uint32_t>(br.u(8));
    t.flags = static_cast<uint32_t>(br.u(24));
    t.written = u32(br);
    t.data_offset = (t.flags & kDataOffset) != 0 ? static_cast<int32_t>(br.i(32)) : 0;
    t.first_flags = (t.flags & kFirstSampleFlags) != 0 ? u32(br) : 0;
    t.entries = br.bit_pos() / 8;
  } catch (const ParseError&) {
    t.flags &= kDataOffset | kFirstSampleFlags;  // header cut: no usable entries
    t.entries = t.bytes.size();
  }
  t.entry_size = 4U * static_cast<uint32_t>(std::popcount(t.flags & 0xF00U));
  const uint64_t fit = t.entry_size == 0 ? t.written : (t.bytes.size() - t.entries) / t.entry_size;
  t.count = static_cast<uint32_t>(std::min<uint64_t>(t.written, fit));
  return t;
}

struct TrunEntry {
  std::optional<uint32_t> duration, size, flags;
  int32_t cts = 0;
};

TrunEntry entry(const Trun& t, uint32_t i) {
  TrunEntry e;
  BitReader br(std::span(t.bytes).subspan(t.entries + size_t{i} * t.entry_size, t.entry_size));
  if ((t.flags & kSampleDuration) != 0) {
    e.duration = u32(br);
  }
  if ((t.flags & kSampleSize) != 0) {
    e.size = u32(br);
  }
  if ((t.flags & kSampleFlags) != 0) {
    e.flags = u32(br);
  }
  if ((t.flags & kSampleCts) != 0) {
    // v0 is unsigned; values ≥ 2^31 are read as negative, as for ctts v0 (§8.4.1 step 3).
    e.cts = static_cast<int32_t>(u32(br));
  }
  return e;
}

std::vector<uint64_t> parse_saio(std::span<const uint8_t> p) {
  std::vector<uint64_t> out;
  try {
    BitReader br(p);
    const bool wide = br.u(8) != 0;
    if ((br.u(24) & 1U) != 0) {
      br.skip_bits(64);  // aux_info_type, aux_info_type_parameter
    }
    const uint32_t n = u32(br);
    for (uint32_t i = 0; i < n && br.bits_left() >= (wide ? 64U : 32U); ++i) {
      out.push_back(u32_or_64(br, wide));
    }
  } catch (const ParseError&) {
  }
  return out;
}

struct SidxBox {
  bool ok = false;
  uint32_t reference_id = 0;
  uint32_t timescale = 0;
  uint64_t ept = 0;
  uint64_t first_offset = 0;
  std::vector<std::unique_ptr<fb::SidxRefT>> refs;
};

SidxBox parse_sidx(std::span<const uint8_t> p) {
  SidxBox s;
  try {
    BitReader br(p);
    const bool wide = br.u(8) != 0;
    br.skip_bits(24);
    s.reference_id = u32(br);
    s.timescale = u32(br);
    s.ept = u32_or_64(br, wide);
    s.first_offset = u32_or_64(br, wide);
    br.skip_bits(16);
    const auto n = static_cast<uint32_t>(br.u(16));
    for (uint32_t i = 0; i < n; ++i) {
      auto r = std::make_unique<fb::SidxRefT>();
      r->reference_type = static_cast<uint8_t>(br.u(1));
      r->referenced_size = static_cast<uint32_t>(br.u(31));
      r->subsegment_duration = u32(br);
      r->starts_with_sap = br.u(1) != 0;
      r->sap_type = static_cast<uint8_t>(br.u(3));
      r->sap_delta_time = static_cast<uint32_t>(br.u(28));
      r->actual_size = -1;
      r->actual_duration = -1;
      s.refs.push_back(std::move(r));
    }
    s.ok = true;
  } catch (const ParseError&) {
    s.ok = !s.refs.empty();
  }
  return s;
}

struct TfraEntry {
  uint64_t time = 0;
  uint64_t moof_offset = 0;
  uint64_t traf = 0, trun = 0, sample = 0;  // 1-based
};

std::pair<uint32_t, std::vector<TfraEntry>> parse_tfra(std::span<const uint8_t> p) {
  std::pair<uint32_t, std::vector<TfraEntry>> out{0, {}};
  try {
    BitReader br(p);
    const bool wide = br.u(8) == 1;
    br.skip_bits(24);
    out.first = u32(br);
    br.skip_bits(26);
    const auto traf_len = static_cast<unsigned>(br.u(2) + 1) * 8U;
    const auto trun_len = static_cast<unsigned>(br.u(2) + 1) * 8U;
    const auto sample_len = static_cast<unsigned>(br.u(2) + 1) * 8U;
    const uint32_t n = u32(br);
    for (uint32_t i = 0; i < n; ++i) {
      TfraEntry e;
      e.time = u32_or_64(br, wide);
      e.moof_offset = u32_or_64(br, wide);
      e.traf = br.u(traf_len);
      e.trun = br.u(trun_len);
      e.sample = br.u(sample_len);
      out.second.push_back(e);
    }
  } catch (const ParseError&) {
  }
  return out;
}

std::optional<uint64_t> add_signed(uint64_t a, int64_t b) {
  uint64_t r = 0;
  if (b >= 0 ? __builtin_add_overflow(a, static_cast<uint64_t>(b), &r)
             : __builtin_sub_overflow(a, static_cast<uint64_t>(-(b + 1)) + 1U, &r)) {
    return std::nullopt;
  }
  return r;
}

std::string seconds(int64_t ticks, uint32_t timescale) {
  if (timescale == 0) {
    return "?";
  }
  return std::to_string(static_cast<double>(ticks) / timescale);
}

// The traf being expanded and where its data stands.
struct TrafRun {
  const Hdr* moof = nullptr;
  const Hdr* mdat = nullptr;  // null when the moof has no mdat
  uint32_t traf_number = 0;   // 1-based within the moof (tfra numbering)
  size_t traf_count = 0;
  uint64_t data_end = 0;  // end of the previous traf's sample data (implicit base, §8.4.3 2.2)
  uint64_t max_end = 0;   // largest sample end of the moof, all tracks (CNT-4022)
};

// Where the samples of one traf of this track went (tfra lookup).
struct TrafLoc {
  uint64_t moof_offset = 0;
  uint32_t traf_number = 0;
  std::vector<std::pair<size_t, uint32_t>> truns;  // first sample index, sample count
};

class Builder {
 public:
  Builder(const io::RandomAccessFile& file, const BoxNodes& top, const FragmentTrackInput& in, jobs::Cancel& cancel)
      : file_(file),
        in_(in),
        cancel_(cancel),
        // SPEC-ASSUMPTION(#788): like a constant-size stsz, the fragment samples claim no more than one byte each
        // of the file, so a few bytes of trun cannot expand to kMaxFragmentSamples (about 2 GB).
        file_bound_(std::max<uint64_t>(file.size(), 1)),
        limit_(std::min({in.max_samples, kMaxFragmentSamples, file_bound_})) {
    for (const auto& n : top) {
      if (n) {
        top_.push_back(hdr_of(*n));
      }
    }
    if (in.has_moov_samples) {
      expected_dts_ = static_cast<int64_t>(std::min<uint64_t>(in.moov_duration, kInt64Max));
    }
  }

  FragmentBuild run() {
    out_.timeline.track_id = in_.track_id;
    std::optional<size_t> styp;
    for (size_t i = 0; i < top_.size(); ++i) {
      if (top_[i].type == "styp") {
        styp = i;
        check_styp(top_[i]);
      } else if (top_[i].type == "moof") {
        cancel_.throw_if_requested();
        fragment(i, styp);
        styp.reset();
      }
    }
    build_duration_prefix();
    min_cts_.assign(out_.timeline.fragments.size(), std::nullopt);
    for (const Hdr& h : top_) {
      if (h.type == "sidx") {
        sidx(h);
      } else if (h.type == "mfra") {
        mfra(h);
      }
    }
    apply_ref_ranges();
    return std::move(out_);
  }

 private:
  void diag(std::string_view code, std::string message, uint64_t box_offset) {
    const DiagInfo* info = ovt::container::diag(code);
    auto d = std::make_unique<fb::DiagnosticT>();
    d->severity = info != nullptr ? static_cast<fb::Severity>(info->severity) : fb::Severity::Error;
    d->code = std::string(code);
    d->message = std::move(message);
    d->spec_ref = info != nullptr ? std::string(info->spec_ref) : std::string{};
    d->span = std::make_unique<fb::BitSpan>(box_offset * 8U, 0U);  // file-relative: the box the finding is about
    out_.timeline.diagnostics.push_back(std::move(d));
  }

  void check_styp(const Hdr& h) {
    std::vector<uint8_t> brands = payload(file_, h);
    if (!first_styp_) {
      first_styp_ = std::move(brands);
    } else if (!styp_reported_ && brands != *first_styp_) {
      styp_reported_ = true;
      diag("CNT-4036", "styp brands at offset " + std::to_string(h.offset) + " differ from the first styp", h.offset);
    }
  }

  // §8.4.3 step 1: the mdat paired with the moof at top_[i] (the next top-level mdat before the next moof).
  const Hdr* pair_mdat(size_t i) {
    for (size_t j = i + 1; j < top_.size() && top_[j].type != "moof"; ++j) {
      if (top_[j].type == "mdat") {
        if (j != i + 1) {
          diag(
              "CNT-4004",
              "moof at offset " + std::to_string(top_[i].offset) + " is followed by " + top_[i + 1].type + ", not mdat",
              top_[i].offset);
        }
        return &top_[j];
      }
    }
    diag("CNT-4004", "moof at offset " + std::to_string(top_[i].offset) + " has no mdat", top_[i].offset);
    return nullptr;
  }

  void fragment(size_t i, std::optional<size_t> styp) {
    const Hdr& moof = top_[i];
    const std::vector<Hdr> kids = children(file_, moof);
    std::vector<const Hdr*> trafs;
    for (const Hdr& k : kids) {
      if (k.type == "traf") {
        trafs.push_back(&k);
      }
    }
    auto info = std::make_unique<fb::FragmentInfoT>();
    info->index = static_cast<uint32_t>(out_.timeline.fragments.size());
    info->moof_offset = moof.offset;
    info->moof_size = moof.size;
    info->styp_offset = styp ? static_cast<int64_t>(top_[*styp].offset) : -1;
    info->sidx_reference_index = -1;
    TrafRun fr{&moof, nullptr, 0, trafs.size(), moof.offset, 0};
    bool mine = false;
    for (const Hdr* t : trafs) {
      const std::vector<uint8_t> tfhd = [&] {
        const std::vector<Hdr> tk = children(file_, *t);
        const Hdr* h = first_of(tk, "tfhd");
        return h != nullptr ? payload(file_, *h) : std::vector<uint8_t>{};
      }();
      const Tfhd h = parse_tfhd(tfhd);
      mine = mine || (h.ok && h.track_id == in_.track_id);
    }
    if (!mine) {
      return;  // no traf of this track: not one of its fragments
    }
    fr.mdat = pair_mdat(i);
    if (fr.mdat != nullptr) {
      info->mdat_offset = fr.mdat->offset;
      info->mdat_size = fr.mdat->size;
    }
    if (const Hdr* mfhd = first_of(kids, "mfhd"); mfhd != nullptr) {
      info->sequence_number = parse_u32_after_fullbox(payload(file_, *mfhd)).value_or(0);
      if (last_seq_ && info->sequence_number <= *last_seq_) {
        diag("CNT-4001",
             "mfhd.sequence_number " + std::to_string(info->sequence_number) + " of fragment " +
                 std::to_string(info->index) + " does not increase (previous " + std::to_string(*last_seq_) + ")",
             moof.offset);
      }
      last_seq_ = info->sequence_number;
    }
    current_ = info.get();
    frag_first_sample_.push_back(out_.samples.count());
    for (const Hdr* t : trafs) {
      ++fr.traf_number;
      traf(*t, fr);
    }
    unreferenced_tail(fr, *info);
    out_.timeline.fragments.push_back(std::move(info));
    current_ = nullptr;
  }

  void unreferenced_tail(const TrafRun& fr, const fb::FragmentInfoT& info) {
    if (fr.mdat == nullptr || fr.max_end < fr.mdat->payload() || fr.max_end >= fr.mdat->end()) {
      return;
    }
    diag("CNT-4022",
         std::to_string(fr.mdat->end() - fr.max_end) + " bytes of mdat at offset " + std::to_string(fr.mdat->offset) +
             " not referenced by trun (fragment " + std::to_string(info.index) + ")",
         fr.mdat->offset);
  }

  // Defaults of §8.4.3 2.1: tfhd value, else trex, else missing.
  struct Defaults {
    std::optional<uint32_t> duration, size, flags;
    uint32_t sdi = 1;
  };

  [[nodiscard]] Defaults defaults_of(const Tfhd& t) const {
    Defaults d;
    const auto trex = in_.trex.find(t.track_id);
    const TrexDefaults* x = trex == in_.trex.end() ? nullptr : &trex->second;
    const auto pick = [&](uint32_t flag, uint32_t tf, uint32_t TrexDefaults::* m) -> std::optional<uint32_t> {
      if ((t.flags & flag) != 0) {
        return tf;
      }
      return x != nullptr ? std::optional<uint32_t>(x->*m) : std::nullopt;
    };
    d.duration = pick(kDurationPresent, t.duration, &TrexDefaults::duration);
    d.size = pick(kSizePresent, t.size, &TrexDefaults::size);
    d.flags = pick(kFlagsPresent, t.sample_flags, &TrexDefaults::flags);
    const std::optional<uint32_t> sdi = pick(kSdiPresent, t.sdi, &TrexDefaults::sample_description_index);
    d.sdi = sdi.value_or(1) != 0 ? sdi.value_or(1) : 1;
    return d;
  }

  // §8.4.3 2.2.
  uint64_t base_of(const Tfhd& t, const TrafRun& fr, bool mine) {
    if ((t.flags & kBaseDataOffset) != 0) {
      return t.base_data_offset;
    }
    if ((t.flags & kDefaultBaseIsMoof) != 0) {
      return fr.moof->offset;
    }
    if (mine && fr.traf_count > 1) {
      diag("CNT-4006",
           "implicit base data offsets with " + std::to_string(fr.traf_count) + " trafs in the moof at " +
               std::to_string(fr.moof->offset),
           fr.moof->offset);
    }
    return fr.traf_number == 1 ? fr.moof->offset : fr.data_end;
  }

  void traf(const Hdr& box, TrafRun& fr) {
    const std::vector<Hdr> kids = children(file_, box);
    const Hdr* tfhd_box = first_of(kids, "tfhd");
    const Tfhd t = tfhd_box != nullptr ? parse_tfhd(payload(file_, *tfhd_box)) : Tfhd{};
    if (!t.ok) {
      return;  // CNT-1011 / CNT-1014 are the decoders' (check_fragment_children)
    }
    const bool mine = t.track_id == in_.track_id;
    const uint64_t base = base_of(t, fr, mine);
    auto ti = std::make_unique<fb::TrafInfoT>();
    ti->track_id = t.track_id;
    ti->tfhd_flags = t.flags;
    ti->base_data_offset = base;
    const Defaults d = defaults_of(t);
    ti->default_sample_duration = d.duration.value_or(0);
    ti->default_sample_size = d.size.value_or(0);
    ti->default_sample_flags = d.flags.value_or(0);
    if (mine) {
      start_dts(kids, *ti);
    }
    const size_t first_sample = out_.samples.count();
    TrafLoc loc{fr.moof->offset, fr.traf_number, {}};
    TrunState st{base, std::nullopt, false, 0, 0};
    for (const Hdr& k : kids) {
      if (k.type == "trun") {
        trun(k, t, d, mine, st, *ti, loc, fr);
      }
    }
    fr.data_end = st.prev_end.value_or(base);
    if (!mine) {
      return;
    }
    if (st.missing_default) {
      diag("CNT-4005",
           "trun of track " + std::to_string(t.track_id) +
               " omits a sample duration/size/flags that neither tfhd nor trex supplies (0 used)",
           box.offset);
    }
    if (st.out_of_mdat != 0) {
      diag("CNT-4021",
           std::to_string(st.out_of_mdat) + " samples of fragment " + std::to_string(current_->index) +
               " lie outside their mdat (first at offset " + std::to_string(st.first_out) + ")",
           box.offset);
    }
    expected_dts_ = dts_;
    saio(kids, *ti);
    sample_groups(kids, first_sample);
    locs_.push_back(std::move(loc));
    current_->trafs.push_back(std::move(ti));
  }

  // §8.4.1 fragment part: the traf's `sbgp` runs over its samples [first, count) (14496-12 §8.9.2). Entries past the
  // traf's samples are ignored; a truncated box keeps the entries read so far.
  void sample_groups(const std::vector<Hdr>& kids, size_t first) {
    const size_t end = out_.samples.count();
    for (const Hdr& k : kids) {
      if (k.type != "sbgp") {
        continue;
      }
      const std::vector<uint8_t> p = payload(file_, k);
      try {
        BitReader br(p);
        const bool v1 = br.u(8) == 1;
        br.skip_bits(24);
        std::string type(4, ' ');
        for (char& c : type) {
          c = static_cast<char>(br.u(8));
        }
        const uint32_t param = v1 ? u32(br) : 0;
        FragmentGroup& g = group_of(type, param, kids);
        const uint32_t entries = u32(br);
        size_t i = first;
        for (uint32_t e = 0; e < entries && i < end; ++e) {
          const uint32_t count = u32(br);
          const uint32_t index = u32(br);
          const size_t stop = std::min<size_t>(end, i + count);
          for (; i < stop; ++i) {
            tick();
            if (index != 0) {
              g.members.emplace_back(i, index);
            }
          }
        }
      } catch (const ParseError&) {
      }
    }
  }

  FragmentGroup& group_of(const std::string& type, uint32_t param, const std::vector<Hdr>& kids) {
    auto& groups = out_.samples.groups;
    auto it = std::ranges::find_if(
        groups, [&](const FragmentGroup& g) { return g.grouping_type == type && g.grouping_type_parameter == param; });
    if (it == groups.end()) {
      groups.push_back(FragmentGroup{type, param, 0, {}});
      it = std::prev(groups.end());
    }
    if (it->sgpd_offset == 0) {
      for (const Hdr& k : kids) {
        if (k.type == "sgpd") {
          const std::vector<uint8_t> p = payload(file_, k);
          if (p.size() >= 8 && std::equal(type.begin(), type.end(), p.begin() + 4)) {
            it->sgpd_offset = k.offset;
            break;
          }
        }
      }
    }
    return *it;
  }

  // §8.4.3 2.4 and 2.5 (CNT-4020 names the 0-based FragmentInfo.index).
  void start_dts(const std::vector<Hdr>& kids, fb::TrafInfoT& ti) {
    const Hdr* tfdt_box = first_of(kids, "tfdt");
    const std::optional<uint64_t> tfdt = tfdt_box != nullptr ? parse_tfdt(payload(file_, *tfdt_box)) : std::nullopt;
    const bool first_traf = current_->trafs.empty();
    if (tfdt) {
      ti.has_tfdt = true;
      ti.tfdt = *tfdt;
      dts_ = static_cast<int64_t>(std::min<uint64_t>(*tfdt, kInt64Max));
      if (expected_dts_ && first_traf) {
        const int64_t gap = dts_ - *expected_dts_;
        current_->expected_dts = static_cast<uint64_t>(*expected_dts_);
        current_->dts_gap = gap;
        if (gap != 0) {
          diag("CNT-4020",
               std::string("tfdt discontinuity: ") + (gap > 0 ? "gap" : "overlap") + " of " +
                   std::to_string(gap > 0 ? gap : -gap) + " ticks (" + seconds(gap, in_.timescale) +
                   " s) before fragment " + std::to_string(current_->index) + " (sequence_number " +
                   std::to_string(current_->sequence_number) + ")",
               current_->moof_offset);
        }
      }
    } else {
      dts_ = expected_dts_.value_or(0);
      if (first_traf) {
        current_->expected_dts = static_cast<uint64_t>(dts_);
      }
      diag("CNT-4008",
           "no tfdt in fragment " + std::to_string(current_->index) + "; DTS " + std::to_string(dts_) +
               " inferred by continuity",
           current_->moof_offset);
    }
    ti.computed_dts_start = static_cast<uint64_t>(dts_);
  }

  struct TrunState {
    uint64_t base = 0;
    std::optional<uint64_t> prev_end;  // first_sample_offset + size_sum of the previous trun of this traf
    bool missing_default = false;
    uint64_t out_of_mdat = 0;
    uint64_t first_out = 0;
  };

  // §8.4.3 2.3: the first sample offset of a trun (nullopt on overflow).
  std::optional<uint64_t> first_offset(const Hdr& box, const Trun& r, const Tfhd& t, bool mine, const TrunState& st) {
    if ((r.flags & kDataOffset) != 0) {
      return add_signed(st.base, r.data_offset);
    }
    if (mine && !st.prev_end && (t.flags & kBaseDataOffset) == 0) {
      diag("CNT-4007",
           "first trun of a traf (offset " + std::to_string(box.offset) +
               ") has no data_offset and the tfhd no base_data_offset",
           box.offset);
    }
    return st.prev_end.value_or(st.base);
  }

  // §8.4.3 2.3, and 2.6 for this track's samples.
  void trun(const Hdr& box, const Tfhd& t, const Defaults& d, bool mine, TrunState& st, fb::TrafInfoT& ti, TrafLoc& loc,
            TrafRun& fr) {
    const Trun r = parse_trun(payload(file_, box));
    auto info = std::make_unique<fb::TrunInfoT>();
    info->trun_offset = box.offset;
    info->flags = r.flags;
    info->sample_count = r.written;
    info->data_offset = r.data_offset;
    if (mine && r.written == 0 && (t.flags & kDurationIsEmpty) == 0) {
      diag("CNT-4035",
           "trun at offset " + std::to_string(box.offset) +
               " has sample_count 0 and the tfhd is not "
               "duration-is-empty",
           box.offset);
    }
    const std::optional<uint64_t> first = first_offset(box, r, t, mine, st);
    if (!first) {
      if (!mine) {
        return;
      }
      diag("CNT-2030", "64-bit overflow in the data offset of the trun at offset " + std::to_string(box.offset),
           box.offset);
      return;
    }
    info->first_sample_offset = *first;
    if (!mine) {  // another track's trun only moves the data end (implicit base chain, CNT-4022)
      const uint64_t end = foreign_end(r, *first, d.size.value_or(0));
      fr.max_end = std::max(fr.max_end, end);
      st.prev_end = end;
      return;
    }
    const size_t first_index = out_.samples.count();
    uint64_t pos = *first;
    for (uint32_t s = 0; s < r.count; ++s) {
      const TrunEntry e = entry(r, s);
      const uint32_t dur = e.duration ? *e.duration : d.duration.value_or(0);
      const uint32_t size = e.size ? *e.size : d.size.value_or(0);
      const bool first_flags = s == 0 && (r.flags & kFirstSampleFlags) != 0;
      const std::optional<uint32_t> flags = first_flags ? std::optional(r.first_flags) : e.flags ? e.flags : d.flags;
      st.missing_default = st.missing_default || (!e.duration && !d.duration) || (!e.size && !d.size) || !flags;
      if (!sample(pos, size, dur, flags.value_or(0), e.cts, d.sdi, fr, st)) {
        break;
      }
      info->duration_sum += dur;
      if (__builtin_add_overflow(pos, uint64_t{size}, &pos)) {
        diag("CNT-2030", "64-bit overflow in the sample offsets of the trun at offset " + std::to_string(box.offset),
             box.offset);
        break;
      }
      fr.max_end = std::max(fr.max_end, pos);
    }
    info->size_sum = pos - *first;
    st.prev_end = pos;
    loc.truns.emplace_back(first_index, static_cast<uint32_t>(out_.samples.count() - first_index));
    ti.truns.push_back(std::move(info));
  }

  // The end of another track's trun data without expanding it: count × default size, or the written sizes (which
  // the payload bounds). Saturates at UINT64_MAX.
  uint64_t foreign_end(const Trun& r, uint64_t first, uint32_t default_size) {
    uint64_t total = 0;
    if ((r.flags & kSampleSize) == 0) {
      if (__builtin_mul_overflow(uint64_t{r.count}, uint64_t{default_size}, &total)) {
        return UINT64_MAX;
      }
    } else {
      for (uint32_t s = 0; s < r.count; ++s) {
        if (s % kCancelEvery == 0) {
          cancel_.throw_if_requested();
        }
        total += entry(r, s).size.value_or(0);  // ≤ 2^26 entries of ≤ 2^32: no overflow
      }
    }
    uint64_t end = 0;
    return __builtin_add_overflow(first, total, &end) ? UINT64_MAX : end;
  }

  // Appends one sample of this track; false when the expansion stops (bound or overflow).
  bool sample(uint64_t offset, uint32_t size, uint32_t dur, uint32_t flags, int32_t cts, uint32_t sdi,
              const TrafRun& fr, TrunState& st) {
    FragmentSamples& o = out_.samples;
    if (o.count() >= limit_) {
      if (!cap_reported_) {
        cap_reported_ = true;
        diag("CNT-2031",
             "more than " + std::to_string(o.count()) + " fragment samples" +
                 (limit_ == file_bound_ ? " in a file of " + std::to_string(file_bound_) + " bytes" : "") +
                 "; table truncated",
             current_->moof_offset);
      }
      return false;
    }
    if (o.count() % kCancelEvery == 0) {
      cancel_.throw_if_requested();
    }
    const bool inside = fr.mdat != nullptr && offset >= fr.mdat->payload() && offset <= fr.mdat->end() &&
                        size <= fr.mdat->end() - offset;
    if (!inside && st.out_of_mdat++ == 0) {
      st.first_out = offset;
    }
    o.dts.push_back(dts_);
    o.cts_offset.push_back(cts);
    o.duration.push_back(dur);
    o.offset.push_back(offset);
    o.size.push_back(size);
    o.flags.push_back(flags);
    o.sample_description_index.push_back(sdi);
    o.fragment_index.push_back(static_cast<int32_t>(current_->index));
    if (__builtin_add_overflow(dts_, int64_t{dur}, &dts_)) {
      diag("CNT-2030", "64-bit overflow in the DTS of fragment " + std::to_string(current_->index),
           current_->moof_offset);
      return false;
    }
    return true;
  }

  // §8.4.3 2.7.
  void saio(const std::vector<Hdr>& kids, fb::TrafInfoT& ti) {
    const Hdr* senc = first_of(kids, "senc");
    ti.senc_offset = senc != nullptr ? senc->offset : 0;
    const Hdr* saio_box = first_of(kids, "saio");
    if (saio_box == nullptr) {
      return;
    }
    for (const uint64_t off : parse_saio(payload(file_, *saio_box))) {
      uint64_t r = 0;
      ti.saio_resolved.push_back(__builtin_add_overflow(ti.base_data_offset, off, &r) ? 0 : r);
    }
    // The aux data of a senc starts at its first IV, after the FullBox header and the u32 sample_count (§8.4.3 2.7).
    if (senc != nullptr && !ti.saio_resolved.empty() && ti.saio_resolved[0] != senc->payload() + 8) {
      diag("CNT-6005",
           "saio resolves to offset " + std::to_string(ti.saio_resolved[0]) +
               ", the senc sample data "
               "starts at " +
               std::to_string(senc->payload() + 8),
           saio_box->offset);
    }
  }

  // One unit of cross-check work: the sidx / tfra checks loop over untrusted counts, so they poll `cancel`.
  void tick() {
    if (++work_ % kCancelEvery == 0) {
      cancel_.throw_if_requested();
    }
  }

  // The box-aligned extent of top-level boxes starting at `begin` and covering [begin, end): its size, or −1 when
  // no top-level box starts at `begin`. `top_` is in file order, so both ends are binary searches.
  [[nodiscard]] int64_t extent(uint64_t begin, uint64_t end) const {
    const auto by_offset = [](const Hdr& h) { return h.offset; };
    const auto it = std::ranges::lower_bound(top_, begin, {}, by_offset);
    if (it == top_.end() || it->offset != begin) {
      return -1;
    }
    const auto last = std::ranges::lower_bound(it, top_.end(), end, {}, by_offset);
    return last == it ? 0 : static_cast<int64_t>(std::prev(last)->end() - begin);
  }

  // Prefix sums of the trun durations of every fragment (CNT-4032 in O(1) per reference). Unsigned, so a crafted
  // sum wraps instead of overflowing; the difference of two prefixes is still the range sum modulo 2^64.
  void build_duration_prefix() {
    dur_prefix_.assign(1, 0);
    for (const auto& f : out_.timeline.fragments) {
      uint64_t d = dur_prefix_.back();
      for (const auto& t : f->trafs) {
        for (const auto& tr : t->truns) {
          d += tr->duration_sum;
        }
      }
      dur_prefix_.push_back(d);
    }
  }

  // §8.4.3 step 3.
  void sidx(const Hdr& box) {
    cancel_.throw_if_requested();
    SidxBox s = parse_sidx(payload(file_, box));
    if (!s.ok || s.reference_id != in_.track_id) {
      return;
    }
    const bool same_scale = in_.timescale == 0 || s.timescale == in_.timescale;
    if (!same_scale) {
      diag("CNT-4031",
           "sidx.timescale " + std::to_string(s.timescale) + " differs from the track timescale " +
               std::to_string(in_.timescale),
           box.offset);
    }
    auto info = std::make_unique<fb::SidxInfoT>();
    info->sidx_offset = box.offset;
    info->reference_id = s.reference_id;
    info->timescale = s.timescale;
    info->earliest_presentation_time = s.ept;
    info->first_offset = s.first_offset;
    uint64_t abs = 0;
    bool wrapped = __builtin_add_overflow(box.end(), s.first_offset, &abs);
    for (size_t k = 0; k < s.refs.size(); ++k) {
      tick();
      fb::SidxRefT& r = *s.refs[k];
      uint64_t end = 0;
      wrapped = wrapped || __builtin_add_overflow(abs, uint64_t{r.referenced_size}, &end);
      if (wrapped) {  // the offsets leave the 64-bit range: this and the remaining references cover nothing
        diag("CNT-4030",
             "sidx reference " + std::to_string(k) + ": offset overflows (first_offset " +
                 std::to_string(s.first_offset) + ")",
             box.offset);
        for (; k < s.refs.size(); ++k) {
          s.refs[k]->actual_size = -1;
          info->refs.push_back(std::move(s.refs[k]));
        }
        break;
      }
      r.abs_offset = abs;
      r.abs_end = end;
      r.actual_size = extent(r.abs_offset, r.abs_end);
      if (r.actual_size != int64_t{r.referenced_size}) {
        diag("CNT-4030",
             "sidx reference " + std::to_string(k) + " at offset " + std::to_string(abs) + ": referenced_size " +
                 std::to_string(r.referenced_size) + ", actual " + std::to_string(r.actual_size),
             box.offset);
      }
      check_ref(r, k, k == 0 ? std::optional(s.ept) : std::nullopt, same_scale, box.offset);
      abs = end;
      info->refs.push_back(std::move(s.refs[k]));
    }
    out_.timeline.sidx.push_back(std::move(info));
  }

  // The smallest CTS of a fragment's samples, computed once per fragment.
  int64_t min_cts(size_t f) {
    if (!min_cts_[f]) {
      const size_t s0 = frag_first_sample_[f];
      const size_t s1 = f + 1 < frag_first_sample_.size() ? frag_first_sample_[f + 1] : out_.samples.count();
      int64_t m = kInt64Max;
      for (size_t i = s0; i < s1; ++i) {
        tick();
        m = std::min(m, out_.samples.dts[i] + out_.samples.cts_offset[i]);
      }
      min_cts_[f] = m;
    }
    return min_cts_[f].value_or(kInt64Max);
  }

  // Duration, EPT and SAP checks of one sidx reference against the fragments it covers. The fragments are in file
  // order, so the covered ones are a binary-searched range [lo, hi).
  void check_ref(fb::SidxRefT& r, size_t k, std::optional<uint64_t> ept, bool same_scale, uint64_t at) {
    const int32_t global = ref_count_++;
    const auto& frags = out_.timeline.fragments;
    const auto by_offset = [](const auto& f) { return f->moof_offset; };
    const auto lo = static_cast<size_t>(std::ranges::lower_bound(frags, r.abs_offset, {}, by_offset) - frags.begin());
    const auto hi = static_cast<size_t>(std::ranges::lower_bound(frags, r.abs_end, {}, by_offset) - frags.begin());
    if (lo >= hi) {
      return;
    }
    if (r.reference_type == 0) {  // a media reference; a type-1 reference covers the sidx it points at
      ref_ranges_.emplace_back(lo, hi, global);
    }
    const auto dur = static_cast<int64_t>(dur_prefix_[hi] - dur_prefix_[lo]);
    r.actual_duration = dur;
    const std::string ref = "sidx reference " + std::to_string(k) + " at offset " + std::to_string(r.abs_offset);
    if (same_scale && dur != int64_t{r.subsegment_duration}) {
      diag("CNT-4032",
           ref + ": subsegment_duration " + std::to_string(r.subsegment_duration) + ", trun durations " +
               std::to_string(dur) + " (delta " + std::to_string(int64_t{r.subsegment_duration} - dur) + ")",
           at);
    }
    const size_t s0 = frag_first_sample_[lo];
    const size_t s1 = lo + 1 < frag_first_sample_.size() ? frag_first_sample_[lo + 1] : out_.samples.count();
    if (s0 >= s1) {
      return;
    }
    if (r.reference_type == 0 && r.starts_with_sap && !out_.samples.is_sync(s0)) {
      diag("CNT-4034", ref + ": starts_with_SAP but the first sample is non-sync", at);
    }
    if (ept && same_scale && static_cast<int64_t>(*ept) != min_cts(lo)) {
      diag("CNT-4033",
           "sidx earliest_presentation_time " + std::to_string(*ept) + " ≠ first CTS " + std::to_string(min_cts(lo)),
           at);
    }
  }

  // FragmentInfo.sidx_reference_index of the media references, the last one in file order winning (as if assigned
  // in turn). Applied backwards with a skip list, so each fragment is written once however the ranges overlap.
  void apply_ref_ranges() {
    auto& frags = out_.timeline.fragments;
    std::vector<size_t> next(frags.size() + 1);
    std::iota(next.begin(), next.end(), size_t{0});
    const auto find = [&](size_t i) {
      while (next[i] != i) {
        next[i] = next[next[i]];
        i = next[i];
      }
      return i;
    };
    for (const auto& [lo, hi, global] : std::ranges::reverse_view(ref_ranges_)) {
      tick();
      for (size_t i = find(lo); i < hi; i = find(i)) {
        frags[i]->sidx_reference_index = global;
        next[i] = i + 1;
      }
    }
  }

  // §8.4.3 step 4.
  void mfra(const Hdr& box) {
    const std::vector<Hdr> kids = children(file_, box);
    if (const Hdr* mfro = first_of(kids, "mfro"); mfro != nullptr) {
      const std::optional<uint32_t> size = parse_u32_after_fullbox(payload(file_, *mfro));
      if (size && *size != box.size) {
        diag("CNT-4011", "mfro.size " + std::to_string(*size) + " ≠ mfra size " + std::to_string(box.size),
             mfro->offset);
      }
    }
    for (const Hdr& k : kids) {
      if (k.type != "tfra") {
        continue;
      }
      const auto [track, entries] = parse_tfra(payload(file_, k));
      if (track != in_.track_id) {
        continue;
      }
      uint64_t bad = 0;
      std::string first;
      for (size_t e = 0; e < entries.size(); ++e) {
        tick();
        const std::string why = check_tfra(entries[e]);
        if (!why.empty() && bad++ == 0) {
          first = "entry " + std::to_string(e) + ": " + why;
        }
      }
      if (bad != 0) {
        diag("CNT-4010", std::to_string(bad) + " tfra entries do not match the fragments (" + first + ")", k.offset);
      }
    }
  }

  [[nodiscard]] std::string check_tfra(const TfraEntry& e) const {
    // locs_ is in file order: sorted by (moof_offset, traf_number).
    using Key = std::pair<uint64_t, uint64_t>;
    const auto loc = std::ranges::lower_bound(locs_, Key{e.moof_offset, e.traf}, {},
                                              [](const TrafLoc& l) { return Key{l.moof_offset, l.traf_number}; });
    if (loc == locs_.end() || loc->moof_offset != e.moof_offset || loc->traf_number != e.traf) {
      return "moof_offset " + std::to_string(e.moof_offset) + " traf " + std::to_string(e.traf) +
             " is no traf of this track";
    }
    if (e.trun == 0 || e.trun > loc->truns.size() || e.sample == 0 || e.sample > loc->truns[e.trun - 1].second) {
      return "trun " + std::to_string(e.trun) + " sample " + std::to_string(e.sample) + " does not exist";
    }
    const size_t i = loc->truns[e.trun - 1].first + (e.sample - 1);
    const int64_t dts = out_.samples.dts[i];
    const auto time = static_cast<int64_t>(e.time);
    if (time != dts && time != dts + out_.samples.cts_offset[i]) {
      return "time " + std::to_string(e.time) + " ≠ DTS " + std::to_string(dts) + " / CTS";
    }
    return {};
  }

  const io::RandomAccessFile& file_;
  const FragmentTrackInput& in_;
  jobs::Cancel& cancel_;
  std::vector<Hdr> top_;
  FragmentBuild out_;
  fb::FragmentInfoT* current_ = nullptr;
  std::vector<size_t> frag_first_sample_;  // first sample index of each fragment
  std::vector<TrafLoc> locs_;
  std::vector<uint64_t> dur_prefix_;                             // trun duration prefix sums by fragment
  std::vector<std::optional<int64_t>> min_cts_;                  // min_cts() cache by fragment
  std::vector<std::tuple<size_t, size_t, int32_t>> ref_ranges_;  // media reference fragment ranges [lo, hi)
  uint64_t work_ = 0;                                            // tick() counter
  int32_t ref_count_ = 0;
  std::optional<int64_t> expected_dts_;
  int64_t dts_ = 0;
  std::optional<uint32_t> last_seq_;
  std::optional<std::vector<uint8_t>> first_styp_;
  bool styp_reported_ = false;
  bool cap_reported_ = false;
  uint64_t file_bound_;  // one byte per sample (#788)
  uint64_t limit_;       // samples expanded at most
};

const fb::BoxNodeT* child(const fb::BoxNodeT& n, std::string_view type) {
  const auto it = std::ranges::find_if(n.children, [&](const auto& c) { return c && c->fourcc == type; });
  return it == n.children.end() ? nullptr : it->get();
}

const fb::BoxNodeT* path(const fb::BoxNodeT* n, std::initializer_list<std::string_view> types) {
  for (const std::string_view t : types) {
    n = n != nullptr ? child(*n, t) : nullptr;
  }
  return n;
}

std::vector<uint8_t> node_payload(const io::RandomAccessFile& file, const fb::BoxNodeT* n) {
  return n != nullptr ? payload(file, hdr_of(*n)) : std::vector<uint8_t>{};
}

// tkhd track_ID, mdhd timescale (after version, flags and the two times).
std::optional<uint32_t> u32_after_times(std::span<const uint8_t> p) {
  try {
    BitReader br(p);
    const bool wide = br.u(8) == 1;
    br.skip_bits(24 + (wide ? 128U : 64U));
    return u32(br);
  } catch (const ParseError&) {
    return std::nullopt;
  }
}

void read_stbl(const io::RandomAccessFile& file, const fb::BoxNodeT* stbl, FragmentTrackInput& in) {
  try {
    const std::vector<uint8_t> stsz = node_payload(file, path(stbl, {"stsz"}));
    if (stsz.size() >= 12) {
      BitReader br(stsz);
      br.skip_bits(64);
      in.has_moov_samples = br.u(32) != 0;
    }
    const std::vector<uint8_t> stts = node_payload(file, path(stbl, {"stts"}));
    if (stts.size() >= 8) {
      BitReader br(stts);
      br.skip_bits(32);
      const uint32_t n = u32(br);
      for (uint32_t i = 0; i < n && br.bits_left() >= 64; ++i) {
        const uint64_t count = br.u(32);
        in.moov_duration += count * br.u(32);
      }
    }
  } catch (const ParseError&) {
  }
}

// Every `moov/mvex/trex`, by track_ID.
std::map<uint32_t, TrexDefaults> read_trex(const io::RandomAccessFile& file, const fb::BoxNodeT& moov) {
  std::map<uint32_t, TrexDefaults> out;
  const fb::BoxNodeT* mvex = child(moov, "mvex");
  if (mvex == nullptr) {
    return out;
  }
  for (const auto& c : mvex->children) {
    const std::vector<uint8_t> p = c && c->fourcc == "trex" ? node_payload(file, c.get()) : std::vector<uint8_t>{};
    if (p.size() < 24) {
      continue;
    }
    BitReader br(p);
    br.skip_bits(32);
    const uint32_t id = u32(br);
    TrexDefaults& t = out[id];
    t.sample_description_index = u32(br);
    t.duration = u32(br);
    t.size = u32(br);
    t.flags = u32(br);
  }
  return out;
}

const fb::BoxNodeT* find_moov(const BoxNodes& top_level) {
  const auto moov = std::ranges::find_if(top_level, [](const auto& n) { return n && n->fourcc == "moov"; });
  return moov == top_level.end() ? nullptr : moov->get();
}

// Gives a column that is shorter than the `have` moov samples (empty: the moov part has no such box) `fill` there.
template <typename T>
void pad_column(std::vector<T>& col, size_t have, T fill) {
  if (col.size() < have) {
    col.resize(have, fill);
  }
}

}  // namespace

FragmentTrackInput fragment_track_input(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                        uint32_t track_id) {
  FragmentTrackInput in;
  in.track_id = track_id;
  const fb::BoxNodeT* moov = find_moov(top_level);
  if (moov == nullptr) {
    return in;
  }
  in.trex = read_trex(file, *moov);
  for (const auto& trak : moov->children) {
    if (!trak || trak->fourcc != "trak" ||
        u32_after_times(node_payload(file, child(*trak, "tkhd"))) != std::optional(track_id)) {
      continue;
    }
    in.timescale = u32_after_times(node_payload(file, path(trak.get(), {"mdia", "mdhd"}))).value_or(0);
    read_stbl(file, path(trak.get(), {"mdia", "minf", "stbl"}), in);
    break;
  }
  return in;
}

FragmentTrackInput fragment_track_input(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                        const fb::TrackInfoT& track, const fb::SampleTableT& moov_table) {
  FragmentTrackInput in;
  in.track_id = track.track_id;
  in.timescale = track.timescale;
  if (const fb::BoxNodeT* moov = find_moov(top_level); moov != nullptr) {
    in.trex = read_trex(file, *moov);
  }
  const auto& fi = moov_table.fragment_index;
  const auto n = static_cast<size_t>(std::ranges::partition_point(fi, [](int32_t f) { return f < 0; }) - fi.begin());
  const size_t moov_count = fi.empty() ? std::min<size_t>(moov_table.dts.size(), moov_table.duration.size()) : n;
  in.has_moov_samples = moov_count > 0;
  if (in.has_moov_samples) {
    const int64_t end = moov_table.dts[moov_count - 1] + int64_t{moov_table.duration[moov_count - 1]};
    in.moov_duration = static_cast<uint64_t>(std::max<int64_t>(0, end));
  }
  in.max_samples = kMaxFragmentSamples - std::min<uint64_t>(moov_count, kMaxFragmentSamples);
  return in;
}

void append_fragment_samples(fb::SampleTableT& table, const fb::FragmentTimelineT& timeline,
                             FragmentSamples&& samples) {
  const size_t have = table.dts.size();
  const size_t n = samples.count();
  if (n == 0) {
    return;
  }
  const auto add = [](auto& dst, const auto& src) { dst.insert(dst.end(), src.begin(), src.end()); };
  add(table.dts, samples.dts);
  add(table.cts_offset, samples.cts_offset);
  add(table.duration, samples.duration);
  add(table.offset, samples.offset);
  add(table.size, samples.size);
  pad_column(table.fragment_index, have, int32_t{-1});
  add(table.fragment_index, samples.fragment_index);
  pad_column(table.chunk_index, have, uint32_t{0});
  table.chunk_index.resize(have + n, 0);
  if (!table.is_partial_sync.empty()) {
    table.is_partial_sync.resize(have + n, false);  // stps is a moov-only box
  }
  // sdtp packs is_leading..sample_has_redundancy (sample_flags bits 27..20) like the sdtp byte (14496-12 §8.8.3.1);
  // a moov part without sdtp keeps 0 ("unknown" in every field).
  pad_column(table.sdtp, have, uint8_t{0});
  table.sdtp.reserve(have + n);
  int32_t min_cts = 0;
  bool any_priority = !table.degradation_priority.empty();
  table.is_sync.reserve(have + n);
  table.sample_description_index.reserve(have + n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t f = samples.flags[i];
    table.is_sync.push_back(samples.is_sync(i));
    table.sample_description_index.push_back(
        static_cast<uint16_t>(std::min<uint32_t>(samples.sample_description_index[i], 0xFFFFU)));
    table.sdtp.push_back(static_cast<uint8_t>((f >> 20U) & 0xFFU));
    any_priority = any_priority || (f & 0xFFFFU) != 0;
    min_cts = std::min(min_cts, samples.cts_offset[i]);
  }
  if (any_priority) {
    pad_column(table.degradation_priority, have, uint16_t{0});
    for (size_t i = 0; i < n; ++i) {
      table.degradation_priority.push_back(static_cast<uint16_t>(samples.flags[i] & 0xFFFFU));
    }
  }
  for (const FragmentGroup& fg : samples.groups) {
    auto it = std::ranges::find_if(table.groups, [&](const auto& g) {
      return g->grouping_type == fg.grouping_type && g->grouping_type_parameter == fg.grouping_type_parameter;
    });
    if (it == table.groups.end()) {
      auto g = std::make_unique<fb::GroupMembershipT>();
      g->grouping_type = fg.grouping_type;
      g->grouping_type_parameter = fg.grouping_type_parameter;
      g->sgpd_offset = fg.sgpd_offset;
      table.groups.push_back(std::move(g));
      it = std::prev(table.groups.end());
    }
    (*it)->description_index.resize(have + n, 0);
    for (const auto& [i, index] : fg.members) {
      (*it)->description_index[have + i] = index;
    }
  }
  for (const auto& g : table.groups) {
    g->description_index.resize(have + n, 0);
  }
  table.composition_to_dts_shift = std::max(table.composition_to_dts_shift, -int64_t{min_cts});
  table.count = have + n;
  for (const auto& d : timeline.diagnostics) {
    if (d && d->code == "CNT-2031") {
      table.diagnostics.push_back(std::make_unique<fb::DiagnosticT>(*d));
    }
  }
  samples = FragmentSamples{};
}

uint64_t trun_sample_count(const fb::FragmentTimelineT& timeline) noexcept {
  uint64_t n = 0;
  for (const auto& f : timeline.fragments) {
    for (const auto& t : f->trafs) {
      for (const auto& tr : t->truns) {
        n += tr->sample_count;
      }
    }
  }
  return n;
}

FragmentBuild build_fragments(const io::RandomAccessFile& file, const BoxNodes& top_level,
                              const FragmentTrackInput& input, jobs::Cancel& cancel) {
  return Builder(file, top_level, input, cancel).run();
}

flatbuffers::DetachedBuffer serialise(const fb::FragmentTimelineT& timeline) {
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(fb::FragmentTimeline::Pack(fbb, &timeline));
  return fbb.Release();
}

}  // namespace ovt::container::isobmff
