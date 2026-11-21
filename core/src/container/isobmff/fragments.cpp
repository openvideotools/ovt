// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// FragmentTimeline builder (chapter 08 §8.4.3, WI-1.219a). See fragments.h.

#include "container/isobmff/fragments.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

class Builder {
 public:
  Builder(const io::RandomAccessFile& file, const BoxNodes& top, const FragmentTrackInput& in, jobs::Cancel& cancel)
      : file_(file), in_(in), cancel_(cancel) {
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
    TrunState st{base, std::nullopt, false, 0, 0};
    for (const Hdr& k : kids) {
      if (k.type == "trun") {
        trun(k, t, d, mine, st, *ti, fr);
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
    current_->trafs.push_back(std::move(ti));
  }

  // §8.4.3 2.4 and 2.5. SPEC-ASSUMPTION(#774): "fragment k" in CNT-4020 is the 0-based FragmentInfo.index.
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
  void trun(const Hdr& box, const Tfhd& t, const Defaults& d, bool mine, TrunState& st, fb::TrafInfoT& ti,
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
    ti.truns.push_back(std::move(info));
  }

  // The end of another track's trun data, without expanding its samples: arithmetic when the trun has no
  // per-sample sizes (a 20-byte trun may claim 2^32 default-sized samples), else the sum of the written sizes,
  // which the payload bounds. Saturates at UINT64_MAX.
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
    if (o.count() >= std::min(in_.max_samples, kMaxFragmentSamples)) {
      if (!cap_reported_) {
        cap_reported_ = true;
        diag("CNT-2031", "more than " + std::to_string(o.count()) + " fragment samples; table truncated",
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
    // SPEC-ASSUMPTION(#774): the aux data of a senc starts at its first IV, after the FullBox header and the u32
    // sample_count.
    if (senc != nullptr && !ti.saio_resolved.empty() && ti.saio_resolved[0] != senc->payload() + 8) {
      diag("CNT-6005",
           "saio resolves to offset " + std::to_string(ti.saio_resolved[0]) +
               ", the senc sample data "
               "starts at " +
               std::to_string(senc->payload() + 8),
           saio_box->offset);
    }
  }

  const io::RandomAccessFile& file_;
  const FragmentTrackInput& in_;
  jobs::Cancel& cancel_;
  std::vector<Hdr> top_;
  FragmentBuild out_;
  fb::FragmentInfoT* current_ = nullptr;
  std::optional<int64_t> expected_dts_;
  int64_t dts_ = 0;
  std::optional<uint32_t> last_seq_;
  std::optional<std::vector<uint8_t>> first_styp_;
  bool styp_reported_ = false;
  bool cap_reported_ = false;
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

}  // namespace

FragmentTrackInput fragment_track_input(const io::RandomAccessFile& file, const BoxNodes& top_level,
                                        uint32_t track_id) {
  FragmentTrackInput in;
  in.track_id = track_id;
  const auto moov = std::ranges::find_if(top_level, [](const auto& n) { return n && n->fourcc == "moov"; });
  if (moov == top_level.end()) {
    return in;
  }
  if (const fb::BoxNodeT* mvex = child(**moov, "mvex"); mvex != nullptr) {
    for (const auto& c : mvex->children) {
      const std::vector<uint8_t> p = c && c->fourcc == "trex" ? node_payload(file, c.get()) : std::vector<uint8_t>{};
      if (p.size() < 24) {
        continue;
      }
      BitReader br(p);
      br.skip_bits(32);
      const uint32_t id = u32(br);
      TrexDefaults& t = in.trex[id];
      t.sample_description_index = u32(br);
      t.duration = u32(br);
      t.size = u32(br);
      t.flags = u32(br);
    }
  }
  for (const auto& trak : (*moov)->children) {
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
