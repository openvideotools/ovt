// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Lazy EPB stripping and the on-demand re-parse of one unit's header tree (chapter 7 §7.4.5, §7.4.6; WI-1.125).

#include "index/lazy_syntax.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

#include "ovt/cxx/index/stream_index_builder.hpp"
#include "ovt/cxx/syntax/unit_scan.hpp"

namespace ovt::index {

// ---- LazyRbsp ------------------------------------------------------------------------------------------------

void LazyRbsp::reset(std::span<const uint8_t> nal) noexcept {
  nal_ = nal;
  rbsp_.clear();
  epb_.clear();
  stripped_ = 0;
  zeros_ = 0;
}

void LazyRbsp::extend(size_t raw_limit) {
  // SPEC-ASSUMPTION(#76): epb_positions are 32-bit, so a NAL over 4 GiB is refused (as rbsp_from_nal does).
  if (nal_.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("LazyRbsp: NAL unit larger than 4 GiB");
  }
  const size_t end = std::min(raw_limit, nal_.size());
  if (end <= stripped_) {
    return;
  }
  rbsp_.reserve(end);
  // The §7.2.5 loop of rbsp_from_nal(), continued from stripped_: an EPB is a 0x03 after two zero bytes counted
  // since the last non-zero byte or EPB, whatever follows it.
  for (size_t i = stripped_; i < end; ++i) {
    const uint8_t b = nal_[i];
    if (zeros_ >= 2 && b == 0x03) {
      epb_.push_back(static_cast<uint32_t>(i));
      zeros_ = 0;
      continue;
    }
    rbsp_.push_back(b);
    zeros_ = b == 0x00 ? zeros_ + 1 : 0;
  }
  stripped_ = end;
}

void LazyRbsp::scan_rest() {
  // A 0x03 is an EPB exactly when the two raw bytes before it are 0x00 (an EPB is itself non-zero, so the loop's
  // zero count is >= 2 if and only if the two preceding raw bytes are zero): memchr for 0x03, then look back.
  const uint8_t* data = nal_.data();
  const size_t n = nal_.size();
  size_t i = stripped_;
  while (i < n) {
    const void* hit = std::memchr(data + i, 0x03, n - i);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (hit == nullptr) {
      break;
    }
    const auto p = static_cast<size_t>(static_cast<const uint8_t*>(hit) - data);
    if (p >= 2 && nal_[p - 1] == 0x00 && nal_[p - 2] == 0x00) {
      epb_.push_back(static_cast<uint32_t>(p));
    }
    i = p + 1;
  }
  stripped_ = n;  // nothing more to strip: epb_ is complete and must stay ascending
}

// ---- reparse_unit_syntax ------------------------------------------------------------------------------------
// SPEC-ASSUMPTION(#711): §7.4.5 wants activation snapshots stored in the index; the schema has no field for them,
// so the units the target depends on are replayed from the index instead (parameter sets and the earlier units
// of its frame; for AV1 everything since the nearest coded key frame).

namespace {

// Decode index of the frame to replay from for an AV1 target in frame `frame`: the nearest key frame at or before
// it that is coded (not show_existing_frame), since a shown key frame refreshes every reference slot; frame 0
// when there is none. Every unit from that frame's first unit on is replayed.
int32_t av1_replay_start(const fb::StreamIndex& index, int32_t frame) {
  const auto* frames = index.frames();
  for (int32_t f = frame; f >= 0 && frames != nullptr; --f) {
    const fb::FrameInfo* fi = frames->Get(static_cast<flatbuffers::uoffset_t>(f));
    if (fi->type() == fb::FrameType::Key && fi->is_rap() && fi->show_existing_of() < 0) {
      return f;
    }
  }
  return 0;
}

// Units of the target's frame (H.26x: the earlier units of the frame; AV1: every unit since the nearest coded key
// frame) that precede the target.
void frame_units_before(const fb::StreamIndex& index, uint32_t target, int32_t frame, std::vector<uint32_t>& out) {
  const auto* frames = index.frames();
  if (frame < 0 || frames == nullptr || static_cast<uint32_t>(frame) >= frames->size()) {
    return;
  }
  if (index.codec() != fb::Codec::AV1) {
    if (const auto* units = frames->Get(static_cast<flatbuffers::uoffset_t>(frame))->units(); units != nullptr) {
      std::copy_if(units->begin(), units->end(), std::back_inserter(out), [target](uint32_t u) { return u < target; });
    }
    return;
  }
  const fb::FrameInfo* start = frames->Get(static_cast<flatbuffers::uoffset_t>(av1_replay_start(index, frame)));
  uint32_t first = target;
  if (start->units() != nullptr && !start->units()->empty()) {
    first = std::min(first, *std::min_element(start->units()->begin(), start->units()->end()));
  }
  for (uint32_t u = first; u < target; ++u) {
    out.push_back(u);
  }
}

// The units fed before the target, ascending and unique.
std::vector<uint32_t> replay_units(const fb::StreamIndex& index, uint32_t target) {
  std::vector<uint32_t> out;
  if (const auto* ps = index.parameter_sets(); ps != nullptr) {
    for (const fb::Unit* u : *ps) {
      if (u->index() < target) {
        out.push_back(u->index());
      }
    }
  }
  const fb::Unit* unit = index.units()->Get(target);
  frame_units_before(index, target, unit->frame() != nullptr ? unit->frame()->decode_index() : -1, out);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void feed(StreamIndexBuilder& builder, const fb::StreamIndex& index, const io::RandomAccessFile& file, uint32_t u) {
  const fb::Unit* unit = index.units()->Get(u);
  const fb::ByteSpan* span = unit->file_span();
  const fb::ByteSpan* prefix = unit->prefix_span();
  const auto len = static_cast<size_t>(std::min<uint64_t>(span->length(), std::numeric_limits<size_t>::max()));
  const io::RandomAccessFile::Holder h = file.view(span->offset(), len);
  UnitInput in;
  in.file_offset = span->offset();
  in.bytes = h.bytes;
  in.prefix_span = prefix != nullptr ? syntax::ByteRange{prefix->offset(), prefix->length()} : syntax::ByteRange{};
  in.sample_index = unit->sample_index();
  builder.add_unit(in);
}

}  // namespace

std::vector<uint8_t> reparse_unit_syntax(const fb::StreamIndex& index, const io::RandomAccessFile& file,
                                         uint32_t unit_index) {
  const std::vector<uint32_t> replay = replay_units(index, unit_index);
  BuilderOptions options;
  options.codec = index.codec();
  options.record_only_unit = static_cast<int64_t>(replay.size());
  StreamIndexBuilder builder(options);
  for (const uint32_t u : replay) {
    feed(builder, index, file, u);
  }
  feed(builder, index, file, unit_index);
  return builder.unit_syntax(static_cast<uint32_t>(replay.size()));
}

}  // namespace ovt::index
