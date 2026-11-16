// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Hand decoders, group E part 1: movie extends and movie fragments (chapter 08 §8.3.5, WI-1.211): `mehd`, `trex`,
// `leva`, `mfhd`, `tfhd`, `tfdt` and `trun`. `mvex`, `moof` and `traf` are containers without fields. The
// FragmentTimeline builder (§8.4.3, WI-1.219) does the cross-box checks (CNT-4001, CNT-4004..4008, CNT-4020...).
//
// Contract as in movie.cpp: the walker has read FullBox version/flags, `br` starts at the first field.

#include "container/isobmff/decoders/fragment_boxes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "container/diagnostics.h"
#include "container/isobmff/box_context.h"
#include "container/isobmff/box_registry.h"
#include "container/isobmff/field_builder.h"
#include "ovt/cxx/bitreader.hpp"

namespace ovt::container::isobmff {

namespace {

constexpr std::string_view kSpecRef = "ISO/IEC 14496-12:2022 ";
constexpr uint64_t kMaxDecodedPayload = 1U << 20;  // the walker's window (box_walk.cpp)

// Stamps `spec_ref` on every top-level field a decoder adds, also when the decoder throws (CNT-1002).
class SpecRefScope {
 public:
  SpecRefScope(FieldBuilder& fb, std::string_view section)
      : fb_(fb), first_(fb.node().fields.size()), ref_(std::string(kSpecRef) + std::string(section)) {}
  SpecRefScope(const SpecRefScope&) = delete;
  SpecRefScope& operator=(const SpecRefScope&) = delete;
  SpecRefScope(SpecRefScope&&) = delete;
  SpecRefScope& operator=(SpecRefScope&&) = delete;
  ~SpecRefScope() {
    auto& fields = fb_.node().fields;
    for (size_t i = first_; i < fields.size(); ++i) {
      if (fields[i] && fields[i]->spec_ref.empty()) {
        fields[i]->spec_ref = ref_;
      }
    }
  }

