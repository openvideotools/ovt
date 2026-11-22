// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The sample-byte rows of the brand checks (chapter 08 §8.4.5, WI-1.222b): CNT-7004, CNT-7008 and CNT-7009 read the
// first sync samples of an `avc1`/`avc2` or `av01` track through the unit splitter. Deep level only.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/diagnostics.h"
#include "container/isobmff/brands.h"
#include "container/isobmff/sample_table.h"
#include "demux/unit_split.h"

namespace ovt::container::isobmff {
namespace {

using fb::BoxNodeT;

constexpr unsigned kProbedSyncSamples = 8;            // §8.4.5: "the first 8 sync samples"
constexpr uint64_t kMaxProbedSampleBytes = 1U << 22;  // a larger sample is cut here (the splitter reports it)

constexpr unsigned kObuSequenceHeader = 1;
constexpr unsigned kObuTemporalDelimiter = 2;
constexpr unsigned kObuMetadata = 5;
constexpr unsigned kObuPadding = 15;
constexpr unsigned kNalSps = 7;
constexpr unsigned kNalPps = 8;

const BoxNodeT* child(const BoxNodeT& n, std::string_view fourcc) {
  const auto it = std::ranges::find_if(n.children, [&](const auto& c) { return c && c->fourcc == fourcc; });
  return it == n.children.end() ? nullptr : it->get();
}

// One sample entry of a track: its box (findings go there) and, for `avc1`/`avc2`, the NAL length size of its avcC.
struct Entry {
  BoxNodeT* node = nullptr;
  uint8_t nal_length_size = 4;
};

// A byte of `avcC` (offset 4: reserved(6) lengthSizeMinusOne(2)); 4 when the box is missing or too short.
uint8_t nal_length_size_of(const io::RandomAccessFile& file, const BoxNodeT& entry) {
  const BoxNodeT* c = child(entry, "avcC");
  if (c == nullptr || !c->file_span) {
    return 4;
  }
  uint8_t b = 0;
  if (file.read(c->file_span->offset() + c->header_length + 4, 1, &b) != 1) {
    return 4;
  }
  const auto size = static_cast<uint8_t>((b & 0x3U) + 1U);
  return size == 3 ? uint8_t{4} : size;  // 3 is not a legal NAL length size (the avcC decoder reports it)
}

// CNT-7004: SPS or PPS NAL units inside an `avc1`/`avc2` sample.
void check_avc_sample(BoxNodeT& entry, uint64_t index, std::span<const uint8_t> sample, uint8_t length_size,
                      size_t& added) {
  std::vector<uint8_t> scratch;
  const demux::SplitResult split = demux::split_units(sample, demux::Framing::LengthPrefixed, length_size, scratch);
  for (const demux::UnitSpan& u : split.units) {
    if (u.length == 0 || u.offset_in_packet >= sample.size()) {
      continue;
    }
    const unsigned type = sample[u.offset_in_packet] & 0x1FU;
    if (type == kNalSps || type == kNalPps) {
      const std::string what = type == kNalSps ? "SPS" : "PPS";
      added += add_finding(entry, DiagCode::CNT_7004,
                           "sample " + std::to_string(index) + " of " + entry.fourcc + " holds a " + what +
                               " NAL unit (parameter sets belong in avcC)")
                   ? 1U
                   : 0U;
    }
  }
}

// SPEC-ASSUMPTION(#804): an OBU without obu_size runs to the end of the sample, so "not the last OBU" cannot be
// observed directly. The reading: an unsized OBU of a type that never ends a sample (sequence header, temporal
// delimiter, metadata, padding) swallowed the OBUs behind it.
// A sized OBU is the only kind that can be followed by another; an unsized one runs to the end of the sample. An
// unsized OBU of a type that never ends a sample therefore swallowed the OBUs behind it.
bool cannot_end_a_sample(unsigned obu_type) {
  return obu_type == kObuSequenceHeader || obu_type == kObuTemporalDelimiter || obu_type == kObuMetadata ||
         obu_type == kObuPadding;
}

// CNT-7008 and CNT-7009 for one `av01` sample.
void check_av1_sample(BoxNodeT& entry, uint64_t index, std::span<const uint8_t> sample, size_t& added) {
  std::vector<uint8_t> scratch;
  const demux::SplitResult split = demux::split_units(sample, demux::Framing::Av1Obu, 0, scratch);
  uint64_t k = 0;
  for (const demux::UnitSpan& u : split.units) {
    if (u.offset_in_packet >= sample.size()) {
      continue;
    }
    const uint8_t header = sample[u.offset_in_packet];
    const unsigned type = (header >> 3U) & 0xFU;
    const bool sized = (header & 0x2U) != 0;
    if (type == kObuTemporalDelimiter) {
      added += add_finding(entry, DiagCode::CNT_7008,
                           "sample " + std::to_string(index) + " of av01 contains an OBU_TEMPORAL_DELIMITER")
                   ? 1U
                   : 0U;
    }
    if (!sized && cannot_end_a_sample(type)) {
      added += add_finding(entry, DiagCode::CNT_7009,
                           "OBU " + std::to_string(k) + " (type " + std::to_string(type) + ") of sample " +
                               std::to_string(index) + " has obu_has_size_field == 0 and is not the last OBU")
                   ? 1U
                   : 0U;
    }
    ++k;
  }
}

enum class Kind : uint8_t { None, Avc, Av1 };

Kind kind_of(std::string_view fourcc) {
  if (fourcc == "avc1" || fourcc == "avc2") {
    return Kind::Avc;
  }
  return fourcc == "av01" ? Kind::Av1 : Kind::None;
}

size_t check_track(const io::RandomAccessFile& file, const BoxNodes& top_level, size_t ordinal, BoxNodeT& stsd,
                   jobs::Cancel& cancel) {
  std::vector<Entry> entries;
  bool any = false;
  for (auto& e : stsd.children) {
    entries.push_back({e.get(), e ? nal_length_size_of(file, *e) : uint8_t{4}});
    any = any || (e && kind_of(e->fourcc) != Kind::None);
  }
  const BoxNodeT* trak = find_trak(top_level, ordinal);
  if (!any || trak == nullptr) {
    return 0;
  }
  const std::unique_ptr<fb::SampleTableT> table = expand_sample_table(file, *trak, cancel);
  size_t added = 0;
  unsigned probed = 0;
  for (uint64_t i = 0; i < table->count && probed < kProbedSyncSamples; ++i) {
    if (i >= table->is_sync.size() || !table->is_sync[i] || i >= table->offset.size() || i >= table->size.size()) {
      continue;
    }
    ++probed;
    const uint64_t description = i < table->sample_description_index.size() ? table->sample_description_index[i] : 1;
    if (description == 0 || description > entries.size() || entries[description - 1].node == nullptr) {
      continue;
    }
    const Entry& entry = entries[description - 1];
    const Kind kind = kind_of(entry.node->fourcc);
    if (kind == Kind::None) {
      continue;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(std::min<uint64_t>(table->size[i], kMaxProbedSampleBytes)));
    bytes.resize(file.read(table->offset[i], bytes.size(), bytes.data()));
    if (bytes.empty()) {
      continue;
    }
    if (kind == Kind::Avc) {
      check_avc_sample(*entry.node, i, bytes, entry.nal_length_size, added);
    } else {
      check_av1_sample(*entry.node, i, bytes, added);
    }
  }
  return added;
}

}  // namespace

size_t check_brand_samples(const io::RandomAccessFile& file, BoxNodes& top_level, jobs::Cancel& cancel) {
  const auto moov = std::ranges::find_if(top_level, [](const auto& n) { return n && n->fourcc == "moov"; });
  if (moov == top_level.end()) {
    return 0;
  }
  size_t added = 0;
  size_t ordinal = 0;
  for (auto& trak : (*moov)->children) {
    if (!trak || trak->fourcc != "trak") {
      continue;
    }
    const BoxNodeT* mdia = child(*trak, "mdia");
    const BoxNodeT* minf = mdia == nullptr ? nullptr : child(*mdia, "minf");
    const BoxNodeT* stbl = minf == nullptr ? nullptr : child(*minf, "stbl");
    BoxNodeT* stsd = stbl == nullptr ? nullptr : const_cast<BoxNodeT*>(child(*stbl, "stsd"));  // NOLINT
    if (stsd != nullptr) {
      added += check_track(file, top_level, ordinal, *stsd, cancel);
    }
    ++ordinal;
  }
  return added;
}

}  // namespace ovt::container::isobmff
