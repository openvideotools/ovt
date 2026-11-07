// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/stream_index_adapter.h"

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <utility>

namespace ovt::app {

namespace {

std::uint64_t unit_begin(const fb::Unit& unit) {
  const fb::ByteSpan* prefix = unit.prefix_span();
  const fb::ByteSpan* payload = unit.file_span();
  const std::uint64_t payload_offset = payload != nullptr ? payload->offset() : 0;
  if (prefix != nullptr && prefix->length() != 0 && prefix->offset() <= payload_offset) {
    return prefix->offset();
  }
  return payload_offset;
}

std::uint64_t unit_end(const fb::Unit& unit) {
  const fb::ByteSpan* payload = unit.file_span();
  return payload != nullptr ? payload->offset() + payload->length() : 0;
}

}  // namespace

StreamIndexHandle::StreamIndexHandle(Private /*tag*/, std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {
  root_ = flatbuffers::GetRoot<fb::StreamIndex>(bytes_.data());
}

std::shared_ptr<const StreamIndexHandle> StreamIndexHandle::adopt(std::vector<std::uint8_t> bytes) {
  flatbuffers::Verifier verifier(bytes.data(), bytes.size());
  if (bytes.empty() || !verifier.VerifyBuffer<fb::StreamIndex>(nullptr)) {
    return nullptr;
  }
  return std::make_shared<StreamIndexHandle>(Private{}, std::move(bytes));
}

StreamIndexAdapter::StreamIndexAdapter(std::shared_ptr<const StreamIndexHandle> handle) : handle_(std::move(handle)) {
  const fb::StreamIndex& index = handle_->index();

  if (const auto* frames = index.frames(); frames != nullptr) {
    for (flatbuffers::uoffset_t i = 0; i < frames->size(); ++i) {
      if (frames->Get(i)->id() == nullptr || frames->Get(i)->id()->decode_index() != static_cast<int>(i)) {
        frames_dense_ = false;
        break;
      }
    }
  }

  const auto* units = index.units();
  if (units == nullptr) {
    return;
  }
  by_byte_.reserve(units->size());
  // SPEC-ASSUMPTION(#219): the StreamIndex has no sample table (container samples come from the container API,
  // WI-1.217), so a sample's span is the union of its units including their prefixes, and its frame is the frame of
  // its first VCL unit. A container sample table, when the file has one, replaces these spans by the span of the
  // sample itself once the file open flow (WI-1.454) supplies it.
  for (flatbuffers::uoffset_t i = 0; i < units->size(); ++i) {
    const fb::Unit& unit = *units->Get(i);
    const std::uint64_t begin = unit_begin(unit);
    const std::uint64_t end = unit_end(unit);
    by_byte_.push_back(ByteEntry{begin, end, static_cast<int>(i)});

    const int sample = unit.sample_index();
    if (sample < 0) {
      continue;
    }
    if (static_cast<std::size_t>(sample) >= samples_.size()) {
      samples_.resize(static_cast<std::size_t>(sample) + 1);
    }
    SelectionSample& s = samples_[static_cast<std::size_t>(sample)];
    if (s.span.length == 0) {
      s.span = ByteSpan{begin, end - begin};
      s.first_unit = static_cast<int>(i);
    } else {
      const std::uint64_t union_begin = std::min(s.span.offset, begin);
      const std::uint64_t union_end = std::max(s.span.offset + s.span.length, end);
      s.span = ByteSpan{union_begin, union_end - union_begin};
    }
    if (s.frame.decode_index < 0 && unit.frame() != nullptr && unit.frame()->decode_index() >= 0) {
      s.frame = FrameId{unit.frame()->decode_index(), unit.frame()->display_index()};
    }
  }
  std::stable_sort(by_byte_.begin(), by_byte_.end(),
                   [](const ByteEntry& a, const ByteEntry& b) { return a.begin < b.begin; });
}

std::optional<SelectionUnit> StreamIndexAdapter::unit(int unit_index) const {
  const auto* units = handle_->index().units();
  if (units == nullptr || unit_index < 0 || static_cast<flatbuffers::uoffset_t>(unit_index) >= units->size()) {
    return std::nullopt;
  }
  const fb::Unit& u = *units->Get(static_cast<flatbuffers::uoffset_t>(unit_index));
  SelectionUnit out;
  if (u.file_span() != nullptr) {
    out.file_span = ByteSpan{u.file_span()->offset(), u.file_span()->length()};
  }
  if (const auto* epb = u.epb_positions(); epb != nullptr && epb->size() != 0) {
    // FlatBuffers stores scalars little-endian and this is a little-endian-only build (Qt 6 targets), so the vector
    // is a plain uint32 array in place.
    out.epb_positions = std::span<const std::uint32_t>(epb->data(), epb->size());
  }
  if (u.frame() != nullptr) {
    out.frame = FrameId{u.frame()->decode_index(), u.frame()->display_index()};
  }
  out.sample_index = u.sample_index();
  return out;
}

int StreamIndexAdapter::decode_index_of(FrameId frame) const {
  const fb::StreamIndex& index = handle_->index();
  const auto* frames = index.frames();
  if (frames == nullptr) {
    return -1;
  }
  const auto frame_count = static_cast<std::int64_t>(frames->size());
  if (frame.decode_index >= 0) {
    if (frames_dense_) {
      return frame.decode_index < frame_count ? frame.decode_index : -1;
    }
    for (flatbuffers::uoffset_t i = 0; i < frames->size(); ++i) {
      if (frames->Get(i)->id() != nullptr && frames->Get(i)->id()->decode_index() == frame.decode_index) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  if (frame.display_index >= 0) {
    const auto* order = index.display_order();
    if (order != nullptr && static_cast<flatbuffers::uoffset_t>(frame.display_index) < order->size()) {
      return decode_index_of(FrameId{order->Get(static_cast<flatbuffers::uoffset_t>(frame.display_index)), -1});
    }
  }
  return -1;
}

std::optional<FrameId> StreamIndexAdapter::resolve_frame(FrameId frame) const {
  const int decode = decode_index_of(frame);
  if (decode < 0) {
    return std::nullopt;
  }
  const auto* frames = handle_->index().frames();
  for (flatbuffers::uoffset_t i = frames_dense_ ? static_cast<flatbuffers::uoffset_t>(decode) : 0; i < frames->size();
       ++i) {
    const fb::FrameId* id = frames->Get(i)->id();
    if (id != nullptr && id->decode_index() == decode) {
      return FrameId{id->decode_index(), id->display_index()};
    }
  }
  return std::nullopt;
}

int StreamIndexAdapter::first_vcl_unit(FrameId frame) const {
  const int decode = decode_index_of(frame);
  if (decode < 0) {
    return -1;
  }
  const auto* frames = handle_->index().frames();
  const auto* units = handle_->index().units();
  const fb::FrameInfo* info = nullptr;
  for (flatbuffers::uoffset_t i = frames_dense_ ? static_cast<flatbuffers::uoffset_t>(decode) : 0; i < frames->size();
       ++i) {
    if (frames->Get(i)->id() != nullptr && frames->Get(i)->id()->decode_index() == decode) {
      info = frames->Get(i);
      break;
    }
  }
  if (info == nullptr || info->units() == nullptr || units == nullptr) {
    return -1;
  }
  // FrameInfo.units also lists the AUD / SEI / parameter-set units that precede the first VCL unit (§6.2.4);
  // a unit is VCL when its own `frame` names this frame (non-VCL units carry decode_index -1).
  for (const std::uint32_t unit_index : *info->units()) {
    if (unit_index < units->size()) {
      const fb::FrameId* owner = units->Get(unit_index)->frame();
      if (owner != nullptr && owner->decode_index() == decode) {
        return static_cast<int>(unit_index);
      }
    }
  }
  return -1;
}

std::optional<SelectionSample> StreamIndexAdapter::sample(int sample_index) const {
  if (sample_index < 0 || static_cast<std::size_t>(sample_index) >= samples_.size() ||
      samples_[static_cast<std::size_t>(sample_index)].span.length == 0) {
    return std::nullopt;
  }
  return samples_[static_cast<std::size_t>(sample_index)];
}

int StreamIndexAdapter::unit_at_byte(std::uint64_t file_offset) const {
  // The last entry that begins at or before the offset; units do not overlap, so it is the only candidate.
  auto it = std::upper_bound(by_byte_.begin(), by_byte_.end(), file_offset,
                             [](std::uint64_t value, const ByteEntry& e) { return value < e.begin; });
  if (it == by_byte_.begin()) {
    return -1;
  }
  --it;
  return file_offset < it->end ? it->unit : -1;
}

}  // namespace ovt::app