 private:
  FieldBuilder& fb_;
  size_t first_;
  std::string ref_;
};

uint8_t version_of(FieldBuilder& fb) {
  const int v = fb.node().version;
  return v < 0 ? 0 : static_cast<uint8_t>(v);
}

uint32_t flags_of(FieldBuilder& fb) {
  const int32_t f = fb.node().flags;
  return f < 0 ? 0U : static_cast<uint32_t>(f);
}

bool known_version(FieldBuilder& fb, uint8_t max_version) {
  const uint8_t v = version_of(fb);
  if (v <= max_version) {
    return true;
  }
  fb.diag(Severity::Warning, "CNT-1003",
          "unknown " + fb.node().fourcc + " version " + std::to_string(v) + "; fields not decoded");
  return false;
}

void check_fixed_size_end(FieldBuilder& fb) {
  if (const uint64_t extra = fb.reader().bits_left() / 8U; extra > 0) {
    fb.diag(Severity::Warning, "CNT-1015",
            fb.node().fourcc + " is " + std::to_string(extra) + " bytes larger than its fields");
  }
}

struct FlagName {
  uint32_t bit;
  std::string_view name;
};

template <size_t N>
void name_flags(FieldBuilder& fb, const std::array<FlagName, N>& names) {
  const uint32_t flags = flags_of(fb);
  std::string text;
  for (const FlagName& n : names) {
    if ((flags & n.bit) != 0) {
      text += (text.empty() ? "" : "|") + std::string(n.name);
    }
  }
  for (auto& f : fb.node().fields) {
    if (f && f->name == "flags") {
      f->svalue = text;
      return;
    }
  }
}

// A String sibling `<name>.derived` / free-standing summary with an empty span at the end of the last field (§8.3
// notation of #567).
void derived_sibling(FieldBuilder& fb, std::string_view name, std::string_view text) {
  auto& fields = fb.node().fields;
  uint64_t at = 0;
  if (!fields.empty() && fields.back() && fields.back()->span) {
    at = fields.back()->span->bit_offset() + fields.back()->span->bit_length();
  }
  auto f = std::make_unique<fb::FieldT>();
  f->name = std::string(name);
  f->kind = fb::SyntaxValueKind::String;
  f->svalue = std::string(text);
  f->span = std::make_unique<fb::BitSpan>(at, 0);
  fields.push_back(std::move(f));
}

std::string hex32(uint32_t v) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string s = "0x00000000";
  for (size_t i = 0; i < 8; ++i) {
    s[9 - i] = digits[(v >> (4U * i)) & 0xFU];
  }
  return s;
}

std::string seconds_text(uint64_t v, uint32_t timescale) {
  if (timescale == 0) {
    return "timescale unknown";
  }
  uint64_t whole = v / timescale;
  uint64_t ms = ((v % timescale) * 1000U + timescale / 2U) / timescale;
  if (ms == 1000) {
    ++whole;
    ms = 0;
  }
  std::string frac = std::to_string(ms);
  frac.insert(0, 3U - frac.size(), '0');
  return std::to_string(whole) + "." + frac + " s";
}

// ---- sample_flags (14496-12 §8.8.8.1; SPEC-ASSUMPTION(#672)) ----------------------------------------------------

std::string sample_flags_text(uint32_t v) {
  const bool non_sync = ((v >> 16U) & 1U) != 0;
  return std::string(non_sync ? "non-sync" : "sync") + " depends_on=" + std::to_string((v >> 24U) & 3U) +
         " is_depended_on=" + std::to_string((v >> 22U) & 3U) + " priority=" + std::to_string(v & 0xFFFFU);
}

// A UInt leaf `name` (the value) followed by the sibling group `name.bits` with the sub-fields; returns the value.
uint32_t sample_flags(FieldBuilder& fb, std::string_view name, bool with_bits = true) {
  const uint64_t pos = fb.reader().bit_pos();
  const uint32_t v = fb.u32(name);
  fb.derived(sample_flags_text(v));
  if (!with_bits) {
    return v;
  }
  fb.reader().seek_bits(pos);
  fb.group(std::string(name) + ".bits");
  fb.reserved(4);
  fb.u("is_leading", 2);
  fb.u("sample_depends_on", 2);
  fb.u("sample_is_depended_on", 2);
  fb.u("sample_has_redundancy", 2);
  fb.u("sample_padding_value", 3);
  fb.u("sample_is_non_sync_sample", 1);
  fb.u("sample_degradation_priority", 16);
  fb.end();
  return v;
}

TrackFragmentInfo* track_info(BoxContext& ctx, uint32_t track_id) {
  return ctx.trex_for_track ? &(*ctx.trex_for_track)[track_id] : nullptr;
}

// ---- mehd (§8.8.2) -------------------------------------------------------------------------------------------

uint64_t decode_mehd(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.2");
  if (!known_version(fb, 1)) {
    return 0;
  }
  fb.duration("fragment_duration", version_of(fb) == 1, ctx.movie_timescale);
  check_fixed_size_end(fb);
  return 0;
}

// ---- trex (§8.8.3) -------------------------------------------------------------------------------------------

uint64_t decode_trex(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.3");
  const uint32_t track_id = fb.u32("track_ID");
  TrackFragmentInfo t;
  t.has_trex = true;
  t.default_sample_description_index = fb.u32("default_sample_description_index");
  t.default_sample_duration = fb.u32("default_sample_duration");
  t.default_sample_size = fb.u32("default_sample_size");
  t.default_sample_flags = sample_flags(fb, "default_sample_flags");
  if (TrackFragmentInfo* slot = track_info(ctx, track_id); slot != nullptr) {
    if (slot->has_trex) {
      fb.diag(Severity::Error, "CNT-1012", "second trex for track_ID " + std::to_string(track_id));
    } else {
      t.timescale = slot->timescale;
      *slot = t;
    }
  }
  check_fixed_size_end(fb);
  return 0;
}

// ---- leva (§8.8.13) ------------------------------------------------------------------------------------------

std::string_view assignment_name(uint64_t type) {
  switch (type) {
    case 0:
      return "sample grouping";
    case 1:
      return "sample grouping with parameter";
    case 2:
      return "level assigned by track";
    case 3:
      return "level assigned by track (type 3)";
    case 4:
      return "sub-track";
    default:
      return "reserved";
  }
}

uint64_t decode_leva(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.13");
  const uint8_t count = fb.u8("level_count");
  fb.array("levels", count);
  for (uint32_t i = 0; i < count; ++i) {
    fb.array_element("level", i);
    fb.u32("track_id");
    fb.u("padding_flag", 1);
    const uint64_t type = fb.u("assignment_type", 7);
    fb.enum_name(assignment_name(type));
    if (type == 0 || type == 1) {
      fb.fourcc("grouping_type");
    }
    if (type == 1) {
      fb.u32("grouping_type_parameter");
    }
    if (type == 4) {
      fb.u32("sub_track_id");
    }
    fb.end();
    if (type > 4) {  // the layout of the rest is unknown
      fb.diag(Severity::Info, "CNT-1007", "unknown assignment_type " + std::to_string(type) + "; levels not decoded");
      break;
    }
  }
  fb.end();
  static_cast<void>(br);
  return 0;
}

// ---- mfhd (§8.8.5) -------------------------------------------------------------------------------------------

uint64_t decode_mfhd(BoxContext& /*ctx*/, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.5");
  fb.u32("sequence_number");
  check_fixed_size_end(fb);
  return 0;
}

// ---- tfhd (§8.8.7) -------------------------------------------------------------------------------------------

constexpr uint32_t kBaseDataOffset = 0x1;
constexpr uint32_t kSampleDescriptionIndex = 0x2;
constexpr uint32_t kDefaultDuration = 0x8;
constexpr uint32_t kDefaultSize = 0x10;
constexpr uint32_t kDefaultFlags = 0x20;
constexpr uint32_t kDurationIsEmpty = 0x10000;
constexpr uint32_t kDefaultBaseIsMoof = 0x20000;

uint64_t decode_tfhd(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.7");
  if (!known_version(fb, 0)) {
    return 0;
  }
  static constexpr auto kNames = std::to_array<FlagName>({{kBaseDataOffset, "base-data-offset-present"},
                                                          {kSampleDescriptionIndex, "sample-description-index-present"},
                                                          {kDefaultDuration, "default-sample-duration-present"},
                                                          {kDefaultSize, "default-sample-size-present"},
                                                          {kDefaultFlags, "default-sample-flags-present"},
                                                          {kDurationIsEmpty, "duration-is-empty"},
                                                          {kDefaultBaseIsMoof, "default-base-is-moof"}});
  name_flags(fb, kNames);
  const uint32_t flags = flags_of(fb);
  const uint32_t track_id = fb.u32("track_ID");
  ctx.track_id = track_id;  // for the tfdt and trun that follow in this traf
  if ((flags & kBaseDataOffset) != 0) {
    fb.u64("base_data_offset");
  }
  uint32_t index = 0;
  bool has_dur = false;
  bool has_size = false;
  bool has_flags = false;
  uint32_t dur = 0;
  uint32_t size = 0;
  uint32_t sflags = 0;
  if ((flags & kSampleDescriptionIndex) != 0) {
    index = fb.u32("sample_description_index");
  }
  if ((flags & kDefaultDuration) != 0) {
    dur = fb.u32("default_sample_duration");
    has_dur = true;
  }
  if ((flags & kDefaultSize) != 0) {
    size = fb.u32("default_sample_size");
    has_size = true;
  }
  if ((flags & kDefaultFlags) != 0) {
    sflags = sample_flags(fb, "default_sample_flags");
    has_flags = true;
  }
  if ((flags & kBaseDataOffset) != 0 && (flags & kDefaultBaseIsMoof) != 0) {
    fb.diag(Severity::Warning, "CNT-4002", "tfhd has both base-data-offset-present and default-base-is-moof");
  }
  check_fixed_size_end(fb);

  // Effective defaults: the tfhd value, else the trex of the track (SPEC-ASSUMPTION(#670)).
  const TrackFragmentInfo* trex = nullptr;
  if (ctx.trex_for_track) {
    if (const auto it = ctx.trex_for_track->find(track_id); it != ctx.trex_for_track->end() && it->second.has_trex) {
      trex = &it->second;
    }
  }
  const auto pick = [&](bool has, uint32_t own, uint32_t TrackFragmentInfo::* member, bool hex) {
    if (has) {
      return (hex ? hex32(own) : std::to_string(own)) + " (tfhd)";
    }
    if (trex != nullptr) {
      return (hex ? hex32(trex->*member) : std::to_string(trex->*member)) + " (trex)";
    }
    return std::string("none");
  };
  const std::string text =
      "sample_description_index=" +
      pick((flags & kSampleDescriptionIndex) != 0, index, &TrackFragmentInfo::default_sample_description_index, false) +
      " duration=" + pick(has_dur, dur, &TrackFragmentInfo::default_sample_duration, false) +
      " size=" + pick(has_size, size, &TrackFragmentInfo::default_sample_size, false) +
      " flags=" + pick(has_flags, sflags, &TrackFragmentInfo::default_sample_flags, true);
  derived_sibling(fb, "effective_defaults", text);
  return 0;
}

// ---- tfdt (§8.8.12) ------------------------------------------------------------------------------------------

uint64_t decode_tfdt(BoxContext& ctx, BitReader& /*br*/, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.12");
  if (!known_version(fb, 1)) {
    return 0;
  }
  const uint64_t t = fb.u32or64("baseMediaDecodeTime", version_of(fb) == 1);
  uint32_t timescale = ctx.track_timescale;
  if (ctx.trex_for_track && ctx.track_id != 0) {
    if (const auto it = ctx.trex_for_track->find(ctx.track_id); it != ctx.trex_for_track->end()) {
      timescale = it->second.timescale;
    }
  }
  fb.derived(seconds_text(t, timescale));
  check_fixed_size_end(fb);
  return 0;
}

// ---- trun (§8.8.8) -------------------------------------------------------------------------------------------

constexpr uint32_t kDataOffsetPresent = 0x1;
constexpr uint32_t kFirstSampleFlagsPresent = 0x4;
constexpr uint32_t kSampleDurationPresent = 0x100;
constexpr uint32_t kSampleSizePresent = 0x200;
constexpr uint32_t kSampleFlagsPresent = 0x400;
constexpr uint32_t kSampleCtsPresent = 0x800;

struct FirstFlags {
  uint32_t value = 0;
  bool present = false;
};

struct RunSums {
  uint64_t duration = 0;
  uint64_t size = 0;
  uint64_t sync = 0;
  uint64_t flagged = 0;  // samples whose flags are known
};

// One entry; the fields come in the fixed order duration, size, flags, cts whatever is set. `with_fields` false reads
// the same bytes without recording them (large arrays, §8.2.5).
void read_run_entry(FieldBuilder& fb, uint32_t flags, bool wide_cts_signed, bool with_fields, RunSums& sums,
                    const FirstFlags& first, bool is_first) {
  BitReader& br = fb.reader();
  if ((flags & kSampleDurationPresent) != 0) {
    sums.duration += with_fields ? fb.u32("sample_duration") : static_cast<uint32_t>(br.u(32));
  }
  if ((flags & kSampleSizePresent) != 0) {
    sums.size += with_fields ? fb.u32("sample_size") : static_cast<uint32_t>(br.u(32));
  }
  uint32_t sflags = 0;
  bool known = false;
  if ((flags & kSampleFlagsPresent) != 0) {
    sflags = with_fields ? sample_flags(fb, "sample_flags") : static_cast<uint32_t>(br.u(32));
    known = true;
  } else if (first.present && is_first) {
    sflags = first.value;
    known = true;
  }
  if (known) {
    ++sums.flagged;
    sums.sync += ((sflags >> 16U) & 1U) == 0 ? 1U : 0U;
  }
  if ((flags & kSampleCtsPresent) != 0) {
    if (with_fields) {
      if (wide_cts_signed) {
        fb.i32("sample_composition_time_offset");
      } else {
        fb.u32("sample_composition_time_offset");
      }
    } else {
      br.skip_bits(32);
    }
  }
}

uint64_t decode_trun(BoxContext& /*ctx*/, BitReader& br, FieldBuilder& fb) {
  const SpecRefScope scope(fb, "§8.8.8");
  if (!known_version(fb, 1)) {
    return 0;
  }
  static constexpr auto kNames =
      std::to_array<FlagName>({{kDataOffsetPresent, "data-offset-present"},
                               {kFirstSampleFlagsPresent, "first-sample-flags-present"},
                               {kSampleDurationPresent, "sample-duration-present"},
                               {kSampleSizePresent, "sample-size-present"},
                               {kSampleFlagsPresent, "sample-flags-present"},
                               {kSampleCtsPresent, "sample-composition-time-offsets-present"}});
  name_flags(fb, kNames);
  const uint32_t flags = flags_of(fb);
  const bool v1 = version_of(fb) == 1;
  const uint32_t declared = fb.u32("sample_count");
  if ((flags & kDataOffsetPresent) != 0) {
    // Relative to the base data offset; the absolute offset needs the tfhd (FragmentTimeline, §8.4.3).
    fb.i32("data_offset");
  }
  uint32_t first_flags = 0;
  const bool has_first = (flags & kFirstSampleFlagsPresent) != 0;
  if (has_first) {
    first_flags = sample_flags(fb, "first_sample_flags");
  }
  if (has_first && (flags & kSampleFlagsPresent) != 0) {
    fb.diag(Severity::Warning, "CNT-4003", "trun has both first-sample-flags-present and sample-flags-present");
  }
  const uint32_t entry_bytes = 4U * static_cast<uint32_t>(std::popcount((flags >> 8U) & 0xFU));
  if (entry_bytes == 0) {  // nothing per sample to read; never iterate sample_count (up to 2^32) times
    check_fixed_size_end(fb);
    return 0;
  }
  const uint64_t room = br.bits_left() / 8U / entry_bytes;
  const bool window_cut = fb.node().file_span && fb.node().file_span->length() > kMaxDecodedPayload;
  uint32_t count = declared;
  if (declared > room) {
    count = static_cast<uint32_t>(room);
    if (!window_cut) {
      fb.diag(Severity::Error, "CNT-1014",
              "trun declares " + std::to_string(declared) + " samples but holds " + std::to_string(room));
    }
  }
  RunSums sums;
  fb.array("entries", count);
  for (uint32_t i = 0; i < count; ++i) {
    const bool shown = FieldBuilder::materialise(i, count);
    if (shown) {
      fb.array_element("entry", i);
    }
    read_run_entry(fb, flags, v1, shown, sums, FirstFlags{first_flags, has_first}, i == 0);
    if (shown) {
      fb.end();
    }
  }
  fb.end();
  std::string text =
      "sum_sample_duration=" + std::to_string(sums.duration) + " sum_sample_size=" + std::to_string(sums.size);
  if (sums.flagged > 0) {
    text += " sync_samples=" + std::to_string(sums.sync) + " of " + std::to_string(sums.flagged) + " with known flags";
  }
  if (window_cut) {
    text += " (first 1 MiB of the box only)";
  }
  derived_sibling(fb, "summary", text);
  if (!window_cut) {
    check_fixed_size_end(fb);
  }
  return 0;
}

// ---- post-children check -------------------------------------------------------------------------------------

bool has_diag(const fb::BoxNodeT& n, std::string_view code, std::string_view message) {
  return std::ranges::any_of(n.diagnostics, [&](const auto& d) { return d->code == code && d->message == message; });
}

void require_child(fb::BoxNodeT& n, std::string_view child, bool must_be_first) {
  const bool ok = must_be_first ? (!n.children.empty() && n.children.front()->fourcc == child)
                                : std::ranges::any_of(n.children, [&](const auto& c) { return c->fourcc == child; });
  const std::string msg = "required box " + std::string(child) + " missing in " + n.fourcc;
  if (ok || has_diag(n, "CNT-1011", msg)) {
    return;
  }
  auto d = std::make_unique<fb::DiagnosticT>();
  d->severity = fb::Severity::Error;
  d->code = "CNT-1011";
  d->message = msg;
  d->span = std::make_unique<fb::BitSpan>(0, static_cast<uint32_t>(n.header_length) * 8U);
  d->spec_ref = "ISO/IEC 14496-12:2022 §8.8";
  n.diagnostics.push_back(std::move(d));
}

}  // namespace

void check_fragment_children(fb::BoxNodeT& n) {
  if (n.fourcc == "moof") {
    require_child(n, "mfhd", true);
    for (const auto& c : n.children) {
      if (c->fourcc != "mfhd") {
        continue;
      }
      for (const auto& f : c->fields) {
        if (f->name == "sequence_number") {
          n.description = "movie fragment, sequence_number " + std::to_string(f->uvalue);
        }
      }
      break;
    }
  } else if (n.fourcc == "traf") {
    require_child(n, "tfhd", false);
  }
}

// One registration per §8.3.5 row with fields.
OVT_BOX_DECODER("mehd", "*", &decode_mehd)
OVT_BOX_DECODER("trex", "*", &decode_trex)
OVT_BOX_DECODER("leva", "*", &decode_leva)
OVT_BOX_DECODER("mfhd", "*", &decode_mfhd)
OVT_BOX_DECODER("tfhd", "*", &decode_tfhd)
OVT_BOX_DECODER("tfdt", "*", &decode_tfdt)
OVT_BOX_DECODER("trun", "*", &decode_trun)

}  // namespace ovt::container::isobmff
