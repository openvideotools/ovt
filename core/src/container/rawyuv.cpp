// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Headerless raw YUV opening (chapter 08 §8.6.6, WI-1.229).

#include "container/rawyuv.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>
#include <system_error>

#include "container/tree_fb.hpp"
#include "ovt/color.h"

namespace ovt::container::raw {

namespace {

static_assert(sizeof(ovt_ColorDescription) == 44, "RawVideoFormat.color is the 44-byte ovt_ColorDescription image");

constexpr uint32_t kMaxSamplesPerTable = 1U << 20U;  // samples one sample_table() call materialises

std::optional<uint64_t> parse_u64(std::string_view s) {
  uint64_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

std::optional<uint32_t> parse_u32(std::string_view s) {
  const auto v = parse_u64(s);
  if (!v || *v > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<uint32_t>(*v);
}

std::optional<uint32_t> parse_dimension(std::string_view s) {
  const auto v = parse_u32(s);
  if (!v || *v == 0 || *v > kMaxDimension) {
    return std::nullopt;
  }
  return v;
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// Fills the derived fields; false when the geometry is out of range.
bool finalize(RawFormat& f) {
  f.frame_bytes = frame_bytes(f.pix, f.width, f.height);
  return f.frame_bytes != 0;
}

std::optional<std::pair<uint32_t, uint32_t>> reduce_ratio(uint64_t num, uint64_t den) {
  if (num == 0 || den == 0) {
    return std::nullopt;
  }
  const uint64_t g = std::gcd(num, den);
  num /= g;
  den /= g;
  if (num > UINT32_MAX || den > UINT32_MAX) {
    return std::nullopt;
  }
  return std::pair<uint32_t, uint32_t>{static_cast<uint32_t>(num), static_cast<uint32_t>(den)};
}

}  // namespace

// ------------------------------------------------------------------------------------------------ strings

std::optional<std::pair<uint32_t, uint32_t>> parse_fps(std::string_view text) {
  if (const size_t slash = text.find('/'); slash != std::string_view::npos) {
    const auto n = parse_u32(text.substr(0, slash));
    const auto d = parse_u32(text.substr(slash + 1));
    if (!n || !d || *n == 0 || *d == 0) {
      return std::nullopt;
    }
    return reduce_ratio(*n, *d);
  }
  // Decimal: digits ["." digits], at most 15 digits in all.
  const size_t dot = text.find('.');
  const std::string_view int_part = text.substr(0, dot);
  const std::string_view frac_part = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (int_part.empty() || (dot != std::string_view::npos && frac_part.empty()) ||
      int_part.size() + frac_part.size() > 15) {
    return std::nullopt;
  }
  const bool digits_only = std::all_of(int_part.begin(), int_part.end(), is_digit) &&
                           std::all_of(frac_part.begin(), frac_part.end(), is_digit);
  if (!digits_only) {
    return std::nullopt;
  }
  uint64_t den = 1;
  for (size_t i = 0; i < frac_part.size(); ++i) {
    den *= 10;
  }
  const auto whole = parse_u64(int_part);
  const auto frac = frac_part.empty() ? std::optional<uint64_t>(0) : parse_u64(frac_part);
  if (!whole || !frac || *whole > UINT32_MAX) {
    return std::nullopt;
  }
  const uint64_t num = *whole * den + *frac;
  // NTSC rates: 23.976, 29.97, 59.94 ... are written as decimals of x·1000/1001.
  const double value = static_cast<double>(num) / static_cast<double>(den);
  const double x = std::round(value * 1.001);
  if (x >= 1.0 && std::fabs(value - x * 1000.0 / 1001.0) < 0.0006 && frac_part.size() >= 2) {
    return reduce_ratio(static_cast<uint64_t>(x) * 1000, 1001);
  }
  return reduce_ratio(num, den);
}

std::optional<RawFormat> parse_format_string(std::string_view text) {
  // "WxH" ":" pix_fmt [":" fps]
  const size_t c1 = text.find(':');
  if (c1 == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view geometry = text.substr(0, c1);
  const std::string_view rest = text.substr(c1 + 1);
  const size_t c2 = rest.find(':');
  const std::string_view fmt_text = rest.substr(0, c2);
  const size_t x = geometry.find_first_of("xX");
  if (x == std::string_view::npos) {
    return std::nullopt;
  }
  RawFormat f;
  const auto w = parse_dimension(geometry.substr(0, x));
  const auto h = parse_dimension(geometry.substr(x + 1));
  auto pix = parse_pix_fmt(fmt_text);
  if (!w || !h || !pix) {
    return std::nullopt;
  }
  f.width = *w;
  f.height = *h;
  f.pix = std::move(*pix);
  if (c2 != std::string_view::npos) {
    const auto fps = parse_fps(rest.substr(c2 + 1));
    if (!fps) {
      return std::nullopt;
    }
    f.fps_num = fps->first;
    f.fps_den = fps->second;
  }
  if (!finalize(f)) {
    return std::nullopt;
  }
  return f;
}

// ------------------------------------------------------------------------------------------------- reader

uint64_t frame_count_of(const RawFormat& fmt, uint64_t file_size) noexcept {
  if (fmt.frame_bytes == 0 || file_size <= fmt.header_bytes) {
    return 0;
  }
  return (file_size - fmt.header_bytes) / fmt.frame_bytes;
}

namespace {

class RawReader final : public Reader {
 public:
  RawReader(uint64_t file_size, RawFormat fmt) : file_size_(file_size), f_(std::move(fmt)) {
    count_ = frame_count_of(f_, file_size_);
    const uint64_t used = f_.header_bytes < file_size_ ? count_ * f_.frame_bytes + f_.header_bytes : file_size_;
    const uint64_t tail = file_size_ - std::min(used, file_size_);
    if (tail > 0) {
      diags_.push_back({DiagCode::CNT_RAW_0001,
                        "`" + std::to_string(tail) + "` trailing bytes do not form a whole frame (ignored)", used});
    }
  }

  [[nodiscard]] int track_count() const noexcept override { return 1; }
  [[nodiscard]] std::vector<uint8_t> tree() const override { return build_tree(); }
  [[nodiscard]] std::vector<uint8_t> raw_format() const override { return build_raw_format(); }
  ovt_status expand(uint64_t box_offset) override { return box_offset == 0 ? OVT_OK : OVT_E_INVALID_ARG; }

  // At most kMaxSamplesPerTable samples per call (count 0 = as many as that); page with `first`.
  [[nodiscard]] std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const override {
    if (track_index != 0 || first > count_) {
      return {};
    }
    const uint64_t left = count_ - first;
    const uint64_t want = count == 0 ? left : std::min<uint64_t>(count, left);
    return build_sample_table(first, static_cast<size_t>(std::min<uint64_t>(want, kMaxSamplesPerTable)));
  }

 private:
  uint64_t file_size_;
  RawFormat f_;
  uint64_t count_ = 0;
  std::vector<Diagnostic> diags_;

  static std::vector<uint8_t> finish(treefb::Fbb& fbb) {
    const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
    return {bytes.begin(), bytes.end()};
  }

  [[nodiscard]] std::vector<uint8_t> build_sample_table(uint32_t first, size_t n) const {
    treefb::Fbb fbb;
    // SPEC-ASSUMPTION(#366): §8.6.6 says `dts = i`, `timescale = fps_num`, `duration = fps_den`; with that timescale
    // a frame lasts fps_den ticks, so dts = i * fps_den (as in the Y4M reader).
    std::vector<int64_t> dts(n);
    std::vector<uint64_t> offset(n);
    for (size_t k = 0; k < n; ++k) {
      const uint64_t i = first + k;
      dts[k] = static_cast<int64_t>(i * f_.fps_den);
      offset[k] = f_.header_bytes + i * f_.frame_bytes;
    }
    const std::vector<int32_t> cts_offset(n, 0);
    const std::vector<uint32_t> duration(n, f_.fps_den);
    const std::vector<uint32_t> size(n, static_cast<uint32_t>(std::min<uint64_t>(f_.frame_bytes, UINT32_MAX)));
    const std::vector<uint8_t> sync(n, 1);
    const std::vector<uint16_t> desc(n, 1);
    const std::vector<uint32_t> chunk(n, 0);
    const std::vector<int32_t> fragment(n, -1);
    const auto table = fb::CreateSampleTable(fbb, 1, f_.fps_num, n, fbb.CreateVector(dts), fbb.CreateVector(cts_offset),
                                             fbb.CreateVector(duration), fbb.CreateVector(offset),
                                             fbb.CreateVector(size), fbb.CreateVector(sync), 0, fbb.CreateVector(desc),
                                             fbb.CreateVector(chunk), fbb.CreateVector(fragment));
    fbb.Finish(table);
    return finish(fbb);
  }

  [[nodiscard]] std::vector<uint8_t> build_raw_format() const {
    treefb::Fbb fbb;
    ovt_ColorDescription color{};
    color.struct_size = sizeof(ovt_ColorDescription);
    color.primaries = 2;  // SPEC-ASSUMPTION(#366): unspecified until the sidecar (part b) can say otherwise
    color.transfer = 2;
    color.matrix = 2;
    color.full_range = 0;
    color.chroma_siting = 0;
    color.chroma_format = f_.pix.chroma_format;
    color.bit_depth = f_.pix.bit_depth;
    std::array<uint8_t, sizeof(ovt_ColorDescription)> image{};
    std::memcpy(image.data(), &color, image.size());

    const auto pix_fmt = fbb.CreateString(f_.pix.name);
    const auto color_vec = fbb.CreateVector(image.data(), image.size());
    fb::RawVideoFormatBuilder b(fbb);
    b.add_width(f_.width);
    b.add_height(f_.height);
    b.add_pix_fmt(pix_fmt);
    b.add_fps_num(f_.fps_num);
    b.add_fps_den(f_.fps_den);
    b.add_chroma_format(f_.pix.chroma_format);
    b.add_bit_depth(f_.pix.bit_depth);
    b.add_little_endian(f_.pix.little_endian);
    b.add_planar(f_.pix.planar);
    b.add_interleaved_uv(f_.pix.interleaved_uv);
    b.add_frame_bytes(f_.frame_bytes);
    b.add_frame_count(count_);
    b.add_header_bytes(f_.header_bytes);
    b.add_color(color_vec);
    fbb.Finish(b.Finish());
    return finish(fbb);
  }

  [[nodiscard]] std::vector<uint8_t> build_tree() const {
    treefb::Fbb fbb;
    const auto uint_f = [&](std::string_view name, uint64_t v) {
      return fb::CreateField(fbb, fbb.CreateString(name.data(), name.size()), fb::SyntaxValueKind::UInt, v);
    };
    const auto text_f = [&](std::string_view name, const std::string& v) {
      return fb::CreateField(fbb, fbb.CreateString(name.data(), name.size()), fb::SyntaxValueKind::String, 0, 0,
                             fbb.CreateString(v));
    };
    // The raw format lives outside the file (sidecar, command line), so the fields carry no bit span.
    std::vector<flatbuffers::Offset<fb::Field>> fields;
    fields.push_back(uint_f("width", f_.width));
    fields.push_back(uint_f("height", f_.height));
    fields.push_back(text_f("pix_fmt", f_.pix.name));
    fields.push_back(text_f("frame_rate", std::to_string(f_.fps_num) + "/" + std::to_string(f_.fps_den)));
    fields.push_back(uint_f("header_bytes", f_.header_bytes));
    fields.push_back(uint_f("frame_bytes", f_.frame_bytes));
    fields.push_back(uint_f("frame_count", count_));

    const fb::ByteSpan root_span(0, file_size_);
    const auto root = fb::CreateBoxNode(fbb, fbb.CreateString("RAW "), 0, &root_span,
                                        static_cast<uint8_t>(std::min<uint64_t>(f_.header_bytes, 255)), -1, -1,
                                        fbb.CreateVector(fields));
    std::vector<flatbuffers::Offset<fb::Diagnostic>> tree_diags;
    tree_diags.reserve(diags_.size());
    for (const Diagnostic& d : diags_) {
      tree_diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
    }

    const uint64_t dur = count_ * f_.fps_den;
    const double dur_s = f_.fps_num == 0 ? -1.0 : static_cast<double>(dur) / f_.fps_num;
    const auto handler_fourcc = fbb.CreateString("vide");
    fb::TrackInfoBuilder tb(fbb);
    tb.add_index(0);
    tb.add_track_id(1);
    tb.add_handler(fb::HandlerType::Video);
    tb.add_handler_fourcc(handler_fourcc);
    tb.add_codec(fb::Codec::Unknown);
    tb.add_timescale(f_.fps_num);
    tb.add_duration(dur);
    tb.add_duration_seconds(dur_s);
    tb.add_enabled(true);
    tb.add_in_movie(true);
    tb.add_width(f_.width);
    tb.add_height(f_.height);
    tb.add_coded_width(f_.width);
    tb.add_coded_height(f_.height);
    tb.add_sample_count(count_);
    const auto track = tb.Finish();
    fbb.Finish(fb::CreateContainerTree(fbb, fbb.CreateString("raw"), fbb.CreateVector(&root, 1),
                                       fbb.CreateVector(&track, 1), fbb.CreateVector(tree_diags)));
    return finish(fbb);
  }
};

}  // namespace

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, const RawFormat& fmt) {
  RawFormat f = fmt;
  if (f.frame_bytes == 0) {
    finalize(f);
  }
  return std::make_unique<RawReader>(file.size(), std::move(f));
}

}  // namespace ovt::container::raw
