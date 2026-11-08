// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The Y4M reader (chapter 08 §8.6.5, WI-1.228). Source of truth: the yuv4mpeg(5) man page and FFmpeg
// libavformat/yuv4mpegdec.c for the extended `C` tags.

#include "container/y4m.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "container/tree_fb.hpp"
#include "ovt/color.h"

namespace ovt::container::y4m {

namespace {

constexpr std::string_view kSignature = "YUV4MPEG2";
constexpr uint32_t kMaxDimension = 1U << 20U;  // per axis; keeps frame_bytes far below 2^64

static_assert(sizeof(ovt_ColorDescription) == 44, "RawVideoFormat.color is the 44-byte ovt_ColorDescription image");

std::optional<uint32_t> parse_u32(std::string_view s) {
  uint32_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (s.empty() || ec != std::errc() || ptr != s.data() + s.size()) {
    return std::nullopt;
  }
  return v;
}

// "num:den"
bool parse_ratio(std::string_view s, uint32_t& num, uint32_t& den) {
  const size_t colon = s.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  const auto n = parse_u32(s.substr(0, colon));
  const auto d = parse_u32(s.substr(colon + 1));
  if (!n || !d) {
    return false;
  }
  num = *n;
  den = *d;
  return true;
}

// Reads the line starting at `pos` (up to `cap` bytes, without the newline). nullopt when there is no newline
// within the cap or before EOF. `end` receives the offset just past the newline.
std::optional<std::string> read_line(const io::RandomAccessFile& file, uint64_t pos, size_t cap, uint64_t& end) {
  std::string line;
  std::array<uint8_t, 512> buf{};
  while (line.size() <= cap && pos + line.size() < file.size()) {
    const size_t got = file.read(pos + line.size(), buf.size(), buf.data());
    if (got == 0) {
      break;
    }
    for (size_t i = 0; i < got; ++i) {
      if (buf[i] == '\n') {
        end = pos + line.size() + i + 1;
        line.append(reinterpret_cast<const char*>(buf.data()), i);
        return line;
      }
      if (line.size() + i >= cap) {
        return std::nullopt;
      }
    }
    line.append(reinterpret_cast<const char*>(buf.data()), got);
  }
  return std::nullopt;
}

// Splits `line` (starting at file offset `base`) on spaces; the first `skip` bytes are not tokenised.
std::vector<Tag> tokenise(const std::string& line, uint64_t base, size_t skip) {
  std::vector<Tag> tags;
  size_t i = skip;
  while (i < line.size()) {
    if (line[i] == ' ') {
      ++i;
      continue;
    }
    const size_t start = i;
    while (i < line.size() && line[i] != ' ') {
      ++i;
    }
    tags.push_back({line.substr(start, i - start), base + start});
  }
  return tags;
}

std::string pix_fmt_of(std::string_view family, uint8_t depth, bool alpha) {
  std::string s(family);
  if (alpha) {
    return "yuva444p";
  }
  if (family == "gray") {
    return depth > 8 ? "gray" + std::to_string(depth) + "le" : "gray";
  }
  s = "yuv" + std::string(family) + "p";
  if (depth > 8) {
    s += std::to_string(depth) + "le";
  }
  return s;
}

uint64_t ceil_div(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

fb::Codec no_codec() {
  return fb::Codec::Unknown;
}

class Y4mReader final : public Reader {
 public:
  explicit Y4mReader(File parsed) : f_(std::move(parsed)) {
    materialised_ = std::min<size_t>(f_.frames.size(), kChildWindow);
  }

  [[nodiscard]] int track_count() const noexcept override { return 1; }
  [[nodiscard]] std::vector<uint8_t> tree() const override { return build_tree(); }
  [[nodiscard]] std::vector<uint8_t> raw_format() const override { return build_raw_format(); }

  ovt_status expand(uint64_t box_offset) override {
    if (box_offset == 0) {  // the Y4M root: the next window of FRAME children
      materialised_ = std::min<size_t>(f_.frames.size(), materialised_ + kChildWindow);
      return OVT_OK;
    }
    const auto it = std::lower_bound(f_.frames.begin(), f_.frames.end(), box_offset,
                                     [](const Frame& fr, uint64_t o) { return fr.header_offset < o; });
    return it != f_.frames.end() && it->header_offset == box_offset ? OVT_OK : OVT_E_INVALID_ARG;
  }

  [[nodiscard]] std::vector<uint8_t> sample_table(int track_index, uint32_t first, uint32_t count) const override {
    if (track_index != 0 || first > f_.frames.size()) {
      return {};
    }
    const size_t n = count == 0 ? f_.frames.size() - first : std::min<size_t>(count, f_.frames.size() - first);
    return build_sample_table(first, n);
  }

 private:
  File f_;
  size_t materialised_ = 0;

  static std::vector<uint8_t> finish(treefb::Fbb& fbb) {
    const std::span<const uint8_t> bytes(fbb.GetBufferPointer(), fbb.GetSize());
    return {bytes.begin(), bytes.end()};
  }

  [[nodiscard]] std::vector<uint8_t> build_sample_table(size_t first, size_t n) const {
    treefb::Fbb fbb;
    // SPEC-ASSUMPTION(#280): §8.6.5 says `dts = i`, `timescale = F.num`, `duration = F.den`; with that timescale a
    // frame lasts F.den ticks, so dts = i * F.den (the sample index times the frame duration).
    std::vector<int64_t> dts(n);
    const std::vector<int32_t> cts_offset(n, 0);
    const std::vector<uint32_t> duration(n, f_.fps_den);
    std::vector<uint64_t> offset(n);
    const std::vector<uint32_t> size(n, static_cast<uint32_t>(std::min<uint64_t>(f_.frame_bytes, UINT32_MAX)));
    const std::vector<uint8_t> sync(n, 1);
    const std::vector<uint16_t> desc(n, 1);
    const std::vector<uint32_t> chunk(n, 0);
    const std::vector<int32_t> fragment(n, -1);
    for (size_t k = 0; k < n; ++k) {
      dts[k] = static_cast<int64_t>(first + k) * f_.fps_den;
      offset[k] = f_.frames[first + k].data_offset;
    }
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
    // SPEC-ASSUMPTION(#280): Y4M carries no primaries/transfer/matrix; H.273 value 2 (unspecified) is written.
    color.primaries = 2;
    color.transfer = 2;
    color.matrix = 2;
    color.full_range = f_.full_range ? 1 : 0;
    color.chroma_siting = f_.colour.siting;
    color.chroma_format = f_.colour.chroma_format;
    color.bit_depth = f_.colour.bit_depth;
    std::array<uint8_t, sizeof(ovt_ColorDescription)> image{};
    std::memcpy(image.data(), &color, image.size());

    const auto pix_fmt = fbb.CreateString(f_.colour.pix_fmt);
    const auto color_vec = fbb.CreateVector(image.data(), image.size());
    fb::RawVideoFormatBuilder b(fbb);
    b.add_width(f_.width);
    b.add_height(f_.height);
    b.add_pix_fmt(pix_fmt);
    b.add_fps_num(f_.fps_num);
    b.add_fps_den(f_.fps_den);
    b.add_chroma_format(f_.colour.chroma_format);
    b.add_bit_depth(f_.colour.bit_depth);
    b.add_little_endian(true);
    b.add_planar(true);
    b.add_interleaved_uv(false);
    b.add_frame_bytes(f_.frame_bytes);
    b.add_frame_count(f_.frames.size());
    b.add_header_bytes(f_.header_bytes);
    b.add_color(color_vec);
    fbb.Finish(b.Finish());
    return finish(fbb);
  }

  [[nodiscard]] std::vector<uint8_t> build_tree() const {
    using treefb::string_field;
    using treefb::uint_field;
    treefb::Fbb fbb;

    std::vector<flatbuffers::Offset<fb::BoxNode>> frames;
    frames.reserve(materialised_);
    for (size_t i = 0; i < materialised_; ++i) {
      const Frame& fr = f_.frames[i];
      std::vector<flatbuffers::Offset<fb::Field>> fields;
      fields.reserve(fr.tags.size());
      for (const Tag& t : fr.tags) {
        fields.push_back(string_field(fbb, "frame_tag", t.text, (t.offset - fr.header_offset) * 8U,
                                      static_cast<uint32_t>(t.text.size() * 8U)));
      }
      const uint64_t line = fr.data_offset - fr.header_offset;
      const fb::ByteSpan span(fr.header_offset, line + f_.frame_bytes);
      const fb::ByteSpan payload(fr.data_offset, f_.frame_bytes);
      frames.push_back(fb::CreateBoxNode(fbb, fbb.CreateString("FRAME"), 0, &span,
                                         static_cast<uint8_t>(std::min<uint64_t>(line, 255)), -1, -1,
                                         fbb.CreateVector(fields), &payload));
    }

    std::vector<flatbuffers::Offset<fb::Field>> header_fields;
    header_fields.reserve(f_.header_tags.size() + 1);
    header_fields.push_back(
        string_field(fbb, "signature", kSignature, 0, static_cast<uint32_t>(kSignature.size() * 8)));
    for (const Tag& t : f_.header_tags) {
      std::string field_name;
      switch (t.text[0]) {
        case 'W':
          field_name = "width";
          break;
        case 'H':
          field_name = "height";
          break;
        case 'F':
          field_name = "frame_rate";
          break;
        case 'I':
          field_name = "interlacing";
          break;
        case 'A':
          field_name = "pixel_aspect";
          break;
        case 'C':
          field_name = "colour_space";
          break;
        case 'X':
          field_name = "comment";
          break;
        default:
          field_name = std::string("tag_") + t.text[0];
          break;  // unknown tags are kept
      }
      const std::string value = t.text[0] == 'X' ? t.text : t.text.substr(1);
      header_fields.push_back(
          string_field(fbb, field_name, value, t.offset * 8U, static_cast<uint32_t>(t.text.size() * 8U)));
    }

    std::vector<flatbuffers::Offset<fb::Diagnostic>> root_diags;
    if (materialised_ < f_.frames.size()) {  // CNT-0007: lazy children
      root_diags.push_back(treefb::diagnostic(fbb, DiagCode::CNT_0007,
                                              "Y4M root at offset `0` has " + std::to_string(materialised_) + " of " +
                                                  std::to_string(f_.frames.size()) +
                                                  " FRAME children (call `ovt_container_expand`)",
                                              0));
    }
    const fb::ByteSpan root_span(0, f_.file_size);
    const auto root = fb::CreateBoxNode(
        fbb, fbb.CreateString("Y4M "), 0, &root_span, static_cast<uint8_t>(std::min<uint64_t>(f_.header_bytes, 255)),
        -1, -1, fbb.CreateVector(header_fields), nullptr, fbb.CreateVector(frames), 0, 0, fbb.CreateVector(root_diags));

    std::vector<flatbuffers::Offset<fb::Diagnostic>> tree_diags;
    tree_diags.reserve(f_.diagnostics.size());
    for (const Diagnostic& d : f_.diagnostics) {
      tree_diags.push_back(treefb::diagnostic(fbb, d.code, d.message, d.offset));
    }

    // TrackInfo (§8.6.5): codec Unknown, timescale F.num, duration = frames * F.den.
    const uint64_t dur = static_cast<uint64_t>(f_.frames.size()) * f_.fps_den;
    const double dur_s = f_.fps_num == 0 ? -1.0 : static_cast<double>(dur) / f_.fps_num;
    const auto handler_fourcc = fbb.CreateString("vide");
    fb::TrackInfoBuilder tb(fbb);
    tb.add_index(0);
    tb.add_track_id(1);
    tb.add_handler(fb::HandlerType::Video);
    tb.add_handler_fourcc(handler_fourcc);
    tb.add_codec(no_codec());
    tb.add_timescale(f_.fps_num);
    tb.add_duration(dur);
    tb.add_duration_seconds(dur_s);
    tb.add_enabled(true);
    tb.add_in_movie(true);
    tb.add_width(f_.width);
    tb.add_height(f_.height);
    tb.add_coded_width(f_.width);
    tb.add_coded_height(f_.height);
    tb.add_sample_count(f_.frames.size());
    const auto track = tb.Finish();
    const auto tracks = fbb.CreateVector(&track, 1);
    fbb.Finish(fb::CreateContainerTree(fbb, fbb.CreateString("y4m"), fbb.CreateVector(&root, 1), tracks,
                                       fbb.CreateVector(tree_diags)));
    return finish(fbb);
  }
};

// Applies the stream header tags to `f` (W, H, F, I, A, C, XCOLORRANGE; unknown tags stay in header_tags only).
void apply_header_tags(File& f, bool& have_w, bool& have_h, bool& have_f, std::string& c_value) {
  for (const Tag& t : f.header_tags) {
    const std::string_view v = std::string_view(t.text).substr(1);
    switch (t.text[0]) {
      case 'W':
        if (const auto n = parse_u32(v)) {
          f.width = *n;
          have_w = true;
        }
        break;
      case 'H':
        if (const auto n = parse_u32(v)) {
          f.height = *n;
          have_h = true;
        }
        break;
      case 'F':
        have_f = parse_ratio(v, f.fps_num, f.fps_den);
        break;
      case 'I':
        f.interlacing = v.empty() ? '?' : v[0];
        break;
      case 'A':
        if (!parse_ratio(v, f.aspect_num, f.aspect_den)) {
          f.aspect_num = f.aspect_den = 0;
        }
        break;
      case 'C':
        c_value = std::string(v);
        break;
      case 'X':
        if (v == "COLORRANGE=FULL") {
          f.full_range = true;
        } else if (v == "COLORRANGE=LIMITED") {
          f.full_range = false;
        }
        break;
      default:  // unknown tags stay in header_tags only
        break;
    }
  }
}

}  // namespace

std::optional<ColourSpace> colour_space(std::string_view c) noexcept {
  ColourSpace cs;
  struct Row {
    std::string_view name;
    uint8_t chroma;
    uint8_t depth;
    uint8_t siting;
    bool alpha;
    std::string_view family;
  };
  static constexpr std::array<Row, 8> kFixed = {{
      {"420jpeg", 1, 8, 2, false, "420"},
      {"420", 1, 8, 2, false, "420"},
      {"420mpeg2", 1, 8, 1, false, "420"},
      {"420paldv", 1, 8, 3, false, "420"},
      {"422", 2, 8, 1, false, "422"},
      {"444", 3, 8, 0, false, "444"},
      {"444alpha", 3, 8, 0, true, "444"},
      {"411", 4, 8, 0, false, "411"},
  }};
  for (const Row& r : kFixed) {
    if (r.name == c) {
      cs.chroma_format = r.chroma;
      cs.bit_depth = r.depth;
      cs.siting = r.siting;
      cs.alpha = r.alpha;
      cs.pix_fmt = pix_fmt_of(r.family, r.depth, r.alpha);
      return cs;
    }
  }
  if (c == "mono") {
    cs = ColourSpace{0, 8, 0, false, "gray"};
    return cs;
  }
  // monoNN and (420|422|444)pNN with NN in {10, 12, 14, 16}; mono14 is OVT-native (§8.6.5).
  const auto depth_of = [](std::string_view s) -> std::optional<uint8_t> {
    if (s == "10" || s == "12" || s == "14" || s == "16") {
      return static_cast<uint8_t>((s[0] - '0') * 10 + (s[1] - '0'));
    }
    return std::nullopt;
  };
  if (c.starts_with("mono")) {
    if (const auto d = depth_of(c.substr(4))) {
      return ColourSpace{0, *d, 0, false, pix_fmt_of("gray", *d, false)};
    }
    return std::nullopt;
  }
  if (c.size() > 4 && c[3] == 'p') {
    const std::string_view family = c.substr(0, 3);
    const auto d = depth_of(c.substr(4));
    if (d && (family == "420" || family == "422" || family == "444")) {
      const uint8_t chroma = family == "420" ? 1 : (family == "422" ? 2 : 3);
      return ColourSpace{chroma, *d, 0, false, pix_fmt_of(family, *d, false)};
    }
  }
  return std::nullopt;
}

uint64_t frame_bytes(const ColourSpace& cs, uint32_t width, uint32_t height) noexcept {
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
    return 0;
  }
  const uint64_t w = width;
  const uint64_t h = height;
  uint64_t samples = w * h;
  switch (cs.chroma_format) {
    case 1:
      samples += 2 * ceil_div(w, 2) * ceil_div(h, 2);
      break;
    case 2:
      samples += 2 * ceil_div(w, 2) * h;
      break;
    case 3:
      samples += 2 * w * h;
      break;
    case 4:
      samples += 2 * ceil_div(w, 4) * h;
      break;
    default:  // 4:0:0
      break;
  }
  if (cs.alpha) {
    samples += w * h;
  }
  return samples * (cs.bit_depth > 8 ? 2U : 1U);
}

std::optional<File> parse(const io::RandomAccessFile& file) {
  File f;
  f.file_size = file.size();
  uint64_t pos = 0;
  const auto header = read_line(file, 0, kMaxHeaderLine, pos);
  if (!header || !header->starts_with(kSignature) ||
      (header->size() > kSignature.size() && (*header)[kSignature.size()] != ' ')) {
    return std::nullopt;
  }
  f.header_bytes = pos;
  f.header_tags = tokenise(*header, 0, kSignature.size());

  bool have_w = false;
  bool have_h = false;
  bool have_f = false;
  std::string c_value = "420jpeg";
  apply_header_tags(f, have_w, have_h, have_f, c_value);
  if (!have_w || !have_h || f.width == 0 || f.height == 0 || f.width > kMaxDimension || f.height > kMaxDimension) {
    return std::nullopt;
  }
  if (!have_f) {
    f.fps_num = 25;
    f.fps_den = 1;
    f.diagnostics.push_back({DiagCode::CNT_Y4M_0001, "Y4M header without `F` tag; `25:1` assumed", 0});
  }

  f.c_value = c_value;
  const auto cs = colour_space(c_value);
  if (!cs) {
    f.unsupported_colour = true;
    f.diagnostics.push_back(
        {DiagCode::CNT_Y4M_0002, "Y4M `C` tag `" + c_value + "` unknown; open fails with `OVT_E_UNSUPPORTED`", 0});
    return f;
  }
  f.colour = *cs;
  f.frame_bytes = frame_bytes(f.colour, f.width, f.height);

  // Frame walk: "FRAME" [tags] "\n" then the planes, verified at every frame.
  while (pos < f.file_size) {
    Frame fr;
    fr.header_offset = pos;
    uint64_t data = 0;
    const auto line = read_line(file, pos, kMaxFrameLine, data);
    if (!line || !(*line == "FRAME" || line->starts_with("FRAME "))) {
      f.diagnostics.push_back({DiagCode::CNT_Y4M_0003,
                               "`FRAME` magic missing at offset `" + std::to_string(pos) + "` (frame `" +
                                   std::to_string(f.frames.size()) + "`); walk stopped",
                               pos});
      break;
    }
    if (f.file_size - data < f.frame_bytes) {
      // SPEC-ASSUMPTION(#280): a last frame whose planes are cut short is not counted and reported with CNT-Y4M-0003.
      f.diagnostics.push_back({DiagCode::CNT_Y4M_0003,
                               "`FRAME` at offset `" + std::to_string(pos) + "` (frame `" +
                                   std::to_string(f.frames.size()) + "`) is truncated: `" +
                                   std::to_string(f.file_size - data) + "` of `" + std::to_string(f.frame_bytes) +
                                   "` plane bytes; walk stopped",
                               pos});
      break;
    }
    fr.data_offset = data;
    fr.tags = tokenise(*line, pos, 5);
    f.frames.push_back(std::move(fr));
    pos = data + f.frame_bytes;
  }
  return f;
}

std::unique_ptr<Reader> make_reader(const io::RandomAccessFile& file, ovt_status* why) {
  auto parsed = parse(file);
  if (!parsed) {
    if (why != nullptr) {
      *why = OVT_E_CORRUPT;
    }
    return nullptr;
  }
  if (parsed->unsupported_colour) {
    if (why != nullptr) {
      *why = OVT_E_UNSUPPORTED;
    }
    return nullptr;
  }
  return std::make_unique<Y4mReader>(std::move(*parsed));
}

}  // namespace ovt::container::y4m
