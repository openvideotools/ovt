// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Helpers shared by the fixture builders (object API construction).

#ifndef OVT_TESTS_GOLDEN_FIXTURES_COMMON_HPP
#define OVT_TESTS_GOLDEN_FIXTURES_COMMON_HPP

#include <flatbuffers/flatbuffers.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fixtures.hpp"
#include "ovt_model_generated.h"

namespace ovt::golden {

namespace fb = ovt::fb;

/// Integer rectangle in luma samples (builder-side; the schema's Rect struct is created from it).
struct Box {
  int x;
  int y;
  int w;
  int h;
};

[[nodiscard]] inline std::unique_ptr<fb::Rect> rect(Box b) {
  return std::make_unique<fb::Rect>(b.x, b.y, b.w, b.h);
}

/// Builds a nested table buffer (§6.3.3: own builder, `Finish()` without identifier, not size-prefixed).
template <class Table>
[[nodiscard]] Bytes nested(const typename Table::NativeTableType& native) {
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Table::Pack(builder, &native));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

/// Finishes @p native as an `OVTA` buffer (not size-prefixed) and returns its bytes.
[[nodiscard]] inline Bytes finish(const fb::FrameAnalysisT& native) {
  flatbuffers::FlatBufferBuilder builder;
  fb::FinishFrameAnalysisBuffer(builder, fb::FrameAnalysis::Pack(builder, &native));
  return {builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize()};
}

/// Everything about one Block node that the builders set; `make_block` turns it into a BlockT.
struct BlockSpec {
  std::uint32_t id = 0;
  std::int32_t parent = -1;
  std::vector<std::uint32_t> children{};
  fb::BlockKind kind = fb::BlockKind::Other;
  Box box{};
  std::uint8_t level = 0;
  std::uint8_t partition_type = 255;
  fb::PredKind pred = fb::PredKind::None;
  bool skip = false;
  bool merge_or_direct = false;
  std::int16_t qp = 0;
  std::uint32_t bits_x8 = 0;
  std::string ext_type{};
  Bytes ext{};
};

[[nodiscard]] inline std::unique_ptr<fb::BlockT> make_block(BlockSpec spec) {
  auto block = std::make_unique<fb::BlockT>();
  block->id = spec.id;
  block->parent = spec.parent;
  block->children = std::move(spec.children);
  block->kind = spec.kind;
  block->rect = rect(spec.box);
  block->level = spec.level;
  block->partition_type = spec.partition_type;
  block->pred = spec.pred;
  block->skip = spec.skip;
  block->merge_or_direct = spec.merge_or_direct;
  block->qp = spec.qp;
  block->bits_x8 = spec.bits_x8;
  block->ext_type = std::move(spec.ext_type);
  block->ext = std::move(spec.ext);
  return block;
}

/// The cell grid (§6.2.1 rule 4) with the painting helpers the three builders share.
struct GridPainter {
  std::uint32_t cells_w;
  std::uint32_t cells_h;

  [[nodiscard]] std::size_t count() const { return std::size_t{cells_w} * cells_h; }

  /// Sets every cell of @p box (luma samples, multiples of 4) in @p cells to @p value.
  template <class T>
  void paint(std::vector<T>& cells, Box box, T value) const {
    if (cells.empty()) {
      cells.assign(count(), T{});
    }
    for (int y = box.y / 4; y < (box.y + box.h) / 4; ++y) {
      for (int x = box.x / 4; x < (box.x + box.w) / 4; ++x) {
        cells[(static_cast<std::size_t>(y) * cells_w) + static_cast<std::size_t>(x)] = value;
      }
    }
  }

