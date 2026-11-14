// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Synthetic StreamIndex FlatBuffers for the GUI tests (WI-1.455): a frame list in, serialized bytes out.

#ifndef OVT_APP_TESTS_SYNTHETIC_INDEX_H
#define OVT_APP_TESTS_SYNTHETIC_INDEX_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "ovt_model_generated.h"

namespace ovt::app::testing {

// The value of an optional the test expects to hold one; throws (failing the test case) when it is empty.
template <typename T>
T must(std::optional<T> value) {
  if (!value.has_value()) {
    throw std::runtime_error("expected a value");
  }
  return *std::move(value);
}

struct SyntheticFrame {
  std::int64_t pts;
  std::uint64_t bytes;
  int qp;
  bool rap;
  int tid;
};

// Appends the two units of frame `i` (an AUD, non-VCL, and a slice) laid out as Annex B with 4-byte start codes.
inline void add_units(ovt::fb::StreamIndexT& index, ovt::fb::FrameInfoT& fi, std::size_t i, std::uint64_t& offset,
                      int display_index, bool with_samples) {
  for (int k = 0; k < 2; ++k) {
    auto u = std::make_unique<ovt::fb::UnitT>();
    u->index = static_cast<std::uint32_t>(2 * i + static_cast<std::size_t>(k));
    const std::uint64_t payload = k == 0 ? 2 : (fi.size_bytes > 8 ? fi.size_bytes - 8 : 1);
    u->prefix_span = std::make_unique<ovt::fb::ByteSpan>(offset, 4);
    u->file_span = std::make_unique<ovt::fb::ByteSpan>(offset + 4, payload);
    offset += 4 + payload;
    u->type_name = k == 0 ? "AUD" : "SLICE";
    u->frame = std::make_unique<ovt::fb::FrameId>(k == 0 ? -1 : static_cast<int>(i), k == 0 ? -1 : display_index);
    if (k == 1) {
      u->epb_positions = {3, 9};
    }
    u->sample_index = with_samples ? static_cast<std::int32_t>(i) : -1;
    fi.units.push_back(u->index);
    index.units.push_back(std::move(u));
  }
}

// Serializes a StreamIndex whose frame i (decode order) has one AUD unit and one slice unit; the display order is the
// pts order.
inline std::vector<std::uint8_t> build_index(const std::vector<SyntheticFrame>& frames, std::int32_t tb_num,
                                             std::int32_t tb_den, bool with_samples = false) {
  ovt::fb::StreamIndexT index;
  index.codec = ovt::fb::Codec::H264;
  index.schema_version = 1;
  std::vector<int> by_pts(frames.size());
  std::iota(by_pts.begin(), by_pts.end(), 0);
  std::stable_sort(by_pts.begin(), by_pts.end(), [&](int a, int b) {
    return frames[static_cast<std::size_t>(a)].pts < frames[static_cast<std::size_t>(b)].pts;
  });
  std::vector<int> display_of(frames.size());
  for (std::size_t slot = 0; slot < by_pts.size(); ++slot) {
    display_of[static_cast<std::size_t>(by_pts[slot])] = static_cast<int>(slot);
  }
  index.display_order = by_pts;

  std::uint64_t offset = 100;  // some bytes before the first start code
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto fi = std::make_unique<ovt::fb::FrameInfoT>();
    fi->id = std::make_unique<ovt::fb::FrameId>(static_cast<int>(i), display_of[i]);
    fi->pts = frames[i].pts;
    fi->dts = static_cast<std::int64_t>(i);
    fi->timebase_num = tb_num;
    fi->timebase_den = tb_den;
    fi->size_bytes = frames[i].bytes;
    fi->qp_base = static_cast<std::int16_t>(frames[i].qp);
    fi->is_rap = frames[i].rap;
    fi->shown = true;
    fi->temporal_id = static_cast<std::uint8_t>(frames[i].tid);
    fi->type = frames[i].rap ? ovt::fb::FrameType::I : ovt::fb::FrameType::P;
    if (!frames[i].rap && i > 0) {
      fi->declared_refs = {static_cast<int>(i) - 1};  // every inter frame refers to the previous frame
    }
    add_units(index, *fi, i, offset, display_of[i], with_samples);
    index.frames.push_back(std::move(fi));
  }
  flatbuffers::FlatBufferBuilder fbb;
  fbb.Finish(ovt::fb::StreamIndex::Pack(fbb, &index));
  const auto bytes = fbb.GetBufferSpan();
  return {bytes.begin(), bytes.end()};
}

// Frames of an IPPP stream with a GOP of 60: an I frame every 60, sizes depend on the position.
inline std::vector<SyntheticFrame> gop60(std::size_t n, std::int64_t pts_step) {
  std::vector<SyntheticFrame> frames;
  for (std::size_t i = 0; i < n; ++i) {
    const bool rap = i % 60 == 0;
    frames.push_back({static_cast<std::int64_t>(i) * pts_step, rap ? 60000U + i : 900U + (i * 37U) % 400U,
                      rap ? 22 : 24 + static_cast<int>(i % 7), rap, 0});
  }
  return frames;
}

}  // namespace ovt::app::testing

#endif  // OVT_APP_TESTS_SYNTHETIC_INDEX_H
