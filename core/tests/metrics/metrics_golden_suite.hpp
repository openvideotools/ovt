// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Shared pieces of the PSNR/SSIM golden suite (chapter 14 §14.4.10, §14.4.12; WI-3.590): the three metric pairs of
// WI-0.565 with their corpus files, a Y4M reader for 8-bit and 10-bit 4:2:0 that yields ovt_MetricPlanes, the
// gzip-compressed oracle goldens of tests/golden/metrics/, and a small metric instance wrapper. The goldens are the
// committed WI-0.565 files and are only read here, never regenerated.

#ifndef OVT_CORE_TESTS_METRICS_METRICS_GOLDEN_SUITE_HPP
#define OVT_CORE_TESTS_METRICS_METRICS_GOLDEN_SUITE_HPP

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ovt/abi/metric_v1.h"
#include "ovt_metrics_generated.h"
#include "test_support/corpus.h"

namespace ovt::test::metrics_suite {

// One metric pair of WI-0.565 (tests/golden/metrics/metrics-index.json "pairs"): the corpus entry and file of each
// side (an empty file name means the entry's primary file) and the number of pictures the goldens cover.
struct GoldenPair {
  std::string_view id;
  std::string_view ref_entry;
  std::string_view ref_file;
  std::string_view dist_entry;
  std::string_view dist_file;
  uint32_t frames;
  uint32_t bit_depth;
  // When the entry holds no `dist_file`: the video file of the entry that is decoded to it (empty = none).
  std::string_view dist_source;
};

// The pairs of §14.4.10 / §14.4.12. syn_metrics_1080p's distorted side is the x264 stream of recipe 25 (chapter 14
// §14.3.3), which the corpus holds as .264/.mp4 only: like tools/goldens/gen_metrics.py's to_y4m(), the test decodes
// the first `frames` pictures of the .mp4 with the ffmpeg CLI into a temporary Y4M (DistY4m).
inline constexpr std::array<GoldenPair, 3> kPairs = {{
    {"netflix_src01_576x324", "netflix_src01_hrc00_576x324", "", "netflix_src01_hrc01_576x324", "", 48, 8, ""},
    {"syn_metrics_1080p", "syn_metrics_1080p_pair", "syn_metrics_1080p_ref.y4m", "syn_metrics_1080p_pair",
     "syn_metrics_1080p_dist.y4m", 60, 8, "syn_metrics_1080p_dist.mp4"},
    {"syn_metrics_pq_1080p", "syn_metrics_pq_1080p_pair", "syn_metrics_pq_1080p_ref.y4m", "syn_metrics_pq_1080p_pair",
     "syn_metrics_pq_1080p_dist.y4m", 60, 10, ""},
}};

// The file `name` of corpus entry `entry` (its primary file when `name` is empty). Skips the running test (or fails
// it under OVT_REQUIRE_CORPORA=1) when the entry is absent, and skips it when the entry lacks the named file.
std::filesystem::path corpus_file(std::string_view entry, std::string_view name);

// The distorted Y4M of `pair`, decoded into a temporary file when the entry holds only `dist_source`; the file is
// removed when the object is destroyed. Skips the test when the entry is absent, and skips it (or fails it under
// OVT_REQUIRE_CORPORA=1) when the ffmpeg CLI is missing or cannot decode the source.
class DistY4m {
 public:
  explicit DistY4m(const GoldenPair& pair);
  DistY4m(const DistY4m&) = delete;
  DistY4m& operator=(const DistY4m&) = delete;
  DistY4m(DistY4m&&) = delete;
  DistY4m& operator=(DistY4m&&) = delete;
  ~DistY4m();
  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  bool temporary_ = false;
};

// A decoded picture: three planes of 4:2:0 samples, 1 byte (8-bit) or 2 bytes little-endian (9..16-bit).
struct Picture {
  std::array<std::vector<uint8_t>, 3> buf;
  ovt_MetricPlanes planes{};

  Picture(uint32_t w, uint32_t h, uint32_t bits) {
    planes.struct_size = sizeof(ovt_MetricPlanes);
    planes.planes = 3;
    planes.bit_depth = static_cast<uint8_t>(bits);
    planes.sample_bytes = bits > 8 ? 2 : 1;
    planes.sample_format = OVT_METRIC_SAMPLE_UINT;
    planes.domain = OVT_METRIC_DOMAIN_CODE_VALUE;
    for (size_t p = 0; p < 3; ++p) {
      const uint32_t pw = p == 0 ? w : (w + 1) / 2;
      const uint32_t ph = p == 0 ? h : (h + 1) / 2;
      const uint32_t stride = pw * planes.sample_bytes;
      buf.at(p).assign(static_cast<size_t>(stride) * ph, 0);
      planes.data[p] = buf.at(p).data();
      planes.stride_bytes[p] = static_cast<int32_t>(stride);
      planes.width[p] = pw;
      planes.height[p] = ph;
    }
  }
  Picture(const Picture&) = delete;
  Picture& operator=(const Picture&) = delete;
  Picture(Picture&&) = delete;
  Picture& operator=(Picture&&) = delete;
  ~Picture() = default;

  [[nodiscard]] uint32_t sample(size_t p, uint32_t x, uint32_t y) const {
    const size_t off = (static_cast<size_t>(y) * static_cast<size_t>(planes.stride_bytes[p])) +
                       (static_cast<size_t>(x) * planes.sample_bytes);
    const auto& b = buf.at(p);
    return planes.sample_bytes == 1 ? b[off]
                                    : (static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8U));
  }
};

