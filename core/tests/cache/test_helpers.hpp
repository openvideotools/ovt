// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared fixtures of the cache tests (WI-0.091): temporary cache roots and FlatBuffer builders for a
// StreamIndex and a FrameAnalysis with a dense CellGrid.

#ifndef OVT_TESTS_CACHE_TEST_HELPERS_HPP
#define OVT_TESTS_CACHE_TEST_HELPERS_HPP

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/cxx/cache/cache.hpp"
#include "ovt_model_generated.h"

namespace ovt::cache::test {

namespace fs = std::filesystem;

/// A fresh directory below the build tree, removed on destruction.
class TempRoot {
 public:
  explicit TempRoot(std::string_view name) : path_(fs::path(OVT_TEST_BINARY_DIR) / "tmp" / std::string(name)) {
    fs::remove_all(path_);
    fs::create_directories(path_.parent_path());
  }
  ~TempRoot() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempRoot(const TempRoot&) = delete;
  TempRoot& operator=(const TempRoot&) = delete;
  TempRoot(TempRoot&&) = delete;
  TempRoot& operator=(TempRoot&&) = delete;
  [[nodiscard]] const fs::path& path() const noexcept { return path_; }

 private:
  fs::path path_;
};

inline SourceHash hash_of(std::uint8_t seed) {
  SourceHash h{};
  for (std::size_t i = 0; i < h.size(); ++i) {
    h.at(i) = static_cast<std::uint8_t>(seed + i);
  }
  return h;
}

inline std::vector<std::uint8_t> bytes_of(const flatbuffers::FlatBufferBuilder& b) {
  return {b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize()};
}

/// A StreamIndex ("OVTI", not size-prefixed) with @p frame_count frames whose decode_index is their position.
/// With @p with_pts every FrameInfo carries pts and poc (int64), so the buffer's minalign is 8 as for every
/// real record; without, it is 4.
inline std::vector<std::uint8_t> make_index(const SourceHash& hash, std::uint32_t frame_count,
                                            fb::Codec codec = fb::Codec::H265, bool with_pts = true) {
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<fb::FrameInfo>> frames;
  for (std::uint32_t i = 0; i < frame_count; ++i) {
    const fb::FrameId id(static_cast<std::int32_t>(i), static_cast<std::int32_t>(i));
    fb::FrameInfoBuilder fib(b);
    fib.add_id(&id);
    if (with_pts) {
      fib.add_pts(static_cast<std::int64_t>(i) * 3000);
      fib.add_poc(static_cast<std::int64_t>(i));
    }
    frames.push_back(fib.Finish());
  }
  const auto hash_v = b.CreateVector(hash.data(), hash.size());
  const auto frames_v = b.CreateVector(frames);
  fb::StreamIndexBuilder sb(b);
  sb.add_codec(codec);
  sb.add_schema_version(1);
  sb.add_source_hash(hash_v);
  sb.add_frames(frames_v);
  b.Finish(sb.Finish(), "OVTI");
  return bytes_of(b);
}

struct FrameSpec {
  std::int32_t decode_index = 0;
  std::string plugin_id = "ovt-plugin-mock";
  std::string plugin_version = "1.0.0";
  std::uint32_t cells_w = 8;  ///< 1080p is 480 x 270 cells of 4x4 luma samples
  std::uint32_t cells_h = 8;
  bool with_pts = true;  ///< FrameInfo.pts set (int64: minalign 8, as every real record)
};

/// A FrameAnalysis ("OVTA", not size-prefixed) with a dense CellGrid: every vector filled, values varying in
/// 16x16-cell blocks like a real grid so that zstd finds structure (WI-0.091 acceptance test 3).
inline std::vector<std::uint8_t> make_frame(const FrameSpec& spec) {
  flatbuffers::FlatBufferBuilder b(1U << 20U);
  const std::size_t n = static_cast<std::size_t>(spec.cells_w) * spec.cells_h;
  std::vector<std::uint32_t> block_id(n);
  std::vector<std::uint16_t> partition_id(n);
  std::vector<std::uint8_t> pred(n);
  std::vector<std::int16_t> qp(n);
  std::vector<std::int16_t> intra_y(n);
  std::vector<std::int16_t> intra_c(n);
  std::vector<std::int16_t> mv0_x(n);
  std::vector<std::int16_t> mv0_y(n);
  std::vector<std::int16_t> mv1_x(n);
  std::vector<std::int16_t> mv1_y(n);
  std::vector<std::int8_t> ref0(n);
  std::vector<std::int8_t> ref1(n);
  std::vector<std::uint8_t> tx_w(n);
  std::vector<std::uint8_t> tx_h(n);
  std::vector<std::uint8_t> tx_type(n);
  std::vector<std::uint8_t> cbf(n);
  std::vector<std::uint32_t> bits(n);
  std::vector<std::uint8_t> bs_v(n);
  std::vector<std::uint8_t> bs_h(n);
  std::vector<std::uint8_t> filter(n);
  std::uint32_t rng = 0x9E3779B9U ^ static_cast<std::uint32_t>(spec.decode_index);
  auto next = [&rng]() {
    rng = rng * 1664525U + 1013904223U;
    return rng >> 8U;
  };
  for (std::uint32_t y = 0; y < spec.cells_h; ++y) {
    for (std::uint32_t x = 0; x < spec.cells_w; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * spec.cells_w + x;
      const std::uint32_t block = (y / 16U) * ((spec.cells_w + 15U) / 16U) + (x / 16U);
      const std::uint32_t r = next();
      block_id[i] = block;
      partition_id[i] = static_cast<std::uint16_t>(y / 64U);
      pred[i] = static_cast<std::uint8_t>((block % 5U == 0) ? 1 : 2);
      qp[i] = static_cast<std::int16_t>(26 + static_cast<int>(block % 7U));
      intra_y[i] = static_cast<std::int16_t>(pred[i] == 1 ? static_cast<int>(block % 35U) : -1);
      intra_c[i] = static_cast<std::int16_t>(pred[i] == 1 ? 4 : -1);
      mv0_x[i] = static_cast<std::int16_t>(static_cast<int>(block % 9U) * 4 - 16 + static_cast<int>(r % 3U));
      mv0_y[i] = static_cast<std::int16_t>(static_cast<int>(block % 5U) * 4 - 8);
      mv1_x[i] = static_cast<std::int16_t>(-mv0_x[i]);
      mv1_y[i] = static_cast<std::int16_t>(-mv0_y[i]);
      ref0[i] = static_cast<std::int8_t>(pred[i] == 1 ? -1 : 0);
      ref1[i] = static_cast<std::int8_t>(pred[i] == 1 ? -1 : static_cast<int>(block % 2U));
      tx_w[i] = static_cast<std::uint8_t>(2 + block % 3U);
      tx_h[i] = tx_w[i];
      tx_type[i] = 0;
      cbf[i] = static_cast<std::uint8_t>(r & 7U);
      bits[i] = 12U + (r % 40U);
      bs_v[i] = static_cast<std::uint8_t>(x % 4U == 0 ? 2 : 0);
      bs_h[i] = static_cast<std::uint8_t>(y % 4U == 0 ? 2 : 0);
      filter[i] = static_cast<std::uint8_t>(block % 3U);
    }
  }
  const auto grid = fb::CreateCellGrid(
      b, spec.cells_w, spec.cells_h, b.CreateVector(block_id), b.CreateVector(partition_id), b.CreateVector(pred),
      b.CreateVector(qp), b.CreateVector(intra_y), b.CreateVector(intra_c), b.CreateVector(mv0_x),
      b.CreateVector(mv0_y), b.CreateVector(mv1_x), b.CreateVector(mv1_y), b.CreateVector(ref0), b.CreateVector(ref1),
      b.CreateVector(tx_w), b.CreateVector(tx_h), b.CreateVector(tx_type), b.CreateVector(cbf), b.CreateVector(bits),
      b.CreateVector(bs_v), b.CreateVector(bs_h), b.CreateVector(filter));
  const auto plugin_id = b.CreateString(spec.plugin_id);
  const auto plugin_version = b.CreateString(spec.plugin_version);
  const fb::FrameId id(spec.decode_index, spec.decode_index);
  fb::FrameInfoBuilder fib(b);
  fib.add_id(&id);
  if (spec.with_pts) {
    fib.add_pts(static_cast<std::int64_t>(spec.decode_index) * 3000);
  }
  const auto frame = fib.Finish();
  fb::FrameAnalysisBuilder fab(b);
  fab.add_schema_version(1);
  fab.add_plugin_id(plugin_id);
  fab.add_plugin_version(plugin_version);
  fab.add_frame(frame);
  fab.add_grid(grid);
  b.Finish(fab.Finish(), "OVTA");
  return bytes_of(b);
}

}  // namespace ovt::cache::test

#endif  // OVT_TESTS_CACHE_TEST_HELPERS_HPP