  /// §6.3.4 / V-020: spreads @p total over the cells of @p box, floor first, remainder to the first cells in
  /// raster order.
  void distribute_bits(std::vector<std::uint32_t>& cells, Box box, std::uint32_t total) const {
    if (cells.empty()) {
      cells.assign(count(), 0U);
    }
    const auto n = static_cast<std::uint32_t>((box.w / 4) * (box.h / 4));
    std::uint32_t index = 0;
    for (int y = box.y / 4; y < (box.y + box.h) / 4; ++y) {
      for (int x = box.x / 4; x < (box.x + box.w) / 4; ++x, ++index) {
        cells[(static_cast<std::size_t>(y) * cells_w) + static_cast<std::size_t>(x)] =
            (total / n) + (index < total % n ? 1U : 0U);
      }
    }
  }
};

/// Frame identity.
[[nodiscard]] inline std::unique_ptr<fb::FrameId> frame_id(std::int32_t decode, std::int32_t display) {
  return std::make_unique<fb::FrameId>(decode, display);
}

/// A SyntaxNode leaf (§0.5.2).
struct NodeSpec {
  std::string name;
  std::string descriptor;
  fb::SyntaxValueKind kind;
  std::int64_t value;
  std::string enum_name;
  std::uint64_t bit_offset;
  std::uint32_t bit_length;
  std::string spec_ref;
};

[[nodiscard]] inline std::unique_ptr<fb::SyntaxNodeT> make_node(NodeSpec spec) {
  auto node = std::make_unique<fb::SyntaxNodeT>();
  node->name = std::move(spec.name);
  node->descriptor = std::move(spec.descriptor);
  node->kind = spec.kind;
  if (spec.kind == fb::SyntaxValueKind::Int) {
    node->ivalue = spec.value;
  } else {
    node->uvalue = static_cast<std::uint64_t>(spec.value);
  }
  node->enum_name = std::move(spec.enum_name);
  node->span = std::make_unique<fb::BitSpan>(spec.bit_offset, spec.bit_length);
  node->spec_ref = std::move(spec.spec_ref);
  return node;
}

/// §6.6.2 blob id of a reconstruction plane: `<source-hash>/<decode-index>/recon/<y|u|v>`.
[[nodiscard]] inline std::string recon_blob_id(std::int32_t decode_index, char plane) {
  return "6f1d2c3b4a5968778695a4b3c2d1e0f0123456789abcdef0123456789abcdef0/" + std::to_string(decode_index) +
         "/recon/" + plane;
}

/// The three Reconstruction PlaneRefs of an 8-bit 4:2:0 frame (§6.3.6), stride rounded up to 64 bytes.
[[nodiscard]] inline std::vector<std::unique_ptr<fb::PlaneRefT>> recon_planes(std::int32_t decode_index,
                                                                              std::uint32_t width,
                                                                              std::uint32_t height) {
  std::vector<std::unique_ptr<fb::PlaneRefT>> planes;
  constexpr std::string_view kNames = "yuv";
  for (std::uint8_t plane = 0; plane < 3; ++plane) {
    auto ref = std::make_unique<fb::PlaneRefT>();
    ref->stage = fb::PlaneStage::Reconstruction;
    ref->plane = plane;
    ref->width = plane == 0 ? width : width / 2;
    ref->height = plane == 0 ? height : height / 2;
    ref->stride_bytes = ((ref->width + 63U) / 64U) * 64U;
    ref->bit_depth = 8;
    ref->sample_bytes = 1;
    ref->is_signed = false;
    ref->blob_id = recon_blob_id(decode_index, kNames[plane]);
    planes.push_back(std::move(ref));
  }
  return planes;
}

/// 256-bin QP histogram (rule V-028): bin = qp + @p bias, bias 128 for H.26x, 0 for AV1.
[[nodiscard]] inline std::vector<std::uint32_t> qp_histogram(const std::vector<std::int16_t>& qp, int bias) {
  std::vector<std::uint32_t> hist(256, 0U);
  for (const auto value : qp) {
    ++hist[static_cast<std::size_t>(value + bias)];
  }
  return hist;
}

}  // namespace ovt::golden

#endif