// Sequential reader of a 4:2:0 Y4M file (C420, C420jpeg, C420mpeg2, C420paldv, C420p10, C420p12; no C tag = 420).
class Y4mReader {
 public:
  explicit Y4mReader(const std::filesystem::path& path) : in_(path, std::ios::binary) {
    REQUIRE(in_);
    std::string header;
    std::getline(in_, header);
    REQUIRE(header.rfind("YUV4MPEG2 ", 0) == 0);
    size_t pos = 10;
    while (pos < header.size()) {
      size_t end = header.find(' ', pos);
      if (end == std::string::npos) {
        end = header.size();
      }
      const std::string tok = header.substr(pos, end - pos);
      if (!tok.empty() && tok[0] == 'W') {
        width_ = static_cast<uint32_t>(std::stoul(tok.substr(1)));
      } else if (!tok.empty() && tok[0] == 'H') {
        height_ = static_cast<uint32_t>(std::stoul(tok.substr(1)));
      } else if (!tok.empty() && tok[0] == 'C') {
        REQUIRE(tok.rfind("C420", 0) == 0);
        const size_t p = tok.find('p', 4);
        if (p != std::string::npos && p + 1 < tok.size() && std::isdigit(static_cast<unsigned char>(tok[p + 1])) != 0) {
          bit_depth_ = static_cast<uint32_t>(std::stoul(tok.substr(p + 1)));
        }
      }
      pos = end + 1;
    }
    REQUIRE(width_ > 0);
    REQUIRE(height_ > 0);
  }

  // Reads the next picture into `pic` (which must have this file's size and depth); false at the end of the file.
  bool next(Picture& pic) {
    std::string line;
    if (!std::getline(in_, line)) {
      return false;
    }
    REQUIRE(line.rfind("FRAME", 0) == 0);
    for (auto& plane : pic.buf) {
      in_.read(reinterpret_cast<char*>(plane.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
               static_cast<std::streamsize>(plane.size()));
    }
    return static_cast<bool>(in_);
  }

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  [[nodiscard]] uint32_t bit_depth() const { return bit_depth_; }

 private:
  std::ifstream in_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t bit_depth_ = 8;
};

// The JSON of a gzip-compressed golden under tests/golden/metrics/ (gzip is on every desktop CI image, as in
// test_metrics_vmaf.cpp; only [needs-corpus] tests read these files).
nlohmann::json read_golden_gz(const std::string& name);

// A score of a golden: a number, or the strings "inf" / "-inf" that tests/oracles/normalize/canon.py writes for
// infinities (FFmpeg prints `inf` for identical planes).
double golden_value(const nlohmann::json& v);

// FFmpeg's `%0.2f` of a double, read back: the value the psnr filter would print for `v`.
double round2(double v);

struct Score {
  std::vector<double> values;
  std::string map_blob_id;
  uint16_t map_block_size = 0;
};

// One instance of an in-tree metric (ovt_metric_v1 function table).
class Metric {
 public:
  Metric(const ovt_MetricApi* api, const char* options) : api_(api) {
    REQUIRE(api_ != nullptr);
    REQUIRE(api_->create(options, &m_) == OVT_OK);
  }
  Metric(const Metric&) = delete;
  Metric& operator=(const Metric&) = delete;
  Metric(Metric&&) = delete;
  Metric& operator=(Metric&&) = delete;
  ~Metric() { api_->destroy(m_); }

  [[nodiscard]] Score score(const ovt_MetricPlanes& r, const ovt_MetricPlanes& d, int32_t frame) const {
    ovt_Buffer out{};
    REQUIRE(api_->score(m_, &r, &d, ovt_FrameId{frame, frame}, &out) == OVT_OK);
    REQUIRE(out.data != nullptr);
    const auto* fs = flatbuffers::GetRoot<ovt::fb::FrameScore>(out.data);
    Score s;
    if (fs->values() != nullptr) {
      s.values.assign(fs->values()->begin(), fs->values()->end());
    }
    if (fs->map_blob_id() != nullptr) {
      s.map_blob_id = fs->map_blob_id()->str();
    }
    s.map_block_size = fs->map_block_size();
    return s;
  }

 private:
  const ovt_MetricApi* api_;
  ovt_Metric* m_ = nullptr;
};

// Runs `fn(frame_index, ref, dist)` over the first `pair.frames` pictures of the pair; returns the count read.
template <typename Fn>
uint32_t for_each_frame(const GoldenPair& pair, Fn&& fn) {
  const auto ref_path = corpus_file(pair.ref_entry, pair.ref_file);
  const DistY4m dist(pair);
  Y4mReader ry(ref_path);
  Y4mReader dy(dist.path());
  REQUIRE(ry.width() == dy.width());
  REQUIRE(ry.height() == dy.height());
  REQUIRE(ry.bit_depth() == pair.bit_depth);
  REQUIRE(dy.bit_depth() == pair.bit_depth);
  Picture r(ry.width(), ry.height(), ry.bit_depth());
  Picture d(dy.width(), dy.height(), dy.bit_depth());
  uint32_t n = 0;
  while (n < pair.frames && ry.next(r) && dy.next(d)) {
    fn(n, r, d);
    ++n;
  }
  return n;
}

}  // namespace ovt::test::metrics_suite

#endif  // OVT_CORE_TESTS_METRICS_METRICS_GOLDEN_SUITE_HPP
